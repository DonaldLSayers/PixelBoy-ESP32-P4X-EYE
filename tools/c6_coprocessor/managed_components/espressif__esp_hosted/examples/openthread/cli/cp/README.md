# openthread / cli — Coprocessor

Runs a full OpenThread CLI (`ot ...`) on the host. The co-processor is the 802.15.4 **Radio Co-Processor (RCP)**: the host drives the RCP lifecycle over ESP-Hosted (RPC) and exchanges 802.15.4 (spinel) traffic over a **dedicated UART**. Adapted from `esp-idf/examples/openthread/ot_cli`.

The co-processor plays two roles: the **ESP-Hosted CP** (control, over the
bus) and the **802.15.4 RCP** (radio, over the dedicated UART). Both are the
same ESP32-C6 (Wi-Fi off) — two roles, not two chips, and not wired to each
other. A working deployment is 3 SoCs: host, that co-processor, and a
separate Thread peer.

```text
  Host (ESP32-P4) - OpenThread stack
    |
    |  ESP-Hosted bus (SDIO / SPI / UART)
    +--- RPC: RCP control -----------> [ ESP-Hosted CP ]      (control stops here)
    |
    |  dedicated UART
    +--- 802.15.4 spinel (data) -----> [ 802.15.4 RCP ] --- 802.15.4 RF ---> Thread peer
```

Data path: host -> spinel UART -> RCP radio -> RF -> peer (a separate SoC).

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

The co-processor runs as the OpenThread **RCP** (Wi-Fi off). Select the ESP-Hosted transport (used for RPC control):

```bash
cd examples/openthread/cli/cp
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
               └── [*] OpenThread RCP (Radio Co-Processor)
                    ├── [*] Auto-initialise OpenThread feature at boot
                    └── OpenThread Transport
                         ├── (X) UART                          <── default (spinel to host)
                         ├── (1)  UART Port to Use
                         ├── (21) TX GPIO number               (ESP32-C6)
                         ├── (20) RX GPIO number               (ESP32-C6)
                         └── (460800) Baud Rate
```

The RCP dependency config is **pre-set in `cp/sdkconfig.defaults`** (do not remove):

```text
CONFIG_ESP_HOSTED_CP_FEAT_OPENTHREAD=y   # ESP-Hosted OpenThread RCP feature
CONFIG_OPENTHREAD_ENABLED=y              # OpenThread stack
CONFIG_OPENTHREAD_RADIO=y                # Radio-Only (RCP) device
CONFIG_ESP_HOSTED_CP_FEAT_WIFI=n         # Wi-Fi off on the RCP
CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y      # SW coexistence ON — matches host RX_ON_WHEN_IDLE=n
```

> [!IMPORTANT]
> `CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y` is the canonical RCP setting
> (doc §2.1). It changes the OpenThread capability set the RCP advertises,
> so the host **must** pair it with `CONFIG_OPENTHREAD_RX_ON_WHEN_IDLE=n`
> — a mismatch makes OpenThread init fail.

`CONFIG_OPENTHREAD_RADIO=y` corresponds to *Component config → OpenThread → Thread Core Features → Thread device type → Radio Only Device*. Then flash and monitor:

```bash
idf.py -p <cp_usb_serial_port> flash monitor
```

<!-- generated from ../README.md — edit that file, not this one -->
