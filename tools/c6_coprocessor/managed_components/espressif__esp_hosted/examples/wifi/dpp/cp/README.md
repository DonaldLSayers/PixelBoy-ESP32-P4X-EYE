# Wi-Fi DPP enrollee — Coprocessor

Wi-Fi Easy-Connect (Device Provisioning Protocol) enrollee — the host MCU prints a QR code, a configurator device (typically a phone) scans it and pushes Wi-Fi credentials, and the station joins the AP without ever having the PSK pre-shared. Ports the upstream IDF `wifi/wifi_easy_connect/dpp-enrollee` example; the radio runs on the **coprocessor**, the app on the **host**. The CP exposes the `eh_host_feat_wifi_ext_dpp` extension; the host pulls in `wpa_supplicant` DPP + mbedTLS.

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

The DPP feature gate (`CONFIG_ESP_HOSTED_CP_FEAT_WIFI_EXT_DPP`) is preset in `cp/sdkconfig.defaults`; still enable the upstream supplicant support (`ESP_WIFI_DPP_SUPPORT` under Component config → Wi-Fi → DPP) and select the transport:

```bash
cd examples/wifi/dpp/cp
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

CP dependency config is **pre-set in `cp/sdkconfig.defaults`** (do not remove):

```text
CONFIG_ESP_HOSTED_CP_FEAT_WIFI=y           # Wi-Fi (default CP feature)
CONFIG_ESP_HOSTED_CP_FEAT_WIFI_EXT_DPP=y   # DPP (Easy-Connect) feature extension
# requires CONFIG_ESP_WIFI_DPP_SUPPORT=y (Component config → Wi-Fi → DPP) — enable it manually
```

Then flash and monitor:

```bash
idf.py -p <cp_usb_serial_port> flash monitor
```

<!-- generated from ../README.md — edit that file, not this one -->
