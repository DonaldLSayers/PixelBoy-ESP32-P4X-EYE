# Wi-Fi apsta — Host

Run Wi-Fi in **AP + STA concurrent** mode — connect to an upstream AP as a station while simultaneously serving a SoftAP — radio on the **coprocessor**, application on the **host**. `mcu_host/main/softap_sta.c` is a byte-for-byte copy of upstream IDF `wifi/softap_sta` (`WIFI_MODE_APSTA` + LWIP NAPT so AP-side clients reach the STA uplink).

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

Select the transport (must match the co-processor) and set the SoftAP + STA credentials:

```bash
cd examples/wifi/apsta/mcu_host
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

```text
Example Configuration
├── SoftAP Configuration
│    ├── (myssid) WiFi AP SSID
│    ├── (mypassword) WiFi AP Password
│    ├── (1) WiFi AP Channel
│    └── (4) Maximal STA connections
└── STA Configuration
     ├── (otherapssid) WiFi Remote AP SSID
     ├── (otherappassword) WiFi Remote AP Password
     ├── (5) Maximum retry
     └── WiFi Scan auth mode threshold
          ├── ( ) OPEN
          ├── ( ) WEP
          ├── ( ) WPA PSK
          ├── (X) WPA2 PSK                     <── default
          ├── ( ) WPA/WPA2 PSK
          ├── ( ) WPA3 PSK
          ├── ( ) WPA2/WPA3 PSK
          └── ( ) WAPI PSK
```

Host dependency config is **pre-set in `mcu_host/sdkconfig.defaults`** (do not remove):

```text
CONFIG_ESP_HOSTED_HOST_FEAT_RPC=y          # RPC control path to the CP
CONFIG_ESP_HOSTED_HOST_FEAT_RPC_EXT_V2=y   # RPC ext-v2 wire (required)
CONFIG_ESP_HOSTED_HOST_FEAT_SYSTEM=y       # system RPCs
CONFIG_ESP_HOSTED_HOST_FEAT_WIFI=y         # host Wi-Fi feature — routes esp_wifi_* to the CP
CONFIG_LWIP_IP_FORWARD=y                   # forward packets between AP and STA netifs
CONFIG_LWIP_IPV4_NAPT=y                    # NAPT so SoftAP clients route via the STA uplink
```

Then flash and monitor:

```bash
idf.py -p <host_usb_serial_port> flash monitor
```

<!-- generated from ../README.md — edit that file, not this one -->
