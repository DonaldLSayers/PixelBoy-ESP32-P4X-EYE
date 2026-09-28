# BLE + Wi-Fi Coexistence — NimBLE over Hosted HCI — Coprocessor

Wi-Fi STA + BLE peripheral running concurrently, both radios on the same
co-processor. Verbatim merge of the upstream IDF NimBLE `bleprph` and
`protocols/icmp_echo` examples: the **host** connects to an AP, pings a target,
and simultaneously advertises a GATT server. Wi-Fi comes up via the hosted
`override_path` = `esp_hosted` override (no source changes); BLE comes up via one
ESP-Hosted call — `esp_hosted_bt_host_stack_setup()` — which brings the controller up
and binds NimBLE to the hosted HCI. See [Porting a BT stack to
ESP-Hosted](https://github.com/espressif/esp-hosted/blob/master/docs/design/bluetooth.md#porting-a-bt-stack-to-esp-hosted).
The co-processor runs the combined Wi-Fi + BT controller firmware
(`wifi_hosted_hci`), with BT HCI carried over the hosted transport
(SDIO / SPI / SPI-HD / UART) via VHCI.

## Supported Platforms and Transports

### Supported Coprocessors (Wi-Fi + BT controller, same chip)

| Coprocessor       | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C5 | ESP32-C6 | ESP32-C61 | ESP32-H2 | ESP32-S3 | ESP32-S2 |
| :---------------- | :---: | :------: | :------: | :------: | :------: | :-------: | :------: | :------: | :------: |
| Wi-Fi + BLE       | Yes   | Yes      | Yes      | Yes      | Yes      | Yes       | No (no Wi-Fi) | Yes | No (no BLE) |

### Supported Host Devices

| Host Device | ESP32-P4 | ESP32-H2 | Other MCUs |
| :---------- | :------: | :------: | :--------: |
| Support     | Yes      | Yes      | [Yes](https://github.com/espressif/esp-hosted/blob/master/docs/getting-started-mcu.md) |

### Supported HCI transports

| HCI over hosted bus (VHCI) | SDIO | SPI Full-Duplex | SPI Half-Duplex | UART |
| :------------------------- | :--: | :-------------: | :-------------: | :--: |
| MCU host                   | Yes  | Yes             | Yes             | Yes  |

This example runs Wi-Fi and BLE concurrently on the same co-processor, so it
uses the combined Wi-Fi + BT CP profile. The `wifi_hosted_hci` CP's
`sdkconfig.defaults` already enables Wi-Fi plus the Bluetooth controller with BT
HCI carried over the host bus (VHCI); you only select the transport (must match
the host):

```bash
cd examples/bluetooth/esp_hosted_nimble/bleprph_wifi_coex/cp
idf.py set-target <cp_chip>
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

Wi-Fi and the Bluetooth profile are pre-selected by `sdkconfig.defaults`;
confirm the BT side under **Features**:

```text
Component config
└── ESP-Hosted
     └── Configure coprocessor
          └── Features
               ├── WiFi                                   <── enabled by sdkconfig.defaults
               └── Bluetooth                              <── enabled by sdkconfig.defaults
                    ├── [*] Auto-initialise Bluetooth at boot
                    └── BT HCI transport
                         ├── (X) HCI over VHCI (SPI/SDIO/SPI-HD)   <── default
                         └── ( ) HCI over UART
```

The CP dependency config is pre-set in `sdkconfig.defaults` (do not remove):

```text
CONFIG_ESP_HOSTED_CP_FEAT_WIFI=y          # Wi-Fi on — Wi-Fi + BT coexistence CP
CONFIG_ESP_HOSTED_CP_FEAT_BT=y            # ESP-Hosted BT controller feature
CONFIG_ESP_HOSTED_CP_BT_ENABLED=y         # required by ESP_HOSTED_CP_FEAT_BT
CONFIG_ESP_HOSTED_CP_FEAT_BT_HCI_VHCI=y   # BT HCI over the host bus (VHCI)
CONFIG_BT_ENABLED=y                       # BT controller stack on the CP
CONFIG_BT_CONTROLLER_ONLY=y               # controller-only profile
```

Then flash and monitor:

```bash
idf.py -p <cp_usb_serial_port> flash monitor
```

<!-- generated from ../README.md — edit that file, not this one -->
