# mcu_hosted_sdio_sdmmc_combined — Host

Demonstrates running ESP-Hosted's SDIO transport on the same SDMMC
controller that also hosts an external SD card — the two share the
bus driver but live on different slots. The host brings up Wi-Fi
through ESP-Hosted on SDMMC slot 1 (on-board ESP32-C6), mounts a
FAT-formatted SD card on SDMMC slot 0, scans Wi-Fi before *and* after
filesystem I/O to confirm the radio survives the card init, then
writes / renames / reads a file via the standard FATFS / VFS
interface.

## Supported Platforms and Transports

### Supported Coprocessors

| Coprocessor | ESP32 | ESP32-C Series | ESP32-S Series |
| :----------: | :---: | :------------: | :------------: |
| Support     | Yes   | Yes            | Yes            |

### Supported Host Devices

| Host Device | ESP32-P4 | ESP32-H2 | Other MCUs | Linux |
| :---------: | :------: | :------: | :--------: | :---: |
| Support     | Yes | Yes | [Yes](https://github.com/espressif/esp-hosted/blob/master/docs/getting-started-mcu.md) | [Yes](https://github.com/espressif/esp-hosted/blob/master/docs/getting-started-linux.md) |

### Supported Connection buses

| Connection bus | SDIO | SPI Full-Duplex | SPI Half-Duplex | UART |
| :------------- | :--: | :-------------: | :-------------: | :--: |
| Linux host     | Yes  | Yes             | No              | No   |
| MCU host       | Yes  | Yes             | Yes             | Yes  |

Select the transport (SDIO, matching the co-processor) and set the SD-card slot / pins:

```bash
cd examples/mcu_hosted_sdio_sdmmc_combined/mcu_host
idf.py set-target esp32p4
idf.py menuconfig
```

```text
Component config
└── ESP-Hosted
     └── Configure host
          └── Host transport
               ├── Communication bus (co-processor <== bus ==> host)
               │    ├── ( ) SPI Full Duplex
               │    ├── (X) SDIO                    <── default (match the co-processor)
               │    ├── ( ) SPI Half Duplex
               │    └── ( ) UART
               └── SDIO Configuration               ← slot, bus width, GPIOs (defaults OK)
```

```text
SD/MMC Example Configuration
├── (0)  SD/MMC slot to use                     ← P4: must be 0 (ESP-Hosted uses slot 1)
├── SD/MMC bus width
│     ├── (X) 4 lines (D0 - D3)                 <── default
│     └── ( ) 1 line (D0)
├── (45)  Card Power Reset                       ← only on chips with external SDMMC IO power
├── CMD GPIO number                             ⎫
├── CLK GPIO number                             ⎬ prompted on S3 (defaults 35/36/37/38/33/34);
├── D0 GPIO number                              │ fixed on P4 (CMD 44, CLK 43, D0-D3 39-42)
├── D1 / D2 / D3 GPIO number                    ⎭ (D1-D3 shown only in 4-line mode)
├── [*] SD power supply comes from internal LDO IO (READ HELP!)  <── default on (P4)
└── (4)  LDO ID                                 ← P4 internal LDO for SD VDD
```

Host dependency config is **pre-set in `sdkconfig.defaults`** (do not remove):

```text
CONFIG_FATFS_VFS_FSTAT_BLKSIZE=4096   # FATFS tuning for the shared SD card
CONFIG_WIFI_RMT_NVS_ENABLED=n         # (ESP32-P4) from sdkconfig.defaults.esp32p4 — disable Wi-Fi RMT NVS
```

Then flash and monitor:

```bash
idf.py -p <host_usb_serial_port> flash monitor
```

<!-- generated from ../README.md — edit that file, not this one -->
