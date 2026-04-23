/**
 * @file nrf24_regs.h
 * @brief nRF24L01/L01+ register, bit, and command definitions.
 */

#ifndef NRF24_REGS_H
#define NRF24_REGS_H

#define NRF_REG_CONFIG          0x00
#define NRF_REG_EN_AA           0x01
#define NRF_REG_EN_RXADDR       0x02
#define NRF_REG_SETUP_AW        0x03
#define NRF_REG_SETUP_RETR      0x04
#define NRF_REG_RF_CH           0x05
#define NRF_REG_RF_SETUP        0x06
#define NRF_REG_STATUS          0x07
#define NRF_REG_RX_ADDR_P0      0x0A
#define NRF_REG_RX_ADDR_P1      0x0B
#define NRF_REG_RX_PW_P0        0x11
#define NRF_REG_RX_PW_P1        0x12
#define NRF_REG_FIFO_STATUS     0x17
#define NRF_REG_DYNPD           0x1C
#define NRF_REG_FEATURE         0x1D

#define NRF_CMD_R_REGISTER      0x00
#define NRF_CMD_W_REGISTER      0x20
#define NRF_CMD_R_RX_PAYLOAD    0x61
#define NRF_CMD_R_RX_PL_WID     0x60
#define NRF_CMD_W_TX_PAYLOAD    0xA0
#define NRF_CMD_FLUSH_TX        0xE1
#define NRF_CMD_FLUSH_RX        0xE2
#define NRF_CMD_ACTIVATE        0x50
#define NRF_CMD_NOP             0xFF
#define NRF_ACTIVATE_MAGIC      0x73

#define NRF_CFG_PRIM_RX         (1U << 0)
#define NRF_CFG_PWR_UP          (1U << 1)
#define NRF_CFG_CRCO            (1U << 2)
#define NRF_CFG_EN_CRC          (1U << 3)
#define NRF_CFG_MASK_MAX_RT     (1U << 4)
#define NRF_CFG_MASK_TX_DS      (1U << 5)
#define NRF_CFG_MASK_RX_DR      (1U << 6)

#define NRF_RF_DR_1M            0x00
#define NRF_RF_DR_2M            (1U << 3)
#define NRF_RF_PWR_0            (3U << 1)
#define NRF_RF_LNA_HCURR        (1U << 0)

#define NRF_FEATURE_EN_DPL      (1U << 2)
#define NRF_DYNPD_P1            (1U << 1)

#define NRF_ST_RX_DR            (1U << 6)
#define NRF_ST_TX_DS            (1U << 5)
#define NRF_ST_MAX_RT           (1U << 4)

#define NRF_FIFO_RX_EMPTY       (1U << 0)
#define NRF_PAYLOAD_WIDTH       32U

#define NRF_TPOR_MS             100U
#define NRF_TPD2STBY_MS         10U
#define NRF_TSTBY2A_US          140U

#endif /* NRF24_REGS_H */
