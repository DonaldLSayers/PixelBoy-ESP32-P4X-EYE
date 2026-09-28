# peer_data_transfer — Coprocessor

Demonstrates the `peer_data` custom-RPC channel: the host sends
arbitrary application-defined messages identified by a `uint32_t`
message ID, the CP echoes them back on a paired response ID, and the
host verifies the payload round-tripped intact. The example exercises
the full supported size range (1 byte → 8166 bytes) and also fires a
deliberately-unregistered ID to confirm the framework rejects it
gracefully.

Message-ID pairs used by the demo:

- `MSG_ID_CAT` / `MSG_ID_MEOW` — 1–1000 bytes (small).
- `MSG_ID_DOG` / `MSG_ID_WOOF` — 1000–4000 bytes (medium).
- `MSG_ID_HUMAN` / `MSG_ID_HELLO` — 4000–8166 bytes (large).
- `MSG_ID_GHOST` — exceeds the configured callback table; expected
  to fail.

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

## Scenario

The host registers a receive callback, sends application-defined data to the co-processor over the custom-data channel, and receives peer data back through the callback.

```mermaid
sequenceDiagram
    participant App as Host app
    participant CP as Co-processor
    App->>CP: eh_host_peer_data_register() / esp_hosted_register_custom_callback()
    App->>CP: eh_host_peer_data_send() / esp_hosted_send_custom_data()
    CP-->>App: peer data delivered to the registered callback
```

Peer-Data support is enabled by the co-processor's `sdkconfig.defaults`
(`ESP_HOSTED_CP_FEAT_PEER_DATA`) — no extra option to enable. Select the
transport via `idf.py menuconfig`:

```bash
cd examples/peer_data_transfer/cp
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

Each bus has its own settings submenu (pins, clock, checksum) — defaults are fine for bring-up.

The CP dependency config is **pre-set in `sdkconfig.defaults`** (do not remove):

```text
CONFIG_ESP_HOSTED_CP_FEAT_PEER_DATA=y          # CP peer-data (custom RPC) feature
CONFIG_BT_ENABLED=n                            # BT off (unused by this example)
```

Then flash and monitor:

```bash
idf.py -p <cp_usb_serial_port> flash monitor
```

<!-- generated from ../README.md — edit that file, not this one -->
