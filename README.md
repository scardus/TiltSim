# iBeacon-Test: ESP32 iBeacon Multi-Advertiser

This PlatformIO project for ESP32 advertises multiple iBeacon frames in sequence, with per-beacon configuration and runtime randomization.  This project has been used to test Tilt Hydrometer iBeacons for a separate project.

## Features
- Advertises a list of iBeacons, each with its own UUID, major (°F), minor (SG), and active flag
- Only beacons marked as active are advertised
- Each run, a random integer variance (±2°F) is added to the base major value for all beacons
- Major value is always an integer in the range 0–99 (°F)
- Each beacon is advertised for a short window, then the next active beacon is advertised
- All configuration is in `src/main.cpp`

## Configuration
Edit the `kBeacons` array in `src/main.cpp` to set:
- `uuid`: iBeacon UUID (canonical format)
- `baseMajorDegF`: base major value (integer, °F)
- `minor`: iBeacon minor value
- `active`: set to `true` to enable, `false` to disable

The array currently contains all the UUID's for the Tilt Hydrometers.

Example:
```cpp
constexpr BeaconConfig kBeacons[] = {
  {"a495bb10-c5b1-4b44-b512-1370f02d74de", 64, 1051, true},   // Red
  {"a495bb20-c5b1-4b44-b512-1370f02d74de", 66, 1052, true},   // Green
  // ...
};
```

## How it works
- On each run (every 5 seconds by default), a random integer variance from -2 to +2 is chosen
- Each active beacon is advertised in turn, with its major set to `baseMajorDegF + variance` (clamped 0–99)
- Each beacon is advertised for 1 second (default)
- After all active beacons are advertised, the process repeats

## Requirements
- ESP32 board (PlatformIO, Arduino framework)
- No external libraries required beyond ESP32 BLE

## Usage
1. Edit `src/main.cpp` to configure your beacons
2. Build and upload with PlatformIO
3. Use a BLE scanner app (e.g., nRF Connect) to observe the advertised beacons and their major/minor values