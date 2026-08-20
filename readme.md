# HeeremaWLED

Reproducible custom WLED firmware for a QuinLED An-Penta-Deca with three
segment-aware wall buttons and WLED's AudioReactive usermod.

The build is pinned to **WLED 16.0.1**. It produces a standard OTA-uploadable
`.bin` for the classic ESP32/4 MB An-Penta-Deca configuration.

## Button mapping

| Physical input | WLED button | GPIO | Default segment |
| --- | ---: | ---: | ---: |
| Button 2 | 1 | 39 | 0 |
| Button 3 | 2 | 34 | 1 |
| Button 4 | 3 | 33 | 2 |

Button 1 (WLED index 0, GPIO 36) is deliberately not intercepted.

Each handled button supports:

- Single press: fade its assigned segment on or off using WLED's transition
  duration.
- Double press: turn the segment on at full value and select its configured
  white color temperature.
- Press, release, then press and hold: start a slower Rainbow color show on
  the segment, if enabled.
- Hold while off: ramp the segment brighter.
- Hold while on: ramp the segment dimmer. Reaching the minimum turns the
  segment logically off and resets its next-on brightness to full.

The segment ID, enable switch, target Kelvin value, and warm/cool endpoint
ratings are configurable separately for each button under **Config →
Usermods**. Global gesture timing and the color-show enable switch are grouped
separately.

## Included WLED usermods

- `wled-usermod-heerema-smart-buttons` from this repository
- `multi_relay`
- `audioreactive`

AudioReactive starts disabled with no microphone pins assigned. WLED's generic
ESP32 defaults conflict with An-Penta-Deca GPIO functions, so configure a known
compatible microphone pinout or network audio sync before enabling it.

## Build locally

Requirements:

- Git
- Python 3
- Node.js 20
- A C/C++ build toolchain supported by PlatformIO

Run:

```bash
./scripts/build.sh
```

The script clones WLED `v16.0.1` into `.build/WLED`, installs PlatformIO 6.1.19
in an isolated environment if necessary, builds the firmware, and writes the
`.bin` and SHA-256 checksum to `dist/`.

To use an existing WLED checkout:

```bash
WLED_DIR=/absolute/path/to/WLED ./scripts/build.sh
```

That checkout must be exactly at tag `v16.0.1`. The script writes its generated
`platformio_override.ini`, so do not point it at a checkout containing an
override file you need to preserve.

## GitHub builds and releases

Every push to `main` and every pull request builds the firmware and uploads it
as a GitHub Actions artifact. Pushing a tag beginning with `v` also creates a
GitHub Release containing the `.bin` and checksum:

```bash
git tag v2.3.0
git push origin v2.3.0
```

## Install only the usermod in another WLED build

Add the tagged repository to that build's `custom_usermods`:

```ini
custom_usermods =
  https://github.com/mgrainger87/HeeremaWLED.git#v2.3.0
```

The complete An-Penta-Deca firmware additionally requires the build flags in
[`build/platformio_override.ini`](build/platformio_override.ini).

## Runtime configuration backup

Segment boundaries, presets, Wi-Fi settings, and usermod settings live on the
controller rather than in the firmware binary. Keep a WLED configuration and
presets backup alongside each deployed release. Do not publish an unsanitized
backup because it may contain network credentials.

## License

Licensed under the EUPL-1.2, matching WLED 16.0.1. See [`LICENSE`](LICENSE).
