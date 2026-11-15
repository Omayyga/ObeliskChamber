/* Appends class rows to a Job.bin image (Fire Emblem: Fortune's Weave). Plain C, no library: the caller gives the memory.
 * The container and what is done to it are described in tools/jobbin.py, which this follows step for step.
 *
 * jb_add(in, in_size, entries, count, mem, &out_size, alloc, log):
 *   entries[i].key  = the new row's key ("JID_Custom01"), entries[i].base = the key of the existing row it is a copy of.
 *   An entry whose key already exists, or whose base does not, is skipped (reported through log). Returns the new image
 *   (from alloc) or 0 when nothing was added or the input is not a Job.bin this understands. */
#ifndef JOBBIN_H
#define JOBBIN_H
#include <stdint.h>

#define JB_JOB_ROW 0x230u
#define JB_KEY_MAX 64

/* What an entry of job.txt may say besides its base: a name and a help text of its own (they become messages "M<key>" and
 * "MJID_H_<key without JID_>" in Job.msbt), and blocks of numbers written over the base's:
 *   stats, modifiers, caps, growths, growth_bonus   11 numbers: HP, Str, Mag, Dex, Spd, Lck, Def, Res, Cha, sight, move
 *   weapon_ranks    8: sword, lance, axe, bow, gauntlet, black magic, white magic, special (-1 = cannot use)
 *   unit_types      5: foot, cavalry, armor, flying, command (-1 = is not)
 *   move_type, rank 1 each
 * In the row (decimal offsets as in the project's notes): stats +288 (0x120; used for units whose character has none), modifiers
 * +310 (everyone), caps +332, growths +354 (units whose character has none), growth_bonus +376 (everyone), weapon ranks 0x18e,
 * unit types 0x196, move type 0x30, rank 0x31. */
enum { JB_B_STATS, JB_B_MOD, JB_B_CAP, JB_B_GROW, JB_B_GBONUS, JB_B_WRANK, JB_B_UTYPE, JB_B_MOVE, JB_B_RANK, JB_B_CCREQ, JB_B_CCSHARE, JB_B_CCFAIL, JB_NBLOCK };
typedef struct JbBlock { const char* name; uint16_t off; uint8_t count, size; int16_t lo, hi; } JbBlock;
static const JbBlock kJbBlock[JB_NBLOCK] = {
    {"stats", 0x120, 11, 2, 0, 99}, {"modifiers", 0x136, 11, 2, -30, 30}, {"caps", 0x14c, 11, 2, 0, 99}, {"growths", 0x162, 11, 2, 0, 200},
    {"growth_bonus", 0x178, 11, 2, -50, 100}, {"weapon_ranks", 0x18e, 8, 1, -1, 9}, {"unit_types", 0x196, 5, 1, -1, 9},
    {"move_type", 0x30, 1, 1, 0, 15}, {"rank", 0x31, 1, 1, 0, 5},
    /* what a unit needs to change into the class: a rank in each weapon and unit type (0 = none asked; +0x1a4, 13 bytes), the three
     * "shared" weapon numbers (+0x1b1; 15 = none), and the factor for a failed exam (+0x1f8) */
    {"cc_ranks", 0x1a4, 13, 1, 0, 15}, {"cc_shared", 0x1b1, 3, 1, 0, 15}, {"cc_fail", 0x1f8, 1, 1, 0, 100}};
#define JB_NAME_MAX 48
#define JB_HELP_MAX 200
typedef struct JbEntry {
    char key[JB_KEY_MAX]; char base[JB_KEY_MAX];
    char name[JB_NAME_MAX]; char help[JB_HELP_MAX];
    uint8_t has[JB_NBLOCK]; int16_t val[JB_NBLOCK][13];
    char master[JB_KEY_MAX];          /* master_skill = SID_... (the skill a unit gets for mastering the class), or none; "" = the base's */
    char attr[JB_KEY_MAX];            /* attributes = JID_... (the class whose attribute skills it has: foot, armored, flying ...), or none; "" = the base's */
    char dismount[JB_KEY_MAX];        /* dismount = JID_... (the class a unit of this class is on foot), or none; "" = the base's */
    char lower[JB_KEY_MAX]; char upper[JB_KEY_MAX];      /* lower = / upper = JID_... (the class line: the class before and after this one, +0x48 / +0x50), or none; "" = the base's */
    char abil[3][JB_KEY_MAX]; uint8_t has_abil;      /* abilities = SID_a, SID_b, SID_c (a list of the class's own, up to three), or none */
    char skills[JB_KEY_MAX];          /* skills = JID_... (the class whose abilities, the owned skills at +0x90, it has), or none; "" = the base's */
    char barred[JB_KEY_MAX];          /* barred = JID_... (the class whose list of barred characters, +0x1e8, it has), or none; "" = the base's */
    uint16_t cexp[2]; uint8_t has_cexp;      /* class_exp = growth, battle (+0x19c, +0x19e): what mastering the class takes, by its rank in the game */
    int8_t sex; uint8_t has_sex;      /* sex = 0 anyone, 1 male only, 2 female only (+0x32) */
    char mount_from[JB_KEY_MAX];      /* mount = JID_... (the class whose mount it has; a class with an on-foot class must have one) */
    uint8_t no_mount;                 /* mount = none: the row has no mount of its own (an on-foot partner) */
    char icon[JB_KEY_MAX];            /* icon = JID_... (the class whose icon it shows); "" = the base's */
} JbEntry;
#define JB_ICON 0x200            /* the row's icon name (a string) */
#define JB_ATTR 0xf0             /* the row's attribute skill list (SID_歩兵, SID_重装, SID_騎兵, SID_飛行, SID_竜, SID_獣, SID_冥魔): the icons beside the class name */
#define JB_MASTER 0x100          /* the row's mastery skill list: u16 count, then at +8 a pointer to that many skill ids */
typedef void* (*JbAlloc)(uint64_t size);
typedef void (*JbLog)(const char* text, const char* arg);

static uint32_t jb_u32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t jb_u64(const uint8_t* p) { return (uint64_t)jb_u32(p) | (uint64_t)jb_u32(p + 4) << 32; }
static void jb_p32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void jb_p64(uint8_t* p, uint64_t v) { jb_p32(p, (uint32_t)v); jb_p32(p + 4, (uint32_t)(v >> 32)); }
static void jb_copy(uint8_t* d, const uint8_t* s, uint64_t n) { while (n--) *d++ = *s++; }
static uint64_t jb_len(const char* s) { uint64_t n = 0; while (s[n]) n++; return n; }
static int jb_same(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static void jb_sort(uint64_t* v, uint64_t n) {      /* shell sort: the lists are nearly sorted already */
    uint64_t gap, i, j;
    for (gap = n / 2; gap; gap /= 2)
        for (i = gap; i < n; ++i) {
            uint64_t x = v[i];
            for (j = i; j >= gap && v[j - gap] > x; j -= gap) v[j] = v[j - gap];
            v[j] = x;
        }
}

/* The general form: rows of `row` bytes appended to the sheet whose header label is `sheet`. quiet = say nothing about entries whose
 * base has no row in this sheet (not every class has one in every sheet). */
static uint8_t* jb_add_rows(const uint8_t* in, uint64_t in_size, const char* sheet, uint32_t JB_ROW, int quiet, const JbEntry* entries, uint32_t count,
                            uint64_t* out_size, JbAlloc alloc, JbLog log) {
    uint64_t dsz, np, nl, pt, lt, st, strs, lo, hdr = 0, rows, rcount, at, delta, i, k;
    uint64_t n_data = 0, n_str = 0, add_slots = 0, add_strs = 0, new_dsz, new_np, new_nl, new_st, total, shift, label_at, line = 0;
    uint32_t use[256], n = 0;         /* the entries taken, and each one's template row */
    uint64_t tpl[256];
    uint64_t *in_data, *in_str;
    uint8_t *out, *data, *labels, *strings;
    if (in_size < 0x20 || jb_u32(in) != in_size) { log("Job.bin: the size field does not match the file", 0); return 0; }
    dsz = jb_u32(in + 4); np = jb_u32(in + 8); nl = jb_u32(in + 12);
    pt = 0x20 + dsz; lt = pt + np * 8; st = lt + nl * 8;
    if (st >= in_size || dsz < JB_ROW) { log("Job.bin: the tables do not fit the file", 0); return 0; }
    strs = in_size - st; lo = st - 0x20;
    if (in[in_size - 1] != 0) { log("Job.bin: the string area does not end with a terminator", 0); return 0; }
#define JB_Q(o) jb_u64(in + 0x20 + (o))
#define JB_TEXT(v) ((const char*)(in + 0x20 + (v)))
    /* the sheet's header: the label with its name */
    for (i = 0; i < nl; ++i) {
        uint64_t name = jb_u32(in + lt + i * 8 + 4);
        if (name < strs && jb_same((const char*)(in + st + name), sheet)) { hdr = jb_u32(in + lt + i * 8); break; }
    }
    if (!hdr || hdr + 0x30 > dsz) { log("Job.bin: the class sheet's header was not found", 0); return 0; }
    rcount = jb_u32(in + 0x20 + hdr + 0x10) >> 16;
    rows = JB_Q(hdr + 0x28);
    at = rows + rcount * JB_ROW;
    if (!rcount || rows >= dsz || at > hdr) { log("Job.bin: the class rows are not where the header says", 0); return 0; }
    for (i = 0; i < rcount; ++i) {
        uint64_t key = JB_Q(rows + i * JB_ROW + 8), ln = (uint64_t)in[0x20 + rows + i * JB_ROW + 0x16] | (uint64_t)in[0x20 + rows + i * JB_ROW + 0x17] << 8;
        if (key && (key < lo || key >= lo + strs)) { log("Job.bin: a row's key is not a string", 0); return 0; }
        if (ln > line) line = ln;
    }
    /* which entries to take */
    for (k = 0; k < count && n < 256; ++k) {
        uint64_t found = rcount, dup = 0;
        uint32_t e;
        if (!entries[k].key[0]) continue;
        for (i = 0; i < rcount; ++i) {
            const char* key = JB_Q(rows + i * JB_ROW + 8) ? JB_TEXT(JB_Q(rows + i * JB_ROW + 8)) : "";
            if (jb_same(key, entries[k].key)) dup = 1;
            if (jb_same(key, entries[k].base)) found = i;
        }
        for (e = 0; e < n; ++e)
            if (jb_same(entries[use[e]].key, entries[k].key)) dup = 1;
        if (dup) { if (!quiet) log("class key already exists, entry skipped: ", entries[k].key); continue; }
        if (found == rcount) { if (!quiet) log("base class not found, entry skipped: ", entries[k].key); continue; }
        use[n] = (uint32_t)k; tpl[n] = rows + found * JB_ROW; n++;
    }
    if (!n) return 0;
    for (k = 0; k < n; ++k) {
        for (i = 0; i < np; ++i) { uint64_t p = jb_u64(in + pt + i * 8); if (p >= tpl[k] && p < tpl[k] + JB_ROW) add_slots++; }
        add_strs += jb_len(entries[use[k]].key) + 1;
        if (JB_ROW == JB_JOB_ROW && entries[use[k]].name[0]) {          /* two message ids; their slots, where the base has none */
            uint64_t q;
            add_strs += 2 * jb_len(entries[use[k]].key) + 8;
            for (q = 0x20; q <= 0x28; q += 8) {
                for (i = 0; i < np && jb_u64(in + pt + i * 8) != tpl[k] + q; ++i) {}
                if (i == np) add_slots++;
            }
        }
        if (JB_ROW == JB_JOB_ROW && entries[use[k]].master[0]) add_slots++;
        if (JB_ROW == JB_JOB_ROW && entries[use[k]].icon[0]) add_slots++;
        if (JB_ROW == JB_JOB_ROW && entries[use[k]].attr[0]) add_slots++;
        if (JB_ROW == JB_JOB_ROW && entries[use[k]].dismount[0]) add_slots++;
        if (JB_ROW == JB_JOB_ROW && entries[use[k]].lower[0]) add_slots++;
        if (JB_ROW == JB_JOB_ROW && entries[use[k]].upper[0]) add_slots++;
        if (JB_ROW == JB_JOB_ROW && entries[use[k]].mount_from[0]) add_slots++;
        if (JB_ROW == JB_JOB_ROW && entries[use[k]].skills[0]) add_slots++;
        if (JB_ROW == JB_JOB_ROW && entries[use[k]].has_abil) add_slots += 4;
        if (JB_ROW == JB_JOB_ROW && entries[use[k]].barred[0]) add_slots++;
    }
    delta = (uint64_t)n * JB_ROW + (JB_ROW == JB_JOB_ROW ? (uint64_t)n * 0x30 : 0);          /* class rows: room after them for a list of three abilities each */
    new_dsz = dsz + delta; new_np = np + add_slots; new_nl = nl + n;
    new_st = 0x20 + new_dsz + new_np * 8 + new_nl * 8;
    total = new_st + strs + add_strs;
    out = (uint8_t*)alloc(total);
    in_data = (uint64_t*)alloc(new_np * 8 + 8);
    in_str = (uint64_t*)alloc(new_np * 8 + 8);
    if (!out || !in_data || !in_str) { log("Job.bin: out of memory", 0); return 0; }
    data = out + 0x20;
#define JB_MV(o) ((o) >= at ? (o) + delta : (o))
    /* 1. the data with room for the rows; every slot and every pointer into the data at or after that point moves */
    jb_copy(out, in, 0x20);
    jb_copy(data, in + 0x20, at);
    for (i = 0; i < delta; ++i) data[at + i] = 0;
    jb_copy(data + at + delta, in + 0x20 + at, dsz - at);
    for (i = 0; i < np; ++i) {
        uint64_t p = jb_u64(in + pt + i * 8), v;
        if (p + 8 > dsz) { log("Job.bin: a pointer slot is outside the data", 0); return 0; }
        v = JB_Q(p);
        if (v < dsz) { in_data[n_data++] = JB_MV(p); jb_p64(data + JB_MV(p), JB_MV(v)); }
        else in_str[n_str++] = JB_MV(p);
    }
    /* 2. the new rows: the template as it now stands (its data pointers are already moved), with its own index, line and slots */
    for (k = 0; k < n; ++k) {
        uint64_t r = at + k * JB_ROW;
        jb_copy(data + r, data + tpl[k], JB_ROW);
        data[r + 0x10] = (uint8_t)(rcount + k); data[r + 0x11] = (uint8_t)((rcount + k) >> 8);
        data[r + 0x12] = 1; data[r + 0x13] = 0; data[r + 0x14] = 0; data[r + 0x15] = 0;
        data[r + 0x16] = (uint8_t)(line + 1 + k); data[r + 0x17] = (uint8_t)((line + 1 + k) >> 8);
        for (i = 0; i < np; ++i) {
            uint64_t p = jb_u64(in + pt + i * 8);
            if (p < tpl[k] || p >= tpl[k] + JB_ROW) continue;
            if (JB_ROW == JB_JOB_ROW && entries[use[k]].master[0] && p == tpl[k] + JB_MASTER + 8) continue;      /* set below */
            if (JB_ROW == JB_JOB_ROW && entries[use[k]].icon[0] && p == tpl[k] + JB_ICON) continue;              /* set below */
            if (JB_ROW == JB_JOB_ROW && entries[use[k]].attr[0] && p == tpl[k] + JB_ATTR + 8) continue;          /* set below */
            if (JB_ROW == JB_JOB_ROW && entries[use[k]].dismount[0] && p == tpl[k] + 0x58) continue;             /* set below */
            if (JB_ROW == JB_JOB_ROW && entries[use[k]].lower[0] && p == tpl[k] + 0x48) continue;                /* set below */
            if (JB_ROW == JB_JOB_ROW && entries[use[k]].upper[0] && p == tpl[k] + 0x50) continue;                /* set below */
            if (JB_ROW == JB_JOB_ROW && (entries[use[k]].skills[0] || entries[use[k]].has_abil) && p == tpl[k] + 0x98) continue;      /* set below */
            if (JB_ROW == JB_JOB_ROW && entries[use[k]].barred[0] && p == tpl[k] + 0x1f0) continue;              /* set below */
            if (JB_ROW == JB_JOB_ROW && (entries[use[k]].no_mount || entries[use[k]].mount_from[0]) && p == tpl[k] + 0x68) continue;      /* set below */
            if (JB_Q(p) < dsz) in_data[n_data++] = r + (p - tpl[k]);
            else in_str[n_str++] = r + (p - tpl[k]);
        }
        if (JB_ROW == JB_JOB_ROW && entries[use[k]].master[0]) {
            /* the mastery skill: the list of a class that has exactly this skill (classes share such lists), or none */
            const char* want = entries[use[k]].master;
            uint64_t src = 0, x;
            for (x = 0; x < rcount && !src && !jb_same(want, "none"); ++x) {
                const uint64_t r2 = rows + x * JB_ROW, n2 = JB_Q(r2 + JB_MASTER) & 0xffff, a2 = JB_Q(r2 + JB_MASTER + 8);
                uint64_t v;
                if (n2 != 1 || a2 + 8 > dsz) continue;
                v = JB_Q(a2);
                if (v >= lo && v < lo + strs && jb_same(JB_TEXT(v), want)) src = r2;
            }
            if (src) {
                jb_copy(data + r + JB_MASTER, data + src + JB_MASTER, 16);      /* as relocated */
                in_data[n_data++] = r + JB_MASTER + 8;
            } else {
                data[r + JB_MASTER] = 0; data[r + JB_MASTER + 1] = 0;
                jb_p64(data + r + JB_MASTER + 8, 0);
                if (!jb_same(want, "none")) log("mastery skill not found among the classes' (the class gets none): ", want);
            }
        }
        if (JB_ROW == JB_JOB_ROW && entries[use[k]].no_mount) { jb_p64(data + r + 0x68, 0); jb_p64(data + r + 0x70, 0); data[r + 0x60] = 2; }          /* an on-foot partner: as the game's own (+0x60 = 2) */
        else if (JB_ROW == JB_JOB_ROW && entries[use[k]].mount_from[0]) {          /* the mount of another class (+0x68) and "deployed mounted" (+0x60 = 2) */
            const char* want = entries[use[k]].mount_from;
            uint64_t src = 0, x, v = 0;
            for (x = 0; x < rcount && !src; ++x) {
                const uint64_t r2 = rows + x * JB_ROW, kv = JB_Q(r2 + 8);
                if (kv >= lo && kv < lo + strs && jb_same(JB_TEXT(kv), want)) src = r2;
            }
            if (src) v = JB_Q(src + 0x68);
            if (v >= lo && v < lo + strs) { jb_p64(data + r + 0x68, v); jb_p64(data + r + 0x70, 0); in_str[n_str++] = r + 0x68; data[r + 0x60] = 2; }
            else { v = JB_Q(tpl[k] + 0x68); if (v >= lo && v < lo + strs) in_str[n_str++] = r + 0x68; log("mount: no mounted class with this key (the base's mount is kept): ", want); }
        }
        if (JB_ROW == JB_JOB_ROW && entries[use[k]].dismount[0]) {          /* +0x58: the row a unit of the class is on foot (a pointer to that row) */
            const char* want = entries[use[k]].dismount;
            uint64_t to = 0, x;
            for (x = 0; x < n && !to; ++x) if (jb_same(entries[use[x]].key, want)) to = at + x * JB_ROW;          /* another new row */
            for (x = 0; x < rcount && !to; ++x) {
                const uint64_t r2 = rows + x * JB_ROW, kv = JB_Q(r2 + 8);
                if (kv >= lo && kv < lo + strs && jb_same(JB_TEXT(kv), want)) to = r2;
            }
            if (to && !jb_u64(data + r + 0x68)) { to = 0; want = "none"; log("dismount: the class has no mount, so the on-foot class is not linked (the game needs both): ", entries[use[k]].key); }
            jb_p64(data + r + 0x58, to);
            if (to) in_data[n_data++] = r + 0x58;
            else if (!jb_same(want, "none")) log("dismount: no class with this key (the class has no on-foot class): ", want);
        }
        {   /* the class line: +0x48 the class before this one, +0x50 the one after (pointers to rows, as the game's own) */
            uint32_t w;
            for (w = 0; JB_ROW == JB_JOB_ROW && w < 2; ++w) {
                const char* want = w ? entries[use[k]].upper : entries[use[k]].lower;
                const uint64_t off = w ? 0x50 : 0x48;
                uint64_t to = 0, x;
                if (!want[0]) continue;
                for (x = 0; x < n && !to; ++x) if (jb_same(entries[use[x]].key, want)) to = at + x * JB_ROW;
                for (x = 0; x < rcount && !to; ++x) {
                    const uint64_t r2 = rows + x * JB_ROW, kv = JB_Q(r2 + 8);
                    if (kv >= lo && kv < lo + strs && jb_same(JB_TEXT(kv), want)) to = r2;
                }
                jb_p64(data + r + off, to);
                if (to) in_data[n_data++] = r + off;
                else if (!jb_same(want, "none")) log(w ? "upper: no class with this key (the class line ends here): " : "lower: no class with this key (the class line starts here): ", want);
            }
        }
/* a list of the row (count at OFF, pointer at OFF + 8) taken from the class named in the entry, shared as the game's rows share theirs */
#define JB_SHARE(WANT, OFF, WHAT) if (JB_ROW == JB_JOB_ROW && (WANT)[0]) { \
            const char* want = (WANT); uint64_t src = 0, x; \
            for (x = 0; x < rcount && !src && !jb_same(want, "none"); ++x) { \
                const uint64_t r2 = rows + x * JB_ROW, kv = JB_Q(r2 + 8); \
                if (kv >= lo && kv < lo + strs && jb_same(JB_TEXT(kv), want)) src = r2; } \
            if (src && (JB_Q(src + (OFF)) & 0xffff) && JB_Q(src + (OFF) + 8) < dsz) { jb_copy(data + r + (OFF), data + src + (OFF), 16); in_data[n_data++] = r + (OFF) + 8; } \
            else { data[r + (OFF)] = 0; data[r + (OFF) + 1] = 0; jb_p64(data + r + (OFF) + 8, 0); if (!src && !jb_same(want, "none")) log(WHAT ": no class with this key (the class gets none): ", want); } }
        if (JB_ROW == JB_JOB_ROW && entries[use[k]].has_abil) {
            /* the class's own abilities: a list in the room after the new rows; each skill's name is the one a class of the game
             * already points at (the table's strings are not added to) */
            const uint64_t L = at + (uint64_t)n * JB_ROW + k * 0x30;
            uint32_t m = 0, a3;
            for (a3 = 0; a3 < 3; ++a3) {
                const char* want = entries[use[k]].abil[a3];
                uint64_t x, sv = 0;
                if (!want[0]) continue;
                for (x = 0; x < rcount && !sv; ++x) {
                    const uint64_t r2 = rows + x * JB_ROW, n2 = JB_Q(r2 + 0x90) & 0xffff, a2 = JB_Q(r2 + 0x98);
                    uint64_t j;
                    if (!n2 || a2 + n2 * 16 > dsz) continue;
                    for (j = 0; j < n2 && !sv; ++j) { const uint64_t v = JB_Q(a2 + j * 16); if (v >= lo && v < lo + strs && jb_same(JB_TEXT(v), want)) sv = v; }
                }
                if (!sv) { log("abilities: no class of the game has this skill as an ability (left out): ", want); continue; }
                jb_p64(data + L + m * 16, sv); jb_p64(data + L + m * 16 + 8, 0); in_str[n_str++] = L + m * 16; m++;
            }
            data[r + 0x90] = (uint8_t)m; data[r + 0x91] = 0; data[r + 0x92] = 0x10; data[r + 0x93] = 0;
            data[r + 0x94] = data[r + 0x95] = data[r + 0x96] = data[r + 0x97] = 0;
            jb_p64(data + r + 0x98, m ? L : 0);
            if (m) in_data[n_data++] = r + 0x98;
        } else
        JB_SHARE(entries[use[k]].skills, 0x90, "skills")
        JB_SHARE(entries[use[k]].barred, 0x1e8, "barred")
        if (JB_ROW == JB_JOB_ROW && entries[use[k]].has_sex) data[r + 0x32] = (uint8_t)entries[use[k]].sex;
        if (JB_ROW == JB_JOB_ROW && entries[use[k]].has_cexp) {
            data[r + 0x19c] = (uint8_t)entries[use[k]].cexp[0]; data[r + 0x19d] = (uint8_t)(entries[use[k]].cexp[0] >> 8);
            data[r + 0x19e] = (uint8_t)entries[use[k]].cexp[1]; data[r + 0x19f] = (uint8_t)(entries[use[k]].cexp[1] >> 8);
        }
        if (JB_ROW == JB_JOB_ROW && entries[use[k]].attr[0]) {          /* the attribute skills of another class: that row's list, shared */
            const char* want = entries[use[k]].attr;
            uint64_t src = 0, x;
            for (x = 0; x < rcount && !src && !jb_same(want, "none"); ++x) {
                const uint64_t r2 = rows + x * JB_ROW, kv = JB_Q(r2 + 8);
                if (kv >= lo && kv < lo + strs && jb_same(JB_TEXT(kv), want)) src = r2;
            }
            if (src && (JB_Q(src + JB_ATTR) & 0xffff) && JB_Q(src + JB_ATTR + 8) < dsz) {
                jb_copy(data + r + JB_ATTR, data + src + JB_ATTR, 16);      /* as relocated */
                in_data[n_data++] = r + JB_ATTR + 8;
            } else {
                data[r + JB_ATTR] = 0; data[r + JB_ATTR + 1] = 0;
                jb_p64(data + r + JB_ATTR + 8, 0);
                if (!src && !jb_same(want, "none")) log("attributes: no class with this key (the class gets none): ", want);
            }
        }
        if (JB_ROW == JB_JOB_ROW && entries[use[k]].icon[0]) {          /* the icon of another class: that row's icon name */
            const char* want = entries[use[k]].icon;
            uint64_t src = 0, x, v = 0;
            for (x = 0; x < rcount && !src; ++x) {
                const uint64_t r2 = rows + x * JB_ROW, kv = JB_Q(r2 + 8);
                if (kv >= lo && kv < lo + strs && jb_same(JB_TEXT(kv), want)) src = r2;
            }
            if (src) v = JB_Q(src + JB_ICON);
            else { v = JB_Q(tpl[k] + JB_ICON); log("icon: no class with this key (the base's icon is kept): ", want); }
            jb_p64(data + r + JB_ICON, v >= lo && v < lo + strs ? v : 0);
            if (v >= lo && v < lo + strs) in_str[n_str++] = r + JB_ICON;
        }
        if (JB_ROW == JB_JOB_ROW) {          /* a class row: the entry's own numbers over the base's */
            const JbEntry* e = &entries[use[k]];
            uint32_t bl, x;
            for (bl = 0; bl < JB_NBLOCK; ++bl) {
                if (!e->has[bl]) continue;
                for (x = 0; x < kJbBlock[bl].count; ++x) {
                    const uint16_t v = (uint16_t)e->val[bl][x];
                    data[r + kJbBlock[bl].off + x * kJbBlock[bl].size] = (uint8_t)v;
                    if (kJbBlock[bl].size == 2) data[r + kJbBlock[bl].off + x * 2 + 1] = (uint8_t)(v >> 8);
                }
            }
            if (e->name[0]) {          /* name and help ids of its own: the two slots are string slots here even when the base had none */
                uint32_t q;
                for (q = 0x20; q <= 0x28; q += 8) {
                    uint64_t x2;
                    for (x2 = 0; x2 < n_str && in_str[x2] != r + q; ++x2) {}
                    if (x2 == n_str) in_str[n_str++] = r + q;
                }
            }
        }
    }
    jb_p32(data + JB_MV(hdr) + 0x10, (uint32_t)((rcount + n) << 16) | (jb_u32(in + 0x20 + hdr + 0x10) & 0xffff));
    /* the pointer slots as they came out (the count above was an upper bound): the tables behind the data are placed by it */
    new_np = n_data + n_str;
    new_st = 0x20 + new_dsz + new_np * 8 + new_nl * 8;
    total = new_st + strs + add_strs;
    /* 3. tables: pointers (data group, then string group, each ascending), labels with the new rows' after the last old row's */
    jb_sort(in_data, n_data); jb_sort(in_str, n_str);
    for (i = 0; i < n_data; ++i) jb_p64(out + 0x20 + new_dsz + i * 8, in_data[i]);
    for (i = 0; i < n_str; ++i) jb_p64(out + 0x20 + new_dsz + (n_data + i) * 8, in_str[i]);
    labels = out + 0x20 + new_dsz + new_np * 8;
    label_at = 0;
    for (i = 0; i < nl; ++i)
        if (jb_u32(in + lt + i * 8) < at) label_at = i + 1;
    strings = out + new_st;
    jb_copy(strings, in + st, strs);
    {
        uint64_t w = 0, soff = strs;
        for (i = 0; i < nl; ++i) {
            if (i == label_at)
                for (k = 0; k < n; ++k) {
                    uint64_t len = jb_len(entries[use[k]].key) + 1;
                    jb_p32(labels + w * 8, (uint32_t)(at + k * JB_ROW)); jb_p32(labels + w * 8 + 4, (uint32_t)soff); w++;
                    jb_copy(strings + soff, (const uint8_t*)entries[use[k]].key, len);
                    jb_p64(data + at + k * JB_ROW + 8, soff + new_st - 0x20);
                    soff += len;
                }
            jb_p32(labels + w * 8, (uint32_t)JB_MV(jb_u32(in + lt + i * 8))); jb_p32(labels + w * 8 + 4, jb_u32(in + lt + i * 8 + 4)); w++;
        }
    }
    /* the names' message ids, after the keys: "M" + key and "MJID_H_" + the key without its "JID_" */
    if (JB_ROW == JB_JOB_ROW) {
        uint64_t soff = strs;
        for (k = 0; k < n; ++k) soff += jb_len(entries[use[k]].key) + 1;
        for (k = 0; k < n; ++k) {
            const char* key = entries[use[k]].key;
            const uint64_t len = jb_len(key), r = at + k * JB_ROW;
            if (!entries[use[k]].name[0]) continue;
            strings[soff] = 'M'; jb_copy(strings + soff + 1, (const uint8_t*)key, len + 1);
            jb_p64(data + r + 0x20, soff + new_st - 0x20); soff += len + 2;
            jb_copy(strings + soff, (const uint8_t*)"MJID_H_", 7);
            jb_copy(strings + soff + 7, (const uint8_t*)(len > 4 ? key + 4 : key), (len > 4 ? len - 4 : len) + 1);
            jb_p64(data + r + 0x28, soff + new_st - 0x20); soff += 7 + (len > 4 ? len - 4 : len) + 1;
        }
        total = new_st + soff;
    }
    /* 4. the string area moved: the string pointers follow it (the new rows' keys were written with the new place already) */
    shift = new_st - st;
    for (i = 0; i < n_str; ++i) {
        uint64_t p = in_str[i];
        if (p >= at && p < at + (uint64_t)n * JB_ROW && ((p - at) % JB_ROW) == 8) continue;
        if (JB_ROW == JB_JOB_ROW && p >= at && p < at + (uint64_t)n * JB_ROW && (((p - at) % JB_ROW) == 0x20 || ((p - at) % JB_ROW) == 0x28) && entries[use[(p - at) / JB_ROW]].name[0]) continue;
        jb_p64(data + p, jb_u64(data + p) + shift);
    }
    jb_p32(out, (uint32_t)total); jb_p32(out + 4, (uint32_t)new_dsz); jb_p32(out + 8, (uint32_t)new_np); jb_p32(out + 12, (uint32_t)new_nl);
    *out_size = total;
    return out;
#undef JB_Q
#undef JB_TEXT
#undef JB_MV
}

static uint8_t* jb_add(const uint8_t* in, uint64_t in_size, const JbEntry* entries, uint32_t count, uint64_t* out_size, JbAlloc alloc, JbLog log) {
    return jb_add_rows(in, in_size, "\xe5\x85\xb5\xe7\xa8\xae", JB_JOB_ROW, 0, entries, count, out_size, alloc, log);      /* 兵種 */
}

/* AssetTable.bin: models and animations for the new classes.
 * Sheet アセットテーブル (rows of 0x180 bytes): a row applies when one of its condition groups holds; +0x20 = group count (low 16 bits),
 * +0x28 = the groups, 16 bytes each: {condition count (low 16 bits), pointer to that many strings}. A class is a condition by its key
 * ("JID_..."), "!JID_..." means not that class.
 * For every entry (key, base): each group naming the base gets a twin naming the key instead, added to the same row (so the order of
 * the rows, which decides what overrides what, stays as it is), and each group with "!base" also gets "!key". The new arrays go at
 * the end of the data. Returns the new image, or 0 when nothing was added. *groups_out = the number of groups added. */
static int jb_has(const uint8_t* in, uint64_t arr, uint64_t n, uint64_t dsz, uint64_t fsize, const char* want, int negated) {
    uint64_t j;
    for (j = 0; j < n; ++j) {
        uint64_t v = jb_u64(in + 0x20 + arr + j * 8);
        const char* t;
        if (v < dsz || v >= fsize) continue;
        t = (const char*)(in + 0x20 + v);
        if (negated) { if (t[0] == '!' && jb_same(t + 1, want)) return 1; }
        else if (jb_same(t, want)) return 1;
    }
    return 0;
}

/* What a created class takes from other rows of the asset table for its look (asset.txt). Each field is a set of byte ranges of an
 * asset row. */
enum { JB_BODY, JB_MATERIAL, JB_COLORS, JB_SCALE, JB_ANIM, JB_FOOTSTEP, JB_ACCESSORY, JB_MOUNT, JB_MAGIC, JB_NFIELD };
#define JB_SRC_MAX 112
/* src[sex][field], sex 0 = the class's male rows (and rows that name no sex), 1 = its female rows. A value is "[cond,cond,...]" =
 * the row that has a condition group of exactly these conditions; "JID_..." = that class's row matching this one; anything else =
 * a model's own name (string fields only); empty = as pasted from the base class. */
typedef struct JbLook { char src[2][JB_NFIELD][JB_SRC_MAX]; } JbLook;
typedef struct JbRange { uint16_t off, len; } JbRange;
static const JbRange kJbField[JB_NFIELD][2] = {
    {{0x30, 8}, {0, 0}},        /* body model */
    {{0xe8, 8}, {0, 0}},        /* armor / clothing material */
    {{0x108, 12}, {0, 0}},      /* outfit colors 0 to 2 */
    {{0xf8, 4}, {0, 0}},        /* map scale */
    {{0x80, 16}, {0, 0}},       /* animation list */
    {{0xd8, 8}, {0, 0}},        /* footstep sound */
    {{0x98, 16}, {0, 0}},       /* accessory list */
    {{0x50, 24}, {0x140, 4}},   /* mount model, its two outfits, its coat color */
    {{0x78, 8}, {0, 0}},        /* magic effect */
};
static const char* const kJbFieldName[JB_NFIELD] = {"body", "material", "colors", "scale", "anim", "footstep", "accessories", "mount", "magic"};
static const uint8_t kJbFieldIsString[JB_NFIELD] = {1, 1, 0, 0, 0, 1, 0, 1, 1};
/* "#RRGGBB,#RRGGBB,-" -> three colors of four bytes (red, green, blue, 255); "-" or a missing one = four zeros (no color) */
static void jb_colors(const char* t, uint8_t* out) {
    uint32_t c, k;
    for (c = 0; c < 12; ++c) out[c] = 0;
    for (c = 0; c < 3 && *t; ++c) {
        while (*t == ' ' || *t == ',') t++;
        if (*t == '#') {
            uint32_t v = 0, n = 0;
            for (t++; n < 6; ++t, ++n) {
                const char h = *t;
                const int d = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
                if (d < 0) break;
                v = v << 4 | (uint32_t)d;
            }
            if (n == 6) { for (k = 0; k < 3; ++k) out[c * 4 + k] = (uint8_t)(v >> (16 - 8 * k)); out[c * 4 + 3] = 255; }
        }
        while (*t && *t != ',') t++;
    }
}
static int jb_is_class(const char* s) { return s[0] == 'J' && s[1] == 'I' && s[2] == 'D' && s[3] == '_'; }

/* A row is a class's own when every one of its condition groups names that class or its dismounted half ("<key>下馬"), and at least
 * one names the class itself: the class's models, as opposed to a character's or a situation's row that mentions the class. */
static int jb_own_row(const uint8_t* in, uint64_t r, uint64_t dsz, uint64_t fsize, const char* base) {
    const uint64_t n = jb_u64(in + 0x20 + r + 0x20) & 0xffff, arr = jb_u64(in + 0x20 + r + 0x28);
    char dis[JB_KEY_MAX + 8];
    uint64_t g, len = jb_len(base), named = 0;
    if (!n || len + 7 > sizeof dis) return 0;
    jb_copy((uint8_t*)dis, (const uint8_t*)base, len);
    jb_copy((uint8_t*)dis + len, (const uint8_t*)"\xe4\xb8\x8b\xe9\xa6\xac", 7);
    for (g = 0; g < n; ++g) {
        const uint64_t cn = jb_u64(in + 0x20 + arr + g * 16) & 0xffff, ca = jb_u64(in + 0x20 + arr + g * 16 + 8);
        if (jb_has(in, ca, cn, dsz, fsize, base, 0)) named++;
        else if (!jb_has(in, ca, cn, dsz, fsize, dis, 0)) return 0;
    }
    return named != 0;
}

/* The row of class `src` that corresponds to row r of class `base`: src's own row whose conditions are r's with the class
 * swapped (the same sex, generic-soldier or portrait conditions). 0 when src has none. */
static uint64_t jb_match_row(const uint8_t* in, uint64_t rows, uint64_t rcount, uint64_t r, const char* base, const char* src, uint64_t dsz, uint64_t fsize) {
    const uint64_t n = jb_u64(in + 0x20 + r + 0x20) & 0xffff, arr = jb_u64(in + 0x20 + r + 0x28);
    uint64_t g, cn = 0, ca = 0, i, j;
    for (g = 0; g < n; ++g) {
        cn = jb_u64(in + 0x20 + arr + g * 16) & 0xffff; ca = jb_u64(in + 0x20 + arr + g * 16 + 8);
        if (jb_has(in, ca, cn, dsz, fsize, base, 0)) break;
    }
    if (g == n) return 0;
    for (i = 0; i < rcount; ++i) {
        const uint64_t r2 = rows + i * 0x180, n2 = jb_u64(in + 0x20 + r2 + 0x20) & 0xffff, arr2 = jb_u64(in + 0x20 + r2 + 0x28);
        uint64_t g2;
        if (!n2 || !jb_own_row(in, r2, dsz, fsize, src)) continue;
        for (g2 = 0; g2 < n2; ++g2) {
            const uint64_t cn2 = jb_u64(in + 0x20 + arr2 + g2 * 16) & 0xffff, ca2 = jb_u64(in + 0x20 + arr2 + g2 * 16 + 8);
            if (cn2 != cn || !jb_has(in, ca2, cn2, dsz, fsize, src, 0)) continue;
            for (j = 0; j < cn; ++j) {
                const uint64_t v = jb_u64(in + 0x20 + ca + j * 8);
                const char* t;
                if (v < dsz || v >= fsize) break;
                t = (const char*)(in + 0x20 + v);
                if (!jb_same(t, base) && !jb_has(in, ca2, cn2, dsz, fsize, t, 0)) break;
            }
            if (j == cn) return r2;
        }
    }
    return 0;
}

/* The row with a condition group of exactly the conditions in ref ("[a,b,c]"); 0 when there is none. */
static uint64_t jb_ref_row(const uint8_t* in, uint64_t rows, uint64_t rcount, const char* ref, uint64_t dsz, uint64_t fsize) {
    char cond[8][JB_KEY_MAX];
    uint32_t nc = 0, k = 0;
    uint64_t i, g, j;
    const char* p = ref + 1;
    if (ref[0] != '[') return 0;
    for (; *p && nc < 8; ++p) {
        if (*p == ',' || *p == ']') { cond[nc][k] = 0; if (k) nc++; k = 0; if (*p == ']') break; }
        else if (k < JB_KEY_MAX - 1) cond[nc][k++] = *p;
    }
    if (!nc) return 0;
    for (i = 0; i < rcount; ++i) {
        const uint64_t r = rows + i * 0x180, n = jb_u64(in + 0x20 + r + 0x20) & 0xffff, arr = jb_u64(in + 0x20 + r + 0x28);
        if (!n || arr + n * 16 > dsz) continue;
        for (g = 0; g < n; ++g) {
            const uint64_t cn = jb_u64(in + 0x20 + arr + g * 16) & 0xffff, ca = jb_u64(in + 0x20 + arr + g * 16 + 8);
            if (cn != nc || ca + cn * 8 > dsz) continue;
            for (j = 0; j < nc && jb_has(in, ca, cn, dsz, fsize, cond[j], 0); ++j) {}
            if (j == nc) return r;
        }
    }
    return 0;
}

/* 1 when the row is one of the class's main rows: its group naming the class holds the class alone or the class and a sex. Values
 * chosen one by one (a row reference or a model's name) go to these; the generic-soldier and portrait rows keep the base's. */
static int jb_row_main(const uint8_t* in, uint64_t r, const char* cls, uint64_t dsz, uint64_t fsize) {
    const uint64_t n = jb_u64(in + 0x20 + r + 0x20) & 0xffff, arr = jb_u64(in + 0x20 + r + 0x28);
    uint64_t g;
    for (g = 0; g < n; ++g) {
        const uint64_t cn = jb_u64(in + 0x20 + arr + g * 16) & 0xffff, ca = jb_u64(in + 0x20 + arr + g * 16 + 8);
        if (!jb_has(in, ca, cn, dsz, fsize, cls, 0)) continue;
        return cn == 1 || (cn == 2 && (jb_has(in, ca, cn, dsz, fsize, "\xe7\x94\xb7", 0) || jb_has(in, ca, cn, dsz, fsize, "\xe5\xa5\xb3", 0)));
    }
    return 0;
}

/* 1 when the row's group naming the class also says 女 (female) */
static int jb_row_female(const uint8_t* in, uint64_t r, const char* cls, uint64_t dsz, uint64_t fsize) {
    const uint64_t n = jb_u64(in + 0x20 + r + 0x20) & 0xffff, arr = jb_u64(in + 0x20 + r + 0x28);
    uint64_t g;
    for (g = 0; g < n; ++g) {
        const uint64_t cn = jb_u64(in + 0x20 + arr + g * 16) & 0xffff, ca = jb_u64(in + 0x20 + arr + g * 16 + 8);
        if (jb_has(in, ca, cn, dsz, fsize, cls, 0)) return jb_has(in, ca, cn, dsz, fsize, "\xe5\xa5\xb3", 0);
    }
    return 0;
}

static uint8_t* jb_asset_groups(const uint8_t* in, uint64_t in_size, const JbEntry* entries, uint32_t count, int skip_own, uint64_t* out_size,
                                uint32_t* groups_out, JbAlloc alloc, JbLog log) {
    enum { AROW = 0x180 };
    static const char sheet[] = "\xe3\x82\xa2\xe3\x82\xbb\xe3\x83\x83\xe3\x83\x88\xe3\x83\x86\xe3\x83\xbc\xe3\x83\x96\xe3\x83\xab";      /* アセットテーブル */
    uint64_t dsz, np, nl, pt, lt, st, strs, hdr = 0, rows, rcount, i, k, g, j;
    uint64_t add_data = 0, add_data_slots = 0, add_str_slots = 0, add_strs = 0, new_dsz, new_np, new_st, total, shift, cur, n_data = 0, n_str = 0;
    static uint64_t key_off[256], neg_off[256];
    uint64_t *in_data, *in_str;
    uint8_t *out, *data;
    uint32_t added = 0;
    if (count > 256) count = 256;
    if (in_size < 0x20 || jb_u32(in) != in_size) { log("AssetTable.bin: the size field does not match the file", 0); return 0; }
    dsz = jb_u32(in + 4); np = jb_u32(in + 8); nl = jb_u32(in + 12);
    pt = 0x20 + dsz; lt = pt + np * 8; st = lt + nl * 8;
    if (st >= in_size) { log("AssetTable.bin: the tables do not fit the file", 0); return 0; }
    strs = in_size - st;
    for (i = 0; i < nl; ++i) {
        uint64_t name = jb_u32(in + lt + i * 8 + 4);
        if (name < strs && jb_same((const char*)(in + st + name), sheet)) { hdr = jb_u32(in + lt + i * 8); break; }
    }
    if (!hdr || hdr + 0x30 > dsz) { log("AssetTable.bin: the asset sheet's header was not found", 0); return 0; }
    rcount = jb_u32(in + 0x20 + hdr + 0x10) >> 16;
    rows = jb_u64(in + 0x20 + hdr + 0x28);
    if (!rcount || rows + rcount * AROW > dsz) { log("AssetTable.bin: the asset rows are not where the header says", 0); return 0; }
#define JA_Q(o) jb_u64(in + 0x20 + (o))
/* a group gets a twin for entry k: it names the base, and the row is not one that was pasted as the class's own */
#define JA_POS(k) (jb_has(in, ca, cn, dsz, in_size - 0x20, entries[k].base, 0) && !(skip_own && jb_own_row(in, r, dsz, in_size - 0x20, entries[k].base)))
    /* pass 1: what has to be added */
    for (i = 0; i < rcount; ++i) {
        const uint64_t r = rows + i * AROW, n = JA_Q(r + 0x20) & 0xffff, arr = JA_Q(r + 0x28);
        uint64_t extra = 0, changed = 0;
        if (!n) continue;
        if (arr + n * 16 > dsz) { log("AssetTable.bin: a row's condition groups are outside the data", 0); return 0; }
        for (g = 0; g < n; ++g) {
            const uint64_t cn = JA_Q(arr + g * 16) & 0xffff, ca = JA_Q(arr + g * 16 + 8);
            uint64_t negs = 0;
            if (ca + cn * 8 > dsz) { log("AssetTable.bin: a condition group is outside the data", 0); return 0; }
            for (k = 0; k < count; ++k) {
                if (!entries[k].key[0]) continue;
                if (JA_POS(k)) { extra++; add_data += cn * 8; add_str_slots += cn; }
                else if (jb_has(in, ca, cn, dsz, in_size - 0x20, entries[k].base, 1)) negs++;
            }
            if (negs) { changed = 1; add_data += (cn + negs) * 8; add_str_slots += cn + negs; }
        }
        if (extra || changed) { add_data += (n + extra) * 16; add_data_slots += n + extra; added += (uint32_t)extra; }
    }
    if (!add_data) return 0;
    for (k = 0; k < count; ++k) {
        const uint64_t len = jb_len(entries[k].key);
        key_off[k] = strs + add_strs; add_strs += len + 1;
        neg_off[k] = strs + add_strs; add_strs += len + 2;
    }
    new_dsz = dsz + add_data; new_np = np + add_data_slots + add_str_slots;
    new_st = 0x20 + new_dsz + new_np * 8 + nl * 8;
    shift = new_st - st;
    total = new_st + strs + add_strs;
    out = (uint8_t*)alloc(total);
    in_data = (uint64_t*)alloc(new_np * 8 + 8);
    in_str = (uint64_t*)alloc(new_np * 8 + 8);
    if (!out || !in_data || !in_str) { log("AssetTable.bin: out of memory", 0); return 0; }
    data = out + 0x20;
    jb_copy(out, in, 0x20 + dsz);
    for (i = 0; i < np; ++i) {
        const uint64_t p = jb_u64(in + pt + i * 8);
        if (p + 8 > dsz) { log("AssetTable.bin: a pointer slot is outside the data", 0); return 0; }
        if (JA_Q(p) < dsz) in_data[n_data++] = p;
        else { in_str[n_str++] = p; jb_p64(data + p, JA_Q(p) + shift); }
    }
    /* pass 2: the new arrays, at the end of the data */
    cur = dsz;
#define JA_STR(off) ((off) + new_st - 0x20)
    for (i = 0; i < rcount; ++i) {
        const uint64_t r = rows + i * AROW, n = JA_Q(r + 0x20) & 0xffff, arr = JA_Q(r + 0x28);
        uint64_t extra = 0, changed = 0, narr, w;
        if (!n) continue;
        for (g = 0; g < n; ++g) {
            const uint64_t cn = JA_Q(arr + g * 16) & 0xffff, ca = JA_Q(arr + g * 16 + 8);
            for (k = 0; k < count; ++k) {
                if (!entries[k].key[0]) continue;
                if (JA_POS(k)) extra++;
                else if (jb_has(in, ca, cn, dsz, in_size - 0x20, entries[k].base, 1)) changed = 1;
            }
        }
        if (!extra && !changed) continue;
        narr = cur; cur += (n + extra) * 16;
        w = 0;
        for (g = 0; g < n; ++g) {          /* the row's own groups, a group with "!base" grown by "!key" */
            const uint64_t gh = JA_Q(arr + g * 16), cn = gh & 0xffff, ca = JA_Q(arr + g * 16 + 8);
            uint64_t negs = 0, dst;
            for (k = 0; k < count; ++k)
                if (entries[k].key[0] && !JA_POS(k) && jb_has(in, ca, cn, dsz, in_size - 0x20, entries[k].base, 1)) negs++;
            if (!negs) { jb_p64(data + narr + w * 16, gh); jb_p64(data + narr + w * 16 + 8, ca); }
            else {
                dst = cur; cur += (cn + negs) * 8;
                for (j = 0; j < cn; ++j) { jb_p64(data + dst + j * 8, JA_Q(ca + j * 8) + shift); in_str[n_str++] = dst + j * 8; }
                for (k = 0; k < count; ++k)
                    if (entries[k].key[0] && !JA_POS(k) && jb_has(in, ca, cn, dsz, in_size - 0x20, entries[k].base, 1)) {
                        jb_p64(data + dst + j * 8, JA_STR(neg_off[k])); in_str[n_str++] = dst + j * 8; j++;
                    }
                jb_p64(data + narr + w * 16, (gh & ~(uint64_t)0xffff) | (cn + negs)); jb_p64(data + narr + w * 16 + 8, dst);
            }
            in_data[n_data++] = narr + w * 16 + 8; w++;
        }
        for (g = 0; g < n; ++g) {          /* the twins: the group with the base's key replaced by the new class's */
            const uint64_t gh = JA_Q(arr + g * 16), cn = gh & 0xffff, ca = JA_Q(arr + g * 16 + 8);
            for (k = 0; k < count; ++k) {
                uint64_t dst;
                if (!entries[k].key[0] || !JA_POS(k)) continue;
                dst = cur; cur += cn * 8;
                for (j = 0; j < cn; ++j) {
                    const uint64_t v = JA_Q(ca + j * 8);
                    const int is_base = v >= dsz && jb_same((const char*)(in + 0x20 + v), entries[k].base);
                    jb_p64(data + dst + j * 8, is_base ? JA_STR(key_off[k]) : v + shift);
                    in_str[n_str++] = dst + j * 8;
                }
                jb_p64(data + narr + w * 16, gh); jb_p64(data + narr + w * 16 + 8, dst);
                in_data[n_data++] = narr + w * 16 + 8; w++;
            }
        }
        jb_p64(data + r + 0x20, (JA_Q(r + 0x20) & ~(uint64_t)0xffff) | (n + extra));
        jb_p64(data + r + 0x28, narr);
    }
    if (cur != new_dsz || n_data + n_str != new_np) { log("AssetTable.bin: the two passes disagree, nothing changed", 0); return 0; }
    jb_sort(in_data, n_data); jb_sort(in_str, n_str);
    for (i = 0; i < n_data; ++i) jb_p64(out + 0x20 + new_dsz + i * 8, in_data[i]);
    for (i = 0; i < n_str; ++i) jb_p64(out + 0x20 + new_dsz + (n_data + i) * 8, in_str[i]);
    jb_copy(out + 0x20 + new_dsz + new_np * 8, in + lt, nl * 8);
    jb_copy(out + new_st, in + st, strs);
    for (k = 0; k < count; ++k) {
        const uint64_t len = jb_len(entries[k].key);
        jb_copy(out + new_st + key_off[k], (const uint8_t*)entries[k].key, len + 1);
        out[new_st + neg_off[k]] = '!';
        jb_copy(out + new_st + neg_off[k] + 1, (const uint8_t*)entries[k].key, len + 1);
    }
    jb_p32(out, (uint32_t)total); jb_p32(out + 4, (uint32_t)new_dsz); jb_p32(out + 8, (uint32_t)new_np);
    *out_size = total;
    *groups_out = added;
    return out;
#undef JA_Q
#undef JA_STR
#undef JA_POS
}

/* The class's own asset rows, pasted: for every entry (key, base), each row that is the base's own (jb_own_row) is copied and the
 * copy placed straight after it, with the groups that name the base renamed to the key (groups for the dismounted half are left to
 * the base). The copies are what the creator edits; everything after a copy moves down by one row and the rows are renumbered.
 * Returns the new image, or 0 when no row was pasted. *rows_out = the number of rows pasted. */
static uint8_t* jb_asset_own(const uint8_t* in, uint64_t in_size, const JbEntry* entries, const JbLook* looks, uint32_t count, uint64_t* out_size,
                             uint32_t* rows_out, JbAlloc alloc, JbLog log) {
    enum { AROW = 0x180 };
    static const char sheet[] = "\xe3\x82\xa2\xe3\x82\xbb\xe3\x83\x83\xe3\x83\x88\xe3\x83\x86\xe3\x83\xbc\xe3\x83\x96\xe3\x83\xab";
    uint64_t dsz, np, nl, pt, lt, st, strs, hdr = 0, rows, rcount, rend, i, k, g, j, pasted = 0, add_tail = 0, add_data_slots = 0, add_str_slots = 0, add_strs = 0;
    uint64_t new_dsz, new_np, new_st, total, shift, delta, cur, n_data = 0, n_str = 0, line = 0, pos;
    static uint64_t key_off[256];
    static uint64_t raw_off[256][2][JB_NFIELD];          /* a field given as a model's own name: where that string goes */
    static uint64_t lit_off[256][2];                     /* accessories given as a list "{a,b}": where its strings go */
    static uint8_t lit_n[256][2], lit_used[256][2];
    uint32_t* before;          /* rows pasted before row i */
    uint32_t f;
    uint64_t *in_data, *in_str;
    uint8_t *out, *data;
    if (count > 256) count = 256;
    if (in_size < 0x20 || jb_u32(in) != in_size) return 0;
    dsz = jb_u32(in + 4); np = jb_u32(in + 8); nl = jb_u32(in + 12);
    pt = 0x20 + dsz; lt = pt + np * 8; st = lt + nl * 8;
    if (st >= in_size) return 0;
    strs = in_size - st;
    for (i = 0; i < nl; ++i) {
        uint64_t name = jb_u32(in + lt + i * 8 + 4);
        if (name < strs && jb_same((const char*)(in + st + name), sheet)) { hdr = jb_u32(in + lt + i * 8); break; }
    }
    if (!hdr || hdr + 0x30 > dsz) return 0;
    rcount = jb_u32(in + 0x20 + hdr + 0x10) >> 16;
    rows = jb_u64(in + 0x20 + hdr + 0x28);
    rend = rows + rcount * AROW;
    if (!rcount || rend > dsz || rend > hdr) return 0;
#define JO_Q(o) jb_u64(in + 0x20 + (o))
    before = (uint32_t*)alloc((rcount + 1) * 4);
    if (!before) return 0;
    for (i = 0; i < rcount; ++i) {
        const uint64_t r = rows + i * AROW, ln = (uint64_t)in[0x20 + r + 0x16] | (uint64_t)in[0x20 + r + 0x17] << 8;
        uint64_t slots = 0, counted = 0;
        if (ln > line) line = ln;
        before[i] = (uint32_t)pasted;
        for (k = 0; k < count; ++k) {
            const uint64_t n = JO_Q(r + 0x20) & 0xffff, arr = JO_Q(r + 0x28);
            uint64_t m = 0;
            if (!entries[k].key[0] || !jb_own_row(in, r, dsz, in_size - 0x20, entries[k].base)) continue;
            if (!counted) {          /* the row's pointer slots, but for its key (+8) and its groups (+0x28, made anew) */
                for (j = 0; j < np; ++j) { const uint64_t p = jb_u64(in + pt + j * 8); if (p >= r && p < r + AROW && p != r + 8) slots++; }
                counted = 1;
            }
            for (g = 0; g < n; ++g) {
                const uint64_t cn = JO_Q(arr + g * 16) & 0xffff, ca = JO_Q(arr + g * 16 + 8);
                if (jb_has(in, ca, cn, dsz, in_size - 0x20, entries[k].base, 0)) { m++; add_tail += 16 + cn * 8; add_str_slots += cn; add_data_slots++; }
            }
            add_data_slots += slots + 0x180 / 8;          /* an upper bound: the row's slots, and room for fields taken from other rows */
            pasted++;
            (void)m;
        }
    }
    before[rcount] = (uint32_t)pasted;
    if (!pasted) return 0;
    if (line + pasted > 0xffff) { log("AssetTable.bin: too many rows to number", 0); return 0; }
    for (k = 0; k < count; ++k) { key_off[k] = strs + add_strs; add_strs += jb_len(entries[k].key) + 1; }
    for (k = 0; k < count; ++k)
        for (f = 0; f < 2 * JB_NFIELD; ++f) {
            const char* v = looks ? looks[k].src[f / JB_NFIELD][f % JB_NFIELD] : "";
            raw_off[k][f / JB_NFIELD][f % JB_NFIELD] = 0;
            if (v[0] && v[0] != '[' && !jb_is_class(v) && !jb_same(v, "none") && kJbFieldIsString[f % JB_NFIELD]) { raw_off[k][f / JB_NFIELD][f % JB_NFIELD] = strs + add_strs; add_strs += jb_len(v) + 1; }
        }
    /* an accessory list written out: "{model(point),model(point)}". Its strings are new, and each main row that takes it (four
     * at most are provided for) gets an array of pointers to them. */
    for (k = 0; k < count; ++k)
        for (f = 0; f < 2; ++f) {
            const char* v = looks ? looks[k].src[f][JB_ACCESSORY] : "";
            uint64_t n = 0, x;
            lit_off[k][f] = 0; lit_n[k][f] = 0; lit_used[k][f] = 0;
            if (v[0] != '{') continue;
            for (x = 1; v[x] && v[x] != '}'; ++x) if (x == 1 || v[x - 1] == ',') n++;
            if (n > 16) n = 16;
            lit_off[k][f] = strs + add_strs; lit_n[k][f] = (uint8_t)n;
            add_strs += jb_len(v) + 1; add_tail += n * 8 * 4; add_str_slots += n * 4;
        }
    delta = pasted * AROW;
    new_dsz = dsz + delta + add_tail; new_np = np + add_data_slots + add_str_slots;
    out = (uint8_t*)alloc(0x20 + new_dsz + new_np * 8 + nl * 8 + strs + add_strs);
    in_data = (uint64_t*)alloc(new_np * 8 + 8);
    in_str = (uint64_t*)alloc(new_np * 8 + 8);
    if (!out || !in_data || !in_str) { log("AssetTable.bin: out of memory", 0); return 0; }
    data = out + 0x20;
/* where an old data offset is now: a row moves down by the rows pasted before it, everything after the rows by all of them */
#define JO_MV(o) ((o) < rows ? (o) : (o) >= rend ? (o) + delta : (o) + (uint64_t)before[((o) - rows) / AROW] * AROW)
    jb_copy(out, in, 0x20);
    jb_copy(data, in + 0x20, rows);
    for (i = 0; i < rcount; ++i) jb_copy(data + JO_MV(rows + i * AROW), in + 0x20 + rows + i * AROW, AROW);
    jb_copy(data + rend + delta, in + 0x20 + rend, dsz - rend);
    for (i = 0; i < np; ++i) {
        const uint64_t p = jb_u64(in + pt + i * 8);
        uint64_t v;
        if (p + 8 > dsz) { log("AssetTable.bin: a pointer slot is outside the data", 0); return 0; }
        v = JO_Q(p);
        if (v < dsz) { in_data[n_data++] = JO_MV(p); jb_p64(data + JO_MV(p), JO_MV(v)); }
        else in_str[n_str++] = JO_MV(p);          /* string pointers are shifted at the end */
    }
    /* the pasted rows, each straight after the row it is a copy of */
    cur = dsz + delta;
    pos = 0;
    for (i = 0; i < rcount; ++i) {
        const uint64_t r = rows + i * AROW, nr0 = JO_MV(r);
        uint64_t copies = 0;
        data[nr0 + 0x10] = (uint8_t)(i + before[i]); data[nr0 + 0x11] = (uint8_t)((i + before[i]) >> 8);
        for (k = 0; k < count; ++k) {
            const uint64_t n = JO_Q(r + 0x20) & 0xffff, arr = JO_Q(r + 0x28);
            uint64_t nr, narr, m = 0, idx;
            if (!entries[k].key[0] || !jb_own_row(in, r, dsz, in_size - 0x20, entries[k].base)) continue;
            copies++;
            nr = nr0 + copies * AROW;
            idx = i + before[i] + copies;
            jb_copy(data + nr, data + nr0, AROW);          /* the original as relocated: its data pointers are already moved */
            jb_p64(data + nr + 8, 0);                       /* no key of its own */
            data[nr + 0x10] = (uint8_t)idx; data[nr + 0x11] = (uint8_t)(idx >> 8);
            data[nr + 0x16] = (uint8_t)(line + 1 + pos); data[nr + 0x17] = (uint8_t)((line + 1 + pos) >> 8);
            pos++;
            {
                /* fields taken from elsewhere (asset.txt): per 8-byte word of the row, the row it comes from (0 = this one), or
                 * ~0 for a word that becomes a string of its own */
                uint64_t from[AROW / 8];
                uint32_t w, q;
                for (w = 0; w < AROW / 8; ++w) from[w] = 0;
                const int sex = jb_row_female(in, r, entries[k].base, dsz, in_size - 0x20);
                const int is_main = jb_row_main(in, r, entries[k].base, dsz, in_size - 0x20);
                for (f = 0; looks && f < JB_NFIELD; ++f) {
                    const char* src = looks[k].src[sex][f];
                    uint64_t sr;
                    /* the body model goes to every own row that has one (generic soldiers and named characters have rows of their
                     * own); the other fields to the main rows, unless a whole class is named */
                    const int body_row = f == JB_BODY && JO_Q(r + kJbField[JB_BODY][0].off) != 0;
                    if (!src[0]) continue;
                    if (f == JB_COLORS && (src[0] == '#' || src[0] == '-')) {          /* colors written out: the main rows and every own row with colors */
                        uint32_t any = 0;
                        for (q = 0; q < 12; ++q) any |= in[0x20 + r + 0x108 + q];
                        if (!is_main && !any) continue;
                    } else
                    if (!jb_is_class(src) && !is_main && !body_row) continue;
                    if (jb_same(src, "none")) {          /* the field emptied: zeros, and no pointer where one was */
                        for (q = 0; q < 2; ++q) {
                            const JbRange rg = kJbField[f][q];
                            uint32_t z;
                            for (z = 0; z < rg.len; ++z) data[nr + rg.off + z] = 0;
                            for (w = rg.off / 8; w < (uint32_t)(rg.off + rg.len + 7) / 8 && rg.len >= 8; ++w) from[w] = ~(uint64_t)0 - 1;
                        }
                        continue;
                    }
                    if (f == JB_COLORS && (src[0] == '#' || src[0] == '-')) {          /* "#RRGGBB,#RRGGBB,-": the three colors, - = none */
                        uint8_t col[12];
                        jb_colors(src, col);
                        for (q = 0; q < 12; ++q) data[nr + 0x108 + q] = col[q];
                        continue;
                    }
                    if (f == JB_ACCESSORY && src[0] == '{') {
                        const uint64_t n = lit_n[k][sex];
                        uint64_t arr = 0, x, e = 0, off = lit_off[k][sex];
                        if (lit_used[k][sex] >= 4) continue;
                        lit_used[k][sex]++;
                        if (n) {
                            arr = cur; cur += n * 8;
                            for (x = 1; src[x] && src[x] != '}' && e < n; ++x)
                                if (x == 1 || src[x - 1] == ',') {
                                    jb_p64(data + arr + e * 8, off | (uint64_t)1 << 63); in_str[n_str++] = arr + e * 8; e++;
                                    { uint64_t y = x; while (src[y] && src[y] != ',' && src[y] != '}') y++; off += y - x + 1; }
                                }
                        }
                        data[nr + 0x98] = (uint8_t)n; data[nr + 0x99] = 0;
                        jb_p64(data + nr + 0xa0, arr);
                        if (n) in_data[n_data++] = nr + 0xa0;
                        from[0x98 / 8] = from[0xa0 / 8] = ~(uint64_t)0 - 1;
                        continue;
                    }
                    if (src[0] != '[' && !jb_is_class(src)) {
                        if (raw_off[k][sex][f]) { from[kJbField[f][0].off / 8] = ~(uint64_t)0; jb_p64(data + nr + kJbField[f][0].off, raw_off[k][sex][f] | (uint64_t)1 << 63); }
                        continue;
                    }
                    sr = src[0] == '[' ? jb_ref_row(in, rows, rcount, src, dsz, in_size - 0x20)
                                       : jb_match_row(in, rows, rcount, r, entries[k].base, src, dsz, in_size - 0x20);
                    if (!sr && body_row && jb_is_class(src)) {          /* no row with the same conditions: that class's main row of the sex */
                        uint64_t i2;
                        for (i2 = 0; i2 < rcount && !sr; ++i2) {
                            const uint64_t r2 = rows + i2 * AROW;
                            if (jb_row_main(in, r2, src, dsz, in_size - 0x20) && jb_row_female(in, r2, src, dsz, in_size - 0x20) == sex) sr = r2;
                        }
                    }
                    if (!sr) continue;
                    for (q = 0; q < 2; ++q) {
                        const JbRange rg = kJbField[f][q];
                        if (!rg.len) continue;
                        jb_copy(data + nr + rg.off, in + 0x20 + sr + rg.off, rg.len);
                        for (w = rg.off / 8; w < (uint32_t)(rg.off + rg.len + 7) / 8 && rg.len >= 8; ++w) from[w] = sr;
                    }
                }
                for (j = 0; j < np; ++j) {
                    const uint64_t p = jb_u64(in + pt + j * 8);
                    if (p >= r && p < r + AROW && p != r + 8 && !from[(p - r) / 8]) {
                        if (JO_Q(p) < dsz) in_data[n_data++] = nr + (p - r);
                        else in_str[n_str++] = nr + (p - r);
                    }
                }
                for (w = 0; w < AROW / 8; ++w) {
                    if (!from[w]) continue;
                    if (from[w] == ~(uint64_t)0) { in_str[n_str++] = nr + w * 8; continue; }
                    if (from[w] == ~(uint64_t)0 - 1) continue;
                    for (j = 0; j < np; ++j) {          /* the word in the row it was taken from: a pointer there is one here */
                        if (jb_u64(in + pt + j * 8) != from[w] + w * 8) continue;
                        if (JO_Q(from[w] + w * 8) < dsz) { in_data[n_data++] = nr + w * 8; jb_p64(data + nr + w * 8, JO_MV(JO_Q(from[w] + w * 8))); }
                        else in_str[n_str++] = nr + w * 8;
                        break;
                    }
                }
            }
            for (g = 0; g < n; ++g) {
                const uint64_t cn = JO_Q(arr + g * 16) & 0xffff, ca = JO_Q(arr + g * 16 + 8);
                if (jb_has(in, ca, cn, dsz, in_size - 0x20, entries[k].base, 0)) m++;
            }
            narr = cur; cur += m * 16;
            m = 0;
            for (g = 0; g < n; ++g) {
                const uint64_t gh = JO_Q(arr + g * 16), cn = gh & 0xffff, ca = JO_Q(arr + g * 16 + 8);
                uint64_t dst;
                if (!jb_has(in, ca, cn, dsz, in_size - 0x20, entries[k].base, 0)) continue;
                dst = cur; cur += cn * 8;
                for (j = 0; j < cn; ++j) {
                    const uint64_t v = JO_Q(ca + j * 8);
                    const int is_base = v >= dsz && v < in_size - 0x20 && jb_same((const char*)(in + 0x20 + v), entries[k].base);
                    /* marked with the top bit: a new string's offset, placed once the string area's position is known */
                    jb_p64(data + dst + j * 8, is_base ? (key_off[k] | (uint64_t)1 << 63) : v);
                    in_str[n_str++] = dst + j * 8;
                }
                jb_p64(data + narr + m * 16, gh); jb_p64(data + narr + m * 16 + 8, dst);
                in_data[n_data++] = narr + m * 16 + 8; m++;
            }
            jb_p64(data + nr + 0x20, (JO_Q(r + 0x20) & ~(uint64_t)0xffff) | m);
            jb_p64(data + nr + 0x28, narr);
        }
    }
    new_dsz = cur; new_np = n_data + n_str;
    new_st = 0x20 + new_dsz + new_np * 8 + nl * 8;
    shift = new_st - st;
    total = new_st + strs + add_strs;
    for (i = 0; i < n_str; ++i) {
        const uint64_t v = jb_u64(data + in_str[i]);
        jb_p64(data + in_str[i], v >> 63 ? (v & ~((uint64_t)1 << 63)) + new_st - 0x20 : v + shift);
    }
    jb_p32(data + JO_MV(hdr) + 0x10, (uint32_t)((rcount + pasted) << 16) | (jb_u32(in + 0x20 + hdr + 0x10) & 0xffff));
    jb_sort(in_data, n_data); jb_sort(in_str, n_str);
    for (i = 0; i < n_data; ++i) jb_p64(out + 0x20 + new_dsz + i * 8, in_data[i]);
    for (i = 0; i < n_str; ++i) jb_p64(out + 0x20 + new_dsz + (n_data + i) * 8, in_str[i]);
    for (i = 0; i < nl; ++i) {
        jb_p32(out + 0x20 + new_dsz + new_np * 8 + i * 8, (uint32_t)JO_MV((uint64_t)jb_u32(in + lt + i * 8)));
        jb_p32(out + 0x20 + new_dsz + new_np * 8 + i * 8 + 4, jb_u32(in + lt + i * 8 + 4));
    }
    jb_copy(out + new_st, in + st, strs);
    for (k = 0; k < count; ++k) jb_copy(out + new_st + key_off[k], (const uint8_t*)entries[k].key, jb_len(entries[k].key) + 1);
    for (k = 0; k < count; ++k)
        for (f = 0; f < 2 * JB_NFIELD; ++f)
            if (raw_off[k][f / JB_NFIELD][f % JB_NFIELD])
                jb_copy(out + new_st + raw_off[k][f / JB_NFIELD][f % JB_NFIELD], (const uint8_t*)looks[k].src[f / JB_NFIELD][f % JB_NFIELD],
                        jb_len(looks[k].src[f / JB_NFIELD][f % JB_NFIELD]) + 1);
    for (k = 0; k < count; ++k)
        for (f = 0; f < 2; ++f) {
            const char* v = looks ? looks[k].src[f][JB_ACCESSORY] : "";
            uint64_t x, o = lit_off[k][f];
            if (!o) continue;
            for (x = 1; v[x] && v[x] != '}'; ++x) out[new_st + o++] = (uint8_t)(v[x] == ',' ? 0 : v[x]);
            out[new_st + o] = 0;
        }
    jb_p32(out, (uint32_t)total); jb_p32(out + 4, (uint32_t)new_dsz); jb_p32(out + 8, (uint32_t)new_np); jb_p32(out + 12, (uint32_t)nl);
    *out_size = total;
    *rows_out = (uint32_t)pasted;
    return out;
#undef JO_Q
#undef JO_MV
}

/* job.txt: "[JID_Key]" starts an entry, "base = JID_Other" names the row it copies (JID_無し when absent); '#' or ';' starts a
 * comment line. Returns the number of entries written to out (at most max). */
static uint32_t jb_parse(const char* text, uint64_t size, JbEntry* out, uint32_t max) {
    uint64_t i = 0;
    uint32_t n = 0;
    static const char none[] = "JID_\xe7\x84\xa1\xe3\x81\x97";
    if (size >= 3 && (uint8_t)text[0] == 0xef && (uint8_t)text[1] == 0xbb && (uint8_t)text[2] == 0xbf) i = 3;
    while (i < size) {
        uint64_t e = i, a, b;
        while (e < size && text[e] != '\n') e++;
        a = i; b = e;
        while (a < b && (text[a] == ' ' || text[a] == '\t')) a++;
        while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t' || text[b - 1] == '\r')) b--;
        if (b > a + 2 && text[a] == '[' && text[b - 1] == ']') {
            if (n < max && b - a - 2 < JB_KEY_MAX) {
                uint64_t k;
                for (k = 0; k < b - a - 2; ++k) out[n].key[k] = text[a + 1 + k];
                out[n].key[k] = 0;
                for (k = 0; none[k]; ++k) out[n].base[k] = none[k];
                out[n].base[k] = 0;
                out[n].name[0] = 0; out[n].help[0] = 0; out[n].master[0] = 0; out[n].icon[0] = 0; out[n].attr[0] = 0; out[n].dismount[0] = 0; out[n].lower[0] = 0; out[n].upper[0] = 0; out[n].no_mount = 0; out[n].mount_from[0] = 0; out[n].skills[0] = 0; out[n].barred[0] = 0; out[n].has_sex = 0; out[n].sex = 0; out[n].has_cexp = 0; out[n].has_abil = 0; out[n].abil[0][0] = out[n].abil[1][0] = out[n].abil[2][0] = 0;
                for (k = 0; k < JB_NBLOCK; ++k) out[n].has[k] = 0;
                n++;
            }
        } else if (n && b > a + 4 && text[a] == 'b' && text[a + 1] == 'a' && text[a + 2] == 's' && text[a + 3] == 'e') {
            uint64_t v = a + 4, k = 0;
            while (v < b && (text[v] == ' ' || text[v] == '\t')) v++;
            if (v < b && text[v] == '=') {
                v++;
                while (v < b && (text[v] == ' ' || text[v] == '\t')) v++;
                if (b - v && b - v < JB_KEY_MAX) { for (; v < b; ++v) out[n - 1].base[k++] = text[v]; out[n - 1].base[k] = 0; }
            }
        } else if (n && b > a && text[a] != '#' && text[a] != ';') {          /* name, help, or a block of numbers */
            uint64_t v = a, k = 0, eq;
            uint32_t bl;
            char word[20];
            while (v < b && k < 19 && ((text[v] >= 'a' && text[v] <= 'z') || text[v] == '_')) word[k++] = text[v++];
            word[k] = 0;
            while (v < b && (text[v] == ' ' || text[v] == '\t')) v++;
            eq = v;
            if (eq < b && text[eq] == '=') {
                v = eq + 1;
                while (v < b && (text[v] == ' ' || text[v] == '\t')) v++;
                if (jb_same(word, "icon")) { for (k = 0; v < b && k < JB_KEY_MAX - 1; ++v) out[n - 1].icon[k++] = text[v]; out[n - 1].icon[k] = 0; }
                else if (jb_same(word, "abilities")) {
                    uint32_t a3 = 0, c3 = 0;
                    out[n - 1].has_abil = 1; out[n - 1].abil[0][0] = out[n - 1].abil[1][0] = out[n - 1].abil[2][0] = 0;
                    for (; v < b && a3 < 3; ++v) {
                        if (text[v] == ',') { out[n - 1].abil[a3][c3] = 0; a3++; c3 = 0; }
                        else if (text[v] != ' ' && text[v] != '\t' && c3 < JB_KEY_MAX - 1) out[n - 1].abil[a3][c3++] = text[v];
                    }
                    if (a3 < 3) out[n - 1].abil[a3][c3] = 0;
                    for (a3 = 0; a3 < 3; ++a3) if (jb_same(out[n - 1].abil[a3], "none")) out[n - 1].abil[a3][0] = 0;
                }
                else if (jb_same(word, "skills")) { for (k = 0; v < b && k < JB_KEY_MAX - 1; ++v) out[n - 1].skills[k++] = text[v]; out[n - 1].skills[k] = 0; }
                else if (jb_same(word, "barred")) { for (k = 0; v < b && k < JB_KEY_MAX - 1; ++v) out[n - 1].barred[k++] = text[v]; out[n - 1].barred[k] = 0; }
                else if (jb_same(word, "class_exp")) {
                    uint32_t x2 = 0, val = 0, any = 0;
                    for (; v <= b && x2 < 2; ++v) {
                        if (v < b && text[v] >= '0' && text[v] <= '9') { val = val * 10 + (uint32_t)(text[v] - '0'); any = 1; }
                        else if (any) { out[n - 1].cexp[x2++] = (uint16_t)val; val = 0; any = 0; }
                    }
                    out[n - 1].has_cexp = x2 == 2;
                }
                else if (jb_same(word, "sex")) { if (v < b && text[v] >= '0' && text[v] <= '2') { out[n - 1].sex = (int8_t)(text[v] - '0'); out[n - 1].has_sex = 1; } }
                else if (jb_same(word, "dismount")) { for (k = 0; v < b && k < JB_KEY_MAX - 1; ++v) out[n - 1].dismount[k++] = text[v]; out[n - 1].dismount[k] = 0; }
                else if (jb_same(word, "lower")) { for (k = 0; v < b && k < JB_KEY_MAX - 1; ++v) out[n - 1].lower[k++] = text[v]; out[n - 1].lower[k] = 0; }
                else if (jb_same(word, "upper")) { for (k = 0; v < b && k < JB_KEY_MAX - 1; ++v) out[n - 1].upper[k++] = text[v]; out[n - 1].upper[k] = 0; }
                else if (jb_same(word, "mount")) {
                    if (v < b && text[v] == 'n') out[n - 1].no_mount = 1;
                    else { for (k = 0; v < b && k < JB_KEY_MAX - 1; ++v) out[n - 1].mount_from[k++] = text[v]; out[n - 1].mount_from[k] = 0; }
                }
                else if (jb_same(word, "attributes")) { for (k = 0; v < b && k < JB_KEY_MAX - 1; ++v) out[n - 1].attr[k++] = text[v]; out[n - 1].attr[k] = 0; }
                else if (jb_same(word, "master_skill")) { for (k = 0; v < b && k < JB_KEY_MAX - 1; ++v) out[n - 1].master[k++] = text[v]; out[n - 1].master[k] = 0; }
                else if (jb_same(word, "name")) { for (k = 0; v < b && k < JB_NAME_MAX - 1; ++v) out[n - 1].name[k++] = text[v]; out[n - 1].name[k] = 0; }
                else if (jb_same(word, "help")) {          /* "\n" in the text is a line break */
                    for (k = 0; v < b && k < JB_HELP_MAX - 1; ++v) {
                        if (text[v] == '\\' && v + 1 < b && text[v + 1] == 'n') { out[n - 1].help[k++] = '\n'; v++; }
                        else out[n - 1].help[k++] = text[v];
                    }
                    out[n - 1].help[k] = 0;
                } else
                    for (bl = 0; bl < JB_NBLOCK; ++bl) {
                        uint32_t x = 0;
                        if (!jb_same(word, kJbBlock[bl].name)) continue;
                        while (v < b && x < kJbBlock[bl].count) {
                            int neg = 0, any = 0;
                            long num = 0;
                            while (v < b && (text[v] == ' ' || text[v] == ',' || text[v] == '\t')) v++;
                            if (v < b && text[v] == '-') { neg = 1; v++; }
                            while (v < b && text[v] >= '0' && text[v] <= '9') { num = num * 10 + (text[v] - '0'); v++; any = 1; if (num > 30000) num = 30000; }
                            if (!any) break;
                            out[n - 1].val[bl][x++] = (int16_t)(neg ? -num : num);
                        }
                        if (x == kJbBlock[bl].count) out[n - 1].has[bl] = 1;          /* a short list is not taken */
                        break;
                    }
            }
        }
        i = e + 1;
    }
    return n;
}

/* job.ini: read the mod's job.txt unless a line says "enabled = false" (or 0, no, off). */
static int jb_enabled(const char* text, uint64_t size) {
    uint64_t i = 0;
    while (i < size) {
        uint64_t e = i, a;
        while (e < size && text[e] != '\n') e++;
        a = i;
        while (a < e && (text[a] == ' ' || text[a] == '\t')) a++;
        if (e - a > 7 && text[a] == 'e' && text[a + 1] == 'n' && text[a + 2] == 'a' && text[a + 3] == 'b' && text[a + 4] == 'l' && text[a + 5] == 'e' && text[a + 6] == 'd') {
            uint64_t v = a + 7;
            while (v < e && (text[v] == ' ' || text[v] == '\t')) v++;
            if (v < e && text[v] == '=') {
                v++;
                while (v < e && (text[v] == ' ' || text[v] == '\t')) v++;
                if (v < e) return !(text[v] == 'f' || text[v] == 'F' || text[v] == '0' || text[v] == 'n' || text[v] == 'N' || (text[v] == 'o' && v + 1 < e && text[v + 1] == 'f'));
            }
        }
        i = e + 1;
    }
    return 1;
}

/* asset.txt: "[JID_Key]" names a created class (an entry of job.txt), then "field = value" lines with the fields of kJbFieldName;
 * "field.m" / "field.f" set the value for the class's male or female rows only.
 * The values are written into looks[] at the index of the entry with that key; lines for a key no entry has are ignored. */
static void jb_parse_looks(const char* text, uint64_t size, const JbEntry* entries, uint32_t count, JbLook* looks) {
    uint64_t i = 0;
    int cur = -1;
    if (size >= 3 && (uint8_t)text[0] == 0xef && (uint8_t)text[1] == 0xbb && (uint8_t)text[2] == 0xbf) i = 3;
    while (i < size) {
        uint64_t e = i, a, b, k;
        uint32_t f, n;
        while (e < size && text[e] != '\n') e++;
        a = i; b = e;
        while (a < b && (text[a] == ' ' || text[a] == '\t')) a++;
        while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t' || text[b - 1] == '\r')) b--;
        if (b > a + 2 && text[a] == '[' && text[b - 1] == ']') {
            cur = -1;
            for (n = 0; n < count; ++n) {
                for (k = 0; k < b - a - 2 && entries[n].key[k] == text[a + 1 + k]; ++k) {}
                if (k == b - a - 2 && !entries[n].key[k]) { cur = (int)n; break; }
            }
        } else if (cur >= 0 && b > a && text[a] != '#' && text[a] != ';') {
            for (f = 0; f < JB_NFIELD; ++f) {
                const char* name = kJbFieldName[f];
                uint64_t v = a;
                for (k = 0; name[k] && v < b && text[v] == name[k]; ++k, ++v) {}
                int sex = 2;          /* both, unless the name ends in .m or .f */
                if (name[k]) continue;
                if (v + 1 < b && text[v] == '.' && (text[v + 1] == 'm' || text[v + 1] == 'f')) { sex = text[v + 1] == 'f'; v += 2; }
                while (v < b && (text[v] == ' ' || text[v] == '\t')) v++;
                if (v >= b || text[v] != '=') continue;
                v++;
                while (v < b && (text[v] == ' ' || text[v] == '\t')) v++;
                if (b - v < JB_SRC_MAX) {
                    uint64_t x;
                    for (k = 0, x = v; x < b; ++x, ++k) {
                        if (sex != 1) looks[cur].src[0][f][k] = text[x];
                        if (sex != 0) looks[cur].src[1][f][k] = text[x];
                    }
                    if (sex != 1) looks[cur].src[0][f][k] = 0;
                    if (sex != 0) looks[cur].src[1][f][k] = 0;
                }
                break;
            }
        }
        i = e + 1;
    }
}
#endif
