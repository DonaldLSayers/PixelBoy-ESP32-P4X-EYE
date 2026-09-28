# Shut down the co-processor when unused — Coprocessor

The host holds the co-processor (CP) in reset (EN low) whenever it doesn't need
networking, cutting the CP rail to ~0 mA. On wake-up the host re-runs
`eh_host_init()` + `eh_host_connect_to_slave()`, re-binds Wi-Fi events,
reconnects to the AP, and resumes. It exercises a full CP cold-boot every cycle,
with heap-tracing wired in so you can confirm there is no leak across cycles.

## Supported Platforms and Transports

| Host device | ESP32-P4 | ESP32-H2 | Other MCUs |
| :---------- | :------: | :------: | :--------: |
| Support     | Yes      | Yes      | [Yes](https://github.com/espressif/esp-hosted/blob/master/docs/getting-started-mcu.md) |

| Coprocessor | Any Espressif chip with Wi-Fi (default ESP32-C6) |
| :---------- | :----------------------------------------------- |
| Support     | Yes                                              |

| Communication bus | SDIO | SPI Full-Duplex | SPI Half-Duplex | UART |
| :---------------- | :--: | :-------------: | :-------------: | :--: |
| MCU host          | Yes  | Yes             | Yes             | Yes  |

No extra co-processor option is needed — the host power-cycles the CP by holding
its EN pin. Select the transport (must match the host):

```bash
cd examples/power_save/cp/shut_down_cp_when_unused/cp
idf.py set-target esp32c6
idf.py menuconfig
```

```text
Component config
└── ESP-Hosted
     └── Configure coprocessor
          └── CP transport
               ├── Communication bus (co-processor <== bus ==> host)
               │    ├── ( ) SPI Full Duplex
               │    ├── (X) SDIO                    <── default
               │    ├── ( ) SPI Half Duplex         ← MCU host only
               │    └── ( ) UART                    ← MCU host only
               └── SDIO Configuration               ← clock, GPIOs, checksum (defaults OK)
```

The CP dependency config is pre-set in `sdkconfig.defaults` (do not remove):

```text
CONFIG_ESP_HOSTED_CP_FEAT_WIFI=y                        # Wi-Fi stack on the CP
CONFIG_ESP_HOSTED_CP_FEAT_CLI=y                         # CP CLI (wake-up-host command)
CONFIG_ESP_HOSTED_CP_FEAT_HOST_PS=y                     # Host Power Save feature
CONFIG_ESP_HOSTED_CP_FEAT_HOST_PS_DEEP_SLEEP=y          # host deep-sleep support
CONFIG_ESP_HOSTED_CP_FEAT_HOST_PS_HOST_WAKEUP_GPIO=2    # slave-out host wakeup GPIO (ESP32-C6)
CONFIG_ESP_HOSTED_CP_FEAT_HOST_PS_HOST_WAKEUP_GPIO_LEVEL_HIGH=y  # drive the wake line high
```

The wake-up GPIO is pinned to GPIO 2 (*slave out*, active-high); if the host
uses a GPIO wake it must be wired to the host-side *Host in: Host Wakeup GPIO* at
a matching level. This example's primary CP power-down path is the host holding
the CP EN pin low.

Then flash and monitor:

```bash
idf.py -p <cp_usb_serial_port> flash monitor
```

<!-- generated from ../README.md — edit that file, not this one -->
