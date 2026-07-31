# Tilt Simulator: ESP32 Hydrometer Emulator

A PlatformIO project for the ESP32 that pretends to be up to eight Tilt
hydrometers and four iSpindels at once, so brewing controllers can be tested
without tying up real hardware or waiting on a real fermentation.

The two are opposites, and the device does both at the same time. A Tilt is a
passive BLE beacon a receiver has to go looking for; an iSpindel wakes on a
timer and HTTP POSTs its reading to an endpoint you nominate. Between them they
exercise both halves of a typical setup.

Everything is controlled from a web page on the device — which colours are
advertising, their temperature and gravity, how much those readings wander,
whether each Tilt behaves as a standard or a Pro, and where each iSpindel posts.
Firmware updates go over the same page.

## Getting started

1. Build and flash over USB: `pio run -t upload`
2. On first boot the device raises a WiFi access point named
   `tiltsim-<chipid>`. Join it and pick your network in the captive portal.
   Credentials are stored on the device, not in this repo.
3. Open **http://tiltsim-chipid.local** — the serial log prints the exact
   name at 115200 baud.

Saving a network in the portal reboots the device once before the admin page
comes up. That is deliberate: the portal's own web server holds port 80 for up
to a minute or so after it closes, and the admin server cannot bind underneath
it. Connecting from the stored credentials on the next boot never opens the
portal at all.

A device with no stored network raises that portal and waits three minutes for
somebody to fill it in. Once a network is saved the portal stays out of the way,
including when the network is missing — see [Staying connected](#staying-connected).

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
- Values outside the allowed range are clamped by the device rather than
  rejected, and the page says which value it settled on instead of reporting a
  plain save.
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
is enabled.

## How advertising works

A real Tilt wakes roughly every five seconds, sends a burst, and sleeps. The
simulator matches the *reading* cadence but not the duty cycle:

- Each cycle is 5000 ms, plus 0–10 ms of `advDelay` jitter as the BLE Core
  Specification requires. Readings are drawn once at the start of a cycle, so
  every colour reports one value per five seconds however often it airs.
- Within the cycle the enabled colours rotate, each holding the radio for
  200 ms, wrapping round and repeating until the cycle ends. Four colours make
  an 800 ms rotation; all eight make 1.6 s.

The rotation is the deliberate departure. A receiver that builds its list of
devices per scan can only list a colour it actually hears inside that window,
and the HM-10 used downstream defaults to a 3 s scan, so one 5 s-spaced burst
per colour is missed more often than not — which reads as colours alternating
exclusively. Re-airing each colour every couple of hundred milliseconds puts all
of them inside any 3 s window. One radio cannot advertise
eight addresses at once (concurrent advertising sets need BLE 5.0 extended
advertising, and the classic ESP32 is 4.2 silicon), so the duty cycle is what
gets traded.

Only one colour is on air at a time, but each advertises from **its own BLE
address** — a static random address derived from the chip's MAC, stable across
restarts, with the colour index in the low bits. A scanner that identifies
devices by address therefore sees eight distinct Tilts rather than one whose
UUID keeps changing.

The device advertises **non-connectable but scannable** (`ADV_SCAN_IND`): it is
a beacon, and nothing can open a connection to it and take the radio away
mid-rotation. The flags byte is `0x1A`, so a full advertisement begins
`02 01 1A 1A FF 4C 00 02 15 …` — the canonical iBeacon layout.

The BLE stack is **NimBLE**, rather than the Bluedroid-based library bundled
with the Arduino core; it leaves an idle device about 110 KB of contiguous heap
and the image at 68% of its app slot. The 30 advertisement bytes are assembled
by hand and handed to the controller raw. NimBLE takes a BLE address least
significant byte first, so `bleAddressLittleEndian()` reverses the six bytes on
the way in, and a test pins the on-air order.

## iSpindel emulation

Four iSpindel / Gravitymon slots sit below the Tilts on the same page. Each has
a name, an endpoint to post to, and the same temperature, gravity and variance
controls as a Tilt. There is no Pro switch — that is a Tilt distinction.

Every 15 minutes each enabled slot with an endpoint POSTs a JSON body. Two
formats are available per slot, chosen with the **Standard / Extended**
toggle:

```json
{ "name": "ispindel-1", "ID": "C2CC7C", "token": "gravmon", "interval": 900,
  "temperature": 68.0, "temp_units": "F", "gravity": 1.0500,
  "angle": 45.34, "battery": 3.67, "RSSI": -12 }
```

Only `name`, `ID`, `temperature` and `gravity` mean anything. `token`, `angle`,
`battery` and `RSSI` are fixed placeholders — nothing here models a battery, a
tilt angle or a link budget, and inventing plausible movement for them would
make the simulation look more faithful than it is. They live as named constants
in `lib/ispindel_encoding/`.

**Extended mode** adds the Gravitymon field set — `corr-gravity`,
`gravity-unit` and `run-time` — and unlocks a further **SG / Plato** toggle. A
plain iSpindel has no field to say which unit `gravity` is in, so a receiver
has to guess; Gravitymon always computes SG internally and, on request,
converts to Plato for output, declaring which one it sent:

```json
{ "...": "as above", "corr-gravity": 1.0500, "gravity-unit": "G", "run-time": 6 }
```

Selecting Plato genuinely converts `gravity` and `corr-gravity` to degrees
Plato (the standard cubic approximation, `sgToPlato()` in
`lib/ispindel_encoding/`) and sets `gravity-unit` to `"P"` — it is not just a
label swap. The card's own Gravity field switches units to match, the same way
the page-wide °F/°C toggle already does for temperature, converting back to SG
before saving. `corr-gravity` always equals `gravity` here: nothing in this
simulator models the temperature-correction curve a real Gravitymon applies.

Some details worth knowing:

- **`ID` is derived from the board's MAC**, one per slot, so it is stable across
  restarts and unique to the board. Same trick as the Tilt BLE addresses, and
  from the same `efuseMacBytes()` helper, so the IDs, the hostname and the BLE
  addresses cannot drift apart.
- **Temperature always goes out in Fahrenheit**, so `temp_units` is always `"F"`.
  A real iSpindel can be configured either way, but the simulator stores °F
  throughout — that is what a Tilt advertises, and one internal unit avoids a
  second conversion path that could disagree with the first. The °C toggle
  converts for display only.
- **Slots start disabled with no endpoint.** One enabled by default would POST
  to whatever is now being served at the URL its previous owner typed.
- **Saving a slot posts straight away**, so a freshly typed URL can be checked
  without waiting a quarter of an hour. Outcomes go to the serial log — the page
  shows no delivery status.
- **`https://` works, but certificates are not validated.** Real iSpindel and
  Gravitymon firmware does the same: there is nowhere to keep a trust store and
  no clock to check validity against. A machine on the path could read or alter
  the readings. The handshake needs about 37 KB of contiguous heap while it runs.
- **Posting happens on its own FreeRTOS task.** `HTTPClient` is blocking, and a
  dead endpoint would otherwise stall the BLE rotation for as long as it took to
  time out. The task is only created once a slot has an endpoint, and its 16 KB
  stack comes from the heap rather than a static array: `.bss` and the heap are
  the same DRAM, so a static buffer would not be free.

## Staying connected

This device is expected to be headless, mains-powered and out of reach, so a
power cut must not need a human. Boot makes three connect attempts of ten
seconds each rather than the single sixty-second attempt WiFiManager does by
default, and **the web server starts whether or not any of them succeed**. That
second part is the one that matters: the bind is retried from the main loop, so
the admin page comes back on its own when the link does.

When the link is down `netLoop()` retries every 30 s. After four of those, about
two minutes, the setup portal goes up for two minutes, and then the device goes
back to retrying — the two alternate for as long as it takes. The retrying alone
would be wrong, because a network that is genuinely gone (moved house, renamed
SSID, new router) would then be unfixable without a USB cable; the portal alone
would be wrong too, because a router that is merely slow to come back does not
need one. The link returning closes the portal immediately.

That portal is **non-blocking**, driven from `netLoop()` with a window timed by
hand because `setConfigPortalTimeout` is documented as unused in that mode. A
blocking portal would stop the BLE rotation for two minutes at a time, over and
over, on a device whose entire job is to advertise. Both web servers want port
80 and AsyncTCP binds without `SO_REUSEADDR`, so the admin server is suspended
while the portal holds it and resumed afterwards.

The boot line and `/api/state` both carry the BSSID alongside the RSSI, and a
reconnect that lands on a different access point is logged. Several APs answer
for one SSID here, and a weak link is either a near AP fading or an association
with a distant one — the RSSI alone cannot tell those apart. Note that *which*
AP gets picked is not controllable from here: `WiFi.setScanMethod()` is not read
by the no-argument `WiFi.begin()` that WiFiManager connects through, so setting
it has no effect. There is nothing useful to configure.

## Firmware updates

Upload a `firmware.bin` on the **Firmware** page. Advertising stops during the
upload, the image is written to the inactive OTA slot, and the device reboots
into it. Files that are not ESP32 images are refused before anything is written.

The footer shows the running version, build date, OTA slot (`app0`/`app1`) and
image hash — the slot and hash are what actually prove an update took effect.

An installed image runs on probation until it proves itself: 60 seconds up,
and answering on port 80 if it has a network. Until then `/api/state` reports
`otaState: "pending verify"`, and any restart puts the previous firmware back.
An image that panics is rolled back by the bootloader on the next boot; one
that runs but stays unreachable rolls itself back after five minutes. So a bad
update costs a reboot rather than a USB cable and a trip to the device.

The rollback needs a working image in the other slot, which a board flashed
only over USB does not have. It stays put in that case rather than falling back
to nothing.

## Development

```sh
pio run                                  # build
pio run -t upload                        # flash over USB (COM7)
pio device monitor                       # serial log, 115200
pio test -e native                       # unit tests on the host, ~5 s
pio test -e esp32dev                     # the same tests on the board, ~2 min
pio check --fail-on-defect=medium        # static analysis
```

Everything under test lives in `lib/` and is Arduino-free, so it links into the
test runner without dragging in `main.cpp` — and the same four suites build for
a desktop compiler as well as the target:

| Suite | Covers |
|---|---|
| `test_tilt_encoding` | The advertised payload byte for byte, Pro scaling and clamping, rotation maths, the per-colour BLE addresses |
| `test_ispindel_encoding` | The posted JSON field by field, escaping of user-entered names, the per-slot device IDs |
| `test_web_support` | Reassembly of a multi-part page at every chunk size, and the OTA stall clock including wraparound |
| `test_config_schema` | The persisted `AppConfig` layout pinned to its magic, the value clamps, and URL validation |

Prefer `-e native` while working: it is about twenty times faster, needs no
board, and does not leave the bench board holding a test binary. The on-target
run stays the authority for anything whose answer can depend on the target —
above all the `AppConfig` offsets, since that struct is what sits in NVS. Both
currently agree.

Running natively needs a host `g++` on `PATH`; on Windows,
`winget install BrechtSanders.WinLibs.POSIX.UCRT` provides one. Without it, use
`-e esp32dev` and let CI cover the host build — `.github/workflows/ci.yml` runs
the build, the native suites and the static analysis on every push, since the
runner has no board to flash.

The payload is assembled in `buildIBeaconPayload()` rather than with the
framework's `BLEBeacon` class, deliberately. `BLEBeacon` byte-swaps every 16-bit
setter, so `setManufacturerId(0x004C)` advertises company ID `0x4C00` — which is
not Apple. Building the bytes in a pure function instead keeps the on-air order
pinned by `test_payload_matches_reference_byte_for_byte`.

Two things in `platformio.ini` are worth knowing:

- `board_build.partitions = min_spiffs.csv`. Both OTA slots are 1.875 MB and the
  image uses 68% of one — the iSpindel side costs about 154 KB of that, almost
  all of it mbedTLS, because `HTTPClient.h` includes `WiFiClientSecure.h`
  unconditionally and links the whole TLS stack whether or not it is used. That
  is also why `https://` is supported rather than refused: the flash is already
  spent. Changing the partition table cannot be done over OTA, so it stays
  regardless.
- The three `CONFIG_BT_NIMBLE_ROLE_*=0` flags. This is a broadcaster and
  nothing else, so the client, scan and server roles are not compiled in. Note
  the names: the library's docs give a `MYNEWT_VAL_BLE_ROLE_*` spelling, which
  on the ESP port is derived from these and therefore loses to them silently.

Keep this checkout out of OneDrive, and out of any sync client that uses
on-demand placeholder files. Such a client dehydrates freshly-extracted package
files; SCons then reads a null mtime and the build dies with
`unsupported operand type(s) for -: 'float' and 'NoneType'`, most visibly on the
Unity package that `pio test` installs.

### Layout

| Path | What it does |
|---|---|
| `src/main.cpp` | Setup, and the advertising scheduler |
| `lib/tilt_encoding/` | Pure payload logic — UUIDs, scaling, rotation timing |
| `lib/ispindel_encoding/` | Pure iSpindel logic — the posted JSON and the slot IDs |
| `lib/config_schema/` | The persisted layout, its magic, the value clamps and URL validation |
| `lib/web_support/` | Multi-part page assembly and the OTA stall clock |
| `src/tilt_config.cpp` | Runtime settings and NVS persistence |
| `src/ispindel.cpp` | The posting task |
| `src/net.cpp` | WiFi provisioning, hostname, mDNS |
| `src/web_server.cpp` | HTTP API and OTA handler |
| `src/ota_rollback.cpp` | Confirms a new image, or puts the old one back |
| `src/web_assets.cpp` | The web UI, embedded in the firmware |

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
- Never hand a whole embedded asset to `beginResponse(code, type, body)`. That
  overload copies the body into a `String`, so a 9 KB page needs a 9 KB
  contiguous allocation — and `/api/state` reports the largest free block a
  running device can actually offer. Serve assets through `sendProgmem()`, which
  streams them from flash a TCP buffer at a time. Getting this wrong does not
  raise an error: the large pages hang with no response while the small
  endpoints keep working, which reads as a crashing server.

## HTTP API

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/state` | Full config plus hostname, IP, BSSID and RSSI, version, OTA slot and probation state, image hash, free heap and largest free block |
| POST | `/api/master` | `{"enabled": true\|false}` |
| POST | `/api/tilt/<0-7>` | Partial patch of one tilt's settings |
| POST | `/api/ispindel/<0-3>` | Partial patch of one iSpindel slot; a URL must start `http://` or `https://` or it is refused with 400 |
| POST | `/api/reset-wifi` | Forget credentials and reboot into the portal |
| POST | `/api/reboot` | Restart the device, keeping its settings |
| POST | `/update` | Multipart firmware upload |

Patches are partial — send only the fields you are changing. Values are clamped
server-side (temperature −40 to 250 °F, gravity 0.900 to 2.000), so the browser
is never trusted.

### No authentication, by design

None of these endpoints is authenticated. Anyone who can reach the device on the
network can change its readings, reboot it, erase its WiFi credentials, or
install firmware over `/update`.

This is a deliberate choice, not an oversight: the simulator is a bench and
brewing-network tool, and it is assumed to live on a trusted isolated network
alongside the receiver it exists to feed. **Do not expose it to an untrusted
network or forward a port to it.** If that assumption ever stops holding,
ESPAsyncWebServer ships `AsyncAuthenticationMiddleware` and the write endpoints
above are the ones to put behind it.

`tempF` and `tempVarianceF` are always Fahrenheit, whatever the web UI happens
to be displaying — the °C option is a browser-side convenience and never
reaches the device. There is no Celsius form of these fields.

```sh
curl -X POST http://tiltsim-a1b2c3.local/api/tilt/0 \
  -H 'Content-Type: application/json' \
  -d '{"enabled":true,"pro":true,"tempF":67.4,"gravity":1.0488}'

curl -X POST http://tiltsim-a1b2c3.local/api/ispindel/0 \
  -H 'Content-Type: application/json' \
  -d '{"enabled":true,"url":"http://192.168.0.20:8080/ispindel"}'
```

Saving an iSpindel slot posts a reading immediately rather than waiting for the
next interval, so the endpoint can be checked straight away.

## Tilt colour UUIDs

`a495bbX0-c5b1-4b44-b512-1370f02d74de`, where `X` is 1–8 for Red, Green, Black,
Purple, Orange, Blue, Yellow and Pink in that order.

## Requirements

- An ESP32 board (developed against `esp32dev`)
- Libraries resolve automatically: ESPAsyncWebServer, AsyncTCP, ArduinoJson,
  WiFiManager and NimBLE-Arduino
