# Wi-Fi iTWT — Host

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

Select the transport (must match the co-processor) and set the Wi-Fi / iTWT options:

```bash
cd examples/wifi/itwt/mcu_host
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
├── (myssid) WiFi SSID                             ← point at a Wi-Fi 6 capable AP
├── (mypassword) WiFi Password
├── [*] enable static ip
│    ├── (192.168.4.2) Static IP address
│    ├── (255.255.255.0) Static netmask address
│    └── (192.168.4.1) Static gateway address
├── [ ] enable keep alive qos null                 ← QoS-Null during TWT keeps the association alive
├── iTWT Configuration
│    ├── [*] trigger-enabled
│    ├── [*] announced
│    ├── (255) itwt minimum wake duration          range 1..255, unit 256 us
│    ├── (0) itwt wake duration unit               0 = 256 us, 1 = TU (1024 us)
│    ├── (10) itwt wake interval exponent          range 0..31
│    ├── (512) itwt wake interval mantissa         range 1..65535 (interval = mant × 2^expn us)
│    ├── (0) itwt connection id                    range 0..32767
│    └── (5000) itwt setup timeout times           range 100..65535 ms
├── Maximum CPU frequency                          ← choice, depends on PM_ENABLE
└── Minimum CPU frequency                          ← choice, depends on PM_ENABLE
```

Host dependency config is **pre-set in `mcu_host/sdkconfig.defaults`** (do not remove):

```text
CONFIG_ESP_HOSTED_HOST_FEAT_RPC=y            # RPC control path to the CP
CONFIG_ESP_HOSTED_HOST_FEAT_RPC_EXT_V2=y     # RPC ext-v2 wire (required)
CONFIG_ESP_HOSTED_HOST_FEAT_SYSTEM=y         # system RPCs
CONFIG_ESP_HOSTED_HOST_FEAT_WIFI=y           # host Wi-Fi feature
CONFIG_ESP_HOSTED_HOST_FEAT_WIFI_EXT_ITWT=y  # host iTWT feature extension
CONFIG_BT_ENABLED=n                          # BT off
CONFIG_PM_ENABLE=y                           # power management — required to realise TWT savings
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y          # tickless idle so the host sleeps during TWT
```

Then flash and monitor:

```bash
idf.py -p <host_usb_serial_port> flash monitor
```

<!-- generated from ../README.md — edit that file, not this one -->
