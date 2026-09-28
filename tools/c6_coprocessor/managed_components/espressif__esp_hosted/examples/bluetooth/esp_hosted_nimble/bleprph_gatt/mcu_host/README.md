# BLE GATT Server — NimBLE over Hosted HCI — Host

Connectable BLE peripheral exposing one custom 128-bit GATT service with two
characteristics (read/write + notify). NimBLE runs on the **host**; the
Bluetooth **controller** runs on the ESP-Hosted **co-processor**, reached over
the hosted transport (SDIO / SPI / SPI-HD / UART) via VHCI.
The only ESP-Hosted-specific code is one call —
`esp_hosted_bt_host_stack_setup()` — which brings the controller up and binds NimBLE
to the hosted HCI; everything else is standard NimBLE. See [Porting a BT stack
to ESP-Hosted](https://github.com/espressif/esp-hosted/blob/master/docs/design/bluetooth.md#porting-a-bt-stack-to-esp-hosted).
The bluedroid counterpart is
`bluetooth/esp_hosted_bluedroid/ble_gatt_server`.

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
cd examples/bluetooth/esp_hosted_nimble/bleprph_gatt/mcu_host
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

This example adds its own options under **BLE Peripheral GATT config**:

```text
BLE Peripheral GATT config
├── (esp-hosted-nim-gatt) BLE device name
└── (1000) Notification interval (ms)                        ← range 100–60000
```

The host dependency config is pre-set in `sdkconfig.defaults` (do not remove).
`CONFIG_ESP_HOSTED_HOST_FEAT_BT` enables the BT host feature; the host stack is
chosen by the IDF BT Kconfig (`CONFIG_BT_NIMBLE_ENABLED`) — no separate hosted
BT-port switch:

```text
CONFIG_ESP_HOSTED_HOST_FEAT_BT=y        # host BT feature (controller runs on the CP)
CONFIG_BT_ENABLED=y                     # BT host stack on
CONFIG_BT_CONTROLLER_DISABLED=y         # no local controller — CP supplies it
CONFIG_BT_NIMBLE_ENABLED=y              # NimBLE host stack (selects the hosted BT adapter)
```

Then flash and monitor:

```bash
idf.py -p <host_usb_serial_port> flash monitor
```

### Verify

- Test with any generic BLE explorer — LightBlue, nRF Connect, `bluetoothctl`.
- Notifications are pushed via `ble_gatts_notify_custom` on the notify
  characteristic at the configured cadence.
- `app_main` calls `esp_hosted_bt_host_stack_setup()` once — controller up + HCI
  bound — then runs standard NimBLE (GAP + GATT server).

<!-- generated from ../README.md — edit that file, not this one -->
