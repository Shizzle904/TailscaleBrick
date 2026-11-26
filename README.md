# TailscaleBrick

[![License: CC BY-NC-SA 4.0](https://img.shields.io/badge/License-CC%20BY--NC--SA%204.0-lightgrey.svg)](LICENSE)

专为 **Trimui Brick（`TG3040` / Allwinner A133P）官方系统（Stock OS）** 打造的原生 C99 + SDL2 Tailscale 组网管理前端与后台守护控制器。

无需外接键盘，直接在掌机上通过内置二维码完成扫码授权，退出界面后 `tailscaled` 依然在后台持续运行，随时随地将掌机接入你的 Tailnet 或自建 Headscale 网络。

---

## 核心特性

- **三页签掌机原生界面**（`L1` / `R1` 切换）：
  1. **状态控制**：启动/连接组网、断开连接、启停后台守护进程、弹出登录二维码、开机自启开关（写入 `/mnt/SDCARD/System/starts/tailscale_boot.sh`）、接收子网路由开关、Tailscale DNS 开关、返回桌面（保持后台运行）、注销账号（Logout）。
  2. **节点列表**：实时查看 Tailnet 中的节点名称、Tailscale IP、系统类型、直连/中继（DERP）状态与收发流量统计，选中节点按 `A` 即可执行 `tailscale ping` 连通性测试。
  3. **日志配置**：展示当前 `config.ini` 生效配置摘要（节点名、TUN 模式、子网路由、DNS、控制服务器、AuthKey 状态）及 `/tmp/tailscaled.log` 实时运行日志，支持方向键滚动与 `L2` 一键清空。
- **内置纯 C99 二维码登录**：处于 `NeedsLogin` 状态时自动生成并渲染 `AuthURL` 二维码，手机微信/浏览器扫码即可授权，无需键盘输入长链接。
- **支持 Headscale 与 AuthKey 无人值守登录**：可通过 `config.ini` 中的 `login_server` 与 `auth_key`，或独立文本 `authkey.txt` / `loginserver.txt` 实现免扫码自动入网。
- **内核 TUN 优先 + 用户态自动降级**：默认优先启用内核 TUN 模式（`--tun=tailscale0`，配合 `tailscale up --netfilter-mode=off`，并自动尝试创建 `/dev/net/tun`）；若当前系统环境不支持 TUN 设备，会自动无缝降级至 `userspace-networking` 模式（并监听本地 SOCKS5 `127.0.0.1:1055` 与 HTTP 代理 `127.0.0.1:1056`）。
- **内置根证书**：打包 Mozilla `cacert.pem`（解决 Trimui Stock OS 默认缺少 `/etc/ssl/certs` 导致 TLS 握手失败的问题）。
- **轻量后台常驻 & LED 共存**：`tailscaled` 通过 `GOMEMLIMIT=48MiB` 与 `GOGC=25` 控制内存占用；返回桌面后继续在后台运行；本应用不接管机身 RGB LED，可与 BrickTools 等系统服务和谐共存。

---

## Trimui Brick Stock OS 安装指南

1. 前往 [Releases](../../releases) 下载最新的 `TailscaleBrick_v1.0.0.zip` 并解压。
2. 将解压后的 `Tailscale` 文件夹复制到掌机 TF 卡的 `/mnt/SDCARD/Apps/` 目录下，最终路径结构如下：
   ```text
   /mnt/SDCARD/Apps/Tailscale/
   ├── tailscale_ui
   ├── launch.sh
   ├── config.json
   ├── config.ini
   ├── config.ini.example
   ├── buttons.ini
   ├── icon.png
   ├── bin/
   │   ├── tailscale
   │   └── tailscaled
   ├── certs/
   │   └── cacert.pem
   └── scripts/
       └── daemon.sh
   ```
3. （可选）如需自定义节点名称、自建 Headscale 地址或 AuthKey 无人值守登录，请编辑 `/mnt/SDCARD/Apps/Tailscale/config.ini`（参考下文配置说明）。
4. 插回 TF 卡开机，在掌机 **Apps** 菜单中打开 **Tailscale**（或 **Tailscale 组网**）即可使用。

> [!NOTE]
> **中文字体回退说明**：Trimui Stock OS 部分固件未内置完整中文字库。`tailscale_ui` 会按以下顺序自动查找字体：
> 1. `./fonts/font.otf`（应用目录下）
> 2. `/mnt/SDCARD/Apps/Vibe/fonts/font.otf`
> 3. `/mnt/SDCARD/Apps/FileManager/res/DroidSansFallback.ttf`
> 4. `/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc`
> 5. `/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc`
> 6. `/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf`
>
> 同时会加载 `./fonts/fb_*.ttf` 与 `/mnt/SDCARD/Apps/Vibe/fonts/fb_*.ttf` 作为补充回退字体。如果界面中文显示为方块，可将任意支持中文的字体（如 Noto Sans CJK / 思源黑体）重命名为 `font.otf` 并放入 `/mnt/SDCARD/Apps/Tailscale/fonts/font.otf`。

---

## 配置文件说明（`config.ini`）

可在 `Tailscale/config.ini` 中配置以下选项（持久化状态文件默认保存在 `/mnt/UDISK/tailscale/tailscaled.state`）：

```ini
accept_routes=1
accept_dns=0
hostname=trimui-brick
tun_mode=auto
# login_server=https://headscale.example.com
# auth_key=tskey-auth-...
```

| 参数 | 默认值 | 说明 |
| :--- | :--- | :--- |
| `accept_routes` | `1` | 是否接收其他节点广播的子网路由（`1` 开启 / `0` 关闭） |
| `accept_dns` | `0` | 是否接管系统 DNS（默认关闭以免影响掌机原生 Wi-Fi 解析） |
| `hostname` | `trimui-brick` | 显示在 Tailnet 中的设备节点名称 |
| `tun_mode` | `auto` | `auto`（优先内核 TUN，失败自动降级用户态）、`kernel`（仅内核 TUN）或 `userspace`（仅用户态代理） |
| `login_server` | 空 | 可选，填写自建 Headscale 控制服务器 URL |
| `auth_key` | 空 | 可选，预授权密钥，配置后可免扫码自动登录；也支持单独放置在同目录 `authkey.txt` / `loginserver.txt` 中 |

---

## 按键操作说明

| 按键 | 功能说明 |
| :--- | :--- |
| `↑` / `↓` | 上下移动光标；在「日志配置」页中上下滚动日志（支持长按连发） |
| `L1` / `R1` | 切换顶部页签（**状态控制** / **节点列表** / **日志配置**） |
| `A` | 执行当前选项；在「节点列表」页中对选中节点执行 `Ping` 测试；确认弹窗选择 |
| `B` | 返回掌机桌面（**`tailscaled` 保持后台运行**）；在菜单/弹窗中关闭当前窗口 |
| `X` | 手动刷新守护进程状态、节点列表与运行日志 |
| `L2` | 在「日志配置」页中清空 `/tmp/tailscaled.log` 日志 |
| `SELECT` | 一键息屏 / 亮屏（适合挂机保持后台连接） |
| `MENU` | 打开/关闭快捷功能菜单（刷新状态、返回桌面保持后台、停止后台服务并退出） |

---

## 从源码编译（Docker 交叉编译）

仓库本身不包含体积较大的官方 `tailscale` / `tailscaled` 二进制文件，本地构建前请先准备官方 ARM64 静态二进制。

### 1. 获取官方静态 ARM64 二进制

前往 Tailscale 官方静态发布页下载最新的 `arm64` 压缩包：
- 下载地址：<https://pkgs.tailscale.com/stable/#static>（选择 `tailscale_<version>_arm64.tgz`）

解压后将 `tailscale` 与 `tailscaled` 两个二进制文件放入仓库的 `pkg/bin/` 目录：
```text
pkg/bin/tailscale
pkg/bin/tailscaled
```

### 2. （可选）预置默认配置

如需在打包时内置自定义配置，可复制示例配置并按需修改（若跳过此步，`docker-build.sh` 会自动回退复制 `pkg/config.ini.example`）：

```sh
cp pkg/config.ini.example pkg/config.ini
```

### 3. 运行标准 Docker 交叉编译

使用 `LoveRetro/tg5040-toolchain` 镜像在当前工作目录执行编译与打包：

```sh
docker run --rm -v "$PWD:/work" ghcr.io/loveretro/tg5040-toolchain:latest bash /work/docker-build.sh
```

编译完成后：
- 目标程序与完整发布目录生成于 `dist/Tailscale/`
- 压缩包生成于 `dist/Tailscale.zip`
- 若容器内存在宿主 `gcc`，脚本同时会编译并运行 `test_ts` 单元测试。

---

## 第三方开源组件与致谢

本项目依赖并参考了以下优秀的开源项目与组件，完整的版权与许可声明请参阅 [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)：

- **[Tailscale](https://github.com/tailscale/tailscale)** (`tailscale`, `tailscaled`) — BSD-3-Clause License
- **[Mozilla CA Root Bundle](https://curl.se/docs/caextract.html)** (`pkg/certs/cacert.pem`) — MPL-2.0 License
- **[cJSON](https://github.com/DaveGamble/cJSON)** v1.7.19 (`src/cJSON.c`, `src/cJSON.h`) — MIT License
- **[stb_image](https://github.com/nothings/stb)** v2.34 (`src/stb_image.h`) — MIT / Public Domain (Unlicense)
- **[SDL2 & SDL2_ttf](https://www.libsdl.org/)** — zlib License
- **[LoveRetro/tg5040-toolchain](https://github.com/LoveRetro/tg5040-toolchain)** — `ghcr.io/loveretro/tg5040-toolchain:latest` 交叉编译工具链
- **[Ponjen/Vibe](https://github.com/Ponjen/Vibe)** — Trimui Brick UI、平台层与启动器架构参考
- **[hugorosario/BrickTools](https://github.com/hugorosario/BrickTools)** — Trimui Brick LED 与系统服务共存方案参考

---

## 🔗 Trimui Brick 配套自制应用推荐

| 项目 | 简介 | 许可协议 |
| :--- | :--- | :--- |
| **[KikoeruBrick](https://github.com/Shizzle904/KikoeruBrick)** | Trimui Brick 原生 Kikoeru (ASMR) 播放器：6 页签浏览、离线下载管理、LRC/VTT/SRT 字幕同步、暗夜床头模式与 VU 律动灯光 | GPL-3.0 |
| **[TailscaleBrick](https://github.com/Shizzle904/TailscaleBrick)** *(本项目)* | Trimui Brick 原生 Tailscale 异地组网客户端：内置离线二维码扫码登录、Headscale 支持、后台常驻与节点测速（配合 KikoeruBrick 可异地直连家中 NAS） | CC BY-NC-SA 4.0 |
| **[UsbAudioBrick](https://github.com/Shizzle904/UsbAudioBrick)** | Trimui Brick Type-C / USB DAC 音频增强工具：事件驱动音量同步、无硬件音量小尾巴软件音量回退与系统级 10 段 ALSA GEQ/PEQ（接小尾巴听感更佳） | CC BY-NC-SA 4.0 |

---

## 开源协议（License）

本项目（`TailscaleBrick`）原创源代码、构建与控制脚本、文档及图标资产均基于 **Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International (CC BY-NC-SA 4.0)** 协议开源。

- 允许在**非商业用途**下自由使用、修改与再分发，但必须保留原作者署名并以相同协议（CC BY-NC-SA 4.0）共享衍生作品。
- 完整协议全文见 [LICENSE](LICENSE)；打包或引用的第三方二进制、CA 证书库及开源库各自适用其原有许可证（详见 [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)）。
