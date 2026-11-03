# zephyr_basic

The Zephyr counterpart of `wifi_basic`'s boot and crash paths: initialise sentry-micro, print
what it learned about this boot, and report either the crash the previous boot died of, with
its backtrace, or else the boot as one event. It is delivered through `scripts/serial_relay.py`
on the laptop the board is plugged into, or, in the [WiFi build](#wifi), straight to ingest
over HTTPS.

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

## WiFi

The same sample posts straight to Sentry over the ESP32-S3's own WiFi when it is built with a
network name and the WiFi configuration fragment:

```bash
export SENTRY_MICRO_WIFI_SSID='my-network'
export SENTRY_MICRO_WIFI_PASSWORD='my-password'   # unset for an open network

west build -p always -b esp32s3_devkitc/esp32s3/procpu -d build-wifi . -- \
    -DEXTRA_CONF_FILE=wifi.conf
```

Add `-DEXTRA_DTC_OVERLAY_FILE=usb_serial.overlay` after the `--` for a board with only the
native USB port, as above. Then flash it and watch the console with any serial terminal; no
relay is involved:

```bash
west flash -d build-wifi --esp-device /dev/ttyUSB0
```

On boot it joins the network (up to 30 s, until DHCP has given it an address; the first try
right after a flash is often refused and the driver's own reconnect then gets through a few
seconds later), sets the clock over SNTP from `pool.ntp.org` (up to three queries), and
reports through
`transport/zephyr/sentry_transport_wifi.h`. The console prints the SSID, the address, the
time it set, and `transport: wifi`.

Straight after that it flushes, which sends this boot's session start for release health and,
after a reboot, the previous boot's close: `[sentry] session ok (init) via wifi: result 0,
http 200`, and on the boot after a demo crash `[sentry] session crashed via wifi: ...` first.
That boot's crash report then carries the demo's two log lines as breadcrumbs.

**Two switches, kept apart on purpose.** The SSID in the environment selects the WiFi
transport in `main.c`; `wifi.conf` supplies the network stack it needs. The stack lives in a
fragment rather than in `prj.conf` because it is large and the serial build has no use for
it: the WiFi driver reserves a 51 KB heap, mbedTLS 60 KB, and the main stack grows to 16 KB
for the TLS handshake. The serial-relay build stays exactly as it was. Setting the SSID
without the fragment stops the build with a message saying so; the fragment without the SSID
builds with a warning and reports over the serial relay. The credentials are read when CMake
configures, like the DSN: change them, rebuild with `-p always`.

**What TLS checks.** The server's certificate must chain to DigiCert Global Root G2, which
Sentry's ingest certificate is issued under, and must name the ingest host. That root is
bundled with sentry-micro (`src/transport/zephyr/sentry_ca_digicert_global_root_g2.c`) and
expires on 2038-01-15; a fleet that has to outlive it, or Sentry changing certificate
authority before then, needs a root of its own passed to `sentry_wifi_transport_init()`.
Certificate *dates* are not checked: mbedTLS checks them only with
`CONFIG_MBEDTLS_HAVE_TIME_DATE`, which is off, and that is why verification works before SNTP
has set the clock. `CONFIG_SENTRY_MICRO_WIFI_TLS_VERIFY=n` turns the check off; the
connection is then encrypted but anyone on the path can pose as ingest and read the DSN key,
and the transport warns once on its first send. The check has been exercised both ways on a
board: the bundled root verifies ingest, and a self-signed root in its place makes the
handshake fail with `ECONNABORTED`, reported as "no route".

Two mbedTLS options in `wifi.conf` are easy to lose and fatal to miss. Server name indication
(`CONFIG_MBEDTLS_SSL_SERVER_NAME_INDICATION`): ingest closes a TLS 1.2 handshake that does not
name the host, before any certificate is sent. The PEM parser
(`CONFIG_MBEDTLS_PEM_PARSE_C`): the bundled root is PEM, and without the parser every handshake
fails on an empty trust store; the module refuses to build that combination.

**SNTP.** The clock is what gives events a timestamp, and what logs and metrics wait for
before they are sent at all. A failed sync is printed and not fatal: events then go out with
ingest's receive time.

**One core.** The ESP32 WiFi driver requires a single-core kernel (`depends on !SMP`), which
the `procpu` target is: Zephyr runs on the first core only, and the second is unused.

**Image size.** The WiFi build links at 721,876 B of flash and 258,704 B of static DRAM on
the ESP32-S3, against 141,620 B and 50,752 B for the serial build: the WiFi driver, the IP
stack and mbedTLS are most of the difference. It fits the 1344 KB application slot of the
board's partition layout with room to spare.

**One warning is expected.** Every WiFi build prints a CMake warning from mbedTLS that
`CONFIG_MBEDTLS_HAVE_TIME_DATE` is off and certificate dates will not be checked. That is the
trade described above, made on purpose so verification works before the clock is set.

## Symbolicated crashes

`scripts/zephyr_release.py` is the Zephyr counterpart of `scripts/release.sh`: it builds the
application with its build-id, load address and image size compiled in, rebuilding until
the measured address and size stop moving, then writes an upload copy of the ELF,
`<build-dir>/zephyr/zephyr.sentry.elf`, with that build-id stamped in. It reads the copy
back with `sentry-cli debug-files check` and uploads it to Sentry, which then resolves
the crash frames to functions, files and lines. From the repository root, with the
environment from [Build](#build) and the secrets exported:

```bash
.venv/bin/python scripts/zephyr_release.py -b esp32s3_devkitc/esp32s3/procpu \
    -d build-wifi examples/zephyr_basic -- \
    -DEXTRA_DTC_OVERLAY_FILE=usb_serial.overlay -DEXTRA_CONF_FILE=wifi.conf
west flash -d build-wifi --esp-device /dev/ttyACM0
```

`-d` is relative to the current directory, as with `west build`, so this `build-wifi` is at
the repository root, not the sample's own; flash from the directory the release ran in.
Everything after `--` goes to CMake, as with `west build`; add `-DSENTRY_DEMO_CRASH=1` there
for a crash to look at. The release defaults to `$SENTRY_MICRO_RELEASE`, else
`<app-dir>@<git describe>` (`-r` to set it); the build-id is the SHA-1 of
`<release>/<board>`, so each board gets its own. Uploading needs an organization auth token
(`SENTRY_AUTH_TOKEN` or `sentry-cli login`). The project comes from the DSN, the org from the
token (or `SENTRY_ORG`). `--no-upload` stops after the check and needs no credentials at all.
`--help` lists the rest, which mirror `release.sh`: `--no-commits`, `--no-sources`,
`--no-wait`, `--json-summary`.

Every pass is a pristine build (`-p always`), because the module reads these values from the
environment when CMake configures and an incremental build would keep the previous pass's
numbers. The WiFi crash build settles in three passes.

**Flashing afterwards is correct.** `west flash` rebuilds before it flashes, but with the
sources unchanged that rebuild has nothing to do (`ninja: no work to do`, even from a shell
without the release variables), so the board gets the image that was measured. That is
also why the stamp goes on a copy: the device image is `zephyr.bin`, which already carries
the build-id and geometry, and `zephyr.elf` is left exactly as Zephyr linked it. Change a
source after the release and the flash would be a different build: run the release again.

**Why the copy's program headers are sorted.** Sentry takes an ELF's load address from its
*first* PT_LOAD header in table order. Zephyr's ESP32-S3 ELF lists 36 bytes of RTC memory at
0x50000000 first, above all of the code, so uploaded as it is nothing would resolve.
`scripts/elf_sort_loads.py` sorts the copy's PT_LOAD entries by address, which makes the first
one 0x00012820, the address the firmware reports.

**As a west command.** West finds extension commands only in projects its manifest lists,
so a workspace that wants `west sentry-release` lists this repository, for example in a
`submanifests/sentry-micro.yml` imported by the workspace's `west.yml`:

```yaml
manifest:
  projects:
    - name: sentry-micro
      url: https://github.com/getsentry/sentry-micro
      path: modules/lib/sentry-micro
      west-commands: zephyr/west-commands.yml
```

Then `west sentry-release` takes the same arguments as the script. The project's `path`
has to be inside the workspace; west rejects one that is not.

What has been checked, for the WiFi crash build: the image settles at `0x12820 +0x4205f850`
on the third pass; `zephyr.bin` contains the build-id, release and image name as strings;
`readelf -n` shows the build-id note on the copy and `readelf -lW` its PT_LOADs in address
order, with `zephyr.elf` untouched. `sentry-cli debug-files check` reads the copy as usable
with the expected debug id. getsentry's `symbolic` library, which Sentry symbolicates with,
resolves the three demo functions, `main` and `z_thread_entry` from the copy to the right
function and line, and none of them from the unsorted `zephyr.elf`. The upload was
accepted and processed (`--wait`). And the board agrees: a crash from the flashed image
renders in Sentry with every frame named, `demo_crash_innermost` at `main.c:375` through
`main` to `z_thread_entry`, each with its source line.

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

- **sentry-micro owns `k_sys_fatal_error_handler()`.** An application with its own handler
  sets `CONFIG_SENTRY_MICRO_FATAL_HANDLER=n` and calls `sentry_zephyr_record_fatal()` from it
  (`src/platform/zephyr/sentry_coredump_zephyr.h`).
- **No offline storage.** An event that cannot be delivered is dropped, not buffered. Over
  WiFi that includes the boot event of a board whose network was not up in time; a recovered
  crash is kept in RTC memory and tried again on the next boot.
- **The WiFi build's memory sizes are generous, not measured.** 60 KB of mbedTLS heap and a
  16 KB main stack carried the handshake on the first run; what they could be trimmed to has
  not been measured.
- **The serial build never sets the wall clock.** Its events go out with no timestamp and
  ingest uses its receive time. Logs, metrics and sessions need a timestamp, so there they are
  held by design and never sent, and a crash's breadcrumbs go out undated. The WiFi build sets
  the clock over SNTP and flushes straight after, so its session start goes out then.
- **Breadcrumbs need lines that were not sent.** Only lines still in the log ring at the crash
  become breadcrumbs, so a line flushed before it is a log, not a breadcrumb. With
  `-DSENTRY_DEMO_CRASH=1` the demo writes two lines and crashes before the next flush.
- **Heap numbers are the `k_malloc()` pool**, not the libc `malloc()` arena — 4 KB on this
  board unless `CONFIG_HEAP_MEM_POOL_SIZE` is raised.
- **The flash size is the board definition's, not the chip's.** `esp32s3_devkitc` declares
  8 MB; on a 16 MB module the event says `storage_size: 8388608` and the upper half goes
  unused until the board has a definition of its own.
- **Three watchdogs collapse into one.** The ESP interrupt, task and RTC watchdogs all report
  as `wdt`, and USB/JTAG/SDIO resets as `unknown`: Zephyr's hwinfo has no finer bits.
