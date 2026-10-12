# ESP-KVM terminal

Firmware for the M5Stack Tab5 that shows an [ESP-KVM](https://github.com/espkvm/espkvm)
device's screen on the Tab5's display. The Tab5's touch screen and keyboard
control the target. No browser or computer is needed.

## Status

Early. It runs on my Tab5 against my own ESP-KVM devices.

Works on hardware:

- Wi-Fi through the Tab5's ESP32-C6: scan, pick a network, type the
  password. The Tab5 remembers it.
- Sign-in with user and password. On the first visit the device's CA is
  shown with its SHA-256 fingerprint and trusted only after you accept it.
- The picture: MJPEG from the device, about 15 fps at 1280x720 over Wi-Fi.
- The menu and the notes over the picture, without flicker.
- Finding devices on the network by mDNS.
- Reconnecting after the device or the network drops.

Built but not tried yet:

- Touch and keyboard input to the target.
- Sign-in with a 2FA code.
- The device list (switching, forgetting).
- Text mode (the target's screen read as characters).
- The target's power, hard off, reset and Wake-on-LAN buttons, and
  Ctrl+Alt+Del.
- Tailscale.

The device's video codec must be set to MJPEG. The P4 has no
H.264 decoder, and software decoding manages about 4 fps at 720p.

## Build

You need ESP-IDF 6.1 and a checkout of
[espkvm](https://github.com/espkvm/espkvm) next to this one. The wire
protocol (`components/kvm_proto`) and the Tailscale client (microlink) come
from there, so the two sides never drift apart.

```sh
git clone https://github.com/espkvm/espkvm.git
git clone https://github.com/espkvm/espkvm-term.git
cd espkvm-term
idf.py set-target esp32p4
idf.py build flash monitor
```

The app runs on its own (with its own `partitions.csv`) or from
[M5 Launcher](https://github.com/bmorcelli/Launcher). Launcher installs only
the app image. Settings live in the `nvs` partition either way.

## How the picture is drawn

1. Frames are decoded by the P4's hardware JPEG decoder.
2. The PPA scales each frame and turns it into the panel's hidden frame
   buffer in one pass.
3. While the picture is up, LVGL renders but does not write the panel. The
   menu, the tab and the notes are taken as ARGB pictures (again only when
   they change) and the PPA blends them over every frame.
4. The two panel buffers then swap at the next refresh, so a half-drawn
   screen is never shown.

In text mode LVGL draws the whole screen again.

## Tab5 keyboard

The M5Stack Keyboard for Tab5 is read over I2C (address 0x6D, SDA G0, SCL
G1, INT G50). While it is attached, the on-screen keyboard stays hidden.

## Debug link

Commands over the USB serial port, for development (the port is
`/dev/ttyACM0`, or set `TAB5_PORT`):

- `tools/shot.py out.png [full]` saves a screenshot.
- `tools/shot.py tap X Y` touches the screen.
- `tools/shot.py chr TEXT` types text.
- `tools/shot.py key N` sends one key.

Typing `heap` on the port prints the free memory.

## Security notes

- Wi-Fi passwords, device logins and Tailscale keys are kept in NVS,
  which is not encrypted.
- A device is trusted by its pinned CA, not by its host name, so a device
  can be reached by IP, `.local` name or tailnet name.

## License

Apache-2.0, see `LICENSE`.

Third-party parts:

- Barlow Condensed and Saira Stencil One fonts: SIL OFL 1.1.
- DejaVu Sans Mono: Bitstream Vera license.
- Font Awesome symbols: SIL OFL 1.1.
- The Tab5 keyboard's key table, from M5Stack's M5Unit-KEYBOARD: MIT.

Each license is in `main/fonts` and `main/LICENSE-M5Unit-KEYBOARD.txt`.
