/* Rows at the end of the asset table for a created class's black magic: "[JID_Key, 黒魔]" with a model for the right hand and / or
 * a spell effect. The game's later rows override earlier ones, and every spell's own row (which names its effect) comes before
 * the end, so a row here decides for the class whatever the spell is. One insertion after the sheet's last row holds the new
 * rows and, behind them, each row's condition group {count, pointer} and its two condition pointers. */
#ifndef CASTROWS_H
#define CASTROWS_H
typedef struct JbCast { char key[JB_KEY_MAX]; char hand[32]; char effect[48]; char cond[48]; } JbCast;          /* cond "" = black magic */
static uint8_t* jb_asset_cast(const uint8_t* in, uint64_t in_size, const JbCast* casts, uint32_t count, uint64_t* out_size, JbAlloc alloc, JbLog log) {
    static const char kSheet[] = "\xe3\x82\xa2\xe3\x82\xbb\xe3\x83\x83\xe3\x83\x88\xe3\x83\x86\xe3\x83\xbc\xe3\x83\x96\xe3\x83\xab";
    static const char kBlack[] = "\xe9\xbb\x92\xe9\xad\x94";
    const uint64_t AR = 0x180;
    uint64_t dsz, np, nl, pt, lt, st, strs, hdr = 0, rows, rcount, P, B, add_slots = 0, add_str = 0, new_dsz, new_np, new_st, grow, total, i, k;
    uint64_t gword = 0, cword = 0, maxline = 0, n_od = 0, n_os = 0, n_nd = 0, n_ns = 0, soff, a, b2, w;
    uint64_t *od, *os, *nd, *ns;
    uint8_t *out, *data;
    if (!count || in_size < 0x20 || jb_u32(in) != in_size) return 0;
    dsz = jb_u32(in + 4); np = jb_u32(in + 8); nl = jb_u32(in + 12);
    pt = 0x20 + dsz; lt = pt + np * 8; st = lt + nl * 8;
    if (st > in_size) return 0;
    strs = in_size - st;
    for (i = 0; i < nl; ++i) {
        const uint64_t name = jb_u32(in + lt + i * 8 + 4);
        if (name < strs && jb_same((const char*)(in + st + name), kSheet)) hdr = jb_u32(in + lt + i * 8);
    }
    if (!hdr || hdr + 0x30 > dsz) { log("AssetTable.bin: the asset sheet was not found (no rows for casting)", 0); return 0; }
    rcount = jb_u32(in + 0x20 + hdr + 0x10) >> 16; rows = jb_u64(in + 0x20 + hdr + 0x28);
    P = rows + rcount * AR;
    if (!rcount || P > dsz || (P & 7)) { log("AssetTable.bin: the asset rows are not where expected (no rows for casting)", 0); return 0; }
    for (i = 0; i < rcount; ++i) {
        const uint64_t r = rows + i * AR, line = in[0x20 + r + 0x16] | (uint64_t)in[0x20 + r + 0x17] << 8, ng = jb_u64(in + 0x20 + r + 0x20) & 0xffff, g = jb_u64(in + 0x20 + r + 0x28);
        if (line > maxline) maxline = line;
        if (!gword && ng && g + 16 <= dsz) { gword = jb_u64(in + 0x20 + r + 0x20) & ~(uint64_t)0xffff; cword = jb_u64(in + 0x20 + g) & ~(uint64_t)0xffff; }
    }
    if (rcount + count > 0xffff || maxline + count > 0xffff) { log("AssetTable.bin: too many rows to add rows for casting", 0); return 0; }
    B = count * (AR + 32);
    for (k = 0; k < count; ++k) {
        add_slots += 4 + (casts[k].hand[0] != 0) + (casts[k].effect[0] != 0);
        add_str += jb_len(casts[k].key) + 1 + jb_len(casts[k].cond[0] ? casts[k].cond : kBlack) + 1 + (casts[k].hand[0] ? jb_len(casts[k].hand) + 1 : 0) + (casts[k].effect[0] ? jb_len(casts[k].effect) + 1 : 0);
    }
    new_dsz = dsz + B; new_np = np + add_slots; new_st = 0x20 + new_dsz + new_np * 8 + nl * 8; grow = new_st - st;
    total = new_st + strs + add_str;
    out = (uint8_t*)alloc(total);
    od = (uint64_t*)alloc(np * 8 + 8); os = (uint64_t*)alloc(np * 8 + 8); nd = (uint64_t*)alloc(add_slots * 8 + 8); ns = (uint64_t*)alloc(add_slots * 8 + 8);
    if (!out || !od || !os || !nd || !ns) { log("AssetTable.bin: out of memory (no rows for casting)", 0); return 0; }
    data = out + 0x20;
    jb_copy(out, in, 0x20);
    jb_copy(data, in + 0x20, P);
    for (i = 0; i < B; ++i) data[P + i] = 0;
    jb_copy(data + P + B, in + 0x20 + P, dsz - P);
    for (i = 0; i < np; ++i) {          /* the old pointers: those at or behind the insertion move, and so does what they point at */
        const uint64_t p = jb_u64(in + pt + i * 8), q = p >= P ? p + B : p;
        uint64_t v;
        if (p + 8 > dsz) { log("AssetTable.bin: a pointer slot is outside the data", 0); return 0; }
        v = jb_u64(in + 0x20 + p);
        if (v < dsz) { jb_p64(data + q, v >= P ? v + B : v); od[n_od++] = q; }
        else { jb_p64(data + q, v + grow); os[n_os++] = q; }
    }
    jb_copy(out + new_st, in + st, strs);
    soff = strs;
#define CAST_STR(slot, text) do { const char* t_ = (text); uint64_t n_ = jb_len(t_) + 1; jb_copy(out + new_st + soff, (const uint8_t*)t_, n_); \
        jb_p64(data + (slot), new_st + soff - 0x20); soff += n_; } while (0)
    for (k = 0; k < count; ++k) {          /* the rows; their string slots in address order */
        const uint64_t r = P + k * AR, ex = P + count * AR + k * 32;
        jb_p32(data + r + 0x10, (uint32_t)(rcount + k) | 1u << 16);
        jb_p32(data + r + 0x14, (uint32_t)(maxline + 1 + k) << 16);
        jb_p64(data + r + 0x20, gword | 1);
        jb_p64(data + r + 0x28, ex); nd[n_nd++] = r + 0x28;
        if (casts[k].hand[0]) { CAST_STR(r + 0x68, casts[k].hand); ns[n_ns++] = r + 0x68; }
        if (casts[k].effect[0]) { CAST_STR(r + 0x78, casts[k].effect); ns[n_ns++] = r + 0x78; }
    }
    for (k = 0; k < count; ++k) {          /* each row's one group of two conditions: the class and "black magic" */
        const uint64_t ex = P + count * AR + k * 32;
        jb_p64(data + ex, cword | 2);
        jb_p64(data + ex + 8, ex + 16); nd[n_nd++] = ex + 8;
        CAST_STR(ex + 16, casts[k].key); ns[n_ns++] = ex + 16;
        CAST_STR(ex + 24, casts[k].cond[0] ? casts[k].cond : kBlack); ns[n_ns++] = ex + 24;
    }
#undef CAST_STR
    /* the pointer table: the slots that point into the data, then those that point at strings, each in ascending order */
    w = 0x20 + new_dsz;
    for (a = 0, b2 = 0; a < n_od || b2 < n_nd; w += 8) { if (b2 >= n_nd || (a < n_od && od[a] < nd[b2])) jb_p64(out + w, od[a++]); else jb_p64(out + w, nd[b2++]); }
    for (a = 0, b2 = 0; a < n_os || b2 < n_ns; w += 8) { if (b2 >= n_ns || (a < n_os && os[a] < ns[b2])) jb_p64(out + w, os[a++]); else jb_p64(out + w, ns[b2++]); }
    for (i = 0; i < nl; ++i) {
        const uint64_t off = jb_u32(in + lt + i * 8);
        jb_p32(out + w + i * 8, (uint32_t)(off >= P ? off + B : off));
        jb_p32(out + w + i * 8 + 4, jb_u32(in + lt + i * 8 + 4));
    }
    { const uint64_t h2 = hdr >= P ? hdr + B : hdr; jb_p32(data + h2 + 0x10, (uint32_t)((rcount + count) << 16) | (jb_u32(in + 0x20 + hdr + 0x10) & 0xffff)); }
    jb_p32(out, (uint32_t)total); jb_p32(out + 4, (uint32_t)new_dsz); jb_p32(out + 8, (uint32_t)new_np);
    *out_size = total;
    return out;
}
/* the table's rows that name a spell effect for one item or weapon kind alone: [AID_x] or [黒魔] with a 魔法 value. Calls fn for each
 * (condition, effect name). */
typedef void (*JbSpellFn)(const char* cond, const char* effect, void* user);
static void jb_asset_spell_rows(const uint8_t* in, uint64_t in_size, JbSpellFn fn, void* user) {
    static const char kSheet[] = "\xe3\x82\xa2\xe3\x82\xbb\xe3\x83\x83\xe3\x83\x88\xe3\x83\x86\xe3\x83\xbc\xe3\x83\x96\xe3\x83\xab";
    uint64_t dsz, np, nl, lt, st, strs, hdr = 0, rows, rcount, i;
    if (in_size < 0x20 || jb_u32(in) != in_size) return;
    dsz = jb_u32(in + 4); np = jb_u32(in + 8); nl = jb_u32(in + 12);
    lt = 0x20 + dsz + np * 8; st = lt + nl * 8;
    if (st > in_size) return;
    strs = in_size - st;
    for (i = 0; i < nl; ++i) { const uint64_t name = jb_u32(in + lt + i * 8 + 4); if (name < strs && jb_same((const char*)(in + st + name), kSheet)) hdr = jb_u32(in + lt + i * 8); }
    if (!hdr || hdr + 0x30 > dsz) return;
    rcount = jb_u32(in + 0x20 + hdr + 0x10) >> 16; rows = jb_u64(in + 0x20 + hdr + 0x28);
    if (rows + rcount * 0x180 > dsz) return;
    for (i = 0; i < rcount; ++i) {
        const uint64_t r = rows + i * 0x180, ng = jb_u64(in + 0x20 + r + 0x20) & 0xffff, g = jb_u64(in + 0x20 + r + 0x28), mg = jb_u64(in + 0x20 + r + 0x78);
        uint64_t ca, c0;
        if (ng != 1 || g + 16 > dsz || mg < dsz || mg + 0x20 >= in_size) continue;
        if ((jb_u64(in + 0x20 + g) & 0xffff) != 1) continue;
        ca = jb_u64(in + 0x20 + g + 8);
        if (ca + 8 > dsz) continue;
        c0 = jb_u64(in + 0x20 + ca);
        if (c0 < dsz || c0 + 0x20 >= in_size) continue;
        fn((const char*)(in + 0x20 + c0), (const char*)(in + 0x20 + mg), user);
    }
}
#endif
