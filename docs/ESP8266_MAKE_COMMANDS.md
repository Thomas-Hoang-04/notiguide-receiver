# ESP8266 RTOS SDK - Make Commands Reference

> **SDK**: ESP8266_RTOS_SDK v3.x (`/opt/esp/ESP8266_RTOS_SDK/`)
> **Build System**: GNU Make (project.mk + component Makefile.projbuild files)
> **Generated**: 2026-05-21

---

## Build

| Command | Description |
|---------|-------------|
| `make all` | Build app, bootloader, and partition table |
| `make app` | Build just the app binary |
| `make bootloader` | Build just the bootloader |
| `make partition_table` | Build just the partition table |

## Flash

| Command | Description |
|---------|-------------|
| `make flash` | Flash app, bootloader, and partition table to chip |
| `make app-flash` | Flash just the app |
| `make bootloader-flash` | Flash just the bootloader |
| `make partition_table-flash` | Flash just the partition table |
| `make erase_flash` | Erase entire flash contents |

## Monitor

| Command | Description |
|---------|-------------|
| `make monitor` | Run idf_monitor tool (serial decode, crash backtrace) |
| `make simple_monitor` | Basic serial output on terminal console |

## Clean

| Command | Description |
|---------|-------------|
| `make clean` | Remove all build output (app + bootloader + config) |
| `make app-clean` | Clean just the app |
| `make bootloader-clean` | Clean just the bootloader |
| `make partition_table-clean` | Clean just the partition table |
| `make config-clean` | Clean configuration (sdkconfig-derived build files) |

## Configuration

| Command | Description |
|---------|-------------|
| `make menuconfig` | Interactive configuration menu (Kconfig TUI) |
| `make defconfig` | Set defaults for all new configuration options |

## Size Analysis

| Command | Description |
|---------|-------------|
| `make size` | Display static memory footprint of the app |
| `make size-components` | Memory footprint broken down by component (archive) |
| `make size-files` | Memory footprint broken down by object file |
| `make size-symbols COMPONENT=<name>` | Per-symbol footprint for a specific component |

**Example:**
```bash
make size-symbols COMPONENT=main
```

## OTA / Partition Data

| Command | Description |
|---------|-------------|
| `make erase_otadata` | Erase ota_data partition; first bootable partition (factory or OTAx) will be used on next boot |
| `make read_otadata` | Read current OTA partition data |
| `make blank_ota_data` | Generate blank OTA data file |

> `erase_otadata` assumes this project's partition table is the one flashed on the device.

## Diagnostics & Misc

| Command | Description |
|---------|-------------|
| `make list-components` | List all components and their paths |
| `make print_flash_cmd` | Print the esptool flash arguments (for scripting / external tools) |
| `make check_python_dependencies` | Verify required Python packages are installed |
| `make help` | Show built-in help text |

## Compound Commands

Commands can be chained to build and flash in one step:

```bash
make flash monitor          # Build, flash, then open serial monitor
make app-flash monitor      # Flash app only, then monitor
make erase_flash flash      # Full erase, then flash everything
```

## Per-Component Targets

The build system generates per-component build/clean targets automatically:

```bash
make component-<name>-build     # Build a single component
make component-<name>-clean     # Clean a single component
```

**Example:**
```bash
make component-main-build
make component-mqtt-clean
```

## Environment Variables

| Variable | Purpose |
|----------|---------|
| `IDF_PATH` | Path to ESP8266_RTOS_SDK (required) |
| `ESPPORT` | Serial port for flashing (e.g. `/dev/ttyUSB0`) |
| `ESPBAUD` | Baud rate for flashing (default: 115200) |
| `V=1` | Verbose build output |
| `COMPONENT` | Target component for `make size-symbols` |

**Example:**
```bash
make flash ESPPORT=/dev/ttyUSB0 ESPBAUD=921600
make all V=1
```
