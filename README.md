# OnnCam Control

Native macOS webcam control software for the Walmart onn. 4K Webcam.

OnnCam Control is a lightweight menu bar app for tuning the onn. 4K webcam on macOS. It exposes practical camera controls that are usually hidden behind Windows-only vendor software, including exposure, gain, white balance, focus, anti-flicker, image tuning, and capture format selection.

The app is built for people who want the webcam to look good in FaceTime, Zoom, Discord, OBS, and other Mac camera apps without booting Windows or using the vendor `.exe`.

## Download

Download the latest `.dmg` from the [Releases](https://github.com/tmansmann0/onncam-control/releases) page.

1. Open the DMG.
2. Drag **OnnCam Control.app** into **Applications**.
3. Launch it from Applications.
4. Use the **OnnCam** menu bar item to adjust the camera.

macOS may warn that the app is from an unidentified developer because this release is ad-hoc signed rather than notarized. If needed, right-click the app and choose **Open** the first time.

## Features

- Native macOS menu bar utility
- Live camera preview pinned above the controls
- Quick controls for exposure, gain, white balance, autofocus, and anti-flicker
- Advanced controls for brightness, contrast, gamma, saturation, hue, focus, and sharpness
- Resolution and FPS selector based on modes reported by AVFoundation
- Preset support for common lighting cases
- Bundled low-level UVC helper for the onn. 4K webcam

## Supported Camera

This tool is built for the Walmart onn. 4K webcam, especially the white-label model that reports as an onn. 4K webcam on macOS.

It may work with related UVC webcams, but the controls and ranges were tuned around this camera and are not guaranteed for other devices.

## Requirements

- macOS 14 or newer
- onn. 4K webcam connected over USB
- Xcode command line tools if building from source

## Build From Source

```bash
git clone https://github.com/tmansmann0/onncam-control.git
cd onncam-control/OnnCamControl
swift build
```

Build the app bundle:

```bash
cd OnnCamControl
SIGNING_MODE=adhoc Scripts/package_app.sh release
```

Build the DMG installer:

```bash
cd OnnCamControl
SIGNING_MODE=adhoc Scripts/make_installer.sh
```

The app bundle is created at:

```text
OnnCamControl/OnnCam Control.app
```

The installer is created at:

```text
OnnCamControl/dist/OnnCam-Control-<version>.dmg
```

## Project Layout

```text
OnnCamControl/        macOS menu bar app
onncam/              bundled low-level UVC helper
```

The macOS app talks to the helper to apply UVC camera settings and uses AVFoundation for preview and capture format discovery.

## Disclaimer

This is an independent utility and is not affiliated with Walmart, onn., or the webcam manufacturer.

The goal is practical macOS control for a webcam whose vendor configuration utility is Windows-only.
