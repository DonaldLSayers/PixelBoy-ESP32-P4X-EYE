# zigbee / thermostat — Coprocessor

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

The co-processor runs as the 802.15.4 **RCP** (Wi-Fi off) — identical to `openthread/cli/cp`. Select the ESP-Hosted transport (used for RPC control):

```bash
cd examples/zigbee/thermostat/cp
idf.py set-target esp32c6
idf.py menuconfig
```

```text
Component config
└── ESP-Hosted
     └── Configure coprocessor
          └── CP transport
               └── Communication bus (co-processor <== bus ==> host)
                    ├── ( ) SPI Full Duplex
                    ├── (X) SDIO                    <── default
                    ├── ( ) SPI Half Duplex         ← MCU host only
                    └── ( ) UART                    ← MCU host only
```

The RCP feature and its 802.15.4 spinel UART (to the host) live under Features → *OpenThread RCP (Radio Co-Processor)* (the RCP is stack-agnostic; the "OpenThread" name is IDF's `esp_openthread` RCP component, which also serves Zigbee hosts).

The RCP dependency config is **pre-set in `cp/sdkconfig.defaults`** (do not remove):

```text
CONFIG_ESP_HOSTED_CP_FEAT_OPENTHREAD=y   # ESP-Hosted 802.15.4 RCP feature
CONFIG_OPENTHREAD_ENABLED=y              # RCP is built via IDF's openthread component
CONFIG_OPENTHREAD_RADIO=y                # Radio-Only (RCP) device
CONFIG_ESP_HOSTED_CP_FEAT_WIFI=n         # Wi-Fi off on the RCP
CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y      # SW coexistence ON — matches host RX_ON_WHEN_IDLE=n
```

> [!IMPORTANT]
> `CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y` is the canonical RCP setting
> (doc §2.1). It changes the 802.15.4 capability set the RCP advertises,
> so the host **must** pair it with `CONFIG_OPENTHREAD_RX_ON_WHEN_IDLE=n`
> — a mismatch makes RCP/stack init fail.

Then flash and monitor:

```bash
idf.py -p <cp_usb_serial_port> flash monitor
```

<!-- generated from ../README.md — edit that file, not this one -->
