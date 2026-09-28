# BT Controller MAC Address — Bluedroid over Hosted HCI — Host

Reads — and optionally overrides — the BT controller's MAC address on the
co-processor **before** the BT controller is initialised, then brings Bluedroid
up over the hosted HCI with one call — `esp_hosted_bt_host_stack_setup()` — and
advertises as a BLE beacon (`Bluedroid_Beacon`).
Ports upstream
`host_bluedroid_bt_controller_mac_addr` verbatim; the only ESP-Hosted-specific
change is that one call. Bluedroid runs on the **host**; the BT **controller**
runs on the ESP-Hosted **co-processor**, reached over the hosted transport
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
cd examples/bluetooth/esp_hosted_bluedroid/controller_mac_addr/mcu_host
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

This example adds its own options under **Example Configuration**:

```text
Example Configuration
└── [*] Update MAC Address of BT Controller
```

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

`app_main` sets the MAC (`bt_mac_actions()`) **before** calling
`esp_hosted_bt_host_stack_setup()`, which brings the controller up and binds Bluedroid
to the hosted HCI. See
[Porting a BT stack to ESP-Hosted](https://github.com/espressif/esp-hosted/blob/master/docs/design/bluetooth.md#porting-a-bt-stack-to-esp-hosted).

Then flash and monitor:

```bash
idf.py -p <host_usb_serial_port> flash monitor
```

### Verify

- The monitor log prints the controller MAC before and (if enabled) after the
  override; the device advertises as `Bluedroid_Beacon`.
- `esp_hosted_iface_mac_addr_set(mac, len, ESP_MAC_BT)` must run **before**
  `esp_hosted_bt_host_stack_setup()` (which initialises the controller) — see
  `main/main.c`.
- The MAC override is **temporary** and reverts on co-processor reset. For a
  permanent change, burn the address into the co-processor's eFuse.

<!-- generated from ../README.md — edit that file, not this one -->
