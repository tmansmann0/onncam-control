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
