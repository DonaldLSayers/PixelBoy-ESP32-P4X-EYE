# Classic BT Discovery — Bluedroid Host over Standard HCI UART — Host

Classic Bluetooth (BR/EDR) device **and** service discovery. The Bluedroid host
stack runs on the **host** MCU with its local controller disabled; it reaches the
Bluetooth controller on the co-processor over a **dedicated physical UART in H4
mode** (not the ESP-Hosted VHCI bridge). The host's own `uart_driver.c` is bound
to Bluedroid via `esp_bluedroid_attach_hci_driver()`. This is the Bluedroid
counterpart of `bluetooth/nimble_uart/bleprph_host_only_uart` (NimBLE / BLE).

## Supported Platforms and Transports

### Supported Coprocessors (BT controller, UART-H4 mode)

Classic BR/EDR requires an ESP32 controller — the BLE-only chips cannot serve
the classic profile this demo discovers.

| Coprocessor | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C6 | ESP32-H2 | ESP32-S3 |
| :---------- | :---: | :------: | :------: | :------: | :------: | :------: |
| BR/EDR (classic) | Yes | No (BLE only) | No (BLE only) | No (BLE only) | No (BLE only) | No (BLE only) |

### Supported Host Devices

| Host device | ESP32-P4 | ESP32-H2 | Other MCUs |
| :---------- | :------: | :------: | :--------: |
| Support     | Yes      | Yes      | [Yes](https://github.com/espressif/esp-hosted/blob/master/docs/getting-started-mcu.md) |

### HCI transport

| HCI link           | Dedicated UART (H4) |
| :----------------- | :-----------------: |
| Host ↔ controller  | Yes — RTS/CTS flow control **mandatory** |

Bluetooth HCI here runs over its own UART, independent of the ESP-Hosted bus a
host may separately use for Wi-Fi.

The host runs the Bluedroid stack with its **local controller disabled** and
drives the HCI UART itself. Set the target and the example options:

```bash
cd examples/bluetooth/bluedroid_uart/bluedroid_host_only_uart/mcu_host
idf.py set-target esp32p4
idf.py menuconfig
```

This example adds one option under **Example Configuration**:

```text
Example Configuration
└── (921600) UART Baudrate for HCI          ← range 115200–921600; must match the controller
```

The host dependency config is pre-set in `sdkconfig.defaults` (do not remove):

```text
CONFIG_BT_ENABLED=y               # BT host stack on
CONFIG_BT_CONTROLLER_DISABLED=y   # no local controller — the CP supplies it over UART
CONFIG_BT_BLUEDROID_ENABLED=y     # Bluedroid host stack
CONFIG_BT_CLASSIC_ENABLED=y       # classic BR/EDR profile (discovery)
```

The host binds its own `uart_driver.c` to Bluedroid at startup:

```c
hci_uart_open();
esp_bluedroid_hci_driver_operations_t ops = {
    .send                  = hci_uart_send,
    .check_send_available  = hci_check_send_available,
    .register_host_callback = hci_register_host_callback,
};
esp_bluedroid_attach_hci_driver(&ops);
```

Then flash and monitor:

```bash
idf.py -p <host_usb_serial_port> flash monitor
```

### Wiring

- Host UART **TX → controller UART RX**, host UART **RX → controller UART TX**,
  and **RTS/CTS cross-wired** — hardware flow control is mandatory for HCI UART.
- Common ground between the two boards.
- The HCI baudrate (host `Example Configuration`) must match the controller's
  HCI UART baudrate.

### Verify

- On boot the host opens the HCI UART, attaches it to Bluedroid, then starts a
  classic **inquiry**; discovered devices (address, class, RSSI, name) are logged,
  followed by SDP **service discovery** on a found device.
- If nothing is discovered, confirm the UART wiring (including RTS/CTS), matching
  baudrates, and that the co-processor was built on an ESP32 with BR/EDR enabled.

<!-- generated from ../README.md — edit that file, not this one -->
