# zigbee / thermostat — Host

Runs a Zigbee **Home Automation Thermostat** (a Zigbee Coordinator) on the host. The co-processor is the 802.15.4 **Radio Co-Processor (RCP)** — the *same* stack-agnostic RCP used by OpenThread: the host drives the RCP lifecycle over ESP-Hosted (RPC) and exchanges 802.15.4 (spinel) traffic over a **dedicated UART**. Zigbee vs OpenThread is purely a host-side choice (the host links `esp-zigbee-lib` instead of `openthread`); the co-processor firmware is identical. Adapted from the [ESP Zigbee SDK Thermostat example](https://github.com/espressif/esp-zigbee-sdk/tree/main/examples/home_automation_devices/thermostat). See the [OpenThread and Zigbee Support](https://github.com/espressif/esp-hosted-mcu/blob/main/docs/openthread_zigbee.md) doc for the RCP architecture.

The co-processor plays two roles: the **ESP-Hosted CP** (control, over the
bus) and the **802.15.4 RCP** (radio, over the dedicated UART). Both are the
same ESP32-C6 (Wi-Fi off) — two roles, not two chips, and not wired to each
other. A working deployment is 3 SoCs: host, that co-processor, and a
separate Zigbee end-device.

```text
  Host (ESP32-P4) - Zigbee Coordinator
    |
    |  ESP-Hosted bus (SDIO / SPI / UART)
    +--- RPC: RCP control -----------> [ ESP-Hosted CP ]      (control stops here)
    |
    |  dedicated UART
    +--- 802.15.4 spinel (data) -----> [ 802.15.4 RCP ] --- 802.15.4 RF ---> Zigbee end-device
```

Data path: host -> spinel UART -> RCP radio -> RF -> end-device (a sensor).

## Supported Platforms and Transports

### Supported Coprocessors

Zigbee uses an 802.15.4 radio co-processor (RCP) — the same RCP firmware as OpenThread.

| Coprocessor | ESP32-C6 | ESP32-H2 | ESP32-H4 | ESP32-C5 |
| :---------: | :------: | :------: | :------: | :------: |
| Support     | Yes      | Yes      | Yes      | Yes      |

### Supported Host Devices

The Zigbee stack (`esp-zigbee-lib`) is ESP-IDF-only, so the host is an ESP-IDF device (role `esp_host`). PSRAM is recommended to be used.

| Host Device | ESP32-P4 | Other MCUs | Linux |
| :---------: | :------: | :--------: | :---: |
| Support     | Yes      | No (esp-zigbee-lib is ESP-IDF-only) | No |

### Supported Connection buses

The ESP-Hosted bus carries the RCP **control** plane (RPC); the 802.15.4 spinel **data** plane always rides a separate dedicated UART.

| Connection bus | SDIO | SPI Full-Duplex | SPI Half-Duplex | UART |
| :------------- | :--: | :-------------: | :-------------: | :--: |
| MCU host       | Yes  | Yes             | Yes             | Yes  |

Select the transport (must match the co-processor) and configure the RCP UART link:

```bash
cd examples/zigbee/thermostat/esp_host
idf.py set-target esp32p4
idf.py menuconfig
```

```text
Component config
└── ESP-Hosted
     └── Configure host
          ├── Host transport
          │    └── Communication bus (match the co-processor)   ← SDIO default
          └── Features
               └── [*] OpenThread / Zigbee (RCP control)
                    ├── [*] Auto-initialize feature at boot
                    └── OpenThread RCP transport (host side)
                         ├── (X) Dedicated UART                  <── default (spinel to RCP)
                         ├── (11)     Host TX pin (to RCP RX)
                         ├── (10)     Host RX pin (from RCP TX)
                         └── (460800) Baud rate
```

The host dependency config is **pre-set in `esp_host/sdkconfig.defaults`** — do not remove:

```text
CONFIG_ZB_ENABLED=y                          # Zigbee stack (esp-zigbee-lib)
CONFIG_ESP_HOSTED_HOST_FEAT_OPENTHREAD=y     # host RCP-control feature (serves Zigbee too)
CONFIG_ESP_HOSTED_HOST_FEAT_RPC_EXT_V2=y     # required RPC ext-v2 (FEAT_OPENTHREAD depends on it)
CONFIG_SPIRAM=y                              # PSRAM integration
CONFIG_ESP_HOSTED_DFLT_TASK_FROM_SPIRAM=y
CONFIG_OPENTHREAD_RX_ON_WHEN_IDLE=n          # keep in sync with RCP coexistence
```

Erase NVRAM before the first flash if you don't want stale network state
(`idf.py -p <host_usb_serial_port> erase-flash`), then flash and monitor:

```bash
idf.py -p <host_usb_serial_port> flash monitor
```

<!-- generated from ../README.md — edit that file, not this one -->
