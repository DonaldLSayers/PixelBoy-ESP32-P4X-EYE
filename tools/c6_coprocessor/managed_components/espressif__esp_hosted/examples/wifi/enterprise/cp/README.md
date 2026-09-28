# Wi-Fi enterprise — Coprocessor

Wi-Fi station bring-up against an **enterprise (802.1X / EAP)** AP — the kind of network found in offices and universities, where each client authenticates with certificates or per-user credentials instead of a shared PSK. The radio runs on the **coprocessor** and the application on the **host**. The CP exposes the `eh_host_feat_wifi_ext_ent` extension; the host pulls in `wpa_supplicant`'s enterprise (EAP) machinery and mbedTLS. EAP configuration (method, identity, certificates, anonymous identity, etc.) is pushed from the host to the CP over RPC.

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

The enterprise feature gate (`CONFIG_ESP_HOSTED_CP_FEAT_WIFI_EXT_ENT`) is preset in `cp/sdkconfig.defaults`; still enable the upstream EAP support (`ESP_WIFI_ENTERPRISE_SUPPORT` under Component config → Wi-Fi → Enterprise) and select the transport:

```bash
cd examples/wifi/enterprise/cp
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
CONFIG_ESP_HOSTED_CP_FEAT_WIFI_EXT_ENT=y   # Enterprise (802.1X / EAP) feature extension
# NOTE: CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT is =n in defaults — enable it manually
#       (Component config → Wi-Fi → Enterprise) or EXT_ENT will not build
```

Then flash and monitor:

```bash
idf.py -p <cp_usb_serial_port> flash monitor
```

<!-- generated from ../README.md — edit that file, not this one -->
