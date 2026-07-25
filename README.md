# Tilt Simulator: ESP32 Tilt Hydrometer Emulator

A PlatformIO project for the ESP32 that pretends to be up to eight Tilt
hydrometers at once, so brewing controllers can be tested without tying up real
hardware or waiting on a real fermentation.

Everything is controlled from a web page on the device — which colours are
advertising, their temperature and gravity, how much those readings wander, and
whether each one behaves as a standard Tilt or a Tilt Pro. Firmware updates go
over the same page.

## Getting started

1. Build and flash over USB: `pio run -t upload`
2. On first boot the device raises a WiFi access point named
   `tiltsim-<chipid>`. Join it and pick your network in the captive portal.
   Credentials are stored on the device, not in this repo.
3. Open **http://tiltsim-chipid.local** — the serial log prints the exact
   name at 115200 baud.

If the saved network is unreachable the portal reappears for three minutes,
then the device carries on advertising offline.

## Web interface

- **Power** — stops and starts all advertising at once.
- **Units** — shows temperatures in °F or °C. This changes the display only:
  the device stores and advertises °F, because that is what Tilt hydrometers
  do. Variance converts as a range rather than a temperature, so ±2 °F reads as
  ±1.1 °C. The choice is remembered per browser rather than on the device, so
  two people looking at the same simulator can each use their own scale.
- One card per Tilt colour, accented in that colour, each with its own enable
  toggle.
- Temperature and gravity, plus a **variance** range for each. Variance is the
  maximum the reading will wander either side of the value you set; leave it at
  zero to advertise a rock-steady figure.
- A **Pro** toggle per tilt.
- Each card shows the resulting `major`/`minor` so what is going on the wire is
  never a mystery.
- **Firmware** page for over-the-air updates.

Settings are saved to flash and survive a reboot.

## Standard and Pro tilts

Tilt Pros report one more decimal place, which they do by advertising ten times
the standard value. You always enter real-world values and the firmware scales
them:

| Mode | You enter | On air |
|---|---|---|
| Standard | 68 °F, 1.053 | major 68, minor 1053 |
| Pro | 68.5 °F, 1.0530 | major 685, minor 10530 |

With **Units** set to °C you enter Celsius instead and the page converts before
sending — 20 °C is the same 68 °F to the device, and the card's on-air caption
still shows the raw `major`/`minor` going over the wire.

Variance is also entered in real units, so ±2 °F means ±2 °F whether or not Pro
is enabled. (Before v0.2.0 the Pro emulation was a hand-scaled row in the source
and the variance was applied *after* scaling, so a Pro only wandered ±0.2 °F.)

## How advertising works

A real Tilt wakes roughly every five seconds, sends a burst, and sleeps. The
simulator matches that:

- Each cycle is 5000 ms, plus 0–10 ms of `advDelay` jitter as the BLE Core
  Specification requires.
- Every enabled colour gets one slot per cycle, `min(1000, 5000 / enabled)` ms
  long. Three colours use the full 1 s burst and leave 2 s quiet; all eight pack
  the cycle at 625 ms each.
- So each colour is seen once per five seconds no matter how many are enabled.

Only one colour is on air at a time — the ESP32 has a single advertising
instance, and all simulated tilts share the one BLE MAC address. A scanner that
identifies devices by MAC will see one device whose payload changes rather than
eight separate ones.

## Firmware updates

Upload a `firmware.bin` on the **Firmware** page. Advertising stops during the
upload, the image is written to the inactive OTA slot, and the device reboots
into it. Files that are not ESP32 images are refused before anything is written.

The footer shows the running version, build date, OTA slot (`app0`/`app1`) and
image hash — the slot and hash are what actually prove an update took effect.

## Development

```sh
pio run                                  # build
pio run -t upload                        # flash over USB (COM7)
pio device monitor                       # serial log, 115200
pio test -e esp32dev                     # unit tests, run on the board
pio check --fail-on-defect=high          # static analysis
```

Unit tests cover `lib/tilt_encoding` — the UUID byte-reversal, the Pro scaling
and clamping, and the slot maths. They run on the target because there is no
host compiler assumed; the module is Arduino-free so it links into the test
runner without dragging in `main.cpp`.

Two things in `platformio.ini` are worth knowing:

- `board_build.partitions = min_spiffs.csv`. BLE alone filled 85% of the
  default 1.25 MB app slot; with WiFi and the web server it does not fit, and
  OTA needs two slots.
- `libdeps_dir` points outside the project. This tree lives under OneDrive,
  which dehydrates freshly-extracted package files; SCons then reads a null
  mtime and the build fails with `unsupported operand type(s) for -: 'float'
  and 'NoneType'`.

### Layout

| Path | What it does |
|---|---|
| `src/main.cpp` | Setup, and the advertising scheduler |
| `lib/tilt_encoding/` | Pure payload logic — UUIDs, scaling, slot timing |
| `src/tilt_config.cpp` | Runtime settings and NVS persistence |
| `src/net.cpp` | WiFi provisioning, hostname, mDNS |
| `src/web_server.cpp` | HTTP API and OTA handler |
| `include/web_assets.h` | The web UI, embedded in the firmware |

The UI is compiled into the binary rather than served from a filesystem, so
there is a single `.bin` to flash and an update cannot leave the firmware and
the interface out of step.

### A note on the web handlers

AsyncWebServer callbacks run on the AsyncTCP task, not `loop()`. Two rules
follow, and breaking either one is a real fault rather than a style point:

- Shared config is behind a mutex, and NVS writes are deferred to `loop()`.
- Never call `delay()` or `ESP.restart()` from a handler. That task is
  watchdogged; doing so panics the device before the response can flush.
  Handlers set a flag and `loop()` acts on it.

## HTTP API

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/state` | Full config plus hostname, IP, version, OTA slot, image hash |
| POST | `/api/master` | `{"enabled": true\|false}` |
| POST | `/api/tilt/<0-7>` | Partial patch of one tilt's settings |
| POST | `/api/reset-wifi` | Forget credentials and reboot into the portal |
| POST | `/update` | Multipart firmware upload |

Patches are partial — send only the fields you are changing. Values are clamped
server-side (temperature −40 to 250 °F, gravity 0.900 to 2.000), so the browser
is never trusted.

`tempF` and `tempVarianceF` are always Fahrenheit, whatever the web UI happens
to be displaying — the °C option is a browser-side convenience and never
reaches the device. There is no Celsius form of these fields.

```sh
curl -X POST http://tiltsim-a1b2c3.local/api/tilt/0 \
  -H 'Content-Type: application/json' \
  -d '{"enabled":true,"pro":true,"tempF":67.4,"gravity":1.0488}'
```

## Tilt colour UUIDs

`a495bbX0-c5b1-4b44-b512-1370f02d74de`, where `X` is 1–8 for Red, Green, Black,
Purple, Orange, Blue, Yellow and Pink in that order.

## Requirements

- An ESP32 board (developed against `esp32dev`)
- Libraries resolve automatically: ESPAsyncWebServer, AsyncTCP, ArduinoJson,
  WiFiManager, and the ESP32 BLE library bundled with the Arduino core
