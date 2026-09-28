# mcu_transport_config — Host

Configures the host-side transport (SDIO / SPI-FD / SPI-HD-dual /
SPI-HD-quad / UART) programmatically at runtime, *before*
`esp_hosted_init()`. The same Kconfig defaults are available via
`menuconfig`, but this example shows how to override them in code —
useful when the bus pins or clock have to come from the application
(board ID, NVS, dip switch) rather than the build configuration.

(MCU-only — Linux host transport is configured via the bus driver,
not this API.)

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

There is no example-specific Kconfig — the host transport is exactly what this example overrides *in code* at runtime. The `menuconfig` transport choice sets the `INIT_DEFAULT_*` pin / clock defaults; the example then overwrites whichever fields it needs (SDIO 1- or 4-bit, SPI-FD, SPI-HD dual / quad, UART):

```bash
cd examples/system/mcu_transport_config/mcu_host
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

Host dependency config is **pre-set in `sdkconfig.defaults`** (do not remove):

```text
CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN=n  # defer init so app_main can set transport config first
CONFIG_ESP_WIFI_ENABLED=y                           # Wi-Fi (default feature set)
CONFIG_BT_ENABLED=y                                 # BT enabled (optional; Bluedroid host)
```

Then flash and monitor:

```bash
idf.py -p <host_usb_serial_port> flash monitor
```

<!-- generated from ../README.md — edit that file, not this one -->
