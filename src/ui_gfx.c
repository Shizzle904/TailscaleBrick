#if !defined(_POSIX_C_SOURCE) && !defined(_GNU_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "ui_gfx.h"

#include <SDL_ttf.h>
#include <glob.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

/* ============================ graphics state ============================ */
#define NFB 10
#define TCN 400
#define WRAP_FLAG 0x100
#define ALIGN_SHIFT 10

static SDL_Window   *win = NULL;
static SDL_Renderer *ren = NULL;
static SDL_Joystick *joy = NULL;

static TTF_Font *fonts[F_COUNT];
static TTF_Font *fbf[NFB][F_COUNT];
static int nfb = 0;

typedef struct {
    uint32_t h;
    uint32_t col;
    int font;
    int maxw;
    int maxh;
    int w;
    int hgt;
    char *s;
    SDL_Texture *t;
} TC;

static TC tc[TCN];
static int tc_next = 0;

void ui_tc_clear(void) {
    for (int i = 0; i < TCN; i++) {
        if (tc[i].t) SDL_DestroyTexture(tc[i].t);
        free(tc[i].s);
        memset(&tc[i], 0, sizeof(tc[i]));
    }
    tc_next = 0;
}

int ui_init(int w, int h, const char *title) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) < 0) {
        return -1;
    }
    if (TTF_Init() < 0) {
        SDL_Quit();
        return -1;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");

    win = SDL_CreateWindow(title ? title : "Kikoeru",
                           SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                           w, h, SDL_WINDOW_SHOWN);
    if (!win) {
        TTF_Quit();
        SDL_Quit();
        return -1;
    }

    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) {
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!ren) {
        SDL_DestroyWindow(win);
        win = NULL;
        TTF_Quit();
        SDL_Quit();
        return -1;
    }

    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_ShowCursor(SDL_DISABLE);

    if (SDL_NumJoysticks() > 0) {
        joy = SDL_JoystickOpen(0);
    }

    static const char *FONT_PATHS[] = {
        "./fonts/font.otf",
        "/mnt/SDCARD/Apps/Vibe/fonts/font.otf",
        "/mnt/SDCARD/Apps/FileManager/res/DroidSansFallback.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        NULL
    };
    static const int px[F_COUNT] = { 20, 24, 28, 34, 42 };

    for (int i = 0; i < F_COUNT; i++) {
        fonts[i] = NULL;
        for (int p = 0; FONT_PATHS[p] && !fonts[i]; p++) {
            fonts[i] = TTF_OpenFont(FONT_PATHS[p], px[i]);
        }
    }

    nfb = 0;
    static const char *FB_GLOBS[] = {
        "./fonts/fb_*.ttf",
        "/mnt/SDCARD/Apps/Vibe/fonts/fb_*.ttf",
        NULL
    };
    for (int gp = 0; FB_GLOBS[gp] && nfb < NFB; gp++) {
        glob_t fg;
        if (glob(FB_GLOBS[gp], 0, NULL, &fg) == 0) {
            for (size_t k = 0; k < fg.gl_pathc && nfb < NFB; k++) {
                int any = 0;
                for (int i = 0; i < F_COUNT; i++) {
                    fbf[nfb][i] = TTF_OpenFont(fg.gl_pathv[k], px[i]);
                    if (fbf[nfb][i]) any = 1;
                }
                if (any) nfb++;
            }
            globfree(&fg);
        }
    }

    return 0;
}

void ui_shutdown(void) {
    ui_tc_clear();
    for (int i = 0; i < F_COUNT; i++) {
        if (fonts[i]) {
            TTF_CloseFont(fonts[i]);
            fonts[i] = NULL;
        }
    }
    for (int j = 0; j < NFB; j++) {
        for (int i = 0; i < F_COUNT; i++) {
            if (fbf[j][i]) {
                TTF_CloseFont(fbf[j][i]);
                fbf[j][i] = NULL;
            }
        }
    }
    nfb = 0;
    if (joy) {
        SDL_JoystickClose(joy);
        joy = NULL;
    }
    if (ren) {
        SDL_DestroyRenderer(ren);
        ren = NULL;
    }
    if (win) {
        SDL_DestroyWindow(win);
        win = NULL;
    }
    TTF_Quit();
    SDL_Quit();
}

SDL_Renderer *ui_renderer(void) { return ren; }
SDL_Window   *ui_window(void)   { return win; }

/* ============================ UTF-8 & font fallback ============================ */
static int u8next(const char **pp) {
    const unsigned char *p = (const unsigned char *)*pp;
    int cp, n;
    if (p[0] < 0x80) {
        cp = p[0];
        n = 1;
    } else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        n = 2;
    } else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        n = 3;
    } else if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        n = 4;
    } else {
        cp = 0xFFFD;
        n = 1;
    }
    *pp += n;
    return cp;
}

static TTF_Font *font_for(int f, int cp) {
    if (cp <= 0x20 || cp > 0xFFFF || TTF_GlyphIsProvided(fonts[f], (Uint16)cp)) {
        return fonts[f];
    }
    for (int j = 0; j < nfb; j++) {
        if (fbf[j][f] && TTF_GlyphIsProvided(fbf[j][f], (Uint16)cp)) {
            return fbf[j][f];
        }
    }
    return fonts[f];
}

#define MAX_RUNS 64

typedef struct {
    TTF_Font *ft;
    const char *ptr;
    int len;
} Run;

static int split_runs(int f, const char *s, Run *runs, int max_runs) {
    int n = 0;
    for (const char *p = s; *p; ) {
        const char *nx = p;
        int cp = u8next(&nx);
        TTF_Font *ft = font_for(f, cp);
        if (n == 0 || runs[n - 1].ft != ft) {
            if (n >= max_runs) break;
            runs[n].ft = ft;
            runs[n].ptr = p;
            runs[n].len = 0;
            n++;
        }
        runs[n - 1].len += (int)(nx - p);
        p = nx;
    }
    return n;
}

static char *dup_slice(const char *s, int len, char *stack_buf, size_t stack_sz) {
    if ((size_t)len < stack_sz) {
        memcpy(stack_buf, s, (size_t)len);
        stack_buf[len] = '\0';
        return stack_buf;
    }
    char *heap = malloc((size_t)len + 1);
    if (!heap) return NULL;
    memcpy(heap, s, (size_t)len);
    heap[len] = '\0';
    return heap;
}

static int chain_width(int f, const char *s) {
    if (f < 0 || f >= F_COUNT || !fonts[f] || !s || !*s) return 0;
    Run runs[MAX_RUNS];
    int n = split_runs(f, s, runs, MAX_RUNS);
    int total = 0;
    char sbuf[512];
    for (int i = 0; i < n; i++) {
        char *tmp = dup_slice(runs[i].ptr, runs[i].len, sbuf, sizeof(sbuf));
        if (!tmp) continue;
        int w = 0;
        TTF_SizeUTF8(runs[i].ft, tmp, &w, NULL);
        total += w;
        if (tmp != sbuf) free(tmp);
    }
    return total;
}

static SDL_Surface *render_chain(int f, const char *s, SDL_Color c) {
    if (f < 0 || f >= F_COUNT || !fonts[f] || !s || !*s) return NULL;
    Run runs[MAX_RUNS];
    int n = split_runs(f, s, runs, MAX_RUNS);
    if (n == 0) return NULL;

    char sbuf[512];
    if (n == 1) {
        char *tmp = dup_slice(runs[0].ptr, runs[0].len, sbuf, sizeof(sbuf));
        if (!tmp) return NULL;
        SDL_Surface *sf = TTF_RenderUTF8_Blended(runs[0].ft, tmp, c);
        if (tmp != sbuf) free(tmp);
        return sf;
    }

    SDL_Surface *part[MAX_RUNS];
    int w = 0;
    int h = TTF_FontHeight(fonts[f]);
    int asc = TTF_FontAscent(fonts[f]);

    for (int i = 0; i < n; i++) {
        part[i] = NULL;
        char *tmp = dup_slice(runs[i].ptr, runs[i].len, sbuf, sizeof(sbuf));
        if (tmp) {
            part[i] = TTF_RenderUTF8_Blended(runs[i].ft, tmp, c);
            if (tmp != sbuf) free(tmp);
        }
        if (part[i]) {
            w += part[i]->w;
            if (part[i]->h > h) h = part[i]->h;
        }
    }

    SDL_Surface *out = SDL_CreateRGBSurfaceWithFormat(0, w > 0 ? w : 1, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (out) {
        SDL_FillRect(out, NULL, 0);
        int x = 0;
        for (int i = 0; i < n; i++) {
            if (!part[i]) continue;
            int dy = asc - TTF_FontAscent(runs[i].ft);
            if (dy < 0) dy = 0;
            SDL_Rect d = { x, dy, 0, 0 };
            SDL_SetSurfaceBlendMode(part[i], SDL_BLENDMODE_NONE);
            SDL_BlitSurface(part[i], NULL, out, &d);
            x += part[i]->w;
        }
    }
    for (int i = 0; i < n; i++) {
        if (part[i]) SDL_FreeSurface(part[i]);
    }
    return out;
}

/* Word-wrap with CJK character-level fallback (no spaces required) within maxw and maxh. */
static SDL_Surface *render_wrapped(int f, const char *s, int maxw, int maxh, int align, SDL_Color c) {
    if (maxw <= 0 || !fonts[f] || !s || !*s) return NULL;

    int skip = TTF_FontLineSkip(fonts[f]);
    int fh = TTF_FontHeight(fonts[f]);
    if (skip <= 0) skip = fh > 0 ? fh : 24;

    int max_lines = 32;
    if (maxh > 0) {
        max_lines = maxh / skip;
        if (max_lines < 1 && maxh >= fh) max_lines = 1;
        if (max_lines <= 0) return NULL;
        if (max_lines > 64) max_lines = 64;
    }

    enum { LINE_CAP = 512 };
    char **lines = calloc((size_t)max_lines, sizeof(char *));
    if (!lines) return NULL;

    char cur[LINE_CAP];
    int nl = 0;
    int cl = 0;
    int lastsp = -1;
    cur[0] = '\0';

    const char *p = s;
    while (*p && nl < max_lines) {
        const char *nx = p;
        int cp = u8next(&nx);
        int n = (int)(nx - p);

        if (cp == '\r') {
            p = nx;
            continue;
        }
        if (cp == '\n') {
            lines[nl] = malloc((size_t)cl + 1);
            if (lines[nl]) memcpy(lines[nl], cur, (size_t)cl + 1);
            nl++;
            cl = 0;
            cur[0] = '\0';
            lastsp = -1;
            p = nx;
            continue;
        }
        if (cl + n >= LINE_CAP - 1) {
            lines[nl] = malloc((size_t)cl + 1);
            if (lines[nl]) memcpy(lines[nl], cur, (size_t)cl + 1);
            nl++;
            cl = 0;
            cur[0] = '\0';
            lastsp = -1;
            if (nl >= max_lines) break;
        }

        memcpy(cur + cl, p, (size_t)n);
        cl += n;
        cur[cl] = '\0';
        if (cp == ' ' || cp == '\t') lastsp = cl - n;

        if (cl > n && chain_width(f, cur) > maxw) {
            /* Break at space only if it's reasonably near the end; otherwise break cleanly at UTF-8 char (CJK). */
            if (lastsp > 0 && lastsp >= cl / 2) {
                lines[nl] = malloc((size_t)lastsp + 1);
                if (lines[nl]) {
                    memcpy(lines[nl], cur, (size_t)lastsp);
                    lines[nl][lastsp] = '\0';
                }
                nl++;
                int rem = cl - (lastsp + 1);
                if (rem > 0) memmove(cur, cur + lastsp + 1, (size_t)rem);
                cur[rem] = '\0';
                cl = rem;
            } else {
                int keep = cl - n;
                lines[nl] = malloc((size_t)keep + 1);
                if (lines[nl]) {
                    memcpy(lines[nl], cur, (size_t)keep);
                    lines[nl][keep] = '\0';
                }
                nl++;
                memcpy(cur, p, (size_t)n);
                cur[n] = '\0';
                cl = n;
            }
            lastsp = -1;
        }
        p = nx;
    }
    if (cl > 0 && nl < max_lines) {
        lines[nl] = malloc((size_t)cl + 1);
        if (lines[nl]) memcpy(lines[nl], cur, (size_t)cl + 1);
        nl++;
    }

    if (nl == 0) {
        free(lines);
        return NULL;
    }

    SDL_Surface **ls = calloc((size_t)nl, sizeof(SDL_Surface *));
    SDL_Surface *out = NULL;
    if (ls) {
        int w = 1;
        for (int i = 0; i < nl; i++) {
            if (lines[i] && lines[i][0]) {
                ls[i] = render_chain(f, lines[i], c);
                if (ls[i] && ls[i]->w > w) w = ls[i]->w;
            }
        }
        int total_h = (nl - 1) * skip + (fh > 0 ? fh : skip);
        if (maxh > 0 && total_h > maxh) total_h = maxh;
        out = SDL_CreateRGBSurfaceWithFormat(0, w, total_h > 0 ? total_h : 1, 32, SDL_PIXELFORMAT_ARGB8888);
        if (out) {
            SDL_FillRect(out, NULL, 0);
            for (int i = 0; i < nl; i++) {
                if (!ls[i]) continue;
                SDL_SetSurfaceBlendMode(ls[i], SDL_BLENDMODE_NONE);
                int dx = 0;
                if (align == ALIGN_CENTER) dx = (w - ls[i]->w) / 2;
                else if (align == ALIGN_RIGHT) dx = w - ls[i]->w;
                SDL_Rect d = { dx, i * skip, 0, 0 };
                SDL_BlitSurface(ls[i], NULL, out, &d);
            }
        }
        for (int i = 0; i < nl; i++) {
            if (ls[i]) SDL_FreeSurface(ls[i]);
        }
        free(ls);
    }

    for (int i = 0; i < nl; i++) free(lines[i]);
    free(lines);
    return out;
}

static TC *text_get(int fw, const char *s, int maxw, int maxh, SDL_Color c) {
    int wrap = fw & WRAP_FLAG;
    int align = (fw >> ALIGN_SHIFT) & 3;
    int f = fw & 0xff;
    if (!ren || f < 0 || f >= F_COUNT || !fonts[f] || !s || !*s) return NULL;

    uint32_t col = ((uint32_t)c.a << 24) | ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h = (h ^ *p) * 16777619u;
    }
    h ^= (uint32_t)(fw * 131 + maxw * 7 + maxh * 31) ^ col;

    for (int i = 0; i < TCN; i++) {
        if (tc[i].t && tc[i].h == h && tc[i].font == fw &&
            tc[i].maxw == maxw && tc[i].maxh == maxh &&
            tc[i].col == col && strcmp(tc[i].s, s) == 0) {
            return &tc[i];
        }
    }

    SDL_Surface *sf = NULL;
    if (wrap) {
        sf = render_wrapped(f, s, maxw, maxh, align, c);
    } else {
        char buf[1024];
        snprintf(buf, sizeof(buf), "%s", s);
        int w = chain_width(f, buf);
        if (maxw > 0 && w > maxw) {
            size_t n = strlen(buf);
            int ok = 0;
            while (n > 0) {
                n--;
                while (n > 0 && (((unsigned char)buf[n]) & 0xC0) == 0x80) n--;
                buf[n] = '\0';
                char t[1040];
                snprintf(t, sizeof(t), "%.1020s...", buf);
                if (chain_width(f, t) <= maxw) {
                    snprintf(buf, sizeof(buf), "%.1023s", t);
                    ok = 1;
                    break;
                }
            }
            if (!ok) snprintf(buf, sizeof(buf), "...");
        }
        sf = render_chain(f, buf, c);
    }
    if (!sf) return NULL;

    TC *e = &tc[tc_next];
    tc_next = (tc_next + 1) % TCN;
    if (e->t) SDL_DestroyTexture(e->t);
    free(e->s);
    memset(e, 0, sizeof(*e));

    e->t = SDL_CreateTextureFromSurface(ren, sf);
    e->w = sf->w;
    e->hgt = sf->h;
    SDL_FreeSurface(sf);
    if (!e->t) return NULL;

    e->s = strdup(s);
    if (!e->s) {
        SDL_DestroyTexture(e->t);
        e->t = NULL;
        return NULL;
    }
    e->h = h;
    e->col = col;
    e->font = fw;
    e->maxw = maxw;
    e->maxh = maxh;
    return e;
}

int ui_text_width(int font_idx, const char *s) {
    return chain_width(font_idx, s);
}

int ui_draw_text(int font_idx, const char *s, int x, int y, int maxw, int align, SDL_Color col) {
    TC *t = text_get(font_idx, s, maxw, 0, col);
    if (!t) return 0;
    SDL_Rect d = { x, y, t->w, t->hgt };
    if (align == ALIGN_CENTER) d.x = x - t->w / 2;
    else if (align == ALIGN_RIGHT) d.x = x - t->w;
    SDL_RenderCopy(ren, t->t, NULL, &d);
    return t->w;
}

int ui_draw_text_wrap_align(int font_idx, const char *s, int x, int y, int maxw, int maxh, int align, SDL_Color col) {
    int fw = font_idx | WRAP_FLAG | ((align & 3) << ALIGN_SHIFT);
    TC *t = text_get(fw, s, maxw, maxh, col);
    if (!t) return 0;
    int h = (maxh > 0 && t->hgt > maxh) ? maxh : t->hgt;
    int sw = (maxw > 0 && t->w > maxw) ? maxw : t->w;
    int dx = x;
    if (align == ALIGN_CENTER && maxw > sw) dx = x + (maxw - sw) / 2;
    else if (align == ALIGN_RIGHT && maxw > sw) dx = x + (maxw - sw);
    SDL_Rect src = { 0, 0, sw, h };
    SDL_Rect d = { dx, y, sw, h };
    SDL_RenderCopy(ren, t->t, &src, &d);
    return h;
}

int ui_draw_text_wrap(int font_idx, const char *s, int x, int y, int maxw, int maxh, SDL_Color col) {
    return ui_draw_text_wrap_align(font_idx, s, x, y, maxw, maxh, ALIGN_LEFT, col);
}

int ui_measure_text_wrap(int font_idx, const char *s, int maxw, int maxh, SDL_Color col) {
    int fw = font_idx | WRAP_FLAG | (ALIGN_CENTER << ALIGN_SHIFT);
    TC *t = text_get(fw, s, maxw, maxh, col);
    if (!t) return 0;
    return (maxh > 0 && t->hgt > maxh) ? maxh : t->hgt;
}

/* ============================ primitives ============================ */
void ui_fill(int x, int y, int w, int h, SDL_Color col) {
    if (!ren || w <= 0 || h <= 0) return;
    SDL_SetRenderDrawColor(ren, col.r, col.g, col.b, col.a);
    SDL_Rect d = { x, y, w, h };
    SDL_RenderFillRect(ren, &d);
}

void ui_fill_round(int x, int y, int w, int h, int r, SDL_Color col) {
    if (!ren || w <= 0 || h <= 0) return;
    if (r <= 0) {
        ui_fill(x, y, w, h, col);
        return;
    }
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    SDL_SetRenderDrawColor(ren, col.r, col.g, col.b, col.a);
    for (int i = 0; i < h; i++) {
        float off = (i < r) ? (r - i - 0.5f) : (i >= h - r ? (i - (h - r) + 0.5f) : 0.f);
        int inset = (off > 0.f) ? r - (int)sqrtf((float)(r * r) - off * off) : 0;
        if (inset < 0) inset = 0;
        if (x + inset <= x + w - 1 - inset) {
            SDL_RenderDrawLine(ren, x + inset, y + i, x + w - 1 - inset, y + i);
        }
    }
}

void ui_rect_outline(int x, int y, int w, int h, int thickness, SDL_Color col) {
    if (!ren || w <= 0 || h <= 0 || thickness <= 0) return;
    if (thickness * 2 >= w || thickness * 2 >= h) {
        ui_fill(x, y, w, h, col);
        return;
    }
    SDL_SetRenderDrawColor(ren, col.r, col.g, col.b, col.a);
    SDL_Rect rects[4] = {
        { x, y, w, thickness },
        { x, y + h - thickness, w, thickness },
        { x, y + thickness, thickness, h - 2 * thickness },
        { x + w - thickness, y + thickness, thickness, h - 2 * thickness }
    };
    SDL_RenderFillRects(ren, rects, 4);
}

void ui_draw_placeholder(int x, int y, int sz, int big) {
    if (!ren || sz <= 0) return;
    SDL_Color bg = { 44, 44, 54, 255 };
    SDL_Color dim = { 120, 120, 132, 255 };
    ui_fill_round(x, y, sz, sz, big ? 0 : (sz * 12 / 100), bg);
    TC *t = text_get(F_L, "\xE2\x99\xAA", 0, 0, dim);
    if (t && t->hgt > 0) {
        int dh = big ? (sz / 3) : t->hgt;
        if (dh < t->hgt) dh = t->hgt;
        int dw = t->w * dh / t->hgt;
        SDL_Rect d = { x + (sz - dw) / 2, y + (sz - dh) / 2, dw, dh };
        SDL_RenderCopy(ren, t->t, NULL, &d);
    }
}

/* ============================ image decoding ============================ */
static SDL_Color neon_color(const uint8_t *rgba, int w, int h) {
    double R = 0.0, G = 0.0, B = 0.0, Wt = 0.0;
    int step = (w >= 16 && h >= 16) ? 4 : 1;
    for (int y = 0; y < h; y += step) {
        for (int x = 0; x < w; x += step) {
            const uint8_t *p = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4;
            int mx = p[0] > p[1] ? p[0] : p[1];
            if (p[2] > mx) mx = p[2];
            int mn = p[0] < p[1] ? p[0] : p[1];
            if (p[2] < mn) mn = p[2];
            double wt = (mx - mn) + 16.0;
            if (mx < 40) wt *= 0.1;
            R += p[0] * wt;
            G += p[1] * wt;
            B += p[2] * wt;
            Wt += wt;
        }
    }
    if (Wt <= 0.0) return (SDL_Color){ 0, 220, 255, 255 };

    double r = R / Wt / 255.0;
    double g = G / Wt / 255.0;
    double b = B / Wt / 255.0;
    double mx = fmax(r, fmax(g, b));
    double mn = fmin(r, fmin(g, b));
    double d = mx - mn;
    double hue = 0.0;
    double sat = (mx > 0.0) ? (d / mx) : 0.0;
    if (d > 0.0) {
        if (mx == r) hue = fmod((g - b) / d, 6.0);
        else if (mx == g) hue = (b - r) / d + 2.0;
        else hue = (r - g) / d + 4.0;
        hue /= 6.0;
        if (hue < 0.0) hue += 1.0;
    }
    if (sat > 0.12) sat = fmax(sat, 0.8);
    else sat = 0.0;

    double v = 1.0;
    double c = v * sat;
    double hh = hue * 6.0;
    double xx = c * (1.0 - fabs(fmod(hh, 2.0) - 1.0));
    double m = v - c;
    double rr = 0.0, gg = 0.0, bb = 0.0;
    if (hh < 1.0)      { rr = c;  gg = xx; bb = 0; }
    else if (hh < 2.0) { rr = xx; gg = c;  bb = 0; }
    else if (hh < 3.0) { rr = 0;  gg = c;  bb = xx; }
    else if (hh < 4.0) { rr = 0;  gg = xx; bb = c; }
    else if (hh < 5.0) { rr = xx; gg = 0;  bb = c; }
    else               { rr = c;  gg = 0;  bb = xx; }

    return (SDL_Color){
        (uint8_t)((rr + m) * 255.0),
        (uint8_t)((gg + m) * 255.0),
        (uint8_t)((bb + m) * 255.0),
        255
    };
}

uint8_t *ui_decode_image_rgba(const uint8_t *buf, size_t len, int target_size, int round_r,
                               int *out_w, int *out_h, SDL_Color *out_accent) {
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (out_accent) *out_accent = (SDL_Color){ 0, 220, 255, 255 };
    if (!buf || len == 0 || len > (size_t)0x7fffffff) return NULL;

    int src_w = 0, src_h = 0, comp = 0;
    uint8_t *src = stbi_load_from_memory(buf, (int)len, &src_w, &src_h, &comp, 4);
    if (!src || src_w <= 0 || src_h <= 0) {
        if (src) stbi_image_free(src);
        return NULL;
    }

    uint8_t *rgba = NULL;
    int w = src_w;
    int h = src_h;

    if (target_size > 0) {
        int crop = (src_w < src_h) ? src_w : src_h;
        int ox = (src_w - crop) / 2;
        int oy = (src_h - crop) / 2;
        uint8_t *dst = malloc((size_t)target_size * (size_t)target_size * 4);
        if (!dst) {
            stbi_image_free(src);
            return NULL;
        }
        if (crop >= target_size * 2) {
            for (int y = 0; y < target_size; y++) {
                int y0 = oy + y * crop / target_size;
                int y1 = oy + (y + 1) * crop / target_size;
                if (y1 <= y0) y1 = y0 + 1;
                for (int x = 0; x < target_size; x++) {
                    int x0 = ox + x * crop / target_size;
                    int x1 = ox + (x + 1) * crop / target_size;
                    if (x1 <= x0) x1 = x0 + 1;
                    unsigned long s0 = 0, s1 = 0, s2 = 0, s3 = 0;
                    unsigned long cnt = 0;
                    for (int yy = y0; yy < y1; yy++) {
                        for (int xx = x0; xx < x1; xx++) {
                            const uint8_t *p = src + ((size_t)yy * (size_t)src_w + (size_t)xx) * 4;
                            s0 += p[0]; s1 += p[1]; s2 += p[2]; s3 += p[3];
                            cnt++;
                        }
                    }
                    uint8_t *o = dst + ((size_t)y * (size_t)target_size + (size_t)x) * 4;
                    o[0] = (uint8_t)(s0 / cnt);
                    o[1] = (uint8_t)(s1 / cnt);
                    o[2] = (uint8_t)(s2 / cnt);
                    o[3] = (uint8_t)(s3 / cnt);
                }
            }
        } else {
            for (int y = 0; y < target_size; y++) {
                float gy = ((y + 0.5f) * crop / (float)target_size) - 0.5f;
                if (gy < 0.f) gy = 0.f;
                if (gy > (float)(crop - 1)) gy = (float)(crop - 1);
                int y0 = (int)gy;
                int y1 = (y0 + 1 < crop) ? (y0 + 1) : y0;
                float fy = gy - (float)y0;
                for (int x = 0; x < target_size; x++) {
                    float gx = ((x + 0.5f) * crop / (float)target_size) - 0.5f;
                    if (gx < 0.f) gx = 0.f;
                    if (gx > (float)(crop - 1)) gx = (float)(crop - 1);
                    int x0 = (int)gx;
                    int x1 = (x0 + 1 < crop) ? (x0 + 1) : x0;
                    float fx = gx - (float)x0;

                    const uint8_t *p00 = src + ((size_t)(oy + y0) * (size_t)src_w + (size_t)(ox + x0)) * 4;
                    const uint8_t *p10 = src + ((size_t)(oy + y0) * (size_t)src_w + (size_t)(ox + x1)) * 4;
                    const uint8_t *p01 = src + ((size_t)(oy + y1) * (size_t)src_w + (size_t)(ox + x0)) * 4;
                    const uint8_t *p11 = src + ((size_t)(oy + y1) * (size_t)src_w + (size_t)(ox + x1)) * 4;
                    uint8_t *o = dst + ((size_t)y * (size_t)target_size + (size_t)x) * 4;

                    float w00 = (1.f - fx) * (1.f - fy);
                    float w10 = fx * (1.f - fy);
                    float w01 = (1.f - fx) * fy;
                    float w11 = fx * fy;
                    for (int c = 0; c < 4; c++) {
                        float val = p00[c] * w00 + p10[c] * w10 + p01[c] * w01 + p11[c] * w11;
                        o[c] = (uint8_t)(val + 0.5f);
                    }
                }
            }
        }
        stbi_image_free(src);
        rgba = dst;
        w = target_size;
        h = target_size;
    } else {
        rgba = src;
    }

    if (out_accent) {
        *out_accent = neon_color(rgba, w, h);
    }

    if (round_r > 0) {
        float r = (float)round_r;
        if (r > w * 0.5f) r = w * 0.5f;
        if (r > h * 0.5f) r = h * 0.5f;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                if (!((x < r || x >= w - r) && (y < r || y >= h - r))) continue;
                float cx = (x < r) ? r : (w - r);
                float cy = (y < r) ? r : (h - r);
                float dx = x + 0.5f - cx;
                float dy = y + 0.5f - cy;
                float d = sqrtf(dx * dx + dy * dy) - r;
                float a = (d <= -0.5f) ? 1.f : (d >= 0.5f) ? 0.f : (0.5f - d);
                size_t idx = ((size_t)y * (size_t)w + (size_t)x) * 4 + 3;
                rgba[idx] = (uint8_t)((float)rgba[idx] * a);
            }
        }
    }

    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return rgba;
}

SDL_Texture *ui_tex_from_rgba(const uint8_t *rgba, int w, int h) {
    if (!ren || !rgba || w <= 0 || h <= 0) return NULL;
    SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
    if (t) {
        SDL_UpdateTexture(t, NULL, rgba, w * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    }
    return t;
}
