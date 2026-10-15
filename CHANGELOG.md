# Changelog

All notable changes to espkvm-term are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/). While it is pre-1.0, a new
feature bumps the minor and a fix bumps the patch.

## [0.2.0] - 2026-10-15

### Added
- Frame acks with ESP-KVM 0.65.0 or newer: the device sends only as many
  frames as the link carries, so a slow link shows the newest frame. Older
  firmware works too, more slowly over a slow link.

### Fixed
- Wi-Fi could stop for good soon after the picture started.
- Video over Wi-Fi stalled for 1-3 s every few seconds: the Wi-Fi driver ran
  out of its small internal memory and dropped packets. Its buffers are in
  PSRAM now.
- Video over Tailscale crawled. It runs at about 11-14 fps at 720p now.
- After the device restarted, the Tab5 could not reach it over Tailscale
  again.
- Every round trip took 300-600 ms while the screen was on (Wi-Fi power
  save). It is off while the screen is on.
- Mouse and ping sends no longer wait for a long frame read.
- Internal RAM ran down to about 1 KB free when the picture started.

## [0.1.0] - 2026-10-13

The first release. What is tried on hardware is listed in the README.

### Added
- Wi-Fi through the Tab5's ESP32-C6. Up to five networks are saved and the
  strongest one in range is joined. The router lists the Tab5 as
  `espkvm-term-xxxx`.
- Finding ESP-KVM devices on the network by mDNS, sign-in, and a device's
  CA trusted only after you accept its fingerprint.
- The target's picture (MJPEG), drawn up to about 20 fps at 1280x720.
- Zoom with two fingers, a trackpad mode, two-finger scroll.
- The Tab5 keyboard, a USB keyboard and mouse, and a bar of extra keys.
- Sleep, screen brightness, the battery level, switching off.
- The screen turns over when the Tab5 is held upside down.
- Updates from GitHub releases, with a way back after a bad one.
- Reconnecting by itself when the device or the network drops, including a
  link lost in the middle of a frame.
