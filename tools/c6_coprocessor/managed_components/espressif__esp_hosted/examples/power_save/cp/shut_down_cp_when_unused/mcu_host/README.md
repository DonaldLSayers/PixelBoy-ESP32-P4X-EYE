# Shut down the co-processor when unused — Host

The host holds the co-processor (CP) in reset (EN low) whenever it doesn't need
networking, cutting the CP rail to ~0 mA. On wake-up the host re-runs
`eh_host_init()` + `eh_host_connect_to_slave()`, re-binds Wi-Fi events,
reconnects to the AP, and resumes. It exercises a full CP cold-boot every cycle,
with heap-tracing wired in so you can confirm there is no leak across cycles.

## Supported Platforms and Transports

| Host device | ESP32-P4 | ESP32-H2 | Other MCUs |
| :---------- | :------: | :------: | :--------: |
| Support     | Yes      | Yes      | [Yes](https://github.com/espressif/esp-hosted/blob/master/docs/getting-started-mcu.md) |

| Coprocessor | Any Espressif chip with Wi-Fi (default ESP32-C6) |
| :---------- | :----------------------------------------------- |
| Support     | Yes                                              |

| Communication bus | SDIO | SPI Full-Duplex | SPI Half-Duplex | UART |
| :---------------- | :--: | :-------------: | :-------------: | :--: |
| MCU host          | Yes  | Yes             | Yes             | Yes  |

Select the transport (must match the co-processor) and set the Wi-Fi credentials:

```bash
cd examples/power_save/cp/shut_down_cp_when_unused/mcu_host
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

The transport's **reset GPIO** must be wired to the CP's EN pin — the example
reads it via `eh_host_transport_get_reset_config()` and drives it directly to
power-cycle the CP. Then set the example's own options:

```text
Example Configuration
├── (myssid)     WiFi SSID
├── (mypassword) WiFi Password
├── (5)          Maximum retry
└── [ ] Use legacy esp_hosted_* compat-surface main    ← default n; flip on to build from main/legacy/main.c
```

The host dependency config is pre-set in `sdkconfig.defaults` (do not remove):

```text
CONFIG_ESP_HOSTED_HOST_FEAT_RPC=y          # RPC control channel to the CP
CONFIG_ESP_HOSTED_HOST_FEAT_RPC_EXT_V2=y   # required RPC ext-v2 surface
CONFIG_ESP_HOSTED_HOST_FEAT_WIFI=y         # host-side Wi-Fi (remote) API
```

Then flash and monitor:

```bash
idf.py -p <host_usb_serial_port> flash monitor
```

### Verify

- Cycle in `power_save_task()`: cleanup Wi-Fi → `eh_host_deinit()` → pull reset
  GPIO low (CP off) → sleep 5 s → re-init Hosted (CP boots on EN release) →
  reconnect Wi-Fi → wait for IP. Repeats forever.
- Heap tracing is on by default (`CONFIG_HEAP_TRACING=y` standalone) so
  `memory_debug_log_heap()` checkpoints show baseline / per-cycle start / per-cycle
  end — useful to verify the init/deinit cycle is leak-clean.
- The Hosted event handler watches `EH_HOST_EVENT_TRANSPORT_UP` /
  `EH_HOST_EVENT_CP_INIT` to release the host past the per-cycle transport-up barrier.
- This example is MCU-only by design — Linux hosts already power-cycle the CP via
  the kmod's normal `rmmod` / `modprobe` cycle and the bus-driver reset GPIO.

<!-- generated from ../README.md — edit that file, not this one -->
