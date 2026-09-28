# mem_monitor — Host

Subscribes the host to periodic heap / task-stat reports from the
coprocessor. The CP samples its own internal-DMA, internal-8bit,
external-DMA, and external-8bit heaps at a configurable interval and
pushes an event up; the host either logs every report or only the ones
that cross a low-water threshold. Useful for catching slow leaks or
sizing buffers without leaving instrumentation in the CP firmware.

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

Select the transport (must match the co-processor) and set the monitor options:

```bash
cd examples/mem_monitor/mcu_host
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
├── [ ] Use legacy esp_hosted_* compat-surface main            <── default off
├── [ ] Query Memory Info only once                            <── default off
│        (the options below apply while this is off)
├── (10)  Memory Monitor Interval (in seconds)
├── [*] Always send a memory report                            <── default on
├── (8192) Threshold for internal DMA Heap Memory (in bytes)   ⎫
├── (8192) Threshold for internal 8-Bit Heap Memory (in bytes) ⎬ used when
├── (8192) Threshold for external DMA Heap Memory (in bytes)   │ "Always send"
├── (8192) Threshold for external 8-Bit Heap Memory (in bytes) ⎭ is off
└── Wifi Config
      ├── (myssid)      WiFi SSID
      ├── (mypassword)  WiFi Password
      ├── WPA3 SAE mode selection ───────────  (X) BOTH         <── default
      ├── (5)  Maximum retry
      └── WiFi Scan auth mode threshold ──────  (X) WPA2 PSK    <── default
```

Host dependency config is **pre-set in `sdkconfig.defaults`** (do not remove):

```text
CONFIG_ESP_HOSTED_HOST_FEAT_RPC=y            # RPC control path
CONFIG_ESP_HOSTED_HOST_FEAT_RPC_EXT_V2=y     # RPC ext-v2 (required)
CONFIG_ESP_HOSTED_HOST_FEAT_SYSTEM=y         # system feature
CONFIG_ESP_HOSTED_HOST_FEAT_WIFI=y           # Wi-Fi (example connects to an AP)
CONFIG_ESP_HOSTED_HOST_FEAT_MEM_MONITOR=y    # subscribe to CP mem-monitor reports
CONFIG_ESP_WIFI_REMOTE_LIBRARY_HOSTED=y      # route esp_wifi_remote through ESP-Hosted
```

Then flash and monitor:

```bash
idf.py -p <host_usb_serial_port> flash monitor
```

<!-- generated from ../README.md — edit that file, not this one -->
