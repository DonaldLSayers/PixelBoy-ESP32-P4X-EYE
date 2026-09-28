# BLE Compatibility Test — Bluedroid over Hosted HCI — Host

BLE interoperability / compatibility test app — a GATT server exercising a
range of characteristics, MTU sizes, notifications, and connection parameters —
used to validate BLE behaviour against phones and other centrals. Ports upstream
`host_bluedroid_ble_compatibility_test` verbatim; the only ESP-Hosted-specific
change is one call — `esp_hosted_bt_host_stack_setup()` — that brings the controller
up and binds Bluedroid to the hosted HCI. Bluedroid runs on the **host**; the BT
**controller** runs on the
ESP-Hosted **co-processor**, reached over the hosted transport
(SDIO / SPI / SPI-HD / UART) via VHCI.

## Supported Platforms and Transports

### Supported Coprocessors (BT controller)

| Coprocessor | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C5 | ESP32-C6 | ESP32-C61 | ESP32-H2 | ESP32-S3 | ESP32-S2 |
| :---------- | :---: | :------: | :------: | :------: | :------: | :-------: | :------: | :------: | :------: |
| BLE support | Yes   | Yes      | Yes      | Yes      | Yes      | Yes       | Yes      | Yes      | No (no BLE) |

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
cd examples/bluetooth/esp_hosted_bluedroid/ble_compatibility_test/mcu_host
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

This example has no example-specific menuconfig options — the test
characteristic layout lives in `main/ble_compatibility_test.{c,h}`.

The host dependency config is pre-set in `sdkconfig.defaults` (do not remove).
`CONFIG_ESP_HOSTED_HOST_FEAT_BT` enables the BT host feature; the host stack is
chosen by the IDF BT Kconfig (`CONFIG_BT_BLUEDROID_ENABLED`) — no separate hosted
BT-port switch:

```text
CONFIG_ESP_HOSTED_HOST_FEAT_BT=y                       # host BT feature (controller runs on the CP)
CONFIG_BT_ENABLED=y                                    # BT host stack on
CONFIG_BT_CONTROLLER_DISABLED=y                        # no local controller — CP supplies it
CONFIG_BT_BLUEDROID_ENABLED=y                          # Bluedroid host stack (selects the hosted BT adapter)
```

`app_main` calls `esp_hosted_bt_host_stack_setup()` once (see the snippet under
**Verify**) to bring the controller up and bind Bluedroid to the hosted HCI.
More detail:
[Porting a BT stack to ESP-Hosted](https://github.com/espressif/esp-hosted/blob/master/docs/design/bluetooth.md#porting-a-bt-stack-to-esp-hosted).

Then flash and monitor:

```bash
idf.py -p <host_usb_serial_port> flash monitor
```

### Verify

Hosted wiring is the only ESP-Hosted-specific part of `app_main` — the rest is
verbatim upstream Bluedroid:

```c
esp_hosted_connect_to_slave();
esp_hosted_bt_host_stack_cfg_t bt = ESP_HOSTED_BT_HOST_STACK_CONFIG_DEFAULT();
ESP_ERROR_CHECK(esp_hosted_bt_host_stack_setup(&bt));  // controller up + HCI bound
esp_bluedroid_init(); esp_bluedroid_enable();
```

- Connect with a phone or other central (LightBlue, nRF Connect) and walk the
  characteristics, MTU negotiation, notifications, and connection-parameter
  updates to validate interoperability.

<!-- generated from ../README.md — edit that file, not this one -->
