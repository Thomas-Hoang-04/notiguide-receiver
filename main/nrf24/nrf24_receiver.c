/**
 * @file nrf24_receiver.c
 * @brief Native ESP-IDF nRF24L01/L01+ receiver implementation.
 */

#include "nrf24/nrf24_receiver.h"
#include <stdint.h> // IWYU pragma: keep
#include <string.h> // IWYU pragma: keep
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "nrf24/nrf24_regs.h" // IWYU pragma: keep
#include "sdkconfig.h"
#include "trigger/rf_trigger.h" // IWYU pragma: keep

#if CONFIG_RECEIVER_RADIO_2_4G

#define NRF24_TAG "NRF24"
#define NRF24_SPI_HOST SPI2_HOST
#define CE_GPIO  ((gpio_num_t)CONFIG_RECEIVER_NRF24_CE)
#define IRQ_GPIO ((gpio_num_t)CONFIG_RECEIVER_NRF24_IRQ)

static spi_device_handle_t s_spi;
static TaskHandle_t s_notify_to;

/**
 * @brief Execute a raw SPI transfer against the nRF24.
 *
 * Sends the command byte followed by an optional payload, copies any returned
 * payload bytes into @p rx, and returns the STATUS byte shifted out by the
 * transceiver during the transaction.
 *
 * @param cmd nRF24 command opcode.
 * @param tx Optional payload to transmit after the command byte.
 * @param rx Optional buffer that receives payload bytes after the status byte.
 * @param size Number of payload bytes transferred after the command byte.
 * @return STATUS register value observed during the transfer.
 */
static uint8_t nrf_xfer(uint8_t cmd, const uint8_t *tx, uint8_t *rx, size_t size)
{
    uint8_t tx_buf[1 + NRF_PAYLOAD_WIDTH] = { cmd };
    uint8_t rx_buf[1 + NRF_PAYLOAD_WIDTH] = { 0 };

    if (tx != NULL && size > 0) {
        memcpy(&tx_buf[1], tx, size);
    }

    spi_transaction_t transaction = {
        .length = (size_t)(8U * (1U + size)),
        .tx_buffer = tx_buf,
        .rx_buffer = rx_buf,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &transaction));

    if (rx != NULL && size > 0) {
        memcpy(rx, &rx_buf[1], size);
    }
    return rx_buf[0];
}

/**
 * @brief Issue a command that has no payload phase.
 *
 * @param cmd nRF24 command opcode.
 * @return STATUS register value observed during the transfer.
 */
static inline uint8_t nrf_cmd(uint8_t cmd)
{
    return nrf_xfer(cmd, NULL, NULL, 0);
}

/**
 * @brief Read one or more bytes from an nRF24 register.
 *
 * @param reg Register address.
 * @param rx Buffer that receives the register bytes.
 * @param size Number of bytes to read.
 * @return STATUS register value observed during the transfer.
 */
static inline uint8_t nrf_rreg(uint8_t reg, uint8_t *rx, size_t size)
{
    return nrf_xfer((uint8_t)(NRF_CMD_R_REGISTER | (reg & 0x1F)), NULL, rx, size);
}

/**
 * @brief Write one or more bytes to an nRF24 register.
 *
 * @param reg Register address.
 * @param tx Buffer containing the bytes to write.
 * @param size Number of bytes to write.
 * @return STATUS register value observed during the transfer.
 */
static inline uint8_t nrf_wreg(uint8_t reg, const uint8_t *tx, size_t size)
{
    return nrf_xfer((uint8_t)(NRF_CMD_W_REGISTER | (reg & 0x1F)), tx, NULL, size);
}

/**
 * @brief Read a single-byte nRF24 register.
 *
 * @param reg Register address.
 * @return Register value read from the device.
 */
static inline uint8_t nrf_rreg8(uint8_t reg)
{
    uint8_t value = 0;
    (void)nrf_rreg(reg, &value, sizeof(value));
    return value;
}

/**
 * @brief Write a single-byte nRF24 register.
 *
 * @param reg Register address.
 * @param value Byte value written to the device.
 */
static inline void nrf_wreg8(uint8_t reg, uint8_t value)
{
    (void)nrf_wreg(reg, &value, sizeof(value));
}

/**
 * @brief Wake the RX task when the nRF24 asserts its IRQ line.
 *
 * @param arg Unused ISR argument.
 */
static void IRAM_ATTR nrf_isr(void *arg)
{
    (void)arg;
    BaseType_t higher_priority_woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_notify_to, &higher_priority_woken);
    portYIELD_FROM_ISR(higher_priority_woken);
}

/**
 * @brief Drain the RX FIFO and forward each payload to the trigger matcher.
 *
 * The task blocks on direct notifications from the IRQ ISR, then reads all
 * queued payloads before waiting again.
 *
 * @param arg Unused task argument.
 */
static void nrf_rx_task(void *arg)
{
    (void)arg;

    uint8_t payload[NRF_PAYLOAD_WIDTH];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        for (;;) {
            if ((nrf_rreg8(NRF_REG_FIFO_STATUS) & NRF_FIFO_RX_EMPTY) != 0U) {
                break;
            }

#if CONFIG_RECEIVER_NRF24_ENABLE_DPL
            uint8_t len = 0;
            (void)nrf_xfer(NRF_CMD_R_RX_PL_WID, NULL, &len, sizeof(len));
            if (len == 0 || len > NRF_PAYLOAD_WIDTH) {
                (void)nrf_cmd(NRF_CMD_FLUSH_RX);
                nrf_wreg8(NRF_REG_STATUS, NRF_ST_RX_DR);
                break;
            }
            (void)nrf_xfer(NRF_CMD_R_RX_PAYLOAD, NULL, payload, len);
            rf_trigger_on_packet(payload, len);
#else
            (void)nrf_xfer(NRF_CMD_R_RX_PAYLOAD, NULL, payload, NRF_PAYLOAD_WIDTH);
            rf_trigger_on_packet(payload, NRF_PAYLOAD_WIDTH);
#endif
            nrf_wreg8(NRF_REG_STATUS, NRF_ST_RX_DR);
        }
    }
}

/**
 * @brief Detect whether the attached radio is a legacy or plus nRF24 variant.
 *
 * The probe relies on the FEATURE register activation behavior, which differs
 * between the original nRF24L01 and the nRF24L01+.
 *
 * @return Detected chip variant, or `NRF_CHIP_UNKNOWN` when probing fails.
 */
static nrf24_variant_t nrf_probe_chip(void)
{
    nrf_wreg8(NRF_REG_FEATURE, NRF_FEATURE_EN_DPL);
    if (nrf_rreg8(NRF_REG_FEATURE) == NRF_FEATURE_EN_DPL) {
        nrf_wreg8(NRF_REG_FEATURE, 0U);
        return NRF_CHIP_PLUS;
    }

    const uint8_t magic = NRF_ACTIVATE_MAGIC;
    (void)nrf_xfer(NRF_CMD_ACTIVATE, &magic, NULL, sizeof(magic));
    nrf_wreg8(NRF_REG_FEATURE, NRF_FEATURE_EN_DPL);
    if (nrf_rreg8(NRF_REG_FEATURE) == NRF_FEATURE_EN_DPL) {
        nrf_wreg8(NRF_REG_FEATURE, 0U);
        return NRF_CHIP_LEGACY;
    }

    return NRF_CHIP_UNKNOWN;
}

/**
 * @brief Configure GPIOs and program the nRF24 into receive mode.
 *
 * @param handle Receiver state structure updated with the detected variant.
 * @return ESP_OK on success, error code otherwise.
 */
static esp_err_t nrf_bringup(nrf24_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, NRF24_TAG, "handle is NULL");

    ESP_RETURN_ON_ERROR(gpio_config(&(gpio_config_t){
        .pin_bit_mask = 1ULL << CE_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    }), NRF24_TAG, "failed to configure CE");
    gpio_set_level(CE_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(NRF_TPOR_MS));

    uint8_t setup_aw = nrf_rreg8(NRF_REG_SETUP_AW);
    ESP_RETURN_ON_FALSE((setup_aw & 0x03U) != 0U && (setup_aw & 0xFCU) == 0U,
                        ESP_ERR_NOT_FOUND, NRF24_TAG, "invalid SETUP_AW: 0x%02x", setup_aw);

    handle->chip = nrf_probe_chip();
    ESP_RETURN_ON_FALSE(handle->chip != NRF_CHIP_UNKNOWN, ESP_ERR_INVALID_RESPONSE, NRF24_TAG,
                        "chip probe failed");

    nrf_wreg8(NRF_REG_CONFIG,
              NRF_CFG_MASK_TX_DS | NRF_CFG_MASK_MAX_RT | NRF_CFG_EN_CRC |
              NRF_CFG_CRCO | NRF_CFG_PRIM_RX);
    nrf_wreg8(NRF_REG_EN_AA, 0x02U);
    nrf_wreg8(NRF_REG_EN_RXADDR, 0x02U);
    nrf_wreg8(NRF_REG_SETUP_AW, 0x03U);
    nrf_wreg8(NRF_REG_SETUP_RETR, 0x00U);
    nrf_wreg8(NRF_REG_RF_CH, (uint8_t)CONFIG_RECEIVER_NRF24_CHANNEL);

#if CONFIG_RECEIVER_NRF24_DR_2M
    nrf_wreg8(NRF_REG_RF_SETUP, NRF_RF_DR_2M | NRF_RF_PWR_0 | NRF_RF_LNA_HCURR);
#else
    nrf_wreg8(NRF_REG_RF_SETUP, NRF_RF_DR_1M | NRF_RF_PWR_0 | NRF_RF_LNA_HCURR);
#endif

#if CONFIG_RECEIVER_NRF24_ENABLE_DPL
    nrf_wreg8(NRF_REG_FEATURE, NRF_FEATURE_EN_DPL);
    nrf_wreg8(NRF_REG_DYNPD, NRF_DYNPD_P1);
#else
    nrf_wreg8(NRF_REG_FEATURE, 0x00U);
    nrf_wreg8(NRF_REG_DYNPD, 0x00U);
    nrf_wreg8(NRF_REG_RX_PW_P1, NRF_PAYLOAD_WIDTH);
#endif

    const uint8_t address[5] = {
        CONFIG_RECEIVER_NRF24_RX_ADDR_B0,
        CONFIG_RECEIVER_NRF24_RX_ADDR_B1,
        CONFIG_RECEIVER_NRF24_RX_ADDR_B2,
        CONFIG_RECEIVER_NRF24_RX_ADDR_B3,
        CONFIG_RECEIVER_NRF24_RX_ADDR_B4,
    };
    (void)nrf_wreg(NRF_REG_RX_ADDR_P1, address, sizeof(address));

    (void)nrf_cmd(NRF_CMD_FLUSH_RX);
    (void)nrf_cmd(NRF_CMD_FLUSH_TX);
    nrf_wreg8(NRF_REG_STATUS, NRF_ST_RX_DR | NRF_ST_TX_DS | NRF_ST_MAX_RT);

    uint8_t config = nrf_rreg8(NRF_REG_CONFIG);
    nrf_wreg8(NRF_REG_CONFIG, (uint8_t)(config | NRF_CFG_PWR_UP));
    vTaskDelay(pdMS_TO_TICKS(NRF_TPD2STBY_MS));

    ESP_LOGI(NRF24_TAG, "detected %s",
             handle->chip == NRF_CHIP_PLUS ? "nRF24L01+" : "nRF24L01");
    return ESP_OK;
}

/**
 * @brief Roll back partially initialized nRF24 resources.
 *
 * This helper is used on startup failures after some combination of task, SPI,
 * GPIO, and ISR resources may already have been acquired.
 *
 * @param handle Receiver state structure to reset when non-NULL.
 */
static void nrf_cleanup_partial(nrf24_handle_t *handle)
{
    if (handle != NULL && handle->task != NULL) {
        vTaskDelete(handle->task);
        handle->task = NULL;
    }
    if (s_spi != NULL) {
        spi_bus_remove_device(s_spi);
        s_spi = NULL;
    }
    spi_bus_free(NRF24_SPI_HOST);
    gpio_isr_handler_remove(IRQ_GPIO);
    gpio_reset_pin(IRQ_GPIO);
    gpio_reset_pin(CE_GPIO);
    if (handle != NULL) {
        handle->spi = NULL;
        handle->rx_active = false;
        handle->rx_suspended = false;
    }
    s_notify_to = NULL;
}

esp_err_t nrf24_recv_start_task(nrf24_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, NRF24_TAG, "handle is NULL");
    if (handle->rx_active) {
        return ESP_OK;
    }

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = CONFIG_RECEIVER_NRF24_MOSI,
        .miso_io_num = CONFIG_RECEIVER_NRF24_MISO,
        .sclk_io_num = CONFIG_RECEIVER_NRF24_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(NRF24_SPI_HOST, &bus_cfg, SPI_DMA_DISABLED),
                        NRF24_TAG, "spi_bus_initialize failed");

    spi_device_interface_config_t dev_cfg = {
        .mode = 0,
        .clock_speed_hz = 4 * 1000 * 1000,
        .spics_io_num = CONFIG_RECEIVER_NRF24_CS,
        .queue_size = 1,
        .flags = 0,
    };
    esp_err_t err = spi_bus_add_device(NRF24_SPI_HOST, &dev_cfg, &s_spi);
    if (err != ESP_OK) {
        spi_bus_free(NRF24_SPI_HOST);
        return err;
    }
    handle->spi = s_spi;

    err = gpio_config(&(gpio_config_t){
        .pin_bit_mask = 1ULL << IRQ_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    });
    if (err != ESP_OK) {
        nrf_cleanup_partial(handle);
        return err;
    }

    err = nrf_bringup(handle);
    if (err != ESP_OK) {
        nrf_cleanup_partial(handle);
        return err;
    }

    if (xTaskCreate(nrf_rx_task, "nrf_rx", 4096, handle, 5, &handle->task) != pdPASS) {
        nrf_cleanup_partial(handle);
        return ESP_ERR_NO_MEM;
    }
    s_notify_to = handle->task;

    err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        nrf_cleanup_partial(handle);
        return err;
    }
    err = gpio_isr_handler_add(IRQ_GPIO, nrf_isr, NULL);
    if (err != ESP_OK) {
        nrf_cleanup_partial(handle);
        return err;
    }

    gpio_set_level(CE_GPIO, 1);
    esp_rom_delay_us(NRF_TSTBY2A_US);
    xTaskNotifyGive(handle->task);

    handle->rx_active = true;
    handle->rx_suspended = false;
    return ESP_OK;
}

esp_err_t nrf24_recv_suspend(nrf24_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, NRF24_TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE(handle->rx_active && !handle->rx_suspended, ESP_ERR_INVALID_STATE,
                        NRF24_TAG, "receiver not active");

    gpio_set_level(CE_GPIO, 0);
    ESP_RETURN_ON_ERROR(gpio_intr_disable(IRQ_GPIO), NRF24_TAG, "failed to disable IRQ");
    vTaskSuspend(handle->task);
    handle->rx_suspended = true;
    return ESP_OK;
}

esp_err_t nrf24_recv_resume(nrf24_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, NRF24_TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE(handle->rx_active && handle->rx_suspended, ESP_ERR_INVALID_STATE,
                        NRF24_TAG, "receiver not suspended");

    nrf_wreg8(NRF_REG_STATUS, NRF_ST_RX_DR | NRF_ST_TX_DS | NRF_ST_MAX_RT);
    (void)nrf_cmd(NRF_CMD_FLUSH_RX);
    ESP_RETURN_ON_ERROR(gpio_intr_enable(IRQ_GPIO), NRF24_TAG, "failed to enable IRQ");
    vTaskResume(handle->task);
    gpio_set_level(CE_GPIO, 1);
    esp_rom_delay_us(NRF_TSTBY2A_US);
    handle->rx_suspended = false;
    return ESP_OK;
}

esp_err_t nrf24_recv_deinit(nrf24_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, NRF24_TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE(handle->rx_active, ESP_ERR_INVALID_STATE, NRF24_TAG, "receiver not active");

    gpio_isr_handler_remove(IRQ_GPIO);
    gpio_set_level(CE_GPIO, 0);
    nrf_wreg8(NRF_REG_CONFIG, NRF_CFG_MASK_TX_DS | NRF_CFG_MASK_MAX_RT | NRF_CFG_EN_CRC |
                              NRF_CFG_CRCO | NRF_CFG_PRIM_RX);
    if (handle->task != NULL) {
        vTaskDelete(handle->task);
        handle->task = NULL;
    }
    if (s_spi != NULL) {
        spi_bus_remove_device(s_spi);
        s_spi = NULL;
    }
    spi_bus_free(NRF24_SPI_HOST);
    gpio_reset_pin(IRQ_GPIO);
    gpio_reset_pin(CE_GPIO);

    handle->spi = NULL;
    handle->rx_active = false;
    handle->rx_suspended = false;
    s_notify_to = NULL;
    return ESP_OK;
}

#else

esp_err_t nrf24_recv_start_task(nrf24_handle_t *handle)
{
    (void)handle;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nrf24_recv_suspend(nrf24_handle_t *handle)
{
    (void)handle;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nrf24_recv_resume(nrf24_handle_t *handle)
{
    (void)handle;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nrf24_recv_deinit(nrf24_handle_t *handle)
{
    (void)handle;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
