# Wi-Fi iTWT — Coprocessor

802.11ax **individual Target Wake Time** — the station negotiates a periodic wake schedule with the AP and sleeps the radio between wake-ups, slashing average power for always-connected devices. Ports the upstream IDF `wifi/itwt` example; the radio runs on the **coprocessor**, the app on the **host**. The CP exposes the `eh_host_feat_wifi_ext_itwt` extension. On boot the host opens a serial console (`itwt>`) so TWT setup / teardown / suspend can be driven interactively.

## Supported Platforms and Transports

### Supported Coprocessors

iTWT is a Wi-Fi 6 (802.11ax) feature — it requires a Wi-Fi 6 coprocessor.

| Coprocessor | ESP32-C5 | ESP32-C6 | ESP32-C61 |
| :---------: | :------: | :------: | :-------: |
| Support     | Yes      | Yes      | Yes       |

### Supported Host Devices

| Host Device | ESP32-P4 | ESP32-H2 | Other MCUs | Linux |
| :---------: | :------: | :------: | :--------: | :---: |
| Support     | Yes | Yes | [Yes](https://github.com/espressif/esp-hosted/blob/master/docs/getting-started-mcu.md) | [Yes](https://github.com/espressif/esp-hosted/blob/master/docs/getting-started-linux.md) |

### Supported Connection buses

| Connection bus | SDIO | SPI Full-Duplex | SPI Half-Duplex | UART |
| :------------- | :--: | :-------------: | :-------------: | :--: |
| Linux host     | Yes  | Yes             | No              | No   |
| MCU host       | Yes  | Yes             | Yes             | Yes  |

The iTWT feature gate (`CONFIG_ESP_HOSTED_CP_FEAT_WIFI_EXT_ITWT`) is preset in `cp/sdkconfig.defaults` and needs a Wi-Fi 6 (HE) capable chip. Select the transport:

```bash
cd examples/wifi/itwt/cp
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
CONFIG_ESP_HOSTED_CP_FEAT_WIFI=y            # Wi-Fi (default CP feature)
CONFIG_ESP_HOSTED_CP_FEAT_WIFI_EXT_ITWT=y   # iTWT (802.11ax TWT) feature extension
# requires an SoC with Wi-Fi 6 / HE support (CONFIG_SOC_WIFI_HE_SUPPORT) — e.g. ESP32-C5 / C6 / C61
```

Then flash and monitor:

```bash
idf.py -p <cp_usb_serial_port> flash monitor
```

<!-- generated from ../README.md — edit that file, not this one -->
