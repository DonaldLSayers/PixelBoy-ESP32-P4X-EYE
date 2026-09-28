# hosted_events — Host

Wires the host's ESP-IDF event loop to ESP-Hosted's lifecycle events
so the user application can react to coprocessor state changes
without polling. Three events on the `ESP_HOSTED_EVENT` base:

- `ESP_HOSTED_EVENT_CP_INIT` — CP came up (or, on a second occurrence,
  rebooted). Carries an `esp_reset_reason_t`.

- `ESP_HOSTED_EVENT_CP_HEARTBEAT` — periodic liveness ping at the
  interval the host asked for; absence implies a CP hang.

- `ESP_HOSTED_EVENT_TRANSPORT_FAILURE` — transport layer gave up on
  the bus.

The example associates to an AP, then waits. If the CP reboots or
the heartbeat times out it tears down the netif, re-inits the
transport, and reconnects — or restarts the host, depending on the
configured recovery method.

## Supported Platforms and Transports

### Supported Coprocessors

| Coprocessor | ESP32 | ESP32-C Series | ESP32-S Series |
| :----------: | :---: | :------------: | :------------: |
| Support     | Yes   | Yes            | Yes            |

### Supported Host Devices

| Host Device | ESP32-P4 | ESP32-H2 | Other MCUs | Linux |
| :---------: | :------: | :------: | :--------: | :---: |
| Support     | Yes | Yes | [Yes](https://github.com/espressif/esp-hosted/blob/master/docs/getting-started-mcu.md) | [Yes](https://github.com/espressif/esp-hosted/blob/master/docs/getting-started-linux.md) |

### Supported Connection buses

| Connection bus | SDIO | SPI Full-Duplex | SPI Half-Duplex | UART |
| :------------- | :--: | :-------------: | :-------------: | :--: |
| Linux host     | Yes  | Yes             | No              | No   |
| MCU host       | Yes  | Yes             | Yes             | Yes  |

Select the transport (must match the co-processor) and set the heartbeat / recovery options:

```bash
cd examples/system/hosted_events/mcu_host
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

```text
Example Configuration
├── [ ] Use legacy esp_hosted_* compat-surface main        <── default off
├── (5)  Heartbeat Interval in Seconds
├── [*] Enable heartbeat timeout monitor                   <── default on
├── (6)  Heartbeat Timeout in Seconds                       ← needs monitor enabled
├── Co-processor Recovery method
│     ├── (X) Restart ESP-Hosted transport                 <── default
│     ├── ( ) Restart the Host
│     └── ( ) Do nothing
├── (myssid)      WiFi SSID
├── (mypassword)  WiFi Password
├── WPA3 SAE mode selection ───────────────  (X) BOTH       <── default
├── (5)  Maximum retry
└── WiFi Scan auth mode threshold ──────────  (X) WPA2 PSK  <── default
```

Host dependency config is **pre-set in `sdkconfig.defaults`** (do not remove):

```text
CONFIG_ESP_HOSTED_HOST_FEAT_RPC=y          # RPC control path
CONFIG_ESP_HOSTED_HOST_FEAT_RPC_EXT_V2=y   # RPC ext-v2 (required)
CONFIG_ESP_HOSTED_HOST_FEAT_SYSTEM=y       # system feature
CONFIG_ESP_HOSTED_HOST_FEAT_WIFI=y         # Wi-Fi (example associates to an AP)
CONFIG_ESP_HOSTED_HOST_FEAT_HEARTBEAT=y    # CP heartbeat / liveness events
CONFIG_ESP_HOSTED_HOST_TRANSPORT_RESTART_ON_FAILURE=n  # app handles failures via hosted-events
```

Then flash and monitor:

```bash
idf.py -p <host_usb_serial_port> flash monitor
```

<!-- generated from ../README.md — edit that file, not this one -->
