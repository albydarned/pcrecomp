/*
 * hle_format.c - printf over the guest's own argument slots.
 *
 * A cdecl variadic call (wsprintfA, a game's debug printf) leaves its
 * arguments as 4-byte slots on the guest stack, 8 for a double. On a 32-bit
 * host a va_list could point straight at them; on a 64-bit host it cannot
 * (its va_list is registers and 8-byte slots), so the format is walked here
 * and each conversion is handed to snprintf one at a time, with the value
 * read from guest memory. The conversions a C runtime of the era and a
 * game's log lines use: %d %i %u %x %X %o %c %s %p %f %e %g %%, with flags,
 * width, precision, * and the h/l/I64 length modifiers. Returns the length
 * the full text would have had, as snprintf does.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "win32hle.h"

int hle_vsnprintf_guest(char *out, size_t cap, const char *fmt, uint32_t args) {
    size_t n = 0;
#define PUT(c) do { if (n + 1 < cap) out[n] = (c); n++; } while (0)
#define PUTS(s) do { for (const char *_s = (s); *_s; _s++) PUT(*_s); } while (0)
    while (*fmt) {
        if (*fmt != '%') { PUT(*fmt++); continue; }
        const char *start = fmt++;
        if (*fmt == '%') { PUT('%'); fmt++; continue; }
        char spec[64], tmp[512];
        size_t k = 0;
        spec[k++] = '%';
        while (*fmt && strchr("-+ #0", *fmt) && k < 20) spec[k++] = *fmt++;
        if (*fmt == '*') { int w = (int32_t)MEM32(args); args += 4; k += (size_t)snprintf(spec + k, sizeof spec - k, "%d", w); fmt++; }
        else while (*fmt >= '0' && *fmt <= '9' && k < 30) spec[k++] = *fmt++;
        if (*fmt == '.') {
            spec[k++] = *fmt++;
            if (*fmt == '*') { int p = (int32_t)MEM32(args); args += 4; k += (size_t)snprintf(spec + k, sizeof spec - k, "%d", p); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9' && k < 40) spec[k++] = *fmt++;
        }
        int wide = 0, is64 = 0;
        for (;;) {
            if (*fmt == 'h') { fmt++; }
            else if (*fmt == 'l') { fmt++; if (*fmt == 'l') { fmt++; is64 = 1; } }
            else if (*fmt == 'I' && fmt[1] == '6' && fmt[2] == '4') { fmt += 3; is64 = 1; }
            else if (*fmt == 'w') { fmt++; wide = 1; }
            else break;
        }
        char c = *fmt ? *fmt++ : 0;
        switch (c) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o':
            if (is64) {
                uint64_t v = (uint64_t)MEM32(args) | (uint64_t)MEM32(args + 4) << 32; args += 8;
                spec[k++] = 'l'; spec[k++] = 'l'; spec[k++] = c; spec[k] = 0;
                snprintf(tmp, sizeof tmp, spec, (unsigned long long)v);
            } else {
                uint32_t v = MEM32(args); args += 4;
                spec[k++] = c; spec[k] = 0;
                snprintf(tmp, sizeof tmp, spec, (c == 'd' || c == 'i') ? (int)(int32_t)v : (int)v);
            }
            PUTS(tmp);
            break;
        case 'c': {
            uint32_t v = MEM32(args); args += 4;
            spec[k++] = 'c'; spec[k] = 0;
            snprintf(tmp, sizeof tmp, spec, (int)(v & 0xFF));
            PUTS(tmp);
            break;
        }
        case 's': case 'S': {
            uint32_t v = MEM32(args); args += 4;
            const char *s;
            char narrow[1024];
            if (wide || c == 'S') {                      /* a UTF-16 string, narrowed */
                const uint16_t *w = v ? (const uint16_t *)G2H(v) : NULL;
                size_t i = 0;
                for (; w && w[i] && i + 1 < sizeof narrow; i++) narrow[i] = (char)(w[i] < 256 ? w[i] : '?');
                narrow[i] = 0;
                s = w ? narrow : "(null)";
            } else s = v ? (const char *)G2H(v) : "(null)";
            spec[k++] = 's'; spec[k] = 0;
            snprintf(tmp, sizeof tmp, spec, s);
            PUTS(tmp);
            break;
        }
        case 'p': {
            uint32_t v = MEM32(args); args += 4;
            spec[k++] = '0'; spec[k++] = '8'; spec[k++] = 'X'; spec[k] = 0;   /* as a 32-bit Windows %p prints */
            snprintf(tmp, sizeof tmp, "%08X", v);
            PUTS(tmp);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            double d;
            uint64_t bits = (uint64_t)MEM32(args) | (uint64_t)MEM32(args + 4) << 32; args += 8;
            memcpy(&d, &bits, 8);
            spec[k++] = c; spec[k] = 0;
            snprintf(tmp, sizeof tmp, spec, d);
            PUTS(tmp);
            break;
        }
        case 'n': args += 4; break;
        default:                                          /* not a conversion we know: print it as it was */
            for (const char *p = start; p < fmt; p++) PUT(*p);
            break;
        }
    }
    if (cap) out[n < cap ? n : cap - 1] = 0;
    return (int)n;
#undef PUT
#undef PUTS
}
