# Third-Party Licenses and Attribution

## Project License Scope

Original source code (`src/main.c`, `src/ts_ctrl.h`, `src/ts_ctrl.c`, `src/qrcode.h`, `src/qrcode.c`, `src/test_ts.c`, `src/platform.h`, `src/platform.c`, `src/ui_gfx.h`, `src/ui_gfx.c`), build scripts, shell scripts, documentation (`README.md`, `CHANGELOG.md`), and custom icon assets (`pkg/icon.png`) authored for **TailscaleBrick** are licensed under the **Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International (CC BY-NC-SA 4.0)** license. See [LICENSE](LICENSE) for the full license text.

Third-party binaries, certificates, libraries, toolchains, and referenced projects remain under their respective original licenses and are explicitly excluded from CC BY-NC-SA 4.0.

---

## Bundled & Referenced Third-Party Components

### 1. Tailscale (`tailscale`, `tailscaled`)
- **Upstream**: https://github.com/tailscale/tailscale
- **Location in Release**: `Tailscale/bin/tailscale`, `Tailscale/bin/tailscaled` (user-supplied in `pkg/bin/` when building from source)
- **License**: **BSD-3-Clause License**
- **Notes**: Official static ARM64 (`arm64`) Tailscale client and daemon binaries redistributed unaltered in binary releases under the terms of Tailscale's BSD-3-Clause License (included in releases as `TAILSCALE_LICENSE.txt`).

### 2. Mozilla CA Root Bundle (`pkg/certs/cacert.pem`)
- **Upstream**: https://curl.se/docs/caextract.html
- **Location**: `pkg/certs/cacert.pem` (and `Tailscale/certs/cacert.pem` in releases)
- **License**: **Mozilla Public License 2.0 (MPL-2.0)**
- **Carve-out Notice**: `pkg/certs/cacert.pem` is explicitly carved out from CC BY-NC-SA 4.0 and remains licensed under **MPL-2.0**. Extracted from Mozilla NSS CA certificates via the curl CA extract service to provide TLS root verification on Trimui Stock OS.

### 3. cJSON v1.7.19
- **Upstream**: https://github.com/DaveGamble/cJSON
- **Location**: `src/cJSON.c`, `src/cJSON.h`
- **License**: **MIT License**
- **Copyright**: Copyright (c) 2009-2017 Dave Gamble and cJSON contributors.

### 4. stb_image v2.34
- **Upstream**: https://github.com/nothings/stb
- **Location**: `src/stb_image.h`
- **License**: **MIT License / Public Domain (Unlicense)**

### 5. SDL2 & SDL2_ttf
- **Upstream**: https://www.libsdl.org/
- **Usage**: System dynamic libraries linked at runtime on Trimui Brick Stock OS (`-lSDL2 -lSDL2_ttf`)
- **License**: **zlib License**

### 6. LoveRetro/tg5040-toolchain
- **Image**: `ghcr.io/loveretro/tg5040-toolchain:latest`
- **Upstream**: https://github.com/LoveRetro/tg5040-toolchain
- **Usage**: Cross-compilation Docker toolchain targeting `aarch64` Linux (`TG3040` / `TG5040`).

### 7. Ponjen/Vibe
- **Upstream**: https://github.com/Ponjen/Vibe
- **Usage**: Reference for Trimui Brick native SDL2 UI rendering, platform input/power management, and `launch.sh` / `config.json` app architecture (`src/platform.c`, `src/ui_gfx.c`).

### 8. hugorosario/BrickTools
- **Upstream**: https://github.com/hugorosario/BrickTools
- **Usage**: Reference for Trimui Brick system service and RGB LED daemon coexistence behavior.
