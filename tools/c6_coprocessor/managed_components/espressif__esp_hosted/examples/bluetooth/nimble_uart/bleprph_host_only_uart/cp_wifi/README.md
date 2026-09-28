# BLE Peripheral — NimBLE Host over Standard HCI UART — Coprocessor

NimBLE host-only BLE peripheral (`bleprph` GATT server) that talks HCI over a
**dedicated physical UART** (H4) to an external BT controller. Unlike the
`hosted_hci` examples, HCI here does **not** traverse the ESP-Hosted VHCI bridge:
the **controller** runs standalone on the **co-processor** in native UART-H4
mode, and the **host** drives the link with its own UART transport
(`main/uart_driver.c`). NimBLE's local controller and its built-in UART transport
are both disabled so the application transport takes over.

The host and controller are wired directly UART-to-UART (host TX ↔ controller RX,
host RX ↔ controller TX, common ground) — there is no SDIO / SPI hosted bus in
this scenario. ESP32-S2 has no BT controller and is unsupported.

## Supported Platforms and Transports

### Supported Coprocessors (BT controller in UART-H4 mode)

| Coprocessor | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C5 | ESP32-C6 | ESP32-C61 | ESP32-H2 | ESP32-S3 | ESP32-S2 |
| :---------- | :---: | :------: | :------: | :------: | :------: | :-------: | :------: | :------: | :------: |
| BLE support | Yes   | Yes      | Yes      | Yes      | Yes      | Yes       | Yes      | Yes      | No (no BT) |

### Supported Host Devices

| Host Device | ESP32-P4 | ESP32 / C-series / S3 | Other MCUs |
| :---------- | :------: | :-------------------: | :--------: |
| Support     | Yes      | Yes                   | [Yes](https://github.com/espressif/esp-hosted/blob/master/docs/getting-started-mcu.md) |

### HCI transport

| HCI over dedicated UART (H4) | Direct wired UART between host and controller       |
| :--------------------------- | :-------------------------------------------------- |
| MCU host                     | Yes — host runs its own UART driver (`main/uart_driver.c`) |

The co-processor runs the BT controller in native UART-H4 mode — HCI is exposed
on a physical UART, not on the ESP-Hosted bus. The per-target
`sdkconfig.defaults.esp32*` overlays already enable the UART-H4 symbols on most
targets; where they are not pre-populated, enable them in menuconfig under the
Bluetooth controller HCI mode before building. (`esp32c61` follows the `esp32c6`
UART-H4 settings; `esp32s2` has no BT controller.)

```bash
cd examples/bluetooth/nimble_uart/bleprph_host_only_uart/cp
idf.py set-target <cp_chip>
idf.py menuconfig
```

```text
Component config
└── Bluetooth
     └── Controller Options
          └── HCI mode
               ├── ( ) VHCI
               └── (X) UART                    <── HCI over a dedicated UART (H4)
                    ├── (5)  HCI UART Tx pin    ← wire to host UART Rx
                    ├── (12) HCI UART Rx pin    ← wire to host UART Tx
                    └── [ ]  HCI UART flow control
```

The CP dependency config is pre-set in `sdkconfig.defaults` (pins shown are the
`esp32c6` overlay — target-specific; do not remove):

```text
CONFIG_ESP_HOSTED_CP_FEAT_BT=y            # ESP-Hosted BT controller feature
CONFIG_ESP_HOSTED_CP_BT_ENABLED=y         # required by ESP_HOSTED_CP_FEAT_BT
CONFIG_ESP_HOSTED_CP_FEAT_BT_HCI_UART=y   # BT HCI on a dedicated UART (H4), NOT the hosted bus
CONFIG_BT_ENABLED=y                       # BT controller stack on the CP
CONFIG_BT_CONTROLLER_ONLY=y               # controller-only profile
CONFIG_BT_LE_HCI_INTERFACE_USE_UART=y     # controller HCI carried on UART
CONFIG_BT_LE_HCI_UART_TX_PIN=5            # controller UART TX (to host RX)
CONFIG_BT_LE_HCI_UART_RX_PIN=12           # controller UART RX (from host TX)
CONFIG_BT_LE_HCI_UART_FLOWCTRL=n          # no HW flow control (c6 default)
CONFIG_ESP_HOSTED_CP_FEAT_WIFI=n          # Wi-Fi off — BT controller-only CP
```

Then flash and monitor:

```bash
idf.py -p <cp_usb_serial_port> flash monitor
```

### CP variants: `cp/` vs `cp_wifi/`

This scenario ships two co-processor firmwares — pick one:

| CP role     | BT controller (UART-H4) | Hosted Wi-Fi (SDIO/SPI) | Flash this when… |
| :---------- | :---------------------: | :---------------------: | :--------------- |
| `cp/`       | Yes                     | No                      | the host needs Bluetooth only (this NimBLE demo) |
| `cp_wifi/`  | Yes                     | Yes                     | the product **also** needs Wi-Fi served to the host over the hosted bus |

`cp_wifi/` is byte-for-byte the same firmware as `cp/` except it enables the
Wi-Fi CP feature:

```text
CONFIG_ESP_HOSTED_CP_FEAT_WIFI=y          # Wi-Fi stack on the CP (served to the host over SDIO/SPI)
```

Build it identically from `.../bleprph_host_only_uart/cp_wifi`. The NimBLE host
app in this example uses Bluetooth only, so `cp_wifi`'s Wi-Fi is consumed by a
separate hosted-Wi-Fi host app (any `wifi/*` mcu_host example) running over the
hosted bus in parallel — the BT path stays on the dedicated UART either way.

<!-- generated from ../README.md — edit that file, not this one -->
