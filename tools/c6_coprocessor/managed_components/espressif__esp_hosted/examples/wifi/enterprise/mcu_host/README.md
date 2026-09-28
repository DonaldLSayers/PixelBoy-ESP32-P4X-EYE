# Wi-Fi enterprise — Host

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

Select the transport (must match the co-processor):

```bash
cd examples/wifi/enterprise/mcu_host
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

This example ships no `Example Configuration` menu — the EAP knobs live in the application sources / embedded PEM files rather than Kconfig. Edit `mcu_host/main/main.c` (and any embedded certificate / key files referenced from `idf_component.yml` / `CMakeLists.txt`) to set:

- EAP method (PEAP / TTLS / TLS / FAST)
- SSID, identity, anonymous identity, username, password (where applicable)
- CA certificate (server validation)
- Client certificate + private key (EAP-TLS / where mutual auth is required)

For EAP-PEAP / EAP-TTLS the typical minimum is identity + password + CA cert. For EAP-TLS replace the password with client cert + key.

Host dependency config is **pre-set in `mcu_host/sdkconfig.defaults`** (do not remove):

```text
CONFIG_ESP_HOSTED_HOST_FEAT_RPC=y            # RPC control path to the CP
CONFIG_ESP_HOSTED_HOST_FEAT_RPC_EXT_V2=y     # RPC ext-v2 wire (required)
CONFIG_ESP_HOSTED_HOST_FEAT_SYSTEM=y         # system RPCs
CONFIG_ESP_HOSTED_HOST_FEAT_WIFI=y           # host Wi-Fi feature
CONFIG_ESP_HOSTED_HOST_FEAT_WIFI_EXT_ENT=y   # host Enterprise (EAP) feature extension
CONFIG_BT_ENABLED=n                          # BT off
```

Then flash and monitor:

```bash
idf.py -p <host_usb_serial_port> flash monitor
```

<!-- generated from ../README.md — edit that file, not this one -->
