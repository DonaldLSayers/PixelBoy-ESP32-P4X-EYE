# hosted_events — Coprocessor

Wires the host's ESP-IDF event loop to ESP-Hosted's lifecycle events
so the user application can react to coprocessor state changes
without polling. Three events on the `ESP_HOSTED_EVENT` base:

- `ESP_HOSTED_EVENT_CP_INIT` — CP came up (or, on a second occurrence,
  rebooted). Carries an `esp_reset_reason_t`.

- `ESP_HOSTED_EVENT_CP_HEARTBEAT` — periodic liveness ping at the
  interval the host asked for; absence implies a CP hang.

- `ESP_HOSTED_EVENT_TRANSPORT_FAILURE` — transport layer gave up on
  the bus.

The example associates to an AP, then waits. If the CP reboots or
the heartbeat times out it tears down the netif, re-inits the
transport, and reconnects — or restarts the host, depending on the
configured recovery method.

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

The co-processor firmware needs no example-specific options — just select the transport:

```bash
cd examples/system/hosted_events/cp
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

Each bus has its own settings submenu (pins, clock, checksum) — defaults are fine for bring-up.

CP dependency config is **pre-set in `sdkconfig.defaults`** (do not remove):

```text
CONFIG_ESP_HOSTED_CP_FEAT_WIFI=y   # Wi-Fi feature on the coprocessor (default)
CONFIG_BT_ENABLED=n                # BT controller off — not needed here
```

Then flash and monitor:

```bash
idf.py -p <cp_usb_serial_port> flash monitor
```

<!-- generated from ../README.md — edit that file, not this one -->
