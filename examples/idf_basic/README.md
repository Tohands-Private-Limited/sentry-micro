# idf_basic

The ESP-IDF counterpart of [`zephyr_basic`](../zephyr_basic/README.md), in plain C: initialise
sentry-micro, print what it learned about this boot, join WiFi, set the clock over SNTP, and
report either the crash the previous boot died of, with its backtrace, or else the boot as one
event, posted straight to ingest over HTTPS with the certificate checked.

Built with ESP-IDF v6.1 for the ESP32-S3 and run on an ESP32-S3 WROOM-1 N16R8 module that has
only the chip's native USB (see [Console](#console)). The boot event and a recovered crash
both reached Sentry.

## Build

Needs ESP-IDF v6.1 with the ESP32-S3 toolchain (`install.sh esp32s3`), set up in the shell with
`. $IDF_PATH/export.sh`.

The DSN, release and WiFi credentials come from the environment when CMake configures, like
the other examples. They become compile definitions of `main` only, and never go into
`sdkconfig` or any other file:

```bash
export SENTRY_MICRO_DSN='https://<key>@o<org>.ingest.sentry.io/<project>'
export SENTRY_MICRO_RELEASE='my-firmware@1.0.0'   # optional
export SENTRY_MICRO_WIFI_SSID='my-network'
export SENTRY_MICRO_WIFI_PASSWORD='my-password'   # unset for an open network

cd examples/idf_basic
idf.py set-target esp32s3
idf.py build
```

Without the DSN or the SSID the build still succeeds, with a CMake warning, and the firmware
reaches nobody. The values are read when CMake configures, so after changing one run
`idf.py reconfigure` before building. `~/.config/sentry-micro/env`
([ONBOARDING](../../ONBOARDING.md#zero-to-a-first-event)) is a convenient place to keep them:
`set -a; . ~/.config/sentry-micro/env; set +a` before `idf.py`.

`idf.py -DSENTRY_DEMO_CRASH=1 build` adds a deliberate crash once the boot event is out: two
`sentry_log()` lines, then a null-pointer store three calls deep. ESP-IDF's panic handler
writes the core dump to flash and restarts; the next boot reports it with its backtrace, reads
`reset reason: panic`, and does not crash again, so one power-on gives one crash-and-report
cycle. `-DSENTRY_DEMO_CRASH=0` with a reconfigure turns it off again.

**How sentry-micro gets in.** `main/idf_component.yml` names this repository's root as a
dependency by path (`path: ../../..`). The root is an ESP-IDF component (`idf_component.yml`,
`CMakeLists.txt`, `Kconfig`), and the component manager adds it under the name in that
manifest, `sentry-micro`, whatever the checkout's directory is called. `EXTRA_COMPONENT_DIRS`
would also work, but it names the component after the directory, so a clone called anything
else would break `PRIV_REQUIRES sentry-micro`. The manager writes `dependencies.lock` beside
the project; it is generated, and ignored.

**Configuration** is in `sdkconfig.defaults`: the USB-Serial-JTAG console, 16 MB of flash,
`partitions.csv` (`nvs`, `phy_init`, a 3 MB `factory` app and a 128 KB `coredump`
partition), core dumps to flash, and a 12 KB main task. sentry-micro's own options are under
`Component config → sentry-micro` in `idf.py menuconfig`; they use the Zephyr module's names
(`CONFIG_SENTRY_MICRO_SESSIONS`, `CONFIG_SENTRY_MICRO_WIFI_TLS_VERIFY`,
`CONFIG_SENTRY_MICRO_WIFI_TIMEOUT_MS`).

## Flash

```bash
idf.py -p /dev/ttyACM0 flash
```

### Console

The console is on the S3's native USB-Serial-JTAG (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`),
because the board this was run on has no USB-UART bridge. With ESP-IDF's default UART0
console it flashes fine and then prints nothing. On a devkit with a bridge, either port works
for flashing; the console stays on the native one unless you change that line.

The chip drops off the USB bus on every reset and comes back about a second later, so a serial
terminal has to reopen the port when it reappears. The run below was read with a few lines of
pyserial that do exactly that; `idf.py -p /dev/ttyACM0 monitor` is the usual tool but was not
used here. `main.c` holds the boot report for 3 s so the host has the port open again before
it is printed; ESP-IDF's own boot log from before that is lost.

## What you should see

```
sentry-micro idf_basic example
[sentry] previous session ended exited (reset reason usb)
[sentry] initialised sentry.micro.esp32/0.1.0, ingest https://o<org>.ingest.de.sentry.io/api/<project>/envelope/
[sentry] device ESP32-S3 dcb4d92a7d94, reset reason: usb

── sentry-micro ──────────────────────────────
sdk           : sentry.micro.esp32 0.1.0
enabled       : yes
release       : idf_basic@0.1.0
...
esp-idf       : v6.1
reset reason  : usb
──────────────────────────────────────────────

[wifi] connecting to "my-network" ...
[wifi] connected: ip=192.168.1.42 in 1630ms
[time] synced from pool.ntp.org: 2026-11-05 10:34:16 UTC
[sentry] transport set to wifi
[sentry] transport: wifi, HTTPS straight to ingest (certificate check on)
[sentry] session exited (init) via wifi: result 0, http 200
[sentry] session ok (init) via wifi: result 0, http 200
[sentry] capturing e4bf531990d04080983767588c768582 as info: "Device booted: usb"
[sentry] sent 764 bytes via wifi: result 0, http 200
[sentry] delivered (http 200)
[mem] main task stack never used: 4976 bytes
```

That is a real run, with the address changed. The two session lines are release health: the previous boot's close and
this boot's start, sent as soon as the clock is set. The event appears in Sentry as
`Device booted: usb` with `os.name` `esp-idf` and the device's own timestamp. `usb` is the
reset `esptool` performs through the USB-Serial-JTAG after flashing. Other resets map as on
the other ports (`poweron`, `panic`, the watchdogs), and the boot after a crash reports the
crash instead of the boot.

The last line is the main task's headroom after the TLS handshake and the report, the figure
to size `CONFIG_ESP_MAIN_TASK_STACK_SIZE` by. About 7 KB of the 12 KB were used.

### After a crash

With `-DSENTRY_DEMO_CRASH=1` the first boot reports itself as above, logs two lines and dies
in `demo_crash_innermost()`. ESP-IDF prints the register dump and saves the core dump
(`Core dump has been saved to flash.`, about 14 KB) before it restarts; on this board the
console stayed up through that. The next boot, also a real run:

```
[sentry] recovered 2 log lines from the previous boot
[sentry] holding them for the crash report, as breadcrumbs
[sentry] previous session ended crashed (reset reason panic)
...
reset reason  : panic  <- previous boot crashed
...
[sentry] session crashed via wifi: result 0, http 200
[sentry] session ok (init) via wifi: result 0, http 200
[sentry] crash recovered: StoreProhibited in main, 6 frames, 2 breadcrumbs

── crash recovered from the previous boot ────
exception     : StoreProhibited
task          : main
pc            : 0x420109ba
accessing     : 0x00000000
backtrace     : 6 frames
  #0 0x420109ba
  #1 0x420109c7
  #2 0x420109cf
  #3 0x420110e9
  #4 0x42008944
  #5 0x420a7bb2
app elf sha256: f144440f3
──────────────────────────────────────────────
[sentry] sent 1502 bytes via wifi: result 0, http 200
[sentry] crash report: delivered (http 200)
[sentry] core dump erased
[sentry] crash event 3be6d0496fcc459ba7fbaf8e4a3d2226
[mem] main task stack never used: 4992 bytes
[sentry] crash reported; its 2 log lines went with it as breadcrumbs
```

The six frames are the three demo functions, `app_main`, and FreeRTOS's `main_task` and
`vPortTaskWrapper` (`addr2line` on the build's ELF), the backtrace the panic handler printed.
The event is fatal, titled `StoreProhibited` with `accessing 0x00000000`, and is sent instead
of a `Device booted: panic` one. The dump is erased only once the send has succeeded; after a
failed send `report_crash()` keeps it for the next boot (that branch has not been run). The
boot after this one reported itself and no crash. `app elf sha256` is the prefix ESP-IDF keeps of the
crashed firmware's ELF hash (`CONFIG_APP_RETRIEVE_LEN_ELF_SHA`, 9 characters by default); it
goes out as `contexts.esp32.app_elf_sha256`. The frames are addresses until the ELF is
uploaded to Sentry, which this example does not do yet.

The crash path used no more stack than the boot path: 4,992 bytes of the 12 KB were left.

**What TLS checks.** The server's certificate must chain to DigiCert Global Root G2, the root
Sentry's ingest certificate is issued under, and must name the ingest host. That root is the
same file the Zephyr transport trusts (`src/transport/sentry_ca_digicert_global_root_g2.c`)
and expires on 2038-01-15; pass your own PEM to `sentry_wifi_transport_init()` to replace it,
or set `CONFIG_SENTRY_MICRO_WIFI_TLS_CRT_BUNDLE=y` to trust ESP-IDF's certificate bundle
instead, at a cost of tens of KB of flash. Certificate dates are not checked
(`CONFIG_MBEDTLS_HAVE_TIME_DATE` is off by default), so verification works before SNTP has set
the clock. `CONFIG_SENTRY_MICRO_WIFI_TLS_VERIFY=n` turns the check off; ESP-TLS then also needs
`CONFIG_ESP_TLS_INSECURE` and `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY`, and the build stops
with a message until they are set. A wrong root is refused: with a self-signed CA passed to
`sentry_wifi_transport_init()` the handshake fails (`mbedtls_ssl_handshake returned -0x2700`),
every send reports "no route", and the firmware carries on.

**Image size.** The whole application is 943,017 bytes. Of that, sentry-micro is 21,763
(`idf.py size-components`): 17,457 of code and 1,361 of constants in flash, 1,741 of `.bss`,
and 1,204 of RTC memory for the log ring and session record that survive a reset.

## Known gaps in this milestone

- **Not checked yet on this port:** the `crashed` session in Sentry's release health (the
  SDK's log above says it went out, HTTP 200), logs, metrics, symbolication of crash frames, and
  offline buffering (`platform/esp-idf/sentry_storage_nvs.c` is built but this example does not
  register it). The crash event in Sentry does carry the two lines as breadcrumbs, three
  seconds apart.
- **No offline storage in the example.** An event that cannot be delivered is dropped; a
  recovered core dump is kept in flash and tried again on the next boot.
- **The 12 KB main task** has about 5 KB to spare on both the boot path and the crash path.
