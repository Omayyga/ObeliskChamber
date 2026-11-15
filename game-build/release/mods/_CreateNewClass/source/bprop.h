/* GameData/Combat/CombatMagic.bprop: copies of the tome and staff spells that start at the right hand, for created classes.
 * Layout: 0x20 header (magic, u32 size of the rest, ...); a node is a u32 (children << 24 | file offset of its table) and the table
 * is {u32 FNV-1 hash of the key, u32 file offset of the child} per child; a child is a node or a plain value (u32, float, or a
 * string: u16 length with its 0, text, 0). The root (0x20) has "Item" = the count (at 0x24) and "Item[0]".."Item[n-1]" = the
 * spells; its table is the end of the file. A spell: Name, LaunchBoneName, TargetBoneName, Speed, Trajectory, Flags and the
 * effect lists Start, Chant, Shoot, Hit, Away (each a node with its own "Item" count and elements).
 * A copy is the spell's bytes again with every offset moved, its name's first letter 'Z', every "Loc_Reserve1" in it
 * "Loc_R_Weapon"; unless it is a dark spell it also gets flag 8 (as the weapon magics have, which show no glyph of the game's
 * own) and, as its Chant list, the one of MagicWP_HadaA: the glyph Magic_Circle, put at Loc_R_Weapon. */
#ifndef BPROP_H
#define BPROP_H
static uint32_t bp_fnv(const char* t, uint32_t h) { while (*t) h = (h * 0x01000193u) ^ (uint8_t)*t++; return h; }
static int bp_is_node(const uint8_t* d, uint32_t o, uint32_t hi) {
    uint32_t w, c, t, i;
    if (o + 4 > hi) return 0;
    w = jb_u32(d + o); c = w >> 24; t = w & 0xffffff;
    if (!c || t <= o || t + 8 * c > hi) return 0;
    for (i = 0; i < c; ++i) { const uint32_t k = jb_u32(d + t + 8 * i + 4); if (k < o + 4 || k >= t) return 0; }
    return 1;
}
/* the node at o of src (its copy is at o + delta in out): its table and every node below it get their offsets moved */
static void bp_move(const uint8_t* src, uint8_t* out, uint32_t o, uint32_t delta, int depth) {
    const uint32_t w = jb_u32(src + o), c = w >> 24, t = w & 0xffffff;
    uint32_t i;
    jb_p32(out + o + delta, c << 24 | (t + delta));
    for (i = 0; i < c; ++i) {
        const uint32_t k = jb_u32(src + t + 8 * i + 4);
        jb_p32(out + t + delta + 8 * i + 4, k + delta);
        if (depth < 12 && bp_is_node(src, k, t)) bp_move(src, out, k, delta, depth + 1);
    }
}
static uint32_t bp_child(const uint8_t* d, uint32_t o, const char* key) {          /* the file offset of a node's child, 0 = none */
    const uint32_t w = jb_u32(d + o), c = w >> 24, t = w & 0xffffff, h = bp_fnv(key, 0x811c9dc5u);
    uint32_t i;
    for (i = 0; i < c; ++i) if (jb_u32(d + t + 8 * i) == h) return jb_u32(d + t + 8 * i + 4);
    return 0;
}
static int bp_wanted(const char* n) {          /* tomes and staves: Magic_..., MagicDark_..., MagicConron_..., Rod_... */
    if (n[0] == 'R' && n[1] == 'o' && n[2] == 'd' && n[3] == '_') return 1;
    if (n[0] != 'M' || n[1] != 'a' || n[2] != 'g' || n[3] != 'i' || n[4] != 'c') return 0;
    return n[5] == '_' || n[5] == 'D' || n[5] == 'C';
}
static void bp_names(uint8_t* p, uint32_t n, const char* from, const char* to) {          /* 12-character names, length in front */
    uint32_t i, k;
    for (i = 2; i + 13 <= n; ++i) {
        if (p[i - 2] != 13 || p[i - 1] != 0 || p[i + 12] != 0) continue;
        for (k = 0; k < 12 && p[i + k] == (uint8_t)from[k]; ++k) {}
        if (k == 12) for (k = 0; k < 12; ++k) p[i + k] = (uint8_t)to[k];
    }
}
static uint8_t* bp_class_copies(const uint8_t* in, uint64_t size, uint64_t* out_size, uint32_t* made, JbAlloc alloc, JbLog log) {
    uint32_t rw, rc, rt, i, n = 0, at, count_off = 0, glyph_o = 0, glyph_end = 0, items;
    static uint32_t pairs[2 * 256];
    uint8_t* out;
    char key[24];
    *made = 0;
    if (size < 0x40 || size > 0xffffff || jb_u32(in) != 0xcdefbcddu) { log("CombatMagic.bprop: not the expected kind of file", 0); return 0; }
    rw = jb_u32(in + 0x20); rc = rw >> 24; rt = rw & 0xffffff;
    if (!rc || rt + 8 * rc != size) { log("CombatMagic.bprop: the list of spells is not where expected", 0); return 0; }
    count_off = bp_child(in, 0x20, "Item");
    if (!count_off || count_off + 4 > rt) { log("CombatMagic.bprop: no count of spells", 0); return 0; }
    items = jb_u32(in + count_off);
    for (i = 0; i < rc; ++i) {          /* the spells to copy, and the weapon magic whose Chant list holds the glyph */
        const uint32_t o = jb_u32(in + rt + 8 * i + 4);
        uint32_t no;
        if (o == count_off || !bp_is_node(in, o, rt) || !(no = bp_child(in, o, "Name")) || no + 3 > rt) continue;
        if (jb_same((const char*)(in + no + 2), "MagicWP_HadaA")) {
            const uint32_t ch = bp_child(in, o, "Chant");
            if (ch && bp_is_node(in, ch, rt) && (jb_u32(in + ch) >> 24) == 2) { glyph_o = ch; glyph_end = (jb_u32(in + ch) & 0xffffff) + 16; }
        }
        if (bp_wanted((const char*)(in + no + 2))) n++;
    }
    if (!n) return 0;
    out = (uint8_t*)alloc(size + size + (uint64_t)n * (glyph_end - glyph_o + 64) + 4096);
    if (!out) { log("CombatMagic.bprop: out of memory", 0); return 0; }
    jb_copy(out, in, rt);
    at = rt;
    for (i = 0; i < rc; ++i) {
        const uint32_t o = jb_u32(in + rt + 8 * i + 4);
        uint32_t no, end, len, fo, co, k;
        const char* name;
        if (o == count_off || !bp_is_node(in, o, rt) || !(no = bp_child(in, o, "Name"))) continue;
        name = (const char*)(in + no + 2);
        if (!bp_wanted(name)) continue;
        end = (jb_u32(in + o) & 0xffffff) + 8 * (jb_u32(in + o) >> 24);
        len = end - o;
        at = (at + 3) & ~3u;
        jb_copy(out + at, in + o, len);
        bp_move(in, out, o, at - o, 0);
        out[no + 2 + (at - o)] = 'Z';
        bp_names(out + at, len, "Loc_Reserve1", "Loc_R_Weapon");
        k = at; at += len;
        if (glyph_o && !(name[5] == 'D') && (fo = bp_child(in, o, "Flags")) && (co = bp_child(in, o, "Chant"))) {
            /* no glyph of the game's own (flag 8), and the glyph as an effect of the spell's Chant list, at the hand */
            const uint32_t glen = glyph_end - glyph_o, t = jb_u32(in + o) & 0xffffff, c = jb_u32(in + o) >> 24, hch = bp_fnv("Chant", 0x811c9dc5u);
            uint32_t j, g;
            jb_p32(out + fo + (k - o), jb_u32(in + fo) | 8);
            at = (at + 3) & ~3u; g = at;
            jb_copy(out + g, in + glyph_o, glen);
            bp_move(in, out, glyph_o, g - glyph_o, 0);
            bp_names(out + g, glen, "Loc_TrailTip", "Loc_R_Weapon");
            at += glen;
            for (j = 0; j < c; ++j) if (jb_u32(in + t + 8 * j) == hch) jb_p32(out + t + (k - o) + 8 * j + 4, g);
        }
        /* its place in the list: "Item[n]" */
        if (*made < 256) {
            char* e = key; uint32_t v = items + *made, dgt = 1000000; const char* pre = "Item[";
            while (*pre) *e++ = *pre++;
            while (dgt > 1 && v < dgt) dgt /= 10;
            for (; dgt; dgt /= 10) *e++ = (char)('0' + v / dgt % 10);
            *e++ = ']'; *e = 0;
            pairs[2 * *made] = bp_fnv(key, 0x811c9dc5u); pairs[2 * *made + 1] = k;
            (*made)++;
        }
    }
    /* the root's table again at the new end: the old pairs, then the copies'; the count; the sizes */
    at = (at + 3) & ~3u;
    jb_copy(out + at, in + rt, 8 * rc);
    for (i = 0; i < *made; ++i) { jb_p32(out + at + 8 * (rc + i), pairs[2 * i]); jb_p32(out + at + 8 * (rc + i) + 4, pairs[2 * i + 1]); }
    jb_p32(out + 0x20, (rc + *made) << 24 | at);
    jb_p32(out + count_off, items + *made);
    *out_size = (uint64_t)at + 8 * (rc + *made);
    jb_p32(out + 4, (uint32_t)(*out_size - 0x20));
    return out;
}
#endif
