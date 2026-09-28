# gpio_expander — Coprocessor

Drives CP-side GPIOs from the host as if the coprocessor were a remote
GPIO expander. The host calls `esp_hosted_cp_gpio_*` (configure / set
level / get level / reset) over RPC; the CP applies each call to its
own `gpio_*` peripheral. Useful when the host MCU runs out of pins and
the coprocessor has spare ones.

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

## Scenario

The host drives and reads the co-processor's GPIOs remotely over the control path (RPC).

```mermaid
sequenceDiagram
    participant App as Host app
    participant CP as Co-processor GPIO
    App->>CP: esp_hosted_cp_gpio_config(pin) (RPC)
    App->>CP: esp_hosted_cp_gpio_set_level(pin, 1) (RPC)
    CP-->>App: ok
    App->>CP: esp_hosted_cp_gpio_get_level(pin) (RPC)
    CP-->>App: level
```

GPIO-expander support is enabled by the co-processor's `sdkconfig.defaults`
(`ESP_HOSTED_CP_FEAT_GPIO_EXP`) — no extra option to enable. Select the
transport via `idf.py menuconfig`:

```bash
cd examples/gpio_expander/cp
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

The CP-side pin under test (default GPIO 2) is set in code via
`SLAVE_GPIO_PIN`. Each bus has its own settings submenu (pins, clock,
checksum) — defaults are fine for bring-up.

The CP dependency config is **pre-set in `sdkconfig.defaults`** (do not remove):

```text
CONFIG_ESP_HOSTED_CP_FEAT_GPIO_EXP=y           # CP GPIO-expander feature
CONFIG_BT_ENABLED=n                            # BT off (unused by this example)
```

Then flash and monitor:

```bash
idf.py -p <cp_usb_serial_port> flash monitor
```

<!-- generated from ../README.md — edit that file, not this one -->
