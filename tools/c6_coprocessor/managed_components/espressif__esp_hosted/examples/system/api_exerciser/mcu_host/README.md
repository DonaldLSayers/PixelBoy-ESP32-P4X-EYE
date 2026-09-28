# API exerciser — Host

A generic host+CP example that exposes the native `eh_host_*` API surface as
console commands, so functional API coverage is added as **data** (a console
command + an expected line) rather than a new example per scenario.

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

## The result contract

Every command prints exactly one newline-terminated line:

```
EH rc=<int> cmd=<name> [k=v ...]
```

- `rc=0` on success; a non-zero `rc` (with `err=<name>`) on failure.
- getters add fields (e.g. `wifi_get_ps` → `EH rc=0 cmd=wifi_get_ps ps=2`).
- Round-trips (set → get → compare) and negative/arg-validation checks are
  expressed entirely in the test, not in firmware.

Type `help` at the `eh>` prompt for the full command list.

<!-- generated from ../README.md — edit that file, not this one -->
