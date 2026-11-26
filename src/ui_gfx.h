#ifndef UI_GFX_H
#define UI_GFX_H

#include <SDL.h>
#include <stddef.h>
#include <stdint.h>

enum {
    F_XS = 0, /* 20px */
    F_S,      /* 24px */
    F_M,      /* 28px */
    F_L,      /* 34px */
    F_XL,     /* 42px */
    F_COUNT
};

enum {
    ALIGN_LEFT = 0,
    ALIGN_CENTER = 1,
    ALIGN_RIGHT = 2
};

int           ui_init(int w, int h, const char *title);
void          ui_shutdown(void);
SDL_Renderer *ui_renderer(void);
SDL_Window   *ui_window(void);
void          ui_tc_clear(void);

int  ui_draw_text(int font_idx, const char *s, int x, int y, int maxw, int align, SDL_Color col);
int  ui_draw_text_wrap(int font_idx, const char *s, int x, int y, int maxw, int maxh, SDL_Color col);
int  ui_draw_text_wrap_align(int font_idx, const char *s, int x, int y, int maxw, int maxh, int align, SDL_Color col);
int  ui_measure_text_wrap(int font_idx, const char *s, int maxw, int maxh, SDL_Color col);
int  ui_text_width(int font_idx, const char *s);

void ui_fill(int x, int y, int w, int h, SDL_Color col);
void ui_fill_round(int x, int y, int w, int h, int r, SDL_Color col);
void ui_rect_outline(int x, int y, int w, int h, int thickness, SDL_Color col);
void ui_draw_placeholder(int x, int y, int sz, int big);

/* Decodes JPEG/PNG in memory to tightly packed RGBA8888. Caller frees returned buffer. */
uint8_t     *ui_decode_image_rgba(const uint8_t *buf, size_t len, int target_size, int round_r,
                                   int *out_w, int *out_h, SDL_Color *out_accent);
SDL_Texture *ui_tex_from_rgba(const uint8_t *rgba, int w, int h);

#endif
