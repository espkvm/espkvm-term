# Changelog

All notable changes to espkvm-term are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/). While it is pre-1.0, a new
feature bumps the minor and a fix bumps the patch.

## [0.1.0] - 2026-10-13

The first release. What is tried on hardware and what is only built is
listed in the README.

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
