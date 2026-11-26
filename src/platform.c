#if !defined(_POSIX_C_SOURCE) && !defined(_GNU_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "platform.h"

#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* ============================ backlight & display ============================ */
static int bl_saved = -1;
static int bl_default = 128;
static int bl_via_ioctl = 0;

static char scr_off_cmd[200];
static char scr_on_cmd[200];

/* screen_blank MUST stay 0 by default: writing to /sys/class/graphics/fb0/blank
   behind the stock OS's back confuses overlays and brightness keys on wake. */
static int screen_ioctl = 1;
static int screen_backlight = 0;
static int screen_blank = 0;
static int g_screen_off = 0;
static int g_dim_mode = 0;
static int dim_brightness = 2;
static int dim_overlay_alpha = 165;
static int led_dim_scale = 15;

static int bl_read(void) {
    glob_t g;
    int v = -1;
    if (glob("/sys/class/backlight/*/brightness", 0, NULL, &g) == 0) {
        if (g.gl_pathc > 0) {
            FILE *f = fopen(g.gl_pathv[0], "r");
            if (f) {
                if (fscanf(f, "%d", &v) != 1) v = -1;
                fclose(f);
            }
        }
        globfree(&g);
        return v;
    }
    FILE *f = fopen("/sys/class/disp/disp/attr/lcdbl", "r");
    if (f) {
        char b[256];
        size_t n = fread(b, 1, sizeof(b) - 1, f);
        b[n] = '\0';
        fclose(f);
        char *p = b;
        char *end = NULL;
        long last = -1;
        while (*p) {
            long x = strtol(p, &end, 10);
            if (end != p) {
                last = x;
                p = end;
            } else {
                p++;
            }
        }
        if (last > 0 && last <= 255) v = (int)last;
    }
    return v;
}

static void bl_write(int v) {
    glob_t g;
    if (glob("/sys/class/backlight/*/brightness", 0, NULL, &g) == 0) {
        if (g.gl_pathc > 0) {
            FILE *f = fopen(g.gl_pathv[0], "w");
            if (f) {
                fprintf(f, "%d", v);
                fclose(f);
            }
        }
        globfree(&g);
        return;
    }
    FILE *f = fopen("/sys/class/disp/disp/attr/lcdbl", "w");
    if (f) {
        fprintf(f, "lcd0 1 %d", v);
        fclose(f);
    }
}

static void disp_backlight(int on) { /* DISP_LCD_BACKLIGHT_ENABLE=0x104, DISABLE=0x105 */
    int fd = open("/dev/disp", O_RDWR);
    if (fd < 0) return;
    unsigned long args[4] = { 0, 0, 0, 0 };
    int rc = ioctl(fd, on ? 0x104UL : 0x105UL, args);
    (void)rc;
    close(fd);
}

static int disp_get_brightness(void) { /* DISP_LCD_GET_BRIGHTNESS=0x103 */
    int fd = open("/dev/disp", O_RDWR);
    if (fd < 0) return -1;
    unsigned long args[4] = { 0, 0, 0, 0 };
    int v = ioctl(fd, 0x103UL, args);
    close(fd);
    return v;
}

static int disp_set_brightness(int v) { /* DISP_LCD_SET_BRIGHTNESS=0x102 */
    int fd = open("/dev/disp", O_RDWR);
    if (fd < 0) return -1;
    unsigned long args[4] = { 0, (unsigned long)v, 0, 0 };
    int r = ioctl(fd, 0x102UL, args);
    close(fd);
    return r;
}

static void panel_power(int on) {
    const char *cmd = on ? scr_on_cmd : scr_off_cmd;
    if (cmd[0]) {
        int rc = system(cmd);
        (void)rc;
    }
    if (screen_backlight) disp_backlight(on);
    if (screen_blank) {
        FILE *f = fopen("/sys/class/graphics/fb0/blank", "w");
        if (f) {
            fprintf(f, "%d", on ? 0 : 4);
            fclose(f);
        }
    }
}

int platform_disp_get_brightness(void) {
    int v = screen_ioctl ? disp_get_brightness() : -1;
    if (v >= 0 && v <= 255) return v;
    return bl_read();
}

void platform_disp_set_brightness(int v) {
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    if (screen_ioctl && disp_set_brightness(v) >= 0) return;
    bl_write(v);
}

static void capture_saved_brightness(void) {
    if (bl_saved > 0) return;
    int dv = screen_ioctl ? disp_get_brightness() : -1;
    bl_via_ioctl = (dv > 0 && dv <= 255);
    if (bl_via_ioctl) {
        bl_saved = dv;
    } else {
        int v = bl_read();
        bl_saved = (v > 0) ? v : bl_default;
    }
}

void platform_screen_off(SDL_Renderer *ren) {
    if (g_screen_off) return;
    if (!g_dim_mode) capture_saved_brightness();
    g_screen_off = 1;
    if (ren) {
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderPresent(ren);
    }
    if (bl_via_ioctl) {
        disp_set_brightness(0);
    } else {
        bl_write(0);
    }
    panel_power(0);
}

void platform_screen_wake(void) {
    if (!g_screen_off) return;
    g_screen_off = 0;
    panel_power(1);
    int target = g_dim_mode ? dim_brightness : (bl_saved > 0 ? bl_saved : bl_default);
    if (bl_via_ioctl) {
        disp_set_brightness(target);
    } else {
        bl_write(target);
    }
    if (!g_dim_mode) bl_saved = -1;
}

int platform_is_screen_off(void) {
    return g_screen_off;
}

void platform_set_dim_mode(int on) {
    int want = (on != 0);
    if (want == g_dim_mode) return;
    if (want) {
        if (!g_screen_off) capture_saved_brightness();
        g_dim_mode = 1;
        if (!g_screen_off) platform_disp_set_brightness(dim_brightness);
    } else {
        g_dim_mode = 0;
        if (!g_screen_off) {
            int target = (bl_saved > 0) ? bl_saved : bl_default;
            platform_disp_set_brightness(target);
            bl_saved = -1;
        }
    }
}

int platform_is_dim_mode(void) {
    return g_dim_mode;
}

int platform_dim_overlay_alpha(void) {
    return dim_overlay_alpha;
}

void platform_restore_brightness(void) {
    if (g_screen_off) {
        g_dim_mode = 0;
        platform_screen_wake();
    } else if (g_dim_mode) {
        platform_set_dim_mode(0);
    }
}

/* ============================ input mapping ============================ */
/* TrimUI Brick stock SDL joystick button indices */
static int jmap[BT_COUNT] = {
    [BT_UP]     = -1,
    [BT_DOWN]   = -1,
    [BT_LEFT]   = -1,
    [BT_RIGHT]  = -1,
    [BT_A]      = 1,
    [BT_B]      = 0,
    [BT_X]      = 3,
    [BT_Y]      = 2,
    [BT_L1]     = 4,
    [BT_R1]     = 5,
    [BT_L2]     = -1,
    [BT_R2]     = -1,
    [BT_SELECT] = 6,
    [BT_START]  = 7,
    [BT_MENU]   = 8
};

static const char *bt_names[BT_COUNT] = {
    [BT_UP]     = "UP",
    [BT_DOWN]   = "DOWN",
    [BT_LEFT]   = "LEFT",
    [BT_RIGHT]  = "RIGHT",
    [BT_A]      = "A",
    [BT_B]      = "B",
    [BT_X]      = "X",
    [BT_Y]      = "Y",
    [BT_L1]     = "L1",
    [BT_R1]     = "R1",
    [BT_L2]     = "L2",
    [BT_R2]     = "R2",
    [BT_SELECT] = "SELECT",
    [BT_START]  = "START",
    [BT_MENU]   = "MENU"
};

static int clock_12h = 0;
static int axis_l2 = 2;
static int axis_r2 = 5;

static int bt_from_name(const char *n) {
    for (int i = 0; i < BT_COUNT; i++) {
        if (bt_names[i] && strcasecmp(n, bt_names[i]) == 0) return i;
    }
    return -1;
}

void platform_load_buttons(const char *path) {
    const char *cfg_path = (path && path[0]) ? path : "buttons.ini";
    FILE *f = fopen(cfg_path, "r");
    if (!f) return;

    char line[300];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = '\0';
        const char *val = eq + 1;
        int v = atoi(val);
        line[strcspn(line, " \t\r\n")] = '\0';

        if (strcasecmp(line, "screen_ioctl") == 0) { screen_ioctl = v; continue; }
        if (strcasecmp(line, "screen_backlight") == 0) { screen_backlight = v; continue; }
        if (strcasecmp(line, "screen_blank") == 0) { /* keep 0 by default; do not enable fb0/blank */ continue; }
        if (strcasecmp(line, "clock_12h") == 0) { clock_12h = v; continue; }
        if (strcasecmp(line, "screen_off_cmd") == 0) {
            snprintf(scr_off_cmd, sizeof(scr_off_cmd), "%s", val);
            scr_off_cmd[strcspn(scr_off_cmd, "\r\n")] = '\0';
            continue;
        }
        if (strcasecmp(line, "screen_on_cmd") == 0) {
            snprintf(scr_on_cmd, sizeof(scr_on_cmd), "%s", val);
            scr_on_cmd[strcspn(scr_on_cmd, "\r\n")] = '\0';
            continue;
        }
        if (strcasecmp(line, "axis_l2") == 0) { axis_l2 = v; continue; }
        if (strcasecmp(line, "axis_r2") == 0) { axis_r2 = v; continue; }
        if (strcasecmp(line, "brightness") == 0) { bl_default = v; continue; }
        if (strcasecmp(line, "dim_brightness") == 0) {
            if (v < 1) v = 1;
            if (v > 40) v = 40;
            dim_brightness = v;
            continue;
        }
        if (strcasecmp(line, "dim_overlay_alpha") == 0) {
            if (v < 0) v = 0;
            if (v > 235) v = 235;
            dim_overlay_alpha = v;
            continue;
        }
        if (strcasecmp(line, "led_dim_scale") == 0) {
            if (v < 5) v = 5;
            if (v > 40) v = 40;
            led_dim_scale = v;
            continue;
        }

        int idx = bt_from_name(line);
        if (idx >= BT_A) jmap[idx] = v;
    }
    fclose(f);
}

int platform_clock_12h(void) {
    return clock_12h;
}

/* ============================ status polling ============================ */
static int sysread(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(out, (int)n, f)) out[0] = '\0';
    fclose(f);
    out[strcspn(out, " \r\n")] = '\0';
    return 1;
}

static void bat_update(int *out_pct, int *out_chg) {
    int pct = -1;
    int chg = 0;
    glob_t g;
    if (glob("/sys/class/power_supply/*/capacity", 0, NULL, &g) == 0) {
        for (size_t k = 0; k < g.gl_pathc && pct < 0; k++) {
            FILE *f = fopen(g.gl_pathv[k], "r");
            int v = -1;
            if (f) {
                if (fscanf(f, "%d", &v) != 1) v = -1;
                fclose(f);
            }
            if (v < 0 || v > 100) continue;
            pct = v;
            chg = 0;
            const char *sl = strrchr(g.gl_pathv[k], '/');
            if (sl) {
                char sp[512];
                size_t dir_len = (size_t)(sl - g.gl_pathv[k]);
                if (dir_len + 8 < sizeof(sp)) {
                    memcpy(sp, g.gl_pathv[k], dir_len);
                    memcpy(sp + dir_len, "/status", 8);
                    f = fopen(sp, "r");
                    if (f) {
                        char st[24] = "";
                        if (fgets(st, sizeof(st), f)) {
                            chg = (st[0] == 'C' || st[0] == 'F') ? 1 : 0;
                        }
                        fclose(f);
                    }
                }
            }
        }
        globfree(&g);
    }
    *out_pct = pct;
    *out_chg = chg;
}

static int radio_wifi_up(void) {
    int rk_wifi = 0;
    int wifi_link = 0;
    int iface_up = 0;
    glob_t g;
    char p[512];
    char t[32];
    char st[8];

    if (glob("/sys/class/rfkill/rfkill*", 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            size_t dlen = strlen(g.gl_pathv[i]);
            if (dlen + 8 >= sizeof(p)) continue;
            memcpy(p, g.gl_pathv[i], dlen);
            memcpy(p + dlen, "/type", 6);
            if (!sysread(p, t, sizeof(t))) continue;
            memcpy(p + dlen, "/state", 7);
            if (!sysread(p, st, sizeof(st))) continue;
            if (strcmp(t, "wlan") == 0 && st[0] == '1') rk_wifi = 1;
        }
        globfree(&g);
    }
    if (glob("/sys/class/net/wl*/operstate", 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            if (sysread(g.gl_pathv[i], t, sizeof(t))) {
                if (strcmp(t, "up") == 0) wifi_link = 1;
                if (strcmp(t, "down") != 0) iface_up = 1;
            }
        }
        globfree(&g);
    }
    return (rk_wifi || wifi_link || iface_up) ? 1 : 0;
}

void platform_poll_status(PlatformStatus *st) {
    static PlatformStatus cached = { -1, 0, 0, 0, "" };
    static Uint32 last_poll = 0;
    static int inited = 0;

    if (!st) return;

    Uint32 now = SDL_GetTicks();
    if (!inited || (now - last_poll) >= 3000u) {
        inited = 1;
        last_poll = now;
        bat_update(&cached.bat_pct, &cached.charging);
        cached.wifi_up = radio_wifi_up();
        cached.clock_12h = clock_12h;

        time_t t = time(NULL);
        struct tm tmv;
        localtime_r(&t, &tmv);
        strftime(cached.clock_str, sizeof(cached.clock_str),
                 clock_12h ? "%I:%M %p" : "%H:%M", &tmv);
    }
    *st = cached;
}

/* ============================ event translation ============================ */
void platform_translate_event(const SDL_Event *e, InputEvent *out) {
    static int hat_prev = 0;
    static int ax_prev[2] = { 0, 0 };
    static int trig[2] = { 0, 0 };

    if (!out) return;
    out->kind = INPUT_NONE;
    out->btn = -1;
    if (!e) return;

    if (e->type == SDL_QUIT) {
        out->kind = INPUT_QUIT;
        return;
    }

    if (e->type == SDL_JOYDEVICEADDED) {
        SDL_JoystickOpen(e->jdevice.which);
        return;
    }

    switch (e->type) {
    case SDL_KEYDOWN: {
        if (e->key.repeat) break;
        SDL_Keycode k = e->key.keysym.sym;
        if (g_screen_off) {
            if (k == SDLK_TAB) platform_screen_wake();
            break;
        }
        if (k == SDLK_UP)         { out->kind = INPUT_DIR_PRESS; out->btn = BT_UP; }
        else if (k == SDLK_DOWN)  { out->kind = INPUT_DIR_PRESS; out->btn = BT_DOWN; }
        else if (k == SDLK_LEFT)  { out->kind = INPUT_DIR_PRESS; out->btn = BT_LEFT; }
        else if (k == SDLK_RIGHT) { out->kind = INPUT_DIR_PRESS; out->btn = BT_RIGHT; }
        else if (k == SDLK_RETURN || k == SDLK_SPACE) { out->kind = INPUT_PRESS; out->btn = BT_A; }
        else if (k == SDLK_BACKSPACE)                 { out->kind = INPUT_PRESS; out->btn = BT_B; }
        else if (k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_m) {
            out->kind = INPUT_PRESS; out->btn = BT_MENU;
        }
        else if (k == SDLK_x)        { out->kind = INPUT_PRESS; out->btn = BT_X; }
        else if (k == SDLK_y)        { out->kind = INPUT_PRESS; out->btn = BT_Y; }
        else if (k == SDLK_l)        { out->kind = INPUT_PRESS; out->btn = BT_L1; }
        else if (k == SDLK_r)        { out->kind = INPUT_PRESS; out->btn = BT_R1; }
        else if (k == SDLK_PAGEUP)   { out->kind = INPUT_PRESS; out->btn = BT_L2; }
        else if (k == SDLK_PAGEDOWN) { out->kind = INPUT_PRESS; out->btn = BT_R2; }
        else if (k == SDLK_s)        { out->kind = INPUT_PRESS; out->btn = BT_START; }
        else if (k == SDLK_TAB)      { out->kind = INPUT_PRESS; out->btn = BT_SELECT; }
        break;
    }
    case SDL_KEYUP: {
        if (g_screen_off) break;
        SDL_Keycode k = e->key.keysym.sym;
        if (k == SDLK_UP)         { out->kind = INPUT_DIR_RELEASE; out->btn = BT_UP; }
        else if (k == SDLK_DOWN)  { out->kind = INPUT_DIR_RELEASE; out->btn = BT_DOWN; }
        else if (k == SDLK_LEFT)  { out->kind = INPUT_DIR_RELEASE; out->btn = BT_LEFT; }
        else if (k == SDLK_RIGHT) { out->kind = INPUT_DIR_RELEASE; out->btn = BT_RIGHT; }
        break;
    }
    case SDL_JOYBUTTONDOWN: {
        int btn = (int)e->jbutton.button;
        if (g_screen_off) {
            if (jmap[BT_SELECT] == btn) platform_screen_wake();
            break;
        }
        for (int i = BT_A; i < BT_COUNT; i++) {
            if (jmap[i] == btn) {
                out->kind = INPUT_PRESS;
                out->btn = i;
                break;
            }
        }
        break;
    }
    case SDL_JOYHATMOTION: {
        int v = (int)e->jhat.value;
        if (g_screen_off) {
            hat_prev = v;
            break;
        }
        static const int bits[4] = { SDL_HAT_UP, SDL_HAT_DOWN, SDL_HAT_LEFT, SDL_HAT_RIGHT };
        int found = 0;
        for (int d = 0; d < 4; d++) {
            if ((v & bits[d]) && !(hat_prev & bits[d])) {
                out->kind = INPUT_DIR_PRESS;
                out->btn = d;
                found = 1;
                break;
            }
        }
        if (!found) {
            for (int d = 0; d < 4; d++) {
                if (!(v & bits[d]) && (hat_prev & bits[d])) {
                    out->kind = INPUT_DIR_RELEASE;
                    out->btn = d;
                    break;
                }
            }
        }
        hat_prev = v;
        break;
    }
    case SDL_JOYAXISMOTION: {
        int a = (int)e->jaxis.axis;
        if (a == axis_l2 || a == axis_r2) {
            int idx = (a == axis_r2) ? 1 : 0;
            int pr = e->jaxis.value > 16000;
            if (pr && !trig[idx]) {
                trig[idx] = 1;
                if (!g_screen_off) {
                    out->kind = INPUT_PRESS;
                    out->btn = (a == axis_l2) ? BT_L2 : BT_R2;
                }
            } else if (!pr) {
                trig[idx] = 0;
            }
            break;
        }
        if (a < 0 || a > 1) break;
        int v = e->jaxis.value;
        int s = (v > 16000) ? 1 : (v < -16000) ? -1 : 0;
        if (g_screen_off) {
            ax_prev[a] = s;
            break;
        }
        int neg = (a == 0) ? BT_LEFT : BT_UP;
        int pos = (a == 0) ? BT_RIGHT : BT_DOWN;
        if (s != ax_prev[a]) {
            if (s == -1) {
                out->kind = INPUT_DIR_PRESS;
                out->btn = neg;
            } else if (s == 1) {
                out->kind = INPUT_DIR_PRESS;
                out->btn = pos;
            } else if (ax_prev[a] == -1) {
                out->kind = INPUT_DIR_RELEASE;
                out->btn = neg;
            } else if (ax_prev[a] == 1) {
                out->kind = INPUT_DIR_RELEASE;
                out->btn = pos;
            }
            ax_prev[a] = s;
        }
        break;
    }
    default:
        break;
    }
}

/* ============================ RGB LED sync & stereo VU ============================ */
static const char *LED_ZONES[4] = { "m", "f1", "f2", "lr" };
static const char *LED_ALL_ZONES[6] = { "m", "f1", "f2", "lr", "l", "r" };
static const char *LED_SCALES[3] = { "max_scale", "max_scale_f1f2", "max_scale_lr" };
static char   led_saved_hex[4][16];
static int    led_saved_fx[4];
static int    led_saved_scale[3] = { -1, -1, -1 };
static int    led_saved_anim_en = -1;
static int    led_saved_valid = 0;
static int    led_override = 1;
static int    led_mode = 0; /* 0 = stock, 1 = active frame/sync, 2 = forced off */
static int    led_has_frame = -1;
static char   led_last_frame[256] = "";
static Uint32 led_last_ms = 0;

static void led_put(const char *path, const char *val) {
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(val, f);
        fclose(f);
    }
}

static void led_zone(int z, const char *hex, int fx) {
    char p[96], v[16];
    if (hex) {
        snprintf(p, sizeof(p), "/sys/class/led_anim/effect_rgb_hex_%s", LED_ZONES[z]);
        led_put(p, hex);
    }
    snprintf(p, sizeof(p), "/sys/class/led_anim/effect_%s", LED_ZONES[z]);
    snprintf(v, sizeof(v), "%d", fx);
    led_put(p, v);
}

static void led_disable_effects(void) {
    led_put("/sys/class/led_anim/anim_frames_enable", "0");
    led_put("/sys/class/led_anim/effect_enable", "0");
    led_put("/sys/class/led_anim/enable", "1");
    for (int z = 0; z < 6; z++) {
        char p[96];
        snprintf(p, sizeof(p), "/sys/class/led_anim/effect_%s", LED_ALL_ZONES[z]);
        led_put(p, "0");
        snprintf(p, sizeof(p), "/sys/class/led_anim/effect_cycles_%s", LED_ALL_ZONES[z]);
        led_put(p, "0");
        snprintf(p, sizeof(p), "/sys/class/led_anim/effect_duration_%s", LED_ALL_ZONES[z]);
        led_put(p, "0");
    }
}

static void led_ensure_scales(void) {
    for (int i = 0; i < 3; i++) {
        char p[96], t[16] = "";
        snprintf(p, sizeof(p), "/sys/class/led_anim/%s", LED_SCALES[i]);
        if (sysread(p, t, sizeof(t))) {
            int v = atoi(t);
            if (v <= 0) led_put(p, "128");
        }
    }
}

static void led_write_all_off(void) {
    led_disable_effects();
    if (led_has_frame > 0) {
        static const char *OFF_FRAME =
            "000000 000000 000000 000000 000000 000000 000000 000000 "
            "000000 000000 000000 000000 000000 000000 000000 000000 "
            "000000 000000 000000 000000 000000 000000 000000 ";
        led_put("/sys/class/led_anim/frame_hex", OFF_FRAME);
    } else {
        for (int z = 0; z < 4; z++) led_zone(z, "000000", 0);
    }
}

void platform_led_init(void) {
    char anim_buf[16] = "";
    led_has_frame = (access("/sys/class/led_anim/frame_hex", W_OK) == 0) ? 1 : 0;
    for (int z = 0; z < 4; z++) {
        char p[96], t[16] = "";
        snprintf(p, sizeof(p), "/sys/class/led_anim/effect_rgb_hex_%s", LED_ZONES[z]);
        if (!sysread(p, led_saved_hex[z], sizeof(led_saved_hex[z]))) {
            snprintf(led_saved_hex[z], sizeof(led_saved_hex[z]), "FFFFFF");
        }
        snprintf(p, sizeof(p), "/sys/class/led_anim/effect_%s", LED_ZONES[z]);
        led_saved_fx[z] = sysread(p, t, sizeof(t)) ? atoi(t) : 4;
    }
    for (int i = 0; i < 3; i++) {
        char p[96], t[16] = "";
        snprintf(p, sizeof(p), "/sys/class/led_anim/%s", LED_SCALES[i]);
        led_saved_scale[i] = sysread(p, t, sizeof(t)) ? atoi(t) : -1;
    }
    led_saved_anim_en = sysread("/sys/class/led_anim/anim_frames_enable", anim_buf, sizeof(anim_buf)) ? atoi(anim_buf) : -1;
    led_saved_valid = 1;
    led_mode = 0;
    led_last_frame[0] = '\0';
    fprintf(stderr, "[LED] init: has_frame=%d scales=%d/%d/%d anim_en=%d\n",
            led_has_frame, led_saved_scale[0], led_saved_scale[1], led_saved_scale[2], led_saved_anim_en);
}

void platform_led_shutdown(void) {
    if (!led_saved_valid) return;
    if (led_mode != 0) {
        for (int i = 0; i < 3; i++) {
            if (led_saved_scale[i] >= 0) {
                char p[96], v[16];
                snprintf(p, sizeof(p), "/sys/class/led_anim/%s", LED_SCALES[i]);
                snprintf(v, sizeof(v), "%d", led_saved_scale[i]);
                led_put(p, v);
            }
        }
        if (led_saved_anim_en >= 0) {
            char v[16];
            snprintf(v, sizeof(v), "%d", led_saved_anim_en);
            led_put("/sys/class/led_anim/anim_frames_enable", v);
        }
        led_put("/sys/class/led_anim/effect_enable", "1");
        for (int z = 0; z < 4; z++) {
            led_zone(z, led_saved_hex[z], led_saved_fx[z]);
        }
        led_mode = 0;
    }
}

void platform_led_set_override(int enabled) {
    int want = (enabled != 0);
    if (want == led_override) return;
    led_override = want;
    led_last_frame[0] = '\0';
    led_last_ms = 0;
    if (!led_override) {
        led_mode = 2;
        led_write_all_off();
    }
}

int platform_led_get_override(void) {
    return led_override;
}

static void fmt_scaled_rgb(SDL_Color base, float scale, int dim_clamp, char out[8]) {
    int r = (int)((float)base.r * scale + 0.5f);
    int g = (int)((float)base.g * scale + 0.5f);
    int b = (int)((float)base.b * scale + 0.5f);
    if (dim_clamp) {
        if (r > 38) r = 38;
        if (g > 38) g = 38;
        if (b > 38) b = 38;
        if (r == 0 && g == 0 && b == 0 && (base.r | base.g | base.b)) {
            if (base.r >= base.g && base.r >= base.b) r = 2;
            else if (base.g >= base.r && base.g >= base.b) g = 2;
            else b = 2;
        }
    }
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    snprintf(out, 8, "%02X%02X%02X", r, g, b);
}

void platform_led_update(SDL_Color accent, float vu_l, float vu_r, int playing,
                         int low_power_dim, int bat_pct, int charging) {
    if (led_has_frame < 0) platform_led_init();

    Uint32 now = SDL_GetTicks();

    int low_bat = (bat_pct >= 0 && bat_pct <= 10 && !charging);
    if (low_bat) {
        if (led_mode != 0) platform_led_shutdown();
        return;
    }

    if (!led_override) {
        if (led_mode != 2) {
            led_mode = 2;
            fprintf(stderr, "[LED] mode -> off (override=0)\n");
            led_write_all_off();
        }
        return;
    }

    if (led_mode != 1) {
        led_mode = 1;
        led_last_frame[0] = '\0';
        led_ensure_scales();
        if (led_has_frame > 0) led_disable_effects();
        fprintf(stderr, "[LED] mode -> active (has_frame=%d)\n", led_has_frame);
    }

    if (led_last_ms && (now - led_last_ms) < 70u) return;
    led_last_ms = now;

    if (vu_l < 0.0f) vu_l = 0.0f; else if (vu_l > 1.0f) vu_l = 1.0f;
    if (vu_r < 0.0f) vu_r = 0.0f; else if (vu_r > 1.0f) vu_r = 1.0f;

    float sl, sr, sm;
    if (playing) {
        sl = 0.22f + 0.78f * vu_l;
        sr = 0.22f + 0.78f * vu_r;
        sm = 0.5f * (sl + sr);
    } else {
        sl = sr = sm = 0.28f;
    }

    if (low_power_dim) {
        float k = (float)led_dim_scale / 100.0f;
        sl *= k;
        sr *= k;
        sm *= k;
    }

    char hl[8], hr[8], hm[8], hml[8], hmr[8];
    fmt_scaled_rgb(accent, sl, low_power_dim, hl);
    fmt_scaled_rgb(accent, sr, low_power_dim, hr);
    fmt_scaled_rgb(accent, sm, low_power_dim, hm);
    fmt_scaled_rgb(accent, sl * 0.7f + sr * 0.3f, low_power_dim, hml);
    fmt_scaled_rgb(accent, sr * 0.7f + sl * 0.3f, low_power_dim, hmr);

    if (led_has_frame > 0) {
        /* Self-heal if an external daemon re-enabled led_anim effects/anim_frames or disabled LEDs */
        char fx_en[8] = "", anim_en[8] = "", led_en[8] = "";
        int drifted = 0;
        if (sysread("/sys/class/led_anim/effect_enable", fx_en, sizeof(fx_en)) && fx_en[0] != '0') drifted = 1;
        if (sysread("/sys/class/led_anim/anim_frames_enable", anim_en, sizeof(anim_en)) && anim_en[0] != '0') drifted = 1;
        if (sysread("/sys/class/led_anim/enable", led_en, sizeof(led_en)) && led_en[0] != '1') drifted = 1;
        if (drifted) {
            led_ensure_scales();
            led_disable_effects();
            led_last_frame[0] = '\0';
        }
        /* Trimui Brick 14 LEDs + 9 trailing slots (23 total):
         * 0..3 top-left (L), 4..7 top-right (R), 8=F2 (R), 9=F1 (L),
         * 10..11=L triggers (L), 12..13=R triggers (R), 14..22=mid */
        char frame[256];
        snprintf(frame, sizeof(frame),
                 "%s %s %s %s %s %s %s %s %s %s %s %s %s %s %s %s %s %s %s %s %s %s %s ",
                 hl, hl, hl, hml, hmr, hr, hr, hr,
                 hr, hl, hl, hl, hr, hr,
                 hm, hm, hm, hm, hm, hm, hm, hm, hm);
        if (strcmp(frame, led_last_frame) != 0) {
            snprintf(led_last_frame, sizeof(led_last_frame), "%s", frame);
            led_put("/sys/class/led_anim/frame_hex", frame);
        }
    } else {
        char key[32];
        snprintf(key, sizeof(key), "%s|%s|%s", hm, hl, hr);
        if (strcmp(key, led_last_frame) != 0) {
            snprintf(led_last_frame, sizeof(led_last_frame), "%s", key);
            led_zone(0, hm, 4);
            led_zone(1, hl, 4);
            led_zone(2, hr, 4);
            led_zone(3, hm, 4);
        }
    }
}

/* ============================ CPU frequency control ============================ */
#define CPU_MAX_FREQS 32

static int     cpu_inited = 0;
static CpuMode cpu_mode = CPU_MODE_DEFAULT;
static int     cpu_avail[CPU_MAX_FREQS];
static int     cpu_navail = 0;
static char    cpu_avail_govs[256] = "";
static char    cpu_saved_gov[64] = "";
static int     cpu_saved_min = 0;
static int     cpu_saved_max = 0;
static int     cpu_applied_khz = 0;

static const char *CPU_GOV_PATHS[2] = {
    "/sys/devices/system/cpu/cpufreq/policy0/scaling_governor",
    "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"
};

static int cpu_read_str(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t nr = fread(out, 1, n - 1, f);
    fclose(f);
    out[nr] = '\0';
    while (nr > 0 && (out[nr - 1] == '\n' || out[nr - 1] == '\r' || out[nr - 1] == ' ')) {
        out[--nr] = '\0';
    }
    return nr > 0;
}

static int cpu_read_int(const char *path) {
    char b[32];
    if (!cpu_read_str(path, b, sizeof(b))) return 0;
    return atoi(b);
}

static void cpu_write_str(const char *path, const char *val) {
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(val, f);
        fclose(f);
    }
}

static void cpu_write_int(const char *path, int v) {
    char b[32];
    snprintf(b, sizeof(b), "%d", v);
    cpu_write_str(path, b);
}

static int cmp_int_asc(const void *a, const void *b) {
    int va = *(const int *)a;
    int vb = *(const int *)b;
    return (va > vb) - (va < vb);
}

static void cpu_parse_avail_freqs(void) {
    static const int FALLBACK_FREQS[] = {
        408000, 600000, 816000, 1008000, 1200000, 1416000, 1608000, 1800000, 2000000
    };
    char buf[512] = "";
    cpu_navail = 0;
    if (cpu_read_str("/sys/devices/system/cpu/cpufreq/policy0/scaling_available_frequencies", buf, sizeof(buf)) ||
        cpu_read_str("/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_frequencies", buf, sizeof(buf))) {
        char *p = buf;
        while (*p && cpu_navail < CPU_MAX_FREQS) {
            char *end = NULL;
            long v = strtol(p, &end, 10);
            if (end != p) {
                if (v > 0) cpu_avail[cpu_navail++] = (int)v;
                p = end;
            } else {
                p++;
            }
        }
    }
    if (cpu_navail <= 0) {
        cpu_navail = (int)(sizeof(FALLBACK_FREQS) / sizeof(FALLBACK_FREQS[0]));
        for (int i = 0; i < cpu_navail; i++) cpu_avail[i] = FALLBACK_FREQS[i];
    } else {
        qsort(cpu_avail, (size_t)cpu_navail, sizeof(int), cmp_int_asc);
    }
}

static int cpu_pick_nearest_freq(int target_khz) {
    if (cpu_navail <= 0) return target_khz;
    int best = cpu_avail[0];
    int best_diff = abs(best - target_khz);
    for (int i = 1; i < cpu_navail; i++) {
        int d = abs(cpu_avail[i] - target_khz);
        if (d < best_diff) {
            best_diff = d;
            best = cpu_avail[i];
        }
    }
    return best;
}

static int cpu_gov_supported(const char *gov) {
    if (!cpu_avail_govs[0]) return 1;
    size_t glen = strlen(gov);
    const char *p = cpu_avail_govs;
    while ((p = strstr(p, gov)) != NULL) {
        int left_ok = (p == cpu_avail_govs || p[-1] == ' ' || p[-1] == '\t');
        int right_ok = (p[glen] == '\0' || p[glen] == ' ' || p[glen] == '\t' || p[glen] == '\n');
        if (left_ok && right_ok) return 1;
        p += glen;
    }
    return 0;
}

static const char *cpu_pick_gov(int want_perf) {
    if (want_perf) {
        if (cpu_gov_supported("performance")) return "performance";
    } else {
        static const char *CANDS[] = { "ondemand", "schedutil", "conservative", "interactive", "powersave", NULL };
        for (int i = 0; CANDS[i]; i++) {
            if (cpu_gov_supported(CANDS[i])) return CANDS[i];
        }
    }
    return cpu_saved_gov[0] ? cpu_saved_gov : "ondemand";
}

static void cpu_write_gov(const char *gov) {
    if (!gov || !gov[0]) return;
    cpu_write_str(CPU_GOV_PATHS[0], gov);
    for (int i = 0; i < 4; i++) {
        char p[96];
        snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", i);
        cpu_write_str(p, gov);
    }
}

static void cpu_write_freq_range(int min_khz, int max_khz) {
    if (min_khz <= 0 || max_khz <= 0) return;
    if (min_khz > max_khz) min_khz = max_khz;
    int floor_khz = (cpu_navail > 0 && cpu_avail[0] < min_khz) ? cpu_avail[0] : min_khz;

    /* Lower min first so max_freq write never fails EINVAL when max < current_min. */
    cpu_write_int("/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq", floor_khz);
    for (int i = 0; i < 4; i++) {
        char p[96];
        snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_min_freq", i);
        cpu_write_int(p, floor_khz);
    }

    cpu_write_int("/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq", max_khz);
    for (int i = 0; i < 4; i++) {
        char p[96];
        snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", i);
        cpu_write_int(p, max_khz);
    }

    if (min_khz != floor_khz) {
        cpu_write_int("/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq", min_khz);
        for (int i = 0; i < 4; i++) {
            char p[96];
            snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_min_freq", i);
            cpu_write_int(p, min_khz);
        }
    }
}

void platform_cpu_init(void) {
    if (cpu_inited) return;
    cpu_inited = 1;
    cpu_parse_avail_freqs();
    if (!cpu_read_str("/sys/devices/system/cpu/cpufreq/policy0/scaling_available_governors",
                      cpu_avail_govs, sizeof(cpu_avail_govs))) {
        cpu_read_str("/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors",
                     cpu_avail_govs, sizeof(cpu_avail_govs));
    }
    if (!cpu_read_str(CPU_GOV_PATHS[0], cpu_saved_gov, sizeof(cpu_saved_gov))) {
        cpu_read_str(CPU_GOV_PATHS[1], cpu_saved_gov, sizeof(cpu_saved_gov));
    }
    cpu_saved_min = cpu_read_int("/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq");
    if (cpu_saved_min <= 0) cpu_saved_min = cpu_read_int("/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq");
    cpu_saved_max = cpu_read_int("/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq");
    if (cpu_saved_max <= 0) cpu_saved_max = cpu_read_int("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq");
    cpu_applied_khz = cpu_saved_max;
}

void platform_cpu_shutdown(void) {
    if (!cpu_inited) return;
    if (cpu_mode != CPU_MODE_DEFAULT) {
        if (cpu_saved_min > 0 && cpu_saved_max > 0) {
            cpu_write_freq_range(cpu_saved_min, cpu_saved_max);
        }
        if (cpu_saved_gov[0]) {
            cpu_write_gov(cpu_saved_gov);
        }
        cpu_mode = CPU_MODE_DEFAULT;
    }
}

void platform_cpu_set_mode(CpuMode mode) {
    if (!cpu_inited) platform_cpu_init();
    if ((int)mode < 0 || mode >= CPU_MODE_COUNT) mode = CPU_MODE_DEFAULT;
    cpu_mode = mode;

    if (mode == CPU_MODE_DEFAULT) {
        if (cpu_saved_min > 0 && cpu_saved_max > 0) {
            cpu_write_freq_range(cpu_saved_min, cpu_saved_max);
            cpu_applied_khz = cpu_saved_max;
        }
        if (cpu_saved_gov[0]) cpu_write_gov(cpu_saved_gov);
        return;
    }

    int min_khz = (cpu_navail > 0) ? cpu_avail[0] : 408000;
    int max_khz = (cpu_navail > 0) ? cpu_avail[cpu_navail - 1] : 2000000;
    const char *gov = NULL;

    if (mode == CPU_MODE_ULTRA_LOW) {
        max_khz = cpu_pick_nearest_freq(600000);
        gov = cpu_pick_gov(0);
    } else if (mode == CPU_MODE_LOW) {
        max_khz = cpu_pick_nearest_freq(816000);
        gov = cpu_pick_gov(0);
    } else if (mode == CPU_MODE_BALANCED) {
        max_khz = cpu_pick_nearest_freq(1200000);
        gov = cpu_pick_gov(0);
    } else if (mode == CPU_MODE_PERF) {
        min_khz = max_khz;
        gov = cpu_pick_gov(1);
    }

    cpu_write_gov(gov);
    cpu_write_freq_range(min_khz, max_khz);
    cpu_applied_khz = max_khz;
}

CpuMode platform_cpu_get_mode(void) {
    return cpu_mode;
}

const char *platform_cpu_mode_label(CpuMode mode) {
    switch (mode) {
    case CPU_MODE_DEFAULT:   return "系统默认";
    case CPU_MODE_ULTRA_LOW: return "极度省电 600M";
    case CPU_MODE_LOW:       return "省电降温 816M";
    case CPU_MODE_BALANCED:  return "均衡模式 1.2G";
    case CPU_MODE_PERF:      return "高性能 (最高频)";
    default:                 return "系统默认";
    }
}

int platform_cpu_applied_mhz(void) {
    return (cpu_applied_khz > 0) ? (cpu_applied_khz / 1000) : 0;
}
