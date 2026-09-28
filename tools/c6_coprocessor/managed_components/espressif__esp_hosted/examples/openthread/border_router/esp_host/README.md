# openthread / border_router — Host

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

Select the transport (must match the co-processor), then configure the OpenThread RCP link:

```bash
cd examples/openthread/border_router/esp_host
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

The host drives the OpenThread RCP over a **dedicated UART**, separate from the ESP-Hosted transport bus:

```text
Component config
└── ESP-Hosted
     └── Configure host
          └── Features
               └── [*] OpenThread (RCP control)
                    └── OpenThread RCP transport (host side)
                         ├── (X) Dedicated UART                       <── default
                         └── ( ) ESP-Hosted transport (not supported yet)
                    ├── (1)      UART port
                    ├── (11)     Host TX pin (to RCP RX)
                    ├── (10)     Host RX pin (from RCP TX)
                    └── (460800) Baud rate
```

Border-router role settings live in the upstream `Component config → OpenThread` (border-router on, RCP over UART spinel, IPv6 forwarding hooks) and `Component config → LWIP` (IPv6 forward + custom IP6 input/route hooks for NAT64 / off-mesh routing).

Host dependency config is **pre-set in `esp_host/sdkconfig.defaults`** (`.esp32p4` for the PSRAM part) — do not remove:

```text
CONFIG_ESP_HOSTED_HOST_FEAT_OPENTHREAD=y     # host OpenThread feature
CONFIG_ESP_HOSTED_HOST_FEAT_WIFI=y           # Wi-Fi backbone
CONFIG_ESP_HOSTED_HOST_FEAT_RPC_EXT_V2=y     # required RPC ext-v2
CONFIG_OPENTHREAD_ENABLED=y
CONFIG_OPENTHREAD_BORDER_ROUTER=y            # border-router role
CONFIG_SPIRAM=y                              # REQUIRED — host won't start without PSRAM
CONFIG_OPENTHREAD_RX_ON_WHEN_IDLE=n          # keep in sync with RCP coexistence
```

Then flash and monitor:

```bash
idf.py -p <host_usb_serial_port> flash monitor
```

<!-- generated from ../README.md — edit that file, not this one -->
