# Changelog

All notable changes to `TailscaleBrick` are documented in this file.

## [v1.0.0] - 2026-11-26

### Added
- **Initial Native Tailscale Manager for Trimui Brick (`TG3040`)**:
  - Built native C99 + SDL2 + SDL2_ttf UI (`1024x768`) reusing the verified Trimui Brick platform and UI graphics foundation (`src/platform.h/c`, `src/ui_gfx.h/c`).
  - Integrated background daemon controller (`src/ts_ctrl.h/c` and POSIX `pkg/scripts/daemon.sh`) keeping `tailscaled` running in the background after returning to `MainUI`.
  - Bundled Mozilla `pkg/certs/cacert.pem` for TLS verification on Stock OS and implemented Kernel TUN mode (`--tun=tailscale0`, `tailscale up --netfilter-mode=off`) with automatic fallback to `userspace-networking`.
  - Implemented 3 gamepad tabs (`状态控制`, `节点列表`, `日志配置`): Connect/Disconnect, Stop Daemon, Built-in C99 QR Code generator (`src/qrcode.h/c`) for phone `AuthURL` login, Boot Autostart (`/mnt/SDCARD/System/starts/tailscale_boot.sh`), Subnet Routes, Tailscale DNS, Background Exit, Logout, live peer list + Ping latency test, and `/tmp/tailscaled.log` viewer.
  - Added unattended login support via `config.ini`, `authkey.txt`, and `loginserver.txt`.
  - Added standard Docker cross-build script (`docker-build.sh`), env-configured remote build/SSH deployment helper scripts (`scripts/`), and 4 host unit tests (`src/test_ts.c`).

### Fixed & Changed
- **LED Coexistence (`src/main.c`, `pkg/launch.sh`, `src/platform.c`)**:
  - Removed unnecessary RGB LED takeover (`platform_led_*`) and `led_daemon.sh` pause/resume calls from `TailscaleBrick` so Tailscale UI never interferes with BrickTools or other apps' LED control.
  - Synced `src/platform.c` with the `anim_frames_enable=0` + 6-zone LED fix so the shared platform module stays bug-free.
