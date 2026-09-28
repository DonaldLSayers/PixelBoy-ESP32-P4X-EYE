# openthread / border_router — Coprocessor

OpenThread Border Router on the host, driving a **single** ESP-Hosted
co-processor that provides **both** the 802.15.4 **Radio Co-Processor
(RCP)** and the Wi-Fi backhaul. The host drives the RCP lifecycle over
ESP-Hosted (RPC) and exchanges 802.15.4 (spinel) over a **dedicated
UART**; Wi-Fi is exposed to the host through the RPC Wi-Fi feature. The
host runs the full OT BR stack (border routing, mDNS, NAT64 / DHCPv6
hooks via the IDF ot_br adaptation). Because OpenThread and Wi-Fi share
one radio on the CP, expect coexistence trade-offs; for a multi-MCU
split (P4 + C6 Wi-Fi + H2 RCP) see the ESP-Thread-BR P4 example linked
below. Adapted from `esp-idf/examples/openthread/ot_br`.

One co-processor plays two roles: a **Wi-Fi CP** (backhaul, over the bus)
and an **802.15.4 RCP** (radio, over the dedicated UART), bridging a Thread
mesh to the Wi-Fi LAN. Both are the same ESP32-C6 (coex) — two roles, not
two chips, and not wired to each other.

```text
  Host (ESP32-P4) - OpenThread Border Router (mDNS / NAT64)
    |
    |  ESP-Hosted bus (SDIO / SPI)
    +--- RPC: control + Wi-Fi ------> [ Wi-Fi CP ] ------- Wi-Fi RF -----> Wi-Fi AP / LAN
    |
    |  dedicated UART
    +--- 802.15.4 spinel (data) ----> [ 802.15.4 RCP ] --- 802.15.4 RF --> Thread mesh
```

Wi-Fi RF leaves the Wi-Fi CP; 802.15.4 RF leaves the RCP radio. For a
two-chip split (C6 Wi-Fi + H2 RCP) see the ESP-Thread-BR P4 example.

## Supported Platforms and Transports

### Supported Coprocessors

OpenThread uses an 802.15.4 radio co-processor (RCP).

| Coprocessor | ESP32-C6 | ESP32-H2 | ESP32-H4 | ESP32-C5 |
| :---------: | :------: | :------: | :------: | :------: |
| Support     | Yes      | Yes      | Yes      | Yes      |

### Supported Host Devices

| Host Device | ESP32-P4 | ESP32-H2 | Other MCUs | Linux |
| :---------: | :------: | :------: | :--------: | :---: |
| Support     | Yes | Yes | [Yes](https://github.com/espressif/esp-hosted/blob/master/docs/getting-started-mcu.md) | [Yes](https://github.com/espressif/esp-hosted/blob/master/docs/getting-started-linux.md) |

### Supported Connection buses

| Connection bus | SDIO | SPI Full-Duplex | SPI Half-Duplex | UART |
| :------------- | :--: | :-------------: | :-------------: | :--: |
| Linux host     | Yes  | Yes             | No              | No   |
| MCU host       | Yes  | Yes             | Yes             | Yes  |

For a multi-MCU split (P4 + C6 Wi-Fi + H2 RCP), see the
[ESP-Thread-BR P4 example](https://github.com/espressif/esp-thread-br/blob/main/examples/basic_thread_border_router/README_esp32p4.md).

> Running OT and Wi-Fi together on the same ESP32-C6 CP shares one
> radio — expect coexistence trade-offs. See the
> [C6 RF coexistence matrix](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-guides/coexist.html).

The co-processor is the OpenThread **RCP** *and* provides the Wi-Fi backbone for the border router (single-radio SW coexistence). Select the ESP-Hosted transport (RPC control):

```bash
cd examples/openthread/border_router/cp
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

The RCP feature and its 802.15.4 spinel UART (to the host) live under Features:

```text
Component config
└── ESP-Hosted
     └── Configure coprocessor
          └── Features
               ├── [*] Wi-Fi                                  (backbone for the border router)
               └── [*] OpenThread RCP (Radio Co-Processor)
                    ├── [*] Auto-initialise OpenThread feature at boot
                    └── OpenThread Transport
                         ├── (X) UART                          <── default (spinel to host)
                         ├── (1)  UART Port to Use
                         ├── (21) TX GPIO number               (ESP32-C6)
                         ├── (20) RX GPIO number               (ESP32-C6)
                         └── (460800) Baud Rate
```

CP dependency config is **pre-set in `cp/sdkconfig.defaults`** (do not remove):

```text
CONFIG_ESP_HOSTED_CP_FEAT_OPENTHREAD=y   # OpenThread RCP feature
CONFIG_ESP_HOSTED_CP_FEAT_WIFI=y         # Wi-Fi backbone for the border router
CONFIG_OPENTHREAD_ENABLED=y
CONFIG_OPENTHREAD_RADIO=y                 # Radio-Only (RCP) device
CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y      # Wi-Fi + 802.15.4 share one radio (SW coex)
```

Then flash and monitor:

```bash
idf.py -p <cp_usb_serial_port> flash monitor
```

<!-- generated from ../README.md — edit that file, not this one -->
