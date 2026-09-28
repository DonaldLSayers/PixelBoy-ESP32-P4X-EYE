# ext_coex — Coprocessor

Configures the coprocessor's external-coex (PTA) work mode and grant
pin sequence so the CP's Wi-Fi / BT radio can share the air with a
third-party radio sitting next to it. The host sends a single RPC
sequence at boot: pick a role (Leader / Follower / Standalone), point
the CP at the four PTA GPIOs (request, priority, grant, tx_line), and
optionally tune the advanced knobs (grant delay, validate-high).

## Supported Platforms and Transports

### Supported Coprocessors

| Coprocessor | ESP32 | ESP32-C Series | ESP32-S Series |
| :----------: | :---: | :------------: | :------------: |
| Support     | No    | Yes            | Yes            |

### Supported Host Devices

| Host Device | ESP32-P4 | ESP32-H2 | Other MCUs | Linux |
| :---------: | :------: | :------: | :--------: | :---: |
| Support     | Yes | Yes | [Yes](https://github.com/espressif/esp-hosted/blob/master/docs/getting-started-mcu.md) | [Yes](https://github.com/espressif/esp-hosted/blob/master/docs/getting-started-linux.md) |

### Supported Connection buses

| Connection bus | SDIO | SPI Full-Duplex | SPI Half-Duplex | UART |
| :------------- | :--: | :-------------: | :-------------: | :--: |
| Linux host     | Yes  | Yes             | No              | No   |
| MCU host       | Yes  | Yes             | Yes             | Yes  |

External-Coexistence support is enabled by the co-processor's
`sdkconfig.defaults`. Select the transport via `idf.py menuconfig`:

```bash
cd examples/ext_coex/cp
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

The CP External-Coexistence feature — and the underlying
`ESP_COEX_EXTERNAL_COEXIST_ENABLE` (**Component config → Wireless
Coexistence → External Coexistence**) — are set in `sdkconfig.defaults`:

```text
Component config
└── ESP-Hosted
     └── Configure coprocessor
          └── Features
               └── [*] External Coexistence
                    └── [*] Auto-initialise External Coexistence at boot
```

Each bus has its own settings submenu (pins, clock, checksum) — defaults are fine for bring-up.

The CP dependency config is **pre-set in `sdkconfig.defaults`** (do not remove):

```text
CONFIG_ESP_HOSTED_CP_FEAT_CP_EXT_COEX=y        # CP external-coex (PTA) feature
CONFIG_ESP_COEX_EXTERNAL_COEXIST_ENABLE=y      # REQUIRED — IDF external-coex core
CONFIG_ESP_HOSTED_CP_FEAT_WIFI=y               # Wi-Fi radio sharing the air
CONFIG_BT_ENABLED=n                            # BT must be off for external coex
```

Then flash and monitor:

```bash
idf.py -p <cp_usb_serial_port> flash monitor
```

<!-- generated from ../README.md — edit that file, not this one -->
