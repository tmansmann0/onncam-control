# onncam

Small macOS command-line controller for the onn 4K Webcam / Sonix UVC camera.

Build:

```sh
make -C onncam
```

Inspect the camera:

```sh
./onncam/onncam list
```

Discover everything the hardware exposes (all UVC selectors, the Sonix
extension unit, the UAC microphone topology, and any HID interfaces):

```sh
./onncam/onncam probe
```

Probe findings for the onn 4K Webcam (3938:1390):

- `mirror` (UVC roll, 0–3): flip/mirror states; matches the hidden slider in
  the vendor's Windows app.
- `pan` / `tilt` (±36000, step 3600): digital pan within the zoom crop.
- `privacy` (0/1) and `ae-priority` (0/1) also work.
- The Sonix extension unit (id 3, GUID 28f03370-6311-4a2e-ba2c-6890eb334016)
  answers selectors 0x01–0x06, 0x18, 0x19. The Windows app's "Filters"
  presets live there (sensor/I2C register access) — payloads undecoded; do
  not write to it blind.
- Microphone: UAC 1.0, stereo. Feature unit has only master mute and
  per-channel volume (−95..0 dB, default 0 dB = already max). No AGC, EQ, or
  boost, and no vendor HID interface — there is nothing to tune over USB.


Set common UVC controls:

```sh
# A sane "stop blowing out my face" starting point.
./onncam/onncam set exposure-auto 1
./onncam/onncam set exposure 700
./onncam/onncam set brightness 0
./onncam/onncam set gain 0
./onncam/onncam set backlight 0
./onncam/onncam set white-auto 0
./onncam/onncam set white 4200
./onncam/onncam set powerline 2
```

Persistent mode (used by the OnnCam Control menu bar app):

```sh
./onncam/onncam serve
```

`serve` reads commands from stdin (`list`, `get <control>`, `set <control> <value>`,
`ping`, `quit`), one per line, keeping the USB device open between commands.
Each command is answered with zero or more data lines followed by exactly one
`ok` or `err <message>` line. If the camera is unplugged, the handle is dropped
and reopened automatically on the next command.

Notes:

- `exposure-auto` values are UVC standard bit flags. `1` usually means manual mode; `3` usually means auto mode.
- `powerline` is commonly `1` for 50 Hz and `2` for 60 Hz.
- In this room, `exposure 50` and `120` were too dark, `300` was still dim, and `700` was usable with no gain/backlight boost.
- Values persist in the camera until the camera or app resets them.
