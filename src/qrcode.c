#include "qrcode.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define QR_MAX_VERSIONS 10
#define QR_MAX_BYTE_CAP 271
#define QR_MAX_DATA_CW  274
#define QR_MAX_TOTAL_CW 346
#define QR_MAX_BLOCKS   5
#define QR_MAX_ECC_CW   30

enum {
    QR_ECC_M = 0, /* Format bits: 00 */
    QR_ECC_L = 1  /* Format bits: 01 */
};

typedef struct {
    uint8_t ecc_per_block;
    uint8_t g1_blocks;
    uint8_t g1_data_cw;
    uint8_t g2_blocks;
    uint8_t g2_data_cw;
} QrEccBlockInfo;

typedef struct {
    uint8_t count;
    uint8_t pos[3];
} QrAlignInfo;

/* ISO/IEC 18004 Table 9: Error correction characteristics for Versions 1..10.
 * Index 0 = ECC-M, Index 1 = ECC-L. */
static const QrEccBlockInfo k_ecc_table[2][QR_MAX_VERSIONS] = {
    /* ECC-M */
    {
        {10, 1, 16, 0, 0}, /* V1:  16 data + 10 ecc = 26 */
        {16, 1, 28, 0, 0}, /* V2:  28 data + 16 ecc = 44 */
        {26, 1, 44, 0, 0}, /* V3:  44 data + 26 ecc = 70 */
        {18, 2, 32, 0, 0}, /* V4:  64 data + 36 ecc = 100 */
        {24, 2, 43, 0, 0}, /* V5:  86 data + 48 ecc = 134 */
        {16, 4, 27, 0, 0}, /* V6: 108 data + 64 ecc = 172 */
        {18, 4, 31, 0, 0}, /* V7: 124 data + 72 ecc = 196 */
        {22, 2, 38, 2, 39},/* V8: 154 data + 88 ecc = 242 */
        {22, 3, 36, 2, 37},/* V9: 182 data + 110 ecc = 292 */
        {26, 4, 43, 1, 44} /* V10:216 data + 130 ecc = 346 */
    },
    /* ECC-L */
    {
        { 7, 1, 19, 0, 0}, /* V1:  19 data +  7 ecc = 26 */
        {10, 1, 34, 0, 0}, /* V2:  34 data + 10 ecc = 44 */
        {15, 1, 55, 0, 0}, /* V3:  55 data + 15 ecc = 70 */
        {20, 1, 80, 0, 0}, /* V4:  80 data + 20 ecc = 100 */
        {26, 1,108, 0, 0}, /* V5: 108 data + 26 ecc = 134 */
        {18, 2, 68, 0, 0}, /* V6: 136 data + 36 ecc = 172 */
        {20, 2, 78, 0, 0}, /* V7: 156 data + 40 ecc = 196 */
        {24, 2, 97, 0, 0}, /* V8: 194 data + 48 ecc = 242 */
        {30, 2,116, 0, 0}, /* V9: 232 data + 60 ecc = 292 */
        {18, 2, 68, 2, 69} /* V10:274 data + 72 ecc = 346 */
    }
};

/* ISO/IEC 18004 Annex E Table E.1: Alignment pattern center coordinates V1..V10. */
static const QrAlignInfo k_align_table[QR_MAX_VERSIONS] = {
    {0, {0,  0,  0}}, /* V1 */
    {2, {6, 18,  0}}, /* V2 */
    {2, {6, 22,  0}}, /* V3 */
    {2, {6, 26,  0}}, /* V4 */
    {2, {6, 30,  0}}, /* V5 */
    {2, {6, 34,  0}}, /* V6 */
    {3, {6, 22, 38}}, /* V7 */
    {3, {6, 24, 42}}, /* V8 */
    {3, {6, 26, 46}}, /* V9 */
    {3, {6, 28, 50}}  /* V10 */
};

static uint8_t gf_mul(uint8_t x, uint8_t y)
{
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) {
        if (y & 1u) {
            r ^= x;
        }
        uint8_t hi = (uint8_t)(x & 0x80u);
        x = (uint8_t)(x << 1);
        if (hi) {
            x ^= 0x1Du; /* Primitive polynomial 0x11D */
        }
        y = (uint8_t)(y >> 1);
    }
    return r;
}

static void rs_compute_generator(int degree, uint8_t *gen)
{
    /* gen[0..degree] represents x^degree + gen[1]*x^(degree-1) + ... + gen[degree]. */
    uint8_t root = 1;
    gen[0] = 1;
    for (int i = 0; i < degree; i++) {
        gen[i + 1] = gf_mul(gen[i], root);
        for (int j = i; j >= 1; j--) {
            gen[j] = (uint8_t)(gen[j] ^ gf_mul(gen[j - 1], root));
        }
        root = gf_mul(root, 2);
    }
}

static void rs_compute_remainder(const uint8_t *data, int data_len,
                                 const uint8_t *gen, int ecc_len,
                                 uint8_t *ecc_out)
{
    memset(ecc_out, 0, (size_t)ecc_len);
    for (int i = 0; i < data_len; i++) {
        uint8_t factor = (uint8_t)(data[i] ^ ecc_out[0]);
        for (int j = 0; j < ecc_len - 1; j++) {
            ecc_out[j] = ecc_out[j + 1];
        }
        ecc_out[ecc_len - 1] = 0;
        for (int j = 0; j < ecc_len; j++) {
            ecc_out[j] = (uint8_t)(ecc_out[j] ^ gf_mul(gen[j + 1], factor));
        }
    }
}

static void append_bits(uint8_t *buf, int *bit_pos, uint32_t val, int num_bits)
{
    for (int i = num_bits - 1; i >= 0; i--) {
        int pos = *bit_pos;
        if ((val >> i) & 1u) {
            buf[pos >> 3] = (uint8_t)(buf[pos >> 3] | (1u << (7 - (pos & 7))));
        }
        *bit_pos = pos + 1;
    }
}

static void set_func_module(uint8_t *modules, uint8_t *is_func, int size,
                            int x, int y, uint8_t dark)
{
    int idx = y * size + x;
    modules[idx] = dark ? 1u : 0u;
    is_func[idx] = 1u;
}

static void draw_finder_pattern(uint8_t *modules, uint8_t *is_func, int size,
                                int ox, int oy)
{
    /* Draw 7x7 finder plus 1-module separator border where within symbol bounds. */
    for (int dy = -1; dy <= 7; dy++) {
        int y = oy + dy;
        if (y < 0 || y >= size) {
            continue;
        }
        for (int dx = -1; dx <= 7; dx++) {
            int x = ox + dx;
            if (x < 0 || x >= size) {
                continue;
            }
            uint8_t dark = 0;
            if (dx >= 0 && dx <= 6 && dy >= 0 && dy <= 6) {
                int ax = dx - 3;
                int ay = dy - 3;
                if (ax < 0) {
                    ax = -ax;
                }
                if (ay < 0) {
                    ay = -ay;
                }
                int dist = ax > ay ? ax : ay;
                dark = (dist != 2) ? 1u : 0u;
            }
            set_func_module(modules, is_func, size, x, y, dark);
        }
    }
}

static void draw_alignment_pattern(uint8_t *modules, uint8_t *is_func, int size,
                                   int cx, int cy)
{
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            int ax = dx < 0 ? -dx : dx;
            int ay = dy < 0 ? -dy : dy;
            int dist = ax > ay ? ax : ay;
            uint8_t dark = (dist != 1) ? 1u : 0u;
            set_func_module(modules, is_func, size, cx + dx, cy + dy, dark);
        }
    }
}

static void draw_format_bits(uint8_t *modules, uint8_t *is_func, int size,
                             int ecc_level, int mask)
{
    /* ECC-M = 00 (0), ECC-L = 01 (1). */
    uint32_t data = ((uint32_t)ecc_level << 3) | (uint32_t)mask;
    uint32_t rem = data << 10;
    for (int i = 4; i >= 0; i--) {
        if ((rem >> (i + 10)) & 1u) {
            rem ^= (0x537u << i);
        }
    }
    uint32_t bits = ((data << 10) | rem) ^ 0x5412u;

    /* Copy 1: around top-left finder pattern. */
    for (int i = 0; i <= 5; i++) {
        set_func_module(modules, is_func, size, 8, i, (uint8_t)((bits >> i) & 1u));
    }
    set_func_module(modules, is_func, size, 8, 7, (uint8_t)((bits >> 6) & 1u));
    set_func_module(modules, is_func, size, 8, 8, (uint8_t)((bits >> 7) & 1u));
    set_func_module(modules, is_func, size, 7, 8, (uint8_t)((bits >> 8) & 1u));
    for (int i = 9; i < 15; i++) {
        set_func_module(modules, is_func, size, 14 - i, 8, (uint8_t)((bits >> i) & 1u));
    }

    /* Copy 2: bottom-left and top-right finder patterns. */
    for (int i = 0; i < 8; i++) {
        set_func_module(modules, is_func, size, size - 1 - i, 8, (uint8_t)((bits >> i) & 1u));
    }
    for (int i = 8; i < 15; i++) {
        set_func_module(modules, is_func, size, 8, size - 15 + i, (uint8_t)((bits >> i) & 1u));
    }
}

static void draw_version_bits(uint8_t *modules, uint8_t *is_func, int size, int version)
{
    if (version < 7) {
        return;
    }
    uint32_t rem = (uint32_t)version << 12;
    for (int i = 5; i >= 0; i--) {
        if ((rem >> (i + 12)) & 1u) {
            rem ^= (0x1F25u << i);
        }
    }
    uint32_t bits = ((uint32_t)version << 12) | rem;

    for (int i = 0; i < 18; i++) {
        uint8_t dark = (uint8_t)((bits >> i) & 1u);
        int a = i / 3;
        int b = i % 3;
        set_func_module(modules, is_func, size, size - 11 + b, a, dark);
        set_func_module(modules, is_func, size, a, size - 11 + b, dark);
    }
}

static void draw_function_patterns(uint8_t *modules, uint8_t *is_func,
                                   int size, int version, int ecc_level)
{
    /* Finder patterns + separators. */
    draw_finder_pattern(modules, is_func, size, 0, 0);
    draw_finder_pattern(modules, is_func, size, size - 7, 0);
    draw_finder_pattern(modules, is_func, size, 0, size - 7);

    /* Timing patterns (row 6 and col 6 between separators). */
    for (int i = 8; i < size - 8; i++) {
        uint8_t dark = (uint8_t)((i & 1) == 0);
        set_func_module(modules, is_func, size, i, 6, dark);
        set_func_module(modules, is_func, size, 6, i, dark);
    }

    /* Alignment patterns. */
    const QrAlignInfo *align = &k_align_table[version - 1];
    for (int yi = 0; yi < (int)align->count; yi++) {
        for (int xi = 0; xi < (int)align->count; xi++) {
            int cx = align->pos[xi];
            int cy = align->pos[yi];
            if ((cx == 6 && cy == 6) ||
                (cx == 6 && cy == size - 7) ||
                (cx == size - 7 && cy == 6)) {
                continue;
            }
            draw_alignment_pattern(modules, is_func, size, cx, cy);
        }
    }

    /* Always-dark module at (8, 4*version + 9) == (8, size - 8). */
    set_func_module(modules, is_func, size, 8, size - 8, 1u);

    /* Reserve format info (placeholder mask 0) and write version info if V7..V10. */
    draw_format_bits(modules, is_func, size, ecc_level, 0);
    draw_version_bits(modules, is_func, size, version);
}

static void place_codewords(uint8_t *modules, const uint8_t *is_func, int size,
                            const uint8_t *codewords, int total_cw)
{
    int bit_idx = 0;
    int total_bits = total_cw * 8;
    int upward = 1;

    for (int right = size - 1; right >= 1; right -= 2) {
        if (right == 6) {
            right = 5;
        }
        for (int step = 0; step < size; step++) {
            int y = upward ? (size - 1 - step) : step;
            for (int dx = 0; dx < 2; dx++) {
                int x = right - dx;
                int idx = y * size + x;
                if (!is_func[idx]) {
                    uint8_t bit = 0;
                    if (bit_idx < total_bits) {
                        bit = (uint8_t)((codewords[bit_idx >> 3] >> (7 - (bit_idx & 7))) & 1u);
                        bit_idx++;
                    }
                    modules[idx] = bit;
                }
            }
        }
        upward = !upward;
    }
}

static int mask_predicate(int mask, int x, int y)
{
    switch (mask) {
    case 0: return ((y + x) % 2) == 0;
    case 1: return (y % 2) == 0;
    case 2: return (x % 3) == 0;
    case 3: return ((y + x) % 3) == 0;
    case 4: return (((y / 2) + (x / 3)) % 2) == 0;
    case 5: return (((y * x) % 2) + ((y * x) % 3)) == 0;
    case 6: return ((((y * x) % 2) + ((y * x) % 3)) % 2) == 0;
    case 7: return ((((y + x) % 2) + ((y * x) % 3)) % 2) == 0;
    default: return 0;
    }
}

static void apply_mask(uint8_t *modules, const uint8_t *is_func, int size, int mask)
{
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            int idx = y * size + x;
            if (!is_func[idx] && mask_predicate(mask, x, y)) {
                modules[idx] ^= 1u;
            }
        }
    }
}

static int score_line_n1_n3(const uint8_t *line, int len)
{
    int score = 0;
    int run_len = 0;
    uint8_t run_col = 2;

    /* N1: 5 or more consecutive modules of the same color. */
    for (int i = 0; i < len; i++) {
        uint8_t c = line[i];
        if (c == run_col) {
            run_len++;
            if (run_len == 5) {
                score += 3;
            } else if (run_len > 5) {
                score += 1;
            }
        } else {
            run_col = c;
            run_len = 1;
        }
    }

    /* N3: 1:1:3:1:1 pattern preceded or followed by 4 light modules. */
    for (int i = 0; i <= len - 11; i++) {
        if ((line[i + 0] == 1 && line[i + 1] == 0 && line[i + 2] == 1 &&
             line[i + 3] == 1 && line[i + 4] == 1 && line[i + 5] == 0 &&
             line[i + 6] == 1 && line[i + 7] == 0 && line[i + 8] == 0 &&
             line[i + 9] == 0 && line[i + 10] == 0) ||
            (line[i + 0] == 0 && line[i + 1] == 0 && line[i + 2] == 0 &&
             line[i + 3] == 0 && line[i + 4] == 1 && line[i + 5] == 0 &&
             line[i + 6] == 1 && line[i + 7] == 1 && line[i + 8] == 1 &&
             line[i + 9] == 0 && line[i + 10] == 1)) {
            score += 40;
        }
    }

    return score;
}

static int compute_penalty(const uint8_t *modules, int size)
{
    int penalty = 0;
    uint8_t line[QR_MAX_SIZE];

    /* N1 & N3 along rows and columns. */
    for (int y = 0; y < size; y++) {
        penalty += score_line_n1_n3(&modules[y * size], size);
    }
    for (int x = 0; x < size; x++) {
        for (int y = 0; y < size; y++) {
            line[y] = modules[y * size + x];
        }
        penalty += score_line_n1_n3(line, size);
    }

    /* N2: 2x2 blocks of the same color. */
    for (int y = 0; y < size - 1; y++) {
        for (int x = 0; x < size - 1; x++) {
            uint8_t c = modules[y * size + x];
            if (c == modules[y * size + (x + 1)] &&
                c == modules[(y + 1) * size + x] &&
                c == modules[(y + 1) * size + (x + 1)]) {
                penalty += 3;
            }
        }
    }

    /* N4: Proportion of dark modules relative to 50% in 5% steps. */
    int dark = 0;
    int total = size * size;
    for (int i = 0; i < total; i++) {
        dark += modules[i];
    }
    int diff = dark * 20 - total * 10;
    if (diff < 0) {
        diff = -diff;
    }
    penalty += (diff / total) * 10;

    return penalty;
}

int qr_encode_text(const char *text, QrCode *out)
{
    if (text == NULL || out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    int len = 0;
    while (len <= QR_MAX_BYTE_CAP && text[len] != '\0') {
        len++;
    }
    if (len == 0 || len > QR_MAX_BYTE_CAP) {
        return -1;
    }

    /* Choose smallest version in ECC-M, falling back to ECC-L up to V10. */
    int version = 0;
    int ecc_level = QR_ECC_M;
    for (int level_idx = 0; level_idx < 2 && version == 0; level_idx++) {
        int lvl = (level_idx == 0) ? QR_ECC_M : QR_ECC_L;
        for (int v = 1; v <= QR_MAX_VERSIONS; v++) {
            const QrEccBlockInfo *info = &k_ecc_table[lvl][v - 1];
            int total_data_cw = (int)info->g1_blocks * (int)info->g1_data_cw +
                                (int)info->g2_blocks * (int)info->g2_data_cw;
            int header_bits = 4 + ((v >= 10) ? 16 : 8);
            int byte_cap = (total_data_cw * 8 - header_bits) / 8;
            if (len <= byte_cap) {
                version = v;
                ecc_level = lvl;
                break;
            }
        }
    }
    if (version == 0) {
        return -1;
    }

    const QrEccBlockInfo *info = &k_ecc_table[ecc_level][version - 1];
    int total_data_cw = (int)info->g1_blocks * (int)info->g1_data_cw +
                        (int)info->g2_blocks * (int)info->g2_data_cw;
    int total_blocks = (int)info->g1_blocks + (int)info->g2_blocks;
    int ecc_len = (int)info->ecc_per_block;
    int total_cw = total_data_cw + total_blocks * ecc_len;

    /* Encode Byte mode bit stream into data codewords. */
    uint8_t data_cw[QR_MAX_DATA_CW];
    memset(data_cw, 0, sizeof(data_cw));
    int bit_pos = 0;

    append_bits(data_cw, &bit_pos, 0x4u, 4); /* Byte mode 0100 */
    append_bits(data_cw, &bit_pos, (uint32_t)len, (version >= 10) ? 16 : 8);
    for (int i = 0; i < len; i++) {
        append_bits(data_cw, &bit_pos, (uint8_t)text[i], 8);
    }

    int total_data_bits = total_data_cw * 8;
    int term_bits = total_data_bits - bit_pos;
    if (term_bits > 4) {
        term_bits = 4;
    }
    if (term_bits > 0) {
        append_bits(data_cw, &bit_pos, 0u, term_bits);
    }
    if ((bit_pos & 7) != 0) {
        append_bits(data_cw, &bit_pos, 0u, 8 - (bit_pos & 7));
    }
    int byte_pos = bit_pos >> 3;
    int pad_toggle = 0;
    while (byte_pos < total_data_cw) {
        data_cw[byte_pos++] = pad_toggle ? 0x11u : 0xECu;
        pad_toggle ^= 1;
    }

    /* Compute Reed-Solomon ECC per block and interleave codewords. */
    uint8_t gen[QR_MAX_ECC_CW + 1];
    rs_compute_generator(ecc_len, gen);

    int block_offset[QR_MAX_BLOCKS];
    int block_len[QR_MAX_BLOCKS];
    uint8_t ecc_blocks[QR_MAX_BLOCKS][QR_MAX_ECC_CW];
    int offset = 0;
    for (int b = 0; b < total_blocks; b++) {
        int blen = (b < (int)info->g1_blocks) ? (int)info->g1_data_cw : (int)info->g2_data_cw;
        block_offset[b] = offset;
        block_len[b] = blen;
        rs_compute_remainder(&data_cw[offset], blen, gen, ecc_len, ecc_blocks[b]);
        offset += blen;
    }

    uint8_t final_cw[QR_MAX_TOTAL_CW];
    int out_idx = 0;
    int max_data_blk = (info->g2_blocks > 0) ? (int)info->g2_data_cw : (int)info->g1_data_cw;
    for (int i = 0; i < max_data_blk; i++) {
        for (int b = 0; b < total_blocks; b++) {
            if (i < block_len[b]) {
                final_cw[out_idx++] = data_cw[block_offset[b] + i];
            }
        }
    }
    for (int i = 0; i < ecc_len; i++) {
        for (int b = 0; b < total_blocks; b++) {
            final_cw[out_idx++] = ecc_blocks[b][i];
        }
    }

    /* Build base symbol with function patterns and unmasked codewords. */
    int size = 17 + 4 * version;
    uint8_t base_modules[QR_MAX_SIZE * QR_MAX_SIZE];
    uint8_t is_func[QR_MAX_SIZE * QR_MAX_SIZE];
    uint8_t candidate[QR_MAX_SIZE * QR_MAX_SIZE];
    memset(base_modules, 0, sizeof(base_modules));
    memset(is_func, 0, sizeof(is_func));

    draw_function_patterns(base_modules, is_func, size, version, ecc_level);
    place_codewords(base_modules, is_func, size, final_cw, total_cw);

    /* Evaluate all 8 masks and choose the one with minimum penalty. */
    int best_mask = 0;
    int best_penalty = 0x7FFFFFFF;
    for (int mask = 0; mask < 8; mask++) {
        memcpy(candidate, base_modules, (size_t)(size * size));
        apply_mask(candidate, is_func, size, mask);
        draw_format_bits(candidate, is_func, size, ecc_level, mask);
        int p = compute_penalty(candidate, size);
        if (p < best_penalty) {
            best_penalty = p;
            best_mask = mask;
        }
    }

    memcpy(out->modules, base_modules, (size_t)(size * size));
    apply_mask(out->modules, is_func, size, best_mask);
    draw_format_bits(out->modules, is_func, size, ecc_level, best_mask);
    out->size = size;

    return 0;
}

int qr_get_module(const QrCode *qr, int x, int y)
{
    if (qr == NULL || qr->size <= 0 || qr->size > QR_MAX_SIZE ||
        x < 0 || y < 0 || x >= qr->size || y >= qr->size) {
        return 0;
    }
    return (int)qr->modules[y * qr->size + x];
}
