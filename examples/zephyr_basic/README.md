# zephyr_basic

The Zephyr counterpart of `wifi_basic`'s boot and crash paths: initialise sentry-micro, print
what it learned about this boot, and report either the crash the previous boot died of, with
its backtrace, or else the boot as one event — delivered through `scripts/serial_relay.py` on
the laptop the board is plugged into. No WiFi.

Target: `esp32s3_devkitc/esp32s3/procpu`. Other boards should build (the device layer falls
back to Zephyr's generic APIs off Espressif), but only this one has been compiled. Both the
boot event and the crash path have been run on an ESP32-S3 (a WROOM-1 N16R8 module with only
the chip's native USB, see below), and both reached Sentry.

## Build

Needs a Zephyr workspace, the Zephyr SDK, and `west` plus Zephyr's Python requirements in this
repository's virtualenv:

```bash
.venv/bin/pip install west esptool
.venv/bin/pip install -r "$ZEPHYR_BASE/scripts/requirements-base.txt"
```

The DSN comes from the environment at configure time, like the PlatformIO examples. Without
it the build still succeeds, with a CMake warning and a placeholder DSN that reaches nowhere.

```bash
export SENTRY_MICRO_DSN='https://<key>@o<org>.ingest.sentry.io/<project>'
export SENTRY_MICRO_RELEASE='my-firmware@1.0.0'  # optional

cd examples/zephyr_basic
PATH=$PWD/../../.venv/bin:$PATH \
ZEPHYR_BASE=/path/to/workspace/zephyr \
ZEPHYR_TOOLCHAIN_VARIANT=zephyr \
ZEPHYR_SDK_INSTALL_DIR=/path/to/zephyr-sdk-1.0.1 \
west build -p always -b esp32s3_devkitc/esp32s3/procpu -d build .
```

`ZEPHYR_BASE` is the `zephyr` checkout inside the west workspace; west finds the workspace
(and so hal_espressif) from there. The values are read when CMake configures, so after
changing the DSN or release rebuild with `-p always`.

Add `-- -DSENTRY_DEMO_CRASH=1` to the build command to crash deliberately once the boot event
is out. sentry-micro's fatal-error handler records the crash in RTC memory and marks the reboot
as a panic; the next boot reports it with its backtrace, reads `reset reason: panic`, and does
not crash again, so one power-on gives one crash-and-report cycle.

No sysbuild and no MCUboot: Espressif boards without sysbuild boot the application directly
(`ESP_SIMPLE_BOOT`), so a plain `west build` produces a flashable image.

## Flash and relay

The devkit has two USB-C ports. The console — and so the relay — is `uart0`, which is the port
labelled **UART** (the USB-to-UART bridge, `/dev/ttyUSB0` on Linux). The one labelled **USB**
is the S3's native USB-Serial-JTAG (`/dev/ttyACM0`); see below for boards that have only that.

```bash
west flash -d build --esp-device /dev/ttyUSB0      # same environment as the build

.venv/bin/python scripts/serial_relay.py --port /dev/ttyUSB0 --baud 115200
```

Run the relay from the repository root. It reads the DSN from `SENTRY_MICRO_DSN` (or
`--dsn`), refuses to POST anywhere but that DSN's host, and passes every other line through to
your terminal, so it is the serial monitor too — the two cannot share the port.

Start the relay, then press **RST** on the board. The boot event is sent the moment the
board comes up and waits 15 s for an answer; with nothing listening it times out as "no
route", and with no offline storage in this milestone it is not retried.

### Boards with only the native USB port

A board that brings out just the S3's own USB, with no bridge on `uart0`, routes the console to
the USB-Serial-JTAG with the overlay in this directory, and then flashes and relays through the
same port:

```bash
west build -p always -b esp32s3_devkitc/esp32s3/procpu -d build . -- \
    -DEXTRA_DTC_OVERLAY_FILE=usb_serial.overlay
west flash -d build --esp-device /dev/ttyACM0
.venv/bin/python scripts/serial_relay.py --port /dev/ttyACM0 --baud 115200
```

Two things differ from a bridge. The chip drops off the USB bus on every reset and comes back
about a second later, so the relay reopens the port when it reappears instead of exiting, and
`main.c` holds the boot event for 3 s so the host has the port open again before it is sent;
whatever the firmware prints before that, the early boot log included, is dropped by the
driver. And the reset `esptool` performs after flashing arrives through the JTAG side, which
Zephyr's hwinfo cannot name, so the first boot after a flash reports `reset reason: unknown`.

## Why `CONFIG_LOG_MODE_MINIMAL`

The relay request is one line on the console UART, around 450 ms on the wire for a 4 KB
envelope at 115200 baud. Any log line printed in that window is spliced into the middle of it,
and the relay decodes garbage. Zephyr's default deferred logging prints from its own thread
whenever that thread next runs, which is exactly how a line lands mid-request. Minimal mode
prints synchronously in the caller's context instead. A UART of its own for the relay is the
robust fix; pass it as the `uart` argument of `sentry_serial_transport_init()`.

For the same reason, do not enable the shell on the relay's UART: it reads RX too, and steals
the `@SENTRY-RELAY-RESULT/1` answer.

## Known gaps in this milestone

- **Frames are hex.** Zephyr links with `--build-id=none` and there is no release flow that
  uploads its ELF, so Sentry cannot symbolicate them yet. Compare them against the backtrace
  Zephyr prints on the console, or resolve them with `addr2line` against `build/zephyr/zephyr.elf`.
- **sentry-micro owns `k_sys_fatal_error_handler()`.** An application with its own handler
  sets `CONFIG_SENTRY_MICRO_FATAL_HANDLER=n` and calls `sentry_zephyr_record_fatal()` from it
  (`src/device/zephyr/sentry_coredump_zephyr.h`).
- **No offline storage.** An event that cannot be delivered is dropped, not buffered.
- **No WiFi.** The serial relay is the only transport.
- **The wall clock is never set.** Events go out with no timestamp and ingest uses its receive
  time. Logs and metrics need a timestamp, so they are held by design and never sent.
- **Heap numbers are the `k_malloc()` pool**, not the libc `malloc()` arena — 4 KB on this
  board unless `CONFIG_HEAP_MEM_POOL_SIZE` is raised.
- **The flash size is the board definition's, not the chip's.** `esp32s3_devkitc` declares
  8 MB; on a 16 MB module the event says `storage_size: 8388608` and the upper half goes
  unused until the board has a definition of its own.
- **Three watchdogs collapse into one.** The ESP interrupt, task and RTC watchdogs all report
  as `wdt`, and USB/JTAG/SDIO resets as `unknown`: Zephyr's hwinfo has no finer bits.
