#define _POSIX_C_SOURCE 200809L
#include "platform.h"
#include "ui_gfx.h"
#include "ts_ctrl.h"
#include "qrcode.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WIN_W 1024
#define WIN_H 768

typedef enum {
    TAB_CTRL = 0,
    TAB_PEERS,
    TAB_LOGS,
    TAB_COUNT
} UiTab;

static const char *TAB_NAMES[TAB_COUNT] = {
    "状态控制", "节点列表", "日志配置"
};

static const SDL_Color COL_BG       = { 14, 16, 24, 255 };
static const SDL_Color COL_BAR      = { 22, 26, 38, 255 };
static const SDL_Color COL_CARD     = { 26, 30, 44, 255 };
static const SDL_Color COL_CARD_SEL = { 38, 48, 70, 255 };
static const SDL_Color COL_PANEL    = { 20, 24, 35, 255 };
static const SDL_Color COL_WHITE    = { 242, 245, 252, 255 };
static const SDL_Color COL_GRAY     = { 170, 176, 192, 255 };
static const SDL_Color COL_DIM      = { 108, 114, 132, 255 };
static const SDL_Color COL_ACCENT   = { 100, 210, 150, 255 };
static const SDL_Color COL_BADGE    = { 38, 86, 68, 255 };
static const SDL_Color COL_BLUE     = { 110, 180, 250, 255 };
static const SDL_Color COL_BLUE_BG  = { 34, 58, 92, 255 };
static const SDL_Color COL_ERR      = { 235, 100, 100, 255 };
static const SDL_Color COL_ERR_BG   = { 92, 34, 42, 255 };

/* ---------------- Toast ---------------- */
static char   g_toast[160];
static Uint32 g_toast_until = 0;

static void show_toast(const char *msg) {
    if (!msg || !msg[0]) return;
    snprintf(g_toast, sizeof(g_toast), "%s", msg);
    g_toast_until = SDL_GetTicks() + 2600;
}

/* ---------------- App State ---------------- */
static UiTab      g_tab = TAB_CTRL;
static int        g_want_quit = 0;
static TsSnapshot g_snap;
static TsConfig   g_cfg;
static uint32_t   g_last_msg_seq = 0;

#define CTRL_ITEMS 8
static int g_ctrl_sel = 0;
static int g_ctrl_scroll = 0;
static int g_peer_sel = 0;
static int g_peer_scroll = 0;

#define LOG_BUF_LINES 120
#define LOG_VIS_LINES 16
static char g_log_lines[LOG_BUF_LINES][160];
static int  g_log_count = 0;
static int  g_log_scroll = 0;

/* QR Code modal */
static int    g_qr_open = 0;
static int    g_wait_qr = 0;
static char   g_qr_url[512] = "";
static QrCode g_qr_code;
static int    g_qr_valid = 0;

/* Confirm modal */
typedef enum {
    CONF_NONE = 0,
    CONF_LOGOUT,
    CONF_STOP_AND_QUIT
} ConfirmKind;

static int         g_confirm_open = 0;
static ConfirmKind g_confirm_kind = CONF_NONE;
static char        g_confirm_title[96];
static char        g_confirm_sub[256];
static int         g_confirm_sel = 0; /* 0 = Cancel, 1 = Confirm */

/* Quick Menu */
#define MENU_ITEMS 4
static int g_menu_open = 0;
static int g_menu_sel = 0;

/* ---------------- Helpers ---------------- */
static void fmt_bytes_short(uint64_t b, char *buf, size_t sz) {
    if (b >= 1024ULL * 1024ULL * 1024ULL) {
        snprintf(buf, sz, "%.2f GB", (double)b / (1024.0 * 1024.0 * 1024.0));
    } else if (b >= 1024ULL * 1024ULL) {
        snprintf(buf, sz, "%.1f MB", (double)b / (1024.0 * 1024.0));
    } else if (b >= 1024ULL) {
        snprintf(buf, sz, "%.1f KB", (double)b / 1024.0);
    } else {
        snprintf(buf, sz, "%llu B", (unsigned long long)b);
    }
}

static void refresh_logs(int stick_bottom) {
    g_log_count = ts_read_log_lines(g_log_lines, LOG_BUF_LINES);
    int max_sc = (g_log_count > LOG_VIS_LINES) ? (g_log_count - LOG_VIS_LINES) : 0;
    if (stick_bottom || g_log_scroll > max_sc) g_log_scroll = max_sc;
    if (g_log_scroll < 0) g_log_scroll = 0;
}

static void open_qr_for_url(const char *url) {
    if (!url || !url[0]) return;
    if (strcmp(g_qr_url, url) != 0 || !g_qr_valid) {
        snprintf(g_qr_url, sizeof(g_qr_url), "%s", url);
        g_qr_valid = (qr_encode_text(g_qr_url, &g_qr_code) == 0);
    }
    g_qr_open = 1;
    g_wait_qr = 0;
}

static void open_confirm(ConfirmKind kind, const char *title, const char *sub) {
    g_confirm_open = 1;
    g_confirm_kind = kind;
    g_confirm_sel = 0; /* default Cancel */
    snprintf(g_confirm_title, sizeof(g_confirm_title), "%s", title ? title : "确认操作");
    snprintf(g_confirm_sub, sizeof(g_confirm_sub), "%s", sub ? sub : "");
}

static void ensure_ctrl_visible(void) {
    const int vis = 6;
    if (g_ctrl_sel < g_ctrl_scroll) g_ctrl_scroll = g_ctrl_sel;
    if (g_ctrl_sel >= g_ctrl_scroll + vis) g_ctrl_scroll = g_ctrl_sel - (vis - 1);
    if (g_ctrl_scroll < 0) g_ctrl_scroll = 0;
}

static void ensure_peer_visible(void) {
    const int vis = 6;
    if (g_peer_sel < g_peer_scroll) g_peer_scroll = g_peer_sel;
    if (g_peer_sel >= g_peer_scroll + vis) g_peer_scroll = g_peer_sel - (vis - 1);
    if (g_peer_scroll < 0) g_peer_scroll = 0;
}

static const char *state_label_cn(const TsSnapshot *s) {
    if (s->busy && s->busy_label[0]) return s->busy_label;
    if (!s->daemon_running) return "未启动";
    if (strcmp(s->backend_state, "Running") == 0) return "已连接";
    if (strcmp(s->backend_state, "NeedsLogin") == 0) return "需扫码登录";
    if (strcmp(s->backend_state, "Stopped") == 0) return "已断开";
    if (strcmp(s->backend_state, "Starting") == 0) return "启动中";
    return s->backend_state[0] ? s->backend_state : "未知";
}

/* ---------------- Actions ---------------- */
static void run_ctrl_item(int idx) {
    int is_running = (strcmp(g_snap.backend_state, "Running") == 0);
    int needs_login = (strcmp(g_snap.backend_state, "NeedsLogin") == 0);

    switch (idx) {
    case 0:
        if (is_running) {
            ts_post_down();
            show_toast("正在断开组网...");
        } else if (needs_login && g_snap.auth_url[0]) {
            open_qr_for_url(g_snap.auth_url);
        } else {
            g_wait_qr = (g_cfg.auth_key[0] == '\0');
            ts_post_start_up();
            show_toast("正在连接 Tailscale...");
        }
        break;
    case 1:
        if (g_snap.daemon_running) {
            g_wait_qr = 0;
            ts_post_stop();
            show_toast("正在停止后台守护...");
        } else {
            ts_post_start_only();
            show_toast("正在启动后台守护...");
        }
        break;
    case 2:
        if (g_snap.auth_url[0]) {
            open_qr_for_url(g_snap.auth_url);
        } else if (is_running) {
            show_toast("当前已登录并在线");
        } else {
            g_wait_qr = 1;
            ts_post_start_up();
            show_toast("正在获取登录二维码...");
        }
        break;
    case 3: {
        int next = !g_snap.boot_autostart;
        if (ts_set_boot_autostart(next) == 0) {
            g_snap.boot_autostart = next;
            show_toast(next ? "已开启开机自启" : "已关闭开机自启");
        } else {
            show_toast("写入开机自启脚本失败");
        }
        break;
    }
    case 4:
        g_cfg.accept_routes = !g_cfg.accept_routes;
        ts_save_config(&g_cfg);
        if (is_running) {
            ts_post_start_up();
            show_toast(g_cfg.accept_routes ? "已开启子网路由并重连应用" : "已关闭子网路由并重连应用");
        } else {
            show_toast(g_cfg.accept_routes ? "接收子网路由: 开" : "接收子网路由: 关");
        }
        break;
    case 5:
        g_cfg.accept_dns = !g_cfg.accept_dns;
        ts_save_config(&g_cfg);
        if (is_running) {
            ts_post_start_up();
            show_toast(g_cfg.accept_dns ? "已开启 Tailscale DNS" : "已关闭 Tailscale DNS");
        } else {
            show_toast(g_cfg.accept_dns ? "Tailscale DNS: 开" : "Tailscale DNS: 关");
        }
        break;
    case 6:
        g_want_quit = 1;
        break;
    case 7:
        if (!g_snap.daemon_running) {
            show_toast("后台守护未运行");
        } else {
            open_confirm(CONF_LOGOUT, "确认注销账号?",
                         "注销后将清除本机登录凭据，下次需重新扫码或使用 AuthKey 登录。");
        }
        break;
    }
}

static void handle_dir(int bt) {
    if (g_qr_open || g_confirm_open) return;

    if (g_menu_open) {
        if (bt == BT_UP) g_menu_sel = (g_menu_sel + MENU_ITEMS - 1) % MENU_ITEMS;
        else if (bt == BT_DOWN) g_menu_sel = (g_menu_sel + 1) % MENU_ITEMS;
        return;
    }

    if (g_tab == TAB_CTRL) {
        if (bt == BT_UP) {
            g_ctrl_sel = (g_ctrl_sel + CTRL_ITEMS - 1) % CTRL_ITEMS;
            ensure_ctrl_visible();
        } else if (bt == BT_DOWN) {
            g_ctrl_sel = (g_ctrl_sel + 1) % CTRL_ITEMS;
            ensure_ctrl_visible();
        }
    } else if (g_tab == TAB_PEERS) {
        if (g_snap.peer_count <= 0) return;
        if (bt == BT_UP) {
            g_peer_sel = (g_peer_sel + g_snap.peer_count - 1) % g_snap.peer_count;
            ensure_peer_visible();
        } else if (bt == BT_DOWN) {
            g_peer_sel = (g_peer_sel + 1) % g_snap.peer_count;
            ensure_peer_visible();
        }
    } else if (g_tab == TAB_LOGS) {
        int max_sc = (g_log_count > LOG_VIS_LINES) ? (g_log_count - LOG_VIS_LINES) : 0;
        if (bt == BT_UP && g_log_scroll > 0) g_log_scroll--;
        else if (bt == BT_DOWN && g_log_scroll < max_sc) g_log_scroll++;
    }
}

static void handle_press(int bt) {
    if (bt == BT_SELECT) {
        if (platform_is_screen_off()) platform_screen_wake();
        else platform_screen_off(ui_renderer());
        return;
    }
    if (platform_is_screen_off()) return;

    if (g_qr_open) {
        if (bt == BT_B || bt == BT_A || bt == BT_MENU) {
            g_qr_open = 0;
        }
        return;
    }

    if (g_confirm_open) {
        if (bt == BT_UP || bt == BT_DOWN || bt == BT_LEFT || bt == BT_RIGHT) {
            g_confirm_sel = !g_confirm_sel;
        } else if (bt == BT_B || bt == BT_MENU) {
            g_confirm_open = 0;
        } else if (bt == BT_A) {
            ConfirmKind k = g_confirm_kind;
            int sel = g_confirm_sel;
            g_confirm_open = 0;
            if (sel == 1) {
                if (k == CONF_LOGOUT) {
                    ts_post_logout();
                    show_toast("正在注销账号...");
                } else if (k == CONF_STOP_AND_QUIT) {
                    ts_post_stop();
                    SDL_Delay(300);
                    g_want_quit = 1;
                }
            }
        }
        return;
    }

    if (bt == BT_MENU) {
        g_menu_open = !g_menu_open;
        g_menu_sel = 0;
        return;
    }

    if (g_menu_open) {
        if (bt == BT_B) {
            g_menu_open = 0;
        } else if (bt == BT_A) {
            g_menu_open = 0;
            if (g_menu_sel == 0) {
                ts_post_refresh();
                if (g_tab == TAB_LOGS) refresh_logs(1);
                show_toast("正在刷新状态...");
            } else if (g_menu_sel == 1) {
                g_want_quit = 1;
            } else if (g_menu_sel == 2) {
                open_confirm(CONF_STOP_AND_QUIT, "停止后台并退出?",
                             "将关闭 tailscaled 守护进程并退出 UI；若需后台继续组网请选「返回桌面」。");
            }
        }
        return;
    }

    if (bt == BT_L1 || bt == BT_R1) {
        int d = (bt == BT_R1) ? 1 : -1;
        g_tab = (UiTab)(((int)g_tab + d + TAB_COUNT) % TAB_COUNT);
        if (g_tab == TAB_LOGS) refresh_logs(1);
        return;
    }

    if (bt == BT_B) {
        g_want_quit = 1;
        return;
    }

    if (bt == BT_X) {
        ts_post_refresh();
        if (g_tab == TAB_LOGS) refresh_logs(1);
        show_toast("正在刷新状态...");
        return;
    }

    if (g_tab == TAB_CTRL) {
        if (bt == BT_A) run_ctrl_item(g_ctrl_sel);
    } else if (g_tab == TAB_PEERS) {
        if (bt == BT_A && g_peer_sel >= 0 && g_peer_sel < g_snap.peer_count) {
            const TsPeer *p = &g_snap.peers[g_peer_sel];
            if (p->ip[0]) {
                ts_post_ping(p->ip);
                show_toast("正在测试连通性...");
            }
        }
    } else if (g_tab == TAB_LOGS) {
        if (bt == BT_L2) {
            ts_clear_log();
            refresh_logs(1);
            show_toast("日志已清空");
        }
    }
}

/* ---------------- Rendering ---------------- */
static int draw_badge(int x, int y, const char *txt, SDL_Color bg, SDL_Color fg) {
    int w = ui_text_width(F_XS, txt) + 18;
    if (w < 54) w = 54;
    ui_fill_round(x, y, w, 26, 6, bg);
    ui_draw_text(F_XS, txt, x + w / 2, y + 1, w - 6, ALIGN_CENTER, fg);
    return w;
}

static void draw_scrollbar(int x, int y, int w, int h, int scroll, int visible, int total, SDL_Color accent) {
    if (total <= visible || visible <= 0 || h <= 0) return;
    ui_fill_round(x, y, w, h, w / 2, (SDL_Color){ 32, 36, 52, 255 });
    int thumb_h = h * visible / total;
    if (thumb_h < 28) thumb_h = 28;
    if (thumb_h > h) thumb_h = h;
    int max_scroll = total - visible;
    int travel = h - thumb_h;
    int thumb_y = y + (max_scroll > 0 ? (travel * scroll / max_scroll) : 0);
    ui_fill_round(x, thumb_y, w, thumb_h, w / 2, accent);
}

static void draw_header(const PlatformStatus *st) {
    ui_fill(0, 0, WIN_W, 64, COL_BAR);
    int tx = 24;
    for (int i = 0; i < TAB_COUNT; i++) {
        int active = ((int)g_tab == i);
        int w = ui_text_width(F_S, TAB_NAMES[i]);
        ui_draw_text(F_S, TAB_NAMES[i], tx, 17, 140, ALIGN_LEFT, active ? COL_WHITE : COL_DIM);
        if (active) ui_fill_round(tx, 54, w, 4, 2, COL_ACCENT);
        tx += w + 28;
    }

    char right[128];
    snprintf(right, sizeof(right), "%s  ·  %s  %d%%%s  %s",
             state_label_cn(&g_snap),
             st->wifi_up ? "WiFi" : "离线",
             st->bat_pct >= 0 ? st->bat_pct : 0,
             st->charging ? "+" : "",
             st->clock_str);
    ui_draw_text(F_XS, right, WIN_W - 24, 18, 520, ALIGN_RIGHT, COL_GRAY);
}

static void draw_footer(const char *hint) {
    ui_fill(0, WIN_H - 46, WIN_W, 46, COL_BAR);
    ui_draw_text(F_XS, hint, 24, WIN_H - 35, WIN_W - 48, ALIGN_LEFT, COL_GRAY);
}

static void draw_ctrl_tab(void) {
    int is_running = (strcmp(g_snap.backend_state, "Running") == 0);
    int needs_login = (strcmp(g_snap.backend_state, "NeedsLogin") == 0);

    /* Hero card */
    ui_fill_round(24, 76, WIN_W - 48, 168, 14, COL_PANEL);
    ui_rect_outline(24, 76, WIN_W - 48, 168, 1,
                    is_running ? COL_ACCENT : (SDL_Color){ 48, 56, 76, 255 });

    SDL_Color st_bg = (SDL_Color){ 42, 46, 58, 255 }, st_fg = COL_GRAY;
    if (is_running) {
        st_bg = COL_BADGE; st_fg = COL_ACCENT;
    } else if (needs_login) {
        st_bg = COL_BLUE_BG; st_fg = COL_BLUE;
    }
    int bx = 42;
    bx += draw_badge(bx, 92, state_label_cn(&g_snap), st_bg, st_fg) + 12;

    char host_s[160];
    snprintf(host_s, sizeof(host_s), "节点名: %s",
             g_snap.self_host[0] ? g_snap.self_host : g_cfg.hostname);
    ui_draw_text(F_XS, host_s, bx, 95, 360, ALIGN_LEFT, COL_GRAY);

    char proc_s[96];
    if (g_snap.daemon_running) {
        snprintf(proc_s, sizeof(proc_s), "PID %d  ·  内存 %.1f MB",
                 g_snap.daemon_pid, (double)g_snap.rss_kb / 1024.0);
    } else {
        snprintf(proc_s, sizeof(proc_s), "守护进程未启动");
    }
    ui_draw_text(F_XS, proc_s, WIN_W - 42, 95, 340, ALIGN_RIGHT,
                 g_snap.daemon_running ? COL_GRAY : COL_DIM);

    ui_draw_text(F_L, g_snap.self_ip[0] ? g_snap.self_ip : "--.--.--.--",
                 42, 128, 440, ALIGN_LEFT, is_running ? COL_ACCENT : COL_WHITE);
    if (g_snap.self_dns[0]) {
        ui_draw_text(F_XS, g_snap.self_dns, WIN_W - 42, 138, 480, ALIGN_RIGHT, COL_GRAY);
    }

    const char *mode_cn = (strcmp(g_snap.tun_mode, "userspace") == 0) ? "用户态代理 (1055/1056)"
                        : (strcmp(g_snap.tun_mode, "kernel") == 0) ? "内核 TUN (tailscale0)"
                        : "自动 (优先内核 TUN)";
    char line3[192];
    snprintf(line3, sizeof(line3), "模式: %s   ·   在线节点: %d/%d   ·   开机自启: %s",
             mode_cn, g_snap.online_peers, g_snap.peer_count,
             g_snap.boot_autostart ? "开" : "关");
    ui_draw_text(F_XS, line3, 42, 178, WIN_W - 84, ALIGN_LEFT, COL_GRAY);

    if (g_snap.health[0]) {
        ui_draw_text(F_XS, g_snap.health, 42, 208, WIN_W - 84, ALIGN_LEFT, COL_ERR);
    } else if (needs_login && g_snap.auth_url[0]) {
        ui_draw_text(F_XS, "请按 A 打开登录二维码，用手机扫码完成设备授权", 42, 208, WIN_W - 84, ALIGN_LEFT, COL_BLUE);
    } else if (is_running) {
        ui_draw_text(F_XS, "组网已就绪 · 按 B 返回掌机桌面后仍会保持后台运行", 42, 208, WIN_W - 84, ALIGN_LEFT, COL_DIM);
    } else {
        ui_draw_text(F_XS, "提示: 可选在安装目录放 authkey.txt 免扫码自动登录", 42, 208, WIN_W - 84, ALIGN_LEFT, COL_DIM);
    }

    /* Action list */
    char lbl0[64], sub0[96];
    if (is_running) {
        snprintf(lbl0, sizeof(lbl0), "断开组网连接");
        snprintf(sub0, sizeof(sub0), "断开 Tailnet，保留后台守护进程");
    } else if (needs_login) {
        snprintf(lbl0, sizeof(lbl0), "继续连接 / 扫码登录");
        snprintf(sub0, sizeof(sub0), "查看授权二维码并等待手机确认");
    } else {
        snprintf(lbl0, sizeof(lbl0), "启动并连接 Tailscale");
        snprintf(sub0, sizeof(sub0), "启动后台守护并加入 Tailnet");
    }

    const char *labels[CTRL_ITEMS] = {
        lbl0,
        g_snap.daemon_running ? "停止后台守护进程" : "仅启动后台守护 (不组网)",
        "显示登录二维码",
        g_snap.boot_autostart ? "开机自动启动: 开" : "开机自动启动: 关",
        g_cfg.accept_routes ? "接收子网路由: 开" : "接收子网路由: 关",
        g_cfg.accept_dns ? "使用 Tailscale DNS: 开" : "使用 Tailscale DNS: 关",
        "返回桌面（保持后台运行）",
        "注销当前账号 (Logout)"
    };
    const char *subs[CTRL_ITEMS] = {
        sub0,
        g_snap.daemon_running ? "彻底关闭 tailscaled 并释放内存" : "仅运行后台服务等待指令",
        g_snap.auth_url[0] ? "已有授权链接，按 A 扫码登录" : "发起连接并生成扫码授权二维码",
        "写入 /mnt/SDCARD/System/starts/tailscale_boot.sh",
        "允许访问远端 NAS 所在内网网段 (如 10.0.0.0/24)",
        "接管系统 DNS (默认关以免影响原生 Wi-Fi)",
        "退出界面回到 MainUI，组网持续在后台生效",
        "清除 /mnt/UDISK/tailscale 下的节点登录凭据"
    };

    const int vis = 6;
    const int row_w = WIN_W - 60;
    for (int r = 0; r < vis; r++) {
        int i = g_ctrl_scroll + r;
        if (i >= CTRL_ITEMS) break;
        int sel = (i == g_ctrl_sel);
        int y = 258 + r * 74;
        ui_fill_round(24, y, row_w, 64, 10, sel ? COL_CARD_SEL : COL_CARD);
        if (sel) ui_rect_outline(24, y, row_w, 64, 2, (i == 7) ? COL_ERR : COL_ACCENT);

        SDL_Color tc = (i == 7) ? (sel ? COL_WHITE : COL_ERR) : (sel ? COL_WHITE : COL_GRAY);
        ui_draw_text(F_S, labels[i], 42, y + 7, row_w - 36, ALIGN_LEFT, tc);
        ui_draw_text(F_XS, subs[i], 42, y + 36, row_w - 36, ALIGN_LEFT, COL_DIM);
    }
    draw_scrollbar(WIN_W - 22, 258, 8, 434, g_ctrl_scroll, vis, CTRL_ITEMS, COL_ACCENT);
    draw_footer("A 执行  X 刷新  L1/R1 切页签  B 返回桌面(后台保持运行)  Select 息屏  Menu 菜单");
}

static void draw_peers_tab(void) {
    char sub[128];
    snprintf(sub, sizeof(sub), "共 %d 个节点  ·  %d 个在线   |   选中节点按 A 测试连通性 (Ping)",
             g_snap.peer_count, g_snap.online_peers);
    ui_fill(0, 64, WIN_W, 42, COL_PANEL);
    ui_draw_text(F_XS, sub, 24, 73, WIN_W - 48, ALIGN_LEFT, COL_GRAY);

    if (g_snap.peer_count == 0) {
        ui_draw_text(F_M, "暂无节点信息", WIN_W / 2, 320, 600, ALIGN_CENTER, COL_GRAY);
        ui_draw_text(F_S, "请先在「状态控制」中启动并登录 Tailscale", WIN_W / 2, 370, 720, ALIGN_CENTER, COL_DIM);
    } else {
        const int vis = 6;
        const int row_w = WIN_W - 60;
        for (int r = 0; r < vis; r++) {
            int i = g_peer_scroll + r;
            if (i >= g_snap.peer_count) break;
            const TsPeer *p = &g_snap.peers[i];
            int sel = (i == g_peer_sel);
            int y = 114 + r * 98;

            ui_fill_round(24, y, row_w, 88, 12, sel ? COL_CARD_SEL : COL_CARD);
            if (sel) ui_rect_outline(24, y, row_w, 88, 2, COL_ACCENT);

            ui_draw_text(F_S, "●", 40, y + 12, 30, ALIGN_LEFT, p->online ? COL_ACCENT : COL_DIM);
            ui_draw_text(F_S, p->hostname, 72, y + 11, 430, ALIGN_LEFT, p->online ? COL_WHITE : COL_GRAY);

            int rx_x = 24 + row_w - 16;
            if (p->ping_result[0]) {
                int bw = ui_text_width(F_XS, p->ping_result) + 18;
                draw_badge(rx_x - bw, y + 12, p->ping_result, COL_BADGE, COL_ACCENT);
            } else {
                const char *st = p->online ? (p->cur_addr[0] ? "直连" : (p->relay[0] ? "中继" : "在线")) : "离线";
                SDL_Color bg = p->online ? (p->cur_addr[0] ? COL_BADGE : COL_BLUE_BG) : (SDL_Color){ 42, 46, 58, 255 };
                SDL_Color fg = p->online ? (p->cur_addr[0] ? COL_ACCENT : COL_BLUE) : COL_DIM;
                int bw = ui_text_width(F_XS, st) + 18;
                if (bw < 54) bw = 54;
                draw_badge(rx_x - bw, y + 12, st, bg, fg);
            }

            char rx_s[24], tx_s[24], meta[256];
            fmt_bytes_short(p->rx_bytes, rx_s, sizeof(rx_s));
            fmt_bytes_short(p->tx_bytes, tx_s, sizeof(tx_s));
            snprintf(meta, sizeof(meta), "%s   ·   %s   ·   收 %s / 发 %s%s%s",
                     p->ip[0] ? p->ip : "-",
                     p->os[0] ? p->os : "-",
                     rx_s, tx_s,
                     p->relay[0] ? "   ·   DERP:" : "",
                     p->relay[0] ? p->relay : "");
            ui_draw_text(F_XS, meta, 72, y + 50, row_w - 92, ALIGN_LEFT, COL_GRAY);
        }
        draw_scrollbar(WIN_W - 22, 114, 8, 578, g_peer_scroll, vis, g_snap.peer_count, COL_ACCENT);
    }

    draw_footer("A Ping 测试  X 刷新  L1/R1 切页签  B 返回桌面(后台保持运行)  Select 息屏");
}

static void draw_logs_tab(void) {
    ui_fill_round(24, 76, WIN_W - 48, 122, 12, COL_PANEL);
    char l1[256], l2[256], l3[256];
    snprintf(l1, sizeof(l1), "节点名称: %s    |    TUN 模式: %s    |    子网路由: %s    |    DNS: %s",
             g_cfg.hostname, g_cfg.tun_mode,
             g_cfg.accept_routes ? "开" : "关",
             g_cfg.accept_dns ? "开" : "关");
    snprintf(l2, sizeof(l2), "控制服务器: %.200s",
             g_cfg.login_server[0] ? g_cfg.login_server : "官方 Tailscale (默认)");
    snprintf(l3, sizeof(l3), "AuthKey: %s    |    状态路径: /mnt/UDISK/tailscale/tailscaled.state",
             g_cfg.auth_key[0] ? "已配置 (config.ini)" : "未配置 (使用交互式扫码登录或 authkey.txt)");
    ui_draw_text(F_XS, l1, 40, 90, WIN_W - 80, ALIGN_LEFT, COL_WHITE);
    ui_draw_text(F_XS, l2, 40, 122, WIN_W - 80, ALIGN_LEFT, COL_GRAY);
    ui_draw_text(F_XS, l3, 40, 154, WIN_W - 80, ALIGN_LEFT, COL_DIM);

    ui_fill_round(24, 210, WIN_W - 60, 496, 12, COL_PANEL);
    ui_draw_text(F_XS, "运行日志 (/tmp/tailscaled.log)", 40, 222, 420, ALIGN_LEFT, COL_ACCENT);

    if (g_log_count == 0) {
        ui_draw_text(F_S, "暂无日志记录", WIN_W / 2, 430, 400, ALIGN_CENTER, COL_DIM);
    } else {
        for (int r = 0; r < LOG_VIS_LINES; r++) {
            int i = g_log_scroll + r;
            if (i >= g_log_count) break;
            ui_draw_text(F_XS, g_log_lines[i], 40, 256 + r * 27, WIN_W - 100, ALIGN_LEFT, COL_GRAY);
        }
        draw_scrollbar(WIN_W - 22, 210, 8, 496, g_log_scroll, LOG_VIS_LINES, g_log_count, COL_ACCENT);
    }

    draw_footer("上下 滚动日志  X 刷新  L2 清空日志  L1/R1 切页签  B 返回桌面(后台保持运行)");
}

static void draw_qr_modal(void) {
    ui_fill(0, 0, WIN_W, WIN_H, (SDL_Color){ 0, 0, 0, 185 });
    int mw = 600, mh = 590;
    int mx = (WIN_W - mw) / 2, my = (WIN_H - mh) / 2;
    ui_fill_round(mx, my, mw, mh, 16, COL_BAR);
    ui_rect_outline(mx, my, mw, mh, 2, COL_ACCENT);

    ui_draw_text(F_M, "手机扫码登录 Tailscale", WIN_W / 2, my + 20, mw - 40, ALIGN_CENTER, COL_WHITE);

    int box_sz = 360;
    int bx = (WIN_W - box_sz) / 2;
    int by = my + 72;
    ui_fill_round(bx, by, box_sz, box_sz, 12, (SDL_Color){ 255, 255, 255, 255 });

    if (g_qr_valid && g_qr_code.size > 0) {
        int mod_px = 320 / g_qr_code.size;
        if (mod_px < 4) mod_px = 4;
        int qr_px = mod_px * g_qr_code.size;
        int ox = bx + (box_sz - qr_px) / 2;
        int oy = by + (box_sz - qr_px) / 2;
        SDL_Renderer *ren = ui_renderer();
        SDL_SetRenderDrawColor(ren, 12, 14, 20, 255);
        for (int y = 0; y < g_qr_code.size; y++) {
            for (int x = 0; x < g_qr_code.size; x++) {
                if (qr_get_module(&g_qr_code, x, y)) {
                    SDL_Rect rc = { ox + x * mod_px, oy + y * mod_px, mod_px, mod_px };
                    SDL_RenderFillRect(ren, &rc);
                }
            }
        }
    } else {
        ui_draw_text(F_S, "二维码生成失败", WIN_W / 2, by + 165, box_sz - 20, ALIGN_CENTER, COL_ERR);
    }

    ui_draw_text_wrap_align(F_XS, g_qr_url, mx + 28, by + box_sz + 18, mw - 56, 68, ALIGN_CENTER, COL_BLUE);
    ui_draw_text(F_XS, "请在浏览器打开或手机扫码授权，完成后自动连接 · 按 B 关闭",
                 WIN_W / 2, my + mh - 42, mw - 40, ALIGN_CENTER, COL_GRAY);
}

static void draw_confirm_modal(void) {
    ui_fill(0, 0, WIN_W, WIN_H, (SDL_Color){ 0, 0, 0, 170 });
    int mw = 600, mh = 276;
    int mx = (WIN_W - mw) / 2, my = (WIN_H - mh) / 2;
    ui_fill_round(mx, my, mw, mh, 16, COL_BAR);
    ui_rect_outline(mx, my, mw, mh, 2, COL_ERR);
    ui_draw_text(F_M, g_confirm_title, WIN_W / 2, my + 24, mw - 48, ALIGN_CENTER, COL_WHITE);
    ui_draw_text_wrap_align(F_XS, g_confirm_sub, mx + 30, my + 74, mw - 60, 68, ALIGN_CENTER, COL_GRAY);

    const char *btns[2] = { "取消", "确认执行" };
    for (int i = 0; i < 2; i++) {
        int bw = 236, bh = 50;
        int bx = mx + 44 + i * (bw + 40);
        int by = my + mh - 82;
        int sel = (i == g_confirm_sel);
        SDL_Color bg = sel ? (i == 1 ? COL_ERR_BG : COL_CARD_SEL) : COL_CARD;
        SDL_Color bd = (i == 1) ? COL_ERR : COL_ACCENT;
        SDL_Color fg = (i == 1) ? (sel ? COL_WHITE : COL_ERR) : (sel ? COL_WHITE : COL_GRAY);
        ui_fill_round(bx, by, bw, bh, 10, bg);
        if (sel) ui_rect_outline(bx, by, bw, bh, 2, bd);
        ui_draw_text(F_S, btns[i], bx + bw / 2, by + 11, bw - 20, ALIGN_CENTER, fg);
    }
}

static void draw_menu_overlay(void) {
    ui_fill(0, 0, WIN_W, WIN_H, (SDL_Color){ 0, 0, 0, 150 });
    int mw = 480, mh = 340;
    int mx = (WIN_W - mw) / 2, my = (WIN_H - mh) / 2;
    ui_fill_round(mx, my, mw, mh, 16, COL_BAR);
    ui_rect_outline(mx, my, mw, mh, 2, COL_ACCENT);
    ui_draw_text(F_M, "功能菜单", WIN_W / 2, my + 22, 300, ALIGN_CENTER, COL_WHITE);

    const char *items[MENU_ITEMS] = {
        "刷新状态与节点",
        "返回桌面 (保持后台运行)",
        "停止后台服务并退出",
        "关闭菜单"
    };
    for (int i = 0; i < MENU_ITEMS; i++) {
        int iy = my + 78 + i * 58;
        int sel = (i == g_menu_sel);
        ui_fill_round(mx + 24, iy, mw - 48, 48, 10, sel ? COL_CARD_SEL : COL_CARD);
        if (sel) ui_rect_outline(mx + 24, iy, mw - 48, 48, 2, (i == 2) ? COL_ERR : COL_ACCENT);
        SDL_Color tc = (i == 2) ? (sel ? COL_WHITE : COL_ERR) : (sel ? COL_WHITE : COL_GRAY);
        ui_draw_text(F_S, items[i], WIN_W / 2, iy + 10, mw - 72, ALIGN_CENTER, tc);
    }
}

static void draw_toast_overlay(void) {
    if (!g_toast[0] || SDL_GetTicks() >= g_toast_until) return;
    int tw = ui_text_width(F_S, g_toast) + 56;
    if (tw < 240) tw = 240;
    if (tw > WIN_W - 80) tw = WIN_W - 80;
    int tx = (WIN_W - tw) / 2;
    ui_fill_round(tx, WIN_H - 110, tw, 46, 12, (SDL_Color){ 20, 24, 34, 235 });
    ui_rect_outline(tx, WIN_H - 110, tw, 46, 1, COL_ACCENT);
    ui_draw_text(F_S, g_toast, WIN_W / 2, WIN_H - 99, tw - 24, ALIGN_CENTER, COL_WHITE);
}

static void crash_handler(int sig) {
    char buf[32] = "FATAL: caught signal ";
    size_t pos = 21;
    int s = sig < 0 ? 0 : sig;
    if (s >= 10) buf[pos++] = (char)('0' + (s / 10) % 10);
    buf[pos++] = (char)('0' + (s % 10));
    buf[pos++] = '\n';
    ssize_t w = write(STDERR_FILENO, buf, pos);
    (void)w;
    _exit(128 + s);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    signal(SIGPIPE, SIG_IGN);
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGBUS, crash_handler);
    signal(SIGFPE, crash_handler);

    platform_load_buttons("buttons.ini");

    if (ui_init(WIN_W, WIN_H, "Tailscale Brick") != 0) {
        fprintf(stderr, "ui_init failed\n");
        return 1;
    }
    platform_cpu_init();

    ts_init("config.ini");
    ts_get_config(&g_cfg);
    ts_get_snapshot(&g_snap);
    g_last_msg_seq = g_snap.msg_seq;

    PlatformStatus pst = {0};
    int held_dir = -1;
    Uint32 next_repeat = 0;
    Uint32 next_log_poll = 0;

    while (!g_want_quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            InputEvent ie;
            platform_translate_event(&ev, &ie);
            if (ie.kind == INPUT_QUIT) {
                g_want_quit = 1;
            } else if (ie.kind == INPUT_PRESS) {
                handle_press(ie.btn);
            } else if (ie.kind == INPUT_DIR_PRESS) {
                handle_dir(ie.btn);
                held_dir = ie.btn;
                next_repeat = SDL_GetTicks() + 340;
            } else if (ie.kind == INPUT_DIR_RELEASE) {
                if (held_dir == ie.btn) held_dir = -1;
            }
        }

        Uint32 now = SDL_GetTicks();
        if (held_dir >= 0 && !platform_is_screen_off() && now >= next_repeat) {
            handle_dir(held_dir);
            next_repeat = now + 90;
        }

        ts_get_snapshot(&g_snap);
        if (g_snap.msg_seq != g_last_msg_seq) {
            g_last_msg_seq = g_snap.msg_seq;
            if (g_snap.last_msg[0]) show_toast(g_snap.last_msg);
        }
        if (g_peer_sel >= g_snap.peer_count) {
            g_peer_sel = g_snap.peer_count > 0 ? g_snap.peer_count - 1 : 0;
        }

        if (g_wait_qr && g_snap.auth_url[0] &&
            strcmp(g_snap.backend_state, "NeedsLogin") == 0) {
            open_qr_for_url(g_snap.auth_url);
        }
        if (g_qr_open && strcmp(g_snap.backend_state, "Running") == 0) {
            g_qr_open = 0;
            g_wait_qr = 0;
            show_toast("Tailscale 登录成功，已连接！");
        }

        if (g_tab == TAB_LOGS && now >= next_log_poll) {
            next_log_poll = now + 1500;
            refresh_logs(0);
        }

        platform_poll_status(&pst);

        if (platform_is_screen_off()) {
            SDL_Delay(40);
            continue;
        }

        SDL_Renderer *ren = ui_renderer();
        SDL_SetRenderDrawColor(ren, COL_BG.r, COL_BG.g, COL_BG.b, 255);
        SDL_RenderClear(ren);

        draw_header(&pst);
        if (g_tab == TAB_CTRL)       draw_ctrl_tab();
        else if (g_tab == TAB_PEERS) draw_peers_tab();
        else                         draw_logs_tab();

        if (g_menu_open) draw_menu_overlay();
        if (g_confirm_open) draw_confirm_modal();
        if (g_qr_open) draw_qr_modal();
        draw_toast_overlay();

        SDL_RenderPresent(ren);
        SDL_Delay(16);
    }

    ts_shutdown();
    platform_restore_brightness();
    platform_cpu_shutdown();
    ui_shutdown();
    return 0;
}
