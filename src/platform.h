#ifndef PLATFORM_H
#define PLATFORM_H

#include <SDL.h>

enum {
    BT_UP = 0,
    BT_DOWN,
    BT_LEFT,
    BT_RIGHT,
    BT_A,
    BT_B,
    BT_X,
    BT_Y,
    BT_L1,
    BT_R1,
    BT_L2,
    BT_R2,
    BT_SELECT,
    BT_START,
    BT_MENU,
    BT_COUNT
};

enum {
    INPUT_NONE = 0,
    INPUT_PRESS,
    INPUT_DIR_PRESS,
    INPUT_DIR_RELEASE,
    INPUT_QUIT
};

typedef struct {
    int kind;
    int btn;
} InputEvent;

typedef struct {
    int bat_pct;
    int charging;
    int wifi_up;
    int clock_12h;
    char clock_str[16];
} PlatformStatus;

void platform_load_buttons(const char *path);
int  platform_disp_get_brightness(void);
void platform_disp_set_brightness(int v);
void platform_screen_off(SDL_Renderer *ren);
void platform_screen_wake(void);
int  platform_is_screen_off(void);
void platform_set_dim_mode(int on);
int  platform_is_dim_mode(void);
int  platform_dim_overlay_alpha(void);
void platform_restore_brightness(void);
void platform_poll_status(PlatformStatus *st);
void platform_translate_event(const SDL_Event *e, InputEvent *out);
int  platform_clock_12h(void);

/* LED synchronization & stereo VU reactive lighting */
void platform_led_init(void);
void platform_led_shutdown(void);
void platform_led_set_override(int enabled);
int  platform_led_get_override(void);
void platform_led_update(SDL_Color accent, float vu_l, float vu_r, int playing,
                         int low_power_dim, int bat_pct, int charging);

/* CPU frequency & governor management (Allwinner A133P / BrickTools compatible) */
typedef enum {
    CPU_MODE_DEFAULT = 0,
    CPU_MODE_ULTRA_LOW,
    CPU_MODE_LOW,
    CPU_MODE_BALANCED,
    CPU_MODE_PERF,
    CPU_MODE_COUNT
} CpuMode;

void        platform_cpu_init(void);
void        platform_cpu_shutdown(void);
void        platform_cpu_set_mode(CpuMode mode);
CpuMode     platform_cpu_get_mode(void);
const char *platform_cpu_mode_label(CpuMode mode);
int         platform_cpu_applied_mhz(void);

#endif
