# get_cp_fw_version — Coprocessor

Smallest end-to-end ESP-Hosted scenario: bring up the host stack, open
a transport to the coprocessor, fetch the CP firmware version via
`eh_host_sys_get_cp_fw_version` (RPC `system` feature), print
`major.minor.patch` + revision / prerelease / build, exit. Used as
the bring-up smoke test for new ports — exercises every layer that
moves a single RPC round-trip, with no Wi-Fi / BT / OTA moving parts.

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

The simplest control-path round trip — a single RPC request and its response. It is the transport-health gate: if this works, the link is up.

```mermaid
sequenceDiagram
    participant App as Host app
    participant CP as Co-processor
    App->>CP: esp_hosted_get_coprocessor_fwversion() (RPC request)
    CP-->>App: version (major.minor.patch)
```

The co-processor firmware needs no example-specific options — just select the transport:

```bash
cd examples/system/get_cp_fw_version/cp
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
