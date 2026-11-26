#ifndef QRCODE_H
#define QRCODE_H

#include <stdint.h>

#define QR_MAX_SIZE 57 /* Version 10 = 57x57 modules */

typedef struct {
    int size;
    uint8_t modules[QR_MAX_SIZE * QR_MAX_SIZE]; /* 1 = dark, 0 = light */
} QrCode;

/* Encodes UTF-8/ASCII text in Byte mode (Versions 1..10, ECC-M preferred,
 * falling back to ECC-L for longer URLs up to 271 bytes).
 * Returns 0 on success, -1 if input is empty, too long, or invalid. */
int qr_encode_text(const char *text, QrCode *out);
int qr_get_module(const QrCode *qr, int x, int y);

#endif
