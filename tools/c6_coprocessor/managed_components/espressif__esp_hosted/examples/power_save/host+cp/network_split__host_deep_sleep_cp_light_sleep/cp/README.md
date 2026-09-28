# power_save / host+cp / network_split__host_deep_sleep_cp_light_sleep — Coprocessor

Both sides asleep at once: **host in deep sleep**, **CP in light
sleep**. Network split keeps Wi-Fi alive on the CP and forwards traffic
into the host netif via the network-split backend; the CP enters light
sleep when the host bus is down, and a wake-up GPIO from CP to host
brings the host back. When the host wakes, callbacks on the CP pin it
back at full clock before the bus comes up.

The host project lives under `esp_host/` — esp_pm, RTC GPIO wake
source, iperf REPL, wifi-cmd glue are ESP-IDF-only.

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

Select the transport, enable CP-side Host Power Save, and pick the CP light-sleep power mode:

```bash
cd examples/power_save/host+cp/network_split__host_deep_sleep_cp_light_sleep/cp
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

```text
Component config
└── ESP-Hosted
     └── Configure coprocessor
          └── Features
               └── [*] Host Power Save
                    └── [*] Allow host to enter deep sleep. Slave will wakeup host using GPIO when needed
                         └── Host deep sleep - wakeup config
                              ├── (2)  Slave out: Host wakeup GPIO     ← ESP32-C6 default
                              └── Host Wakeup GPIO Level
                                   ├── (X) High                        <── default
                                   └── ( ) Low
```

The CP light-sleep behaviour comes from this example's own `Kconfig.projbuild` menu (top level in `menuconfig`):

```text
Example: CP Light Sleep on Host Deep Sleep
└── CP Light Sleep Power Management
     ├── (X) Disabled                                  <── default
     ├── ( ) Minimum Power Saving (development mode)
     ├── ( ) Maximum Power Saving (production mode)
     └── ( ) Manual (I'll configure everything myself)
     └── Light Sleep Parameters                        ← shown once a mode is enabled
          └── (10) Minimum CPU frequency (MHz)          ← ESP32-C6 default
```

CP dependency config is **pre-set in `sdkconfig.defaults`** (do not remove):

```text
CONFIG_ESP_HOSTED_CP_FEAT_WIFI=y  # Wi-Fi stack on the CP, kept alive while the host sleeps
CONFIG_ESP_HOSTED_CP_FEAT_NW_SPLIT=y  # network-split backend — CP owns low-port (61440–65535) traffic
CONFIG_ESP_HOSTED_CP_FEAT_NW_SPLIT_WIFI_AUTO_CONNECT_ON_STA_START=n  # wifi-cmd is the single connect owner
CONFIG_ESP_HOSTED_CP_FEAT_HOST_PS=y  # Host Power Save — CP wakes the host over a GPIO
CONFIG_ESP_HOSTED_CP_FEAT_HOST_PS_DEEP_SLEEP=y  # let the host enter deep sleep
CONFIG_ESP_HOSTED_CP_FEAT_HOST_PS_AUTO_INIT=n  # host drives power-save init (no CP auto-init)
CONFIG_ESP_HOSTED_CP_FEAT_HOST_PS_UNLOAD_BUS_WHILE_SLEEPING=y  # release the bus so the CP can light-sleep
```

The **wake-up GPIO** pair is the key power-save dependency: the *Slave out: Host wakeup GPIO* takes its Kconfig default (GPIO 2 on ESP32-C6, shown above) and must be physically wired to the host-side *Host in: Host Wakeup GPIO*, with both `Host Wakeup GPIO Level` settings matching.

Then flash and monitor:

```bash
idf.py -p <cp_usb_serial_port> flash monitor
```

<!-- generated from ../README.md — edit that file, not this one -->
