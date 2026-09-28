# BLE Advertise Minimal — Bluedroid over Hosted HCI — Host

Smallest possible Bluedroid + ESP-Hosted example: it brings the BT
**controller** up on the ESP-Hosted **co-processor** and binds Bluedroid to the
hosted HCI with one call — `esp_hosted_bt_host_stack_setup()` — then starts
non-connectable BLE advertising. Bluedroid runs on the **host**; the controller
is reached over the hosted transport (SDIO / SPI / SPI-HD / UART) via VHCI. That
one call is the only ESP-Hosted-specific code — everything after
`esp_bluedroid_init()` is standard IDF Bluedroid. The NimBLE counterpart is
`bluetooth/esp_hosted_nimble/bleprph_minimal`.

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
cd examples/bluetooth/esp_hosted_bluedroid/ble_advertise_minimal/mcu_host
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

This example adds its own options under **BLE Advertise Minimal config**:

```text
BLE Advertise Minimal config
├── (ESP_HOSTED_BLE) BLE device name (advertised)
└── (100) Advertising interval (ms)                          ← range 20–10240
```

The host dependency config is pre-set in `sdkconfig.defaults` (do not remove).
`CONFIG_ESP_HOSTED_HOST_FEAT_BT` enables the BT host feature; the host stack is
chosen by the IDF BT Kconfig (`CONFIG_BT_BLUEDROID_ENABLED`) — no separate hosted
BT-port switch:

```text
CONFIG_ESP_HOSTED_HOST_FEAT_BT=y                      # host BT feature (controller runs on the CP)
CONFIG_BT_ENABLED=y                                  # BT host stack on
CONFIG_BT_CONTROLLER_DISABLED=y                      # no local controller — CP supplies it
CONFIG_BT_BLUEDROID_ENABLED=y                        # Bluedroid host stack (selects the hosted BT adapter)
```

`app_main()` calls `esp_hosted_bt_host_stack_setup()` (from `esp_hosted_bt_host_stack.h`)
once, after `esp_hosted_connect_to_slave()` and before `esp_bluedroid_init()` —
it brings the controller up and binds Bluedroid to the hosted HCI.

Then flash and monitor:

```bash
idf.py -p <host_usb_serial_port> flash monitor
```

### Verify

- Scan with any generic BLE explorer — LightBlue, nRF Connect, `bluetoothctl` —
  and look for the advertised device name.
- `app_main` calls `esp_hosted_bt_host_stack_setup()` once — it does controller
  init + enable + HCI bind in one call — then runs standard Bluedroid.
- The hosted BT adapter is documented in
  [Porting a BT stack to ESP-Hosted](https://github.com/espressif/esp-hosted/blob/master/docs/design/bluetooth.md#porting-a-bt-stack-to-esp-hosted).
- ESP32 CP supplies BLE 4.2 only — do **not** enable
  `CONFIG_BT_BLE_50_FEATURES_SUPPORTED=y` against an ESP32 CP, the build will
  fail.
- For a connectable peripheral with GATT services, see
  `bluetooth/esp_hosted_bluedroid/ble_gatt_server`.

<!-- generated from ../README.md — edit that file, not this one -->
