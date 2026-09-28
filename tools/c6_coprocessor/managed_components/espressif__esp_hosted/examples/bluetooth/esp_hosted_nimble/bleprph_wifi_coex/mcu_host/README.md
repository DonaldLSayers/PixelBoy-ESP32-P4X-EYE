# BLE + Wi-Fi Coexistence — NimBLE over Hosted HCI — Host

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

Select the transport (must match the co-processor):

```bash
cd examples/bluetooth/esp_hosted_nimble/bleprph_wifi_coex/mcu_host
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

Set the Wi-Fi credentials under **Example Connection Configuration** (provided by
the `eh_example_connect` helper):

```text
Example Connection Configuration
├── (myssid)     WiFi SSID
└── (mypassword) WiFi Password
```

This example also adds its own options under **Example Configuration**:

```text
Example Configuration
├── (1.1.1.1) Ping target
└── (100) Ping count                                         ← one ping per second
```

The host dependency config is pre-set in `sdkconfig.defaults` (do not remove):

```text
CONFIG_ESP_HOSTED_HOST_FEAT_WIFI=y       # host Wi-Fi feature (remote radio on the CP)
CONFIG_ESP_HOSTED_HOST_FEAT_BT=y         # host BT feature (controller runs on the CP)
CONFIG_BT_ENABLED=y                      # BT host stack on
CONFIG_BT_CONTROLLER_DISABLED=y          # no local controller — CP supplies it
CONFIG_BT_NIMBLE_ENABLED=y               # NimBLE host stack (selects the hosted BT adapter)
CONFIG_ESP_WIFI_REMOTE_LIBRARY_HOSTED=y  # Wi-Fi via esp_wifi_remote over Hosted
```

Then flash and monitor:

```bash
idf.py -p <host_usb_serial_port> flash monitor
```

### Verify

- Test with any BLE scanner (LightBlue, nRF Connect) **plus** an AP with
  internet reachability to the configured ping target — BLE advertising and the
  Wi-Fi ping stream run concurrently.
- BLE comes up via one `esp_hosted_bt_host_stack_setup()` call; Wi-Fi's
  `override_path` = `esp_hosted` swaps in the hosted Wi-Fi implementation
  transparently — the source is otherwise verbatim IDF.
- Currently IPv4-only.

<!-- generated from ../README.md — edit that file, not this one -->
