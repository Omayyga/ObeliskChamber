/* Messages added to a MsgStdBn (.msbt) file, as this game's message files are: UTF-8 text (encoding byte 0), sections LBL1 (labels in
 * hash buckets), ATO1, ATR1 (one fixed-size attribute entry per message, with offsets to strings kept after the entries), TSY1 (one
 * u32 per message), TXT2 (offsets, then the texts) and AOCM (a u16 count and one byte per message). A new message gets the attribute
 * entry, style and AOCM byte of an existing one ("like"), with empty attribute strings of its own. Plain C, as jobbin.h. */
#ifndef MSBT_ADD_H
#define MSBT_ADD_H
#include <stdint.h>
typedef struct MbNew { const char* label; const char* text; const char* like; } MbNew;
typedef void* (*MbAlloc)(uint64_t size);
static uint32_t mb_u32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void mb_p32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint64_t mb_len(const char* s) { uint64_t n = 0; while (s[n]) n++; return n; }
static uint32_t mb_hash(const char* s, uint32_t buckets) { uint32_t h = 0; while (*s) h = h * 0x492u + (uint8_t)*s++; return h % buckets; }
static int mb_is(const uint8_t* p, const char* tag) { return p[0] == (uint8_t)tag[0] && p[1] == (uint8_t)tag[1] && p[2] == (uint8_t)tag[2] && p[3] == (uint8_t)tag[3]; }
/* the index of a label, or 0xffffffff */
static uint32_t mb_find(const uint8_t* lbl, uint32_t size, const char* name) {
    const uint32_t nb = mb_u32(lbl), len = (uint32_t)mb_len(name);
    uint32_t b, i, k;
    for (b = 0; b < nb; ++b) {
        const uint32_t cnt = mb_u32(lbl + 4 + b * 8);
        uint32_t q = mb_u32(lbl + 8 + b * 8);
        for (i = 0; i < cnt && q < size; ++i) {
            const uint32_t ln = lbl[q];
            if (q + 1 + ln + 4 > size) return 0xffffffffu;
            for (k = 0; k < ln && k < len && lbl[q + 1 + k] == (uint8_t)name[k]; ++k) {}
            if (ln == len && k == len) return mb_u32(lbl + q + 1 + ln);
            q += 1 + ln + 4;
        }
    }
    return 0xffffffffu;
}
static uint8_t* mb_add(const uint8_t* in, uint64_t in_size, const MbNew* add, uint32_t count, uint64_t* out_size, uint32_t* added, MbAlloc alloc) {
    const uint8_t *sec[16], *lbl = 0, *atr = 0, *tsy = 0, *txt = 0, *aocm = 0, *ato = 0;
    uint32_t ssize[16], nsec, i, k, n = 0, take[64], like[64], lbl_size = 0, atr_size = 0, tsy_size = 0, txt_size = 0, aocm_size = 0, ato_size = 0, msgs, nb;
    uint64_t p = 0x20, total, w;
    uint8_t* out;
    *added = 0;
    if (in_size < 0x20 || !mb_is(in, "MsgS") || in[0xc] != 0 || in[8] != 0xff || in[9] != 0xfe) return 0;          /* UTF-8, little endian only */
    nsec = (uint32_t)in[0xe] | (uint32_t)in[0xf] << 8;
    if (nsec > 16) return 0;
    for (i = 0; i < nsec; ++i) {
        if (p + 0x10 > in_size) return 0;
        sec[i] = in + p; ssize[i] = mb_u32(in + p + 4);
        if (p + 0x10 + ssize[i] > in_size) return 0;
        if (mb_is(in + p, "LBL1")) { lbl = in + p + 0x10; lbl_size = ssize[i]; }
        if (mb_is(in + p, "ATR1")) { atr = in + p + 0x10; atr_size = ssize[i]; }
        if (mb_is(in + p, "TSY1")) { tsy = in + p + 0x10; tsy_size = ssize[i]; }
        if (mb_is(in + p, "TXT2")) { txt = in + p + 0x10; txt_size = ssize[i]; }
        if (mb_is(in + p, "AOCM")) { aocm = in + p + 0x10; aocm_size = ssize[i]; }
        if (mb_is(in + p, "ATO1")) { ato = in + p + 0x10; ato_size = ssize[i]; }
        p += 0x10 + (((uint64_t)ssize[i] + 15) & ~(uint64_t)15);
    }
    if (!lbl || !txt || lbl_size < 4 || txt_size < 4) return 0;
    nb = mb_u32(lbl); msgs = mb_u32(txt);
    if (!nb || 4 + nb * 8 > lbl_size || 4 + (uint64_t)msgs * 4 > txt_size) return 0;
    if (atr && (atr_size < 8 || mb_u32(atr) != msgs || 8 + (uint64_t)msgs * mb_u32(atr + 4) > atr_size)) return 0;
    if (tsy && tsy_size != msgs * 4) return 0;
    if (aocm && aocm_size != 2 + msgs) return 0;
    for (k = 0; k < count && n < 64; ++k) {
        uint32_t j, dup = 0;
        if (!add[k].label[0] || mb_len(add[k].label) > 60 || mb_find(lbl, lbl_size, add[k].label) != 0xffffffffu) continue;
        for (j = 0; j < n; ++j) { const char *a = add[take[j]].label, *b = add[k].label; while (*a && *a == *b) { a++; b++; } if (*a == *b) dup = 1; }
        if (dup) continue;
        like[n] = add[k].like ? mb_find(lbl, lbl_size, add[k].like) : 0;
        if (like[n] >= msgs) like[n] = 0;
        take[n++] = k;
    }
    if (!n) return 0;
    total = in_size + 0x200;
    for (k = 0; k < n; ++k) total += mb_len(add[take[k]].label) + 5 + mb_len(add[take[k]].text) + 1 + 4 + 4 + 1 + (atr ? mb_u32(atr + 4) + 64 : 0);
    out = (uint8_t*)alloc(total);
    if (!out) return 0;
    for (i = 0; i < 0x20; ++i) out[i] = in[i];
    w = 0x20;
    for (i = 0; i < nsec; ++i) {
        const uint8_t* body = sec[i] + 0x10;
        const uint64_t head = w;
        uint64_t size = 0;
        uint8_t* o = out + w + 0x10;
        uint32_t x;
        for (x = 0; x < 0x10; ++x) out[w + x] = sec[i][x];
        if (body == lbl) {          /* the buckets again: each bucket's old labels, then the new ones that hash to it */
            uint64_t q = 4 + (uint64_t)nb * 8;
            uint32_t b;
            mb_p32(o, nb);
            for (b = 0; b < nb; ++b) {
                uint32_t cnt = mb_u32(lbl + 4 + b * 8), src = mb_u32(lbl + 8 + b * 8), c2 = cnt;
                mb_p32(o + 8 + b * 8, (uint32_t)q);
                for (x = 0; x < cnt; ++x) { const uint32_t ln = lbl[src]; uint32_t y; for (y = 0; y < 1 + ln + 4; ++y) o[q++] = lbl[src++]; }
                for (k = 0; k < n; ++k) {
                    const char* name = add[take[k]].label;
                    const uint32_t ln = (uint32_t)mb_len(name);
                    uint32_t y;
                    if (mb_hash(name, nb) != b) continue;
                    o[q++] = (uint8_t)ln;
                    for (y = 0; y < ln; ++y) o[q++] = (uint8_t)name[y];
                    mb_p32(o + q, msgs + k); q += 4; c2++;
                }
                mb_p32(o + 4 + b * 8, c2);
            }
            size = q;
        } else if (body == atr) {          /* entries, then the strings: every offset moves by the room the new entries take */
            const uint32_t es = mb_u32(atr + 4), sbase = 8 + msgs * es, grow = n * es;
            uint8_t isoff[256];
            uint32_t f, nf = 0, foff[64];
            uint64_t q;
            for (f = 0; f < 256; ++f) isoff[f] = 0;
            for (f = 0; ato && f + 4 <= ato_size && nf < 64; f += 4) {          /* the 4-byte attributes that hold a string offset in every entry */
                const uint32_t fo = mb_u32(ato + f);
                uint32_t m, ok = fo != 0xffffffffu && fo + 4 <= es && es < 256;
                for (m = 0; ok && m < msgs; ++m) { const uint32_t v = mb_u32(atr + 8 + m * es + fo); if (v < sbase || v >= atr_size) ok = 0; }
                if (ok && !isoff[fo]) { isoff[fo] = 1; foff[nf++] = fo; }
            }
            mb_p32(o, msgs + n); mb_p32(o + 4, es);
            for (x = 0; x < msgs * es; ++x) o[8 + x] = atr[8 + x];
            for (x = 0; x < msgs; ++x) for (f = 0; f < nf; ++f) mb_p32(o + 8 + x * es + foff[f], mb_u32(atr + 8 + x * es + foff[f]) + grow);
            q = 8 + (uint64_t)(msgs + n) * es;
            for (x = sbase; x < atr_size; ++x) o[q++] = atr[x];
            for (k = 0; k < n; ++k) {
                uint8_t* e = o + 8 + (uint64_t)(msgs + k) * es;
                for (x = 0; x < es; ++x) e[x] = atr[8 + like[k] * es + x];
                for (f = 0; f < nf; ++f) { mb_p32(e + foff[f], (uint32_t)q); o[q++] = 0; }
            }
            size = q;
        } else if (body == tsy) {
            for (x = 0; x < tsy_size; ++x) o[x] = tsy[x];
            for (k = 0; k < n; ++k) mb_p32(o + tsy_size + k * 4, mb_u32(tsy + like[k] * 4));
            size = tsy_size + n * 4;
        } else if (body == txt) {
            uint64_t q = 4 + (uint64_t)(msgs + n) * 4;
            const uint32_t old_first = 4 + msgs * 4;
            mb_p32(o, msgs + n);
            for (x = 0; x < msgs; ++x) mb_p32(o + 4 + x * 4, mb_u32(txt + 4 + x * 4) + n * 4);
            for (x = old_first; x < txt_size; ++x) o[q++] = txt[x];
            for (k = 0; k < n; ++k) {
                const char* t = add[take[k]].text;
                mb_p32(o + 4 + (msgs + k) * 4, (uint32_t)q);
                while (*t) o[q++] = (uint8_t)*t++;
                o[q++] = 0;
            }
            size = q;
        } else if (body == aocm) {
            const uint32_t c2 = msgs + n;
            o[0] = (uint8_t)c2; o[1] = (uint8_t)(c2 >> 8);
            for (x = 0; x < msgs; ++x) o[2 + x] = aocm[2 + x];
            for (k = 0; k < n; ++k) o[2 + msgs + k] = aocm[2 + like[k]];
            size = 2 + c2;
        } else {
            for (x = 0; x < ssize[i]; ++x) o[x] = body[x];
            size = ssize[i];
        }
        mb_p32(out + head + 4, (uint32_t)size);
        w = head + 0x10 + size;
        while (w & 15) out[w++] = 0xab;
    }
    mb_p32(out + 0x12, (uint32_t)w);
    *out_size = w; *added = n;
    return out;
}
#endif
