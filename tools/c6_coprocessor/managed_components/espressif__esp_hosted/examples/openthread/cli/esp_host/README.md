# openthread / cli — Host

Runs a full OpenThread CLI (`ot ...`) on the host. The co-processor is the 802.15.4 **Radio Co-Processor (RCP)**: the host drives the RCP lifecycle over ESP-Hosted (RPC) and exchanges 802.15.4 (spinel) traffic over a **dedicated UART**. Adapted from `esp-idf/examples/openthread/ot_cli`.

The co-processor plays two roles: the **ESP-Hosted CP** (control, over the
bus) and the **802.15.4 RCP** (radio, over the dedicated UART). Both are the
same ESP32-C6 (Wi-Fi off) — two roles, not two chips, and not wired to each
other. A working deployment is 3 SoCs: host, that co-processor, and a
separate Thread peer.

```text
  Host (ESP32-P4) - OpenThread stack
    |
    |  ESP-Hosted bus (SDIO / SPI / UART)
    +--- RPC: RCP control -----------> [ ESP-Hosted CP ]      (control stops here)
    |
    |  dedicated UART
    +--- 802.15.4 spinel (data) -----> [ 802.15.4 RCP ] --- 802.15.4 RF ---> Thread peer
```

Data path: host -> spinel UART -> RCP radio -> RF -> peer (a separate SoC).

## Supported Platforms and Transports

### Supported Coprocessors

OpenThread uses an 802.15.4 radio co-processor (RCP).

| Coprocessor | ESP32-C6 | ESP32-H2 | ESP32-H4 | ESP32-C5 |
| :---------: | :------: | :------: | :------: | :------: |
| Support     | Yes      | Yes      | Yes      | Yes      |

### Supported Host Devices

| Host Device | ESP32-P4 | ESP32-H2 | Other MCUs | Linux |
| :---------: | :------: | :------: | :--------: | :---: |
| Support     | Yes | Yes | [Yes](https://github.com/espressif/esp-hosted/blob/master/docs/getting-started-mcu.md) | [Yes](https://github.com/espressif/esp-hosted/blob/master/docs/getting-started-linux.md) |

### Supported Connection buses

| Connection bus | SDIO | SPI Full-Duplex | SPI Half-Duplex | UART |
| :------------- | :--: | :-------------: | :-------------: | :--: |
| Linux host     | Yes  | Yes             | No              | No   |
| MCU host       | Yes  | Yes             | Yes             | Yes  |

Select the transport (must match the co-processor) and configure the OpenThread RCP UART link:

```bash
cd examples/openthread/cli/esp_host
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
Component config
└── ESP-Hosted
     └── Configure host
          └── Features
               └── [*] OpenThread (RCP control)
                    ├── [*] Auto-initialize OpenThread feature at boot
                    └── OpenThread RCP transport (host side)
                         ├── (X) Dedicated UART                       <── default
                         ├── ( ) ESP-Hosted transport (not supported yet)
                         ├── (1)      UART port
                         ├── (11)     Host TX pin (to RCP RX)
                         ├── (10)     Host RX pin (from RCP TX)
                         └── (460800) Baud rate
```

The host dependency config is **pre-set in `esp_host/sdkconfig.defaults`** (`sdkconfig.defaults.esp32p4` for the PSRAM part) — do not remove:

```text
CONFIG_ESP_HOSTED_HOST_FEAT_OPENTHREAD=y     # host OpenThread feature
CONFIG_ESP_HOSTED_HOST_FEAT_RPC_EXT_V2=y     # required RPC ext-v2
CONFIG_OPENTHREAD_ENABLED=y
CONFIG_SPIRAM=y                              # REQUIRED — host won't start without PSRAM
CONFIG_OPENTHREAD_RX_ON_WHEN_IDLE=n          # keep in sync with RCP coexistence
```

Then flash and monitor:

```bash
idf.py -p <host_usb_serial_port> flash monitor
```

<!-- generated from ../README.md — edit that file, not this one -->
