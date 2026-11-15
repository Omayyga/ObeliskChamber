/* Create New Class 1.0 - a code mod for Obelisk Chamber (the Fire Emblem: Fortune's Weave recompilation), mod interface 4.
 * Contains nothing of the game.
 *
 * Makes classes of one's own from the Class Change screen (left stick click opens the creator). Each class is an entry of this
 * mod's job.txt, added as a row of the game's class table (Job.bin) when the game's files are set up (jobbin.h); its look is an
 * entry of asset.txt, added as rows of the game's look table (AssetTable.bin). Numbers in a loaded row are changed in memory
 * at once; anything that needs a row the game reads only at start is written to the files and applies after a restart.
 * Written for update 131072 of the game: every address below is of that build's main. The findings behind them are in the
 * project's notes (Class_Creator_*.md).
 *
 * Logging: warnings always; the detailed trace only with "debug = true" in job.ini.
 *
 * Build: clang --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin -fno-stack-protector -c create_class.c
 *        lld-link /DLL /NOENTRY /NODEFAULTLIB /OUT:create_class.dll create_class.obj
 */
#include <stdint.h>

/* the start of the runtime's Ctx (recomp/include/nxrecomp/ctx.h): only the general registers are used here, and a Ctx is never
 * copied or made by this mod */
typedef struct Ctx { uint64_t x[32]; } Ctx;

typedef void (*NxModInlineFn)(Ctx*, uint64_t, void*);
typedef int (*NxModSiteFn)(Ctx*, uint64_t, void*);
typedef void (*NxModFrameFn)(Ctx*, void*);
typedef void (*NxModFunctionFn)(Ctx*, void*);
typedef struct NxModFile {
    const char* path;
    const uint8_t* data;
    uint64_t size;
    void (*replace)(const struct NxModFile*, const void*, uint64_t);
} NxModFile;
typedef void (*NxModFileFn)(const NxModFile*, void*);
/* version 4: a list screen in the game's style (rows with a label and a value; left / right and confirm are ours) */
typedef struct NxModList {
    uint32_t size;
    int pages;
    const char* titles[3];
    int count;
    void* user;
    int (*page)(int, void*);
    void (*label)(int, char*, uint64_t, void*);
    void (*value)(int, char*, uint64_t, void*);
    void (*help)(int, char*, uint64_t, void*);
    uint32_t (*pick)(Ctx*, int, void*);
    void (*back)(void*);
    int (*side)(Ctx*, int, int, void*);
} NxModList;
typedef struct NxModApi {
    uint32_t version, size;
    const char* game_id;
    uint64_t main_base;
    void (*log)(const struct NxModApi*, int, const char*);
    uint32_t (*hook_inline)(uint64_t, NxModInlineFn, void*);
    uint32_t (*hook_mid)(uint64_t, NxModSiteFn, void*);
    uint32_t (*hook_entry)(uint64_t, NxModSiteFn, void*);
    int (*hook_remove)(uint32_t);
    uint64_t (*call_guest)(Ctx*, uint64_t, const uint64_t*, int);
    int (*is_mapped)(uint64_t, uint64_t);
    void (*on_frame)(NxModFrameFn, void*);
    int (*is_function)(uint64_t);
    /* version 2 */
    uint64_t (*make_function)(NxModFunctionFn, void*);
    int (*make_writable)(uint64_t, uint64_t);
    /* version 3 */
    int (*edit_file)(const char*, NxModFileFn, void*);
    uint32_t (*mod_count)(void);
    const char* (*mod_folder)(uint32_t);
    int64_t (*read_file)(const char*, void*, uint64_t);
    int (*write_file)(const char*, const void*, uint64_t);
    int64_t (*zstd_decompress)(const void*, uint64_t, void*, uint64_t);
    int64_t (*zstd_compress)(const void*, uint64_t, void*, uint64_t, int);
    void* (*alloc)(uint64_t);
    void (*free_mem)(void*);
    /* version 4 */
    int (*open_list)(Ctx*, uint64_t, const NxModList*);
} NxModApi;
#define API_V3_SIZE ((uint32_t)(uint64_t)&((NxModApi*)0)->open_list)          /* the table as version 3 has it */

#include "jobbin.h"
#include "castrows.h"
#include "bprop.h"
#include "msbt.h"

enum { SLOTS = 71, SLOT_TEXT = 2, SLOT_MID = 3, SLOT_SELECT = 20, SLOT_ACALL = 25, SLOT_BCALL = 26, SLOT_NEXT = 39, SLOT_PREV = 40, SLOT_DECIDE_SE = 41 };
#define ALLOCATE   0x652310ull     /* HeapManager::Allocate(heap, size, align, 0) */
#define ADD_ITEM   0x1714cc0ull    /* BasicMenu::AddItem(menu, item) */
#define HEAP_GOT   0x2169fa8ull
#define VT_SHAPE   0x2128ee8ull    /* a plain 71-slot menu item (the System list's mouse guide entry): the shape of ours */
#define ITEM_BYTES 0x90ull

/* the five Information entry classes: vtable, and the ACall its slot 25 holds in this build */

static const NxModApi* g_api;
static int g_trace;          /* "debug = true" in job.ini: the detailed trace of what the mod does, for finding faults */
#define TRACE(LINE) do { if (g_trace) g_api->log(g_api, 2, (LINE)); } while (0)
static uint64_t g_base;
static uint64_t g_vt[2 + SLOTS];       /* our item's vtable: two header words, then the slots */
static int g_hooked, g_failed;

/* the compiler may ask for these even in a freestanding build */
int _fltused = 0;      /* the linker wants this as soon as a float is used */
void* memset(void* d, int v, unsigned long long n) { volatile unsigned char* p = (volatile unsigned char*)d; while (n--) *p++ = (unsigned char)v; return d; }
void* memcpy(void* d, const void* s, unsigned long long n) { volatile unsigned char* p = (volatile unsigned char*)d; const unsigned char* q = (const unsigned char*)s; while (n--) *p++ = *q++; return d; }

static char* put_text(char* at, const char* s) { while (*s) *at++ = *s++; return at; }
static char* put_hex(char* at, uint64_t v) {
    int i, started = 0;
    *at++ = '0'; *at++ = 'x';
    for (i = 60; i >= 0; i -= 4) {
        unsigned d = (unsigned)((v >> i) & 15);
        if (d || started || i == 0) { *at++ = (char)(d < 10 ? '0' + d : 'a' + d - 10); started = 1; }
    }
    return at;
}
static char* put_int(char* at, uint64_t v) {
    char t[24]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *at++ = t[--n];
    return at;
}
static int rd64(uint64_t a, uint64_t* out) {
    if (!a || !g_api->is_mapped(a, 8)) return 0;
    *out = *(const uint64_t*)a;
    return 1;
}
static uint64_t call(Ctx* c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t args[4];
    args[0] = a0; args[1] = a1; args[2] = a2; args[3] = a3;
    return g_api->call_guest(c, fn, args, 4);
}

/* One log line per kind of Information list: what the game put in it. Each item is named by its vtable (relative to main) and, when
 * its GetMid answers a readable string, by that message id. */


/* ---- the class table ----
 * The game finds a class by key (0x12e6a60 area): hash = 0x7ac700("JID_..."), then a hash map in the object at [main + 0x2209a00]:
 * buckets +0x58, bucket count +0x60, first node +0x68, size +0x70; a node is {next, hash, key, JobData*}. Every class is in the map
 * under more than one name, so the rows are collected without repeats, in address order (= the order of Job.bin: one array of
 * 0x230-byte rows). A row: +0 key hash, +8 key string, +0x10 index, +0x18 a pointer the loader writes, +0x20 name id ("MJID_..."),
 * +0x28 help id, +0x31 rank (1 to 5 for the classes units can have), the rest as in the file. */
#define JOB_MANAGER 0x2209a00ull
#define MAX_JOBS 512
#define ROW_BYTES 0x230u
static uint64_t g_jobs[MAX_JOBS];
static int g_job_count;

static int text_at(uint64_t a, char* out, int max) {      /* copies a printable string (UTF-8 bytes included); 0 when it is not one */
    int n = 0;
    if (!a || !g_api->is_mapped(a, 1)) return 0;
    while (n < max - 1) {
        unsigned char ch;
        if (!g_api->is_mapped(a + (uint64_t)n, 1)) return 0;
        ch = ((const unsigned char*)a)[n];
        if (!ch) break;
        if (ch < 0x20 || ch == 0x7f) return 0;
        out[n++] = (char)ch;
    }
    out[n] = 0;
    return n >= 3;
}

static int collect_jobs(void) {
    uint64_t mgr = 0, node = 0;
    int n = 0, i, j, guard = 0;
    if (!rd64(g_base + JOB_MANAGER, &mgr) || !mgr || !rd64(mgr + 0x68, &node)) return 0;
    while (node && guard++ < 4096) {
        uint64_t next = 0, value = 0;
        if (!rd64(node, &next) || !rd64(node + 0x18, &value)) break;
        for (i = 0; i < n && g_jobs[i] != value; ++i) {}
        if (value && i == n && n < MAX_JOBS && g_api->is_mapped(value, ROW_BYTES)) g_jobs[n++] = value;
        node = next;
    }
    for (i = 1; i < n; ++i) {          /* address order */
        uint64_t v = g_jobs[i];
        for (j = i; j > 0 && g_jobs[j - 1] > v; --j) g_jobs[j] = g_jobs[j - 1];
        g_jobs[j] = v;
    }
    g_job_count = n;
    return n;
}
static int row_key(uint64_t row, char* out, int max) { uint64_t q = 0; return rd64(row + 8, &q) && text_at(q, out, max); }
static int same_text(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static uint64_t find_job(const char* key) {
    char k[JB_KEY_MAX];
    int i;
    for (i = 0; i < g_job_count; ++i)
        if (row_key(g_jobs[i], k, sizeof k) && same_text(k, key)) return g_jobs[i];
    return 0;
}

/* ---- the creator ----
 * Create New Class opens a sub menu the way the game's own entries do (0x171fa20(root, {item, 1, 0}) makes it, AddItem fills it,
 * the entry answers 10): the placeholder classes of this mod's job.txt. A placeholder opens the five ranks, a rank opens the
 * classes of that rank, and choosing a class makes the placeholder a copy of it, in the game's memory at once and in job.txt for
 * the next start. Which item is which is kept in a table by the item's address (the game owns and frees the items). */
#define MAKE_SUB 0x171fa20ull
#define MAX_SLOTS 64
enum { TAG_SLOT = 1, TAG_RANK = 2, TAG_CLASS = 3, TAG_FIELD = 4, TAG_RANGE = 5, TAG_VALUE = 6, TAG_CAT = 7, TAG_POINT = 8, TAG_AVALUE = 9, TAG_BLOCK = 10, TAG_NUM = 11, TAG_SKILL = 12,
       TAG_CTOP = 13, TAG_CSLOT = 14, TAG_CEXACT = 15, TAG_CHUE = 16, TAG_CSHADE = 17, TAG_WTOP = 18, TAG_WSLOT = 19, TAG_WVALUE = 20, TAG_WSRC = 21, TAG_KLINE = 22, TAG_KVAL = 23, TAG_TYPE = 24, TAG_INFO = 25, TAG_ABIL = 26, TAG_GROUP = 27, TAG_BCAT = 28, TAG_PICK = 29, TAG_PPAGE = 30, TAG_NAME = 31, TAG_SPAGE = 32, TAG_REMOVE = 33 };
#define FIELD_LOWER 0xfa                /* the class line: the class before this one */
#define FIELD_UPPER 0xf9                /* the class line: the class after this one */
#define FIELD_NAME 0xfe                 /* the line with the class's own name */
#define FIELD_CAST 0xfa                 /* the line that opens the spell casting choices */
#define FIELD_ICON 0xfb                 /* the line that takes another class's icon */
#define FIELD_SKILL 0xfc                /* the mastery skill line and its list */
#define FIELD_BASE 0xff                 /* the line that chooses the base class; other values are look fields (jobbin.h) */
typedef struct Tag { uint64_t item; uint8_t kind, slot, rank, field; uint64_t row; } Tag;
static Tag g_tags[256];
static uint32_t g_tag_next;
static JbEntry g_own[MAX_SLOTS];          /* this mod's job.txt: key and base of each placeholder */
static uint32_t g_own_count;
static char g_mod_dir[1024];
static JbLook g_own_look[MAX_SLOTS];      /* this mod's asset.txt: what each placeholder takes from other classes */
static char g_field_text[MAX_SLOTS][JB_NFIELD + 1][160];
/* animations for one weapon kind only: g_wanim[slot][sex][kind] = the animation names ("AB_998MA_W#,AB_320MA_W#"), "" = as the
 * class's own list. Kinds as the game numbers them in the names: 0..7 = W1..W8, 8 = W0 (no weapon).
 * asset.txt: "anim.sword = ...", "anim.sword.m" / ".f" for one sex. */
#define N_WKIND 9
/* black magic in a created class: a model held in the right hand while casting and / or the effect every spell uses (asset.txt:
 * "cast_hand = W1_014", "cast_effect = MagicWP_Thunder"); rows for it are added to the asset table at start (castrows.h) */
static JbCast g_cast[MAX_SLOTS];
static const char* const kCastOpt[2][6] = {{"", "W1_000", "W1_001", "W1_007", "W1_014", 0}, {"", "MagicWP_Thunder", "MagicWP_PoisonSword", "MagicWP_StarLight_MartialArts", "MagicWP_Otter_MartialArts", "MagicWP_Torn_MartialArts"}};
static const char* const kCastOptLabel[2][6] = {{"(nothing)", "Bronze sword", "Iron sword", "Poison sword", "Thunder sword", 0}, {"(each spell's own)", "Thunder sword's", "Poison sword's", "Starlight (martial arts)", "Otter (martial arts)", "Torn (martial arts)"}};
static const uint8_t kCastOptN[2] = {5, 6};
static uint8_t g_cast_spells[MAX_SLOTS];          /* "cast_spells = hand": the class's tome and staff spells start at its right hand */
static char g_wanim[MAX_SLOTS][2][N_WKIND][JB_SRC_MAX];
static const char* const kWKindName[N_WKIND] = {"sword", "lance", "axe", "bow", "gauntlet", "black", "white", "special", "unarmed"};
static const char* const kWKindLabel[N_WKIND] = {"Sword", "Lance", "Axe", "Bow", "Gauntlet", "Black magic", "White magic", "Special", "No weapon"};
static void wanim_parse(const char* text, uint64_t size) {
    uint64_t i = 0;
    int cur = -1;
    uint32_t s2, k, n;
    for (n = 0; n < MAX_SLOTS; ++n) for (s2 = 0; s2 < 2; ++s2) for (k = 0; k < N_WKIND; ++k) g_wanim[n][s2][k][0] = 0;
    for (n = 0; n < MAX_SLOTS; ++n) { g_cast[n].hand[0] = g_cast[n].effect[0] = g_cast[n].cond[0] = 0; g_cast_spells[n] = 0; }
    while (i < size) {
        uint64_t e = i, a, b, v, x;
        while (e < size && text[e] != '\n') e++;
        a = i; b = e; i = e + 1;
        while (a < b && (text[a] == ' ' || text[a] == '\t')) a++;
        while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t' || text[b - 1] == '\r')) b--;
        if (b > a + 2 && text[a] == '[' && text[b - 1] == ']') {
            cur = -1;
            for (n = 0; n < g_own_count; ++n) {
                for (x = 0; x < b - a - 2 && g_own[n].key[x] == text[a + 1 + x]; ++x) {}
                if (x == b - a - 2 && !g_own[n].key[x]) { cur = (int)n; break; }
            }
            continue;
        }
        if (cur >= 0 && b > a + 6 && text[a] == 'c' && text[a + 1] == 'a' && text[a + 2] == 's' && text[a + 3] == 't' && text[a + 4] == '_') {
            const int hand = text[a + 5] == 'h';
            char* to = hand ? g_cast[cur].hand : g_cast[cur].effect;
            if (text[a + 5] == 's') {          /* cast_spells = hand */
                for (v = a; v < b && text[v] != '='; ++v) {}
                if (v < b) { v++; while (v < b && (text[v] == ' ' || text[v] == '\t')) v++; g_cast_spells[cur] = v < b && text[v] == 'h'; }
                continue;
            }
            const uint64_t max = hand ? sizeof g_cast[cur].hand : sizeof g_cast[cur].effect;
            for (v = a; v < b && text[v] != '='; ++v) {}
            if (v < b) { v++; while (v < b && (text[v] == ' ' || text[v] == '\t')) v++; for (x = 0; v + x < b && x + 1 < max; ++x) to[x] = text[v + x]; to[x] = 0; }
            continue;
        }
        if (cur < 0 || b < a + 6 || text[a] != 'a' || text[a + 1] != 'n' || text[a + 2] != 'i' || text[a + 3] != 'm' || text[a + 4] != '.') continue;
        for (k = 0; k < N_WKIND; ++k) {
            const char* name = kWKindName[k];
            int sex = 2;
            v = a + 5;
            for (x = 0; name[x] && v < b && text[v] == name[x]; ++x, ++v) {}
            if (name[x]) continue;
            if (v + 1 < b && text[v] == '.' && (text[v + 1] == 'm' || text[v + 1] == 'f')) { sex = text[v + 1] == 'f'; v += 2; }
            while (v < b && (text[v] == ' ' || text[v] == '\t')) v++;
            if (v >= b || text[v] != '=') continue;
            v++;
            while (v < b && (text[v] == ' ' || text[v] == '\t')) v++;
            if (b - v >= JB_SRC_MAX) break;
            for (s2 = 0; s2 < 2; ++s2) {
                if (sex != 2 && sex != (int)s2) continue;
                for (x = 0; v + x < b; ++x) g_wanim[cur][s2][k][x] = text[v + x];
                g_wanim[cur][s2][k][x] = 0;
            }
            break;
        }
    }
}
static const char* const kFieldLabel[JB_NFIELD] = {"Body", "Material", "Outfit Colors", "Map scale", "Animations", "Footsteps", "Accessories", "Mount model", "Magic"};
/* the internal names behind each class's look, read from AssetTable.bin when it is handed over at start (shown in brackets) */
typedef struct ClassVals { char key[JB_KEY_MAX]; char val[JB_NFIELD][32]; } ClassVals;
static ClassVals g_vals[192];
static uint32_t g_val_count;
static uint8_t g_fresh[MAX_SLOTS];        /* made a copy in this session: its models are added at the next start */
static char g_slot_text[MAX_SLOTS][160];
#define PAGE 12                           /* the rows one of the game's sub menus shows; a longer list is split into pages */
#define MAX_PAGES 8
static char g_rank_text[5][MAX_PAGES][40];
static uint8_t g_name_item[ITEM_BYTES];   /* a stand-in item, to ask the game for a class's name in the current language */
static uint64_t g_vt_dyn[2 + SLOTS];      /* placeholder and rank items: our text, our ACall */
static uint64_t g_vt_cls[2 + SLOTS];      /* class items: the game's own text from our message id (the class's name), our ACall */

static Tag* tag_of(uint64_t item) {
    uint32_t i;
    for (i = 0; i < 256; ++i)
        if (g_tags[i].item == item) return &g_tags[i];
    return 0;
}
static void tag_set(uint64_t item, uint8_t kind, uint8_t slot, uint8_t rank, uint8_t field, uint64_t row) {
    Tag* t = tag_of(item);          /* an address the game has reused for a new item */
    if (!t) { t = &g_tags[g_tag_next]; g_tag_next = (g_tag_next + 1) & 255; }
    t->item = item; t->kind = kind; t->slot = slot; t->rank = rank; t->field = field; t->row = row;
}

static uint64_t new_item(Ctx* c, uint64_t sub, const uint64_t* vt) {
    uint64_t heap_var = 0, heap = 0, item, i;
    if (!rd64(g_base + HEAP_GOT, &heap_var) || !rd64(heap_var, &heap) || !heap) return 0;
    item = call(c, g_base + ALLOCATE, heap, ITEM_BYTES, 16, 0);
    if (!item || !g_api->is_mapped(item, ITEM_BYTES)) return 0;
    for (i = 0; i < ITEM_BYTES; ++i) ((volatile uint8_t*)item)[i] = 0;
    *(uint64_t*)item = (uint64_t)&vt[2];
    *(uint16_t*)(item + 0x50) = 0x0101;
    *(uint8_t*)(item + 0x52) = 1;
    call(c, g_base + ADD_ITEM, sub, item, 0, 0);
    return item;
}

/* the sub menu an item opens: under the menu's root, as the game's entries make theirs (0x171fa20(root, {item, u32 side, float x, float y})) */
/* The right stick is the camera on the class change screen. The game's menus move their cursor up and down with it too: the
 * menu tick (0x1713860, slot 2 of a menu's table) takes up / down from three sources in the input object's words (+0x3b0 held,
 * +0x3b4 repeated, +0x3b8 pressed): bits 15 / 16, bits 19 / 20 (the d-pad and the left stick) and bits 23 / 24 (the right stick,
 * which the game itself leaves out in a few input layers). The creator's lists get a copy of their table whose slot 2 runs the
 * game's tick with bits 23 to 26 taken out of those words for that call only, so the camera still has the stick. */
#define RSTICK_BITS 0x07800000u
static uint64_t g_mvt[2 + 256], g_menu_vt_game, g_menu_tick;
static void menu_tick(Ctx* c, void* user) {
    uint64_t p = 0, in = 0, r;
    uint32_t w[3] = {0, 0, 0}, k;
    int masked = 0;
    (void)user;
    if (rd64(g_base + 0x216a4b0ull, &p) && rd64(p, &in) && in > 0x10000 && g_api->is_mapped(in + 0x3b0, 12)) {
        for (k = 0; k < 3; ++k) { w[k] = *(const uint32_t*)(in + 0x3b0 + 4 * k); *(volatile uint32_t*)(in + 0x3b0 + 4 * k) = w[k] & ~RSTICK_BITS; }
        masked = 1;
    }
    r = call(c, g_base + 0x1713860ull, c->x[0], c->x[1], c->x[2], c->x[3]);
    if (masked) for (k = 0; k < 3; ++k) *(volatile uint32_t*)(in + 0x3b0 + 4 * k) = (*(const uint32_t*)(in + 0x3b0 + 4 * k) & ~RSTICK_BITS) | (w[k] & RSTICK_BITS);
    c->x[0] = r;
}
static void menu_own(uint64_t sub) {
    uint64_t vt = 0, fn = 0;
    uint32_t n;
    if (!rd64(sub, &vt) || vt < 0x10000) return;
    if (!g_menu_vt_game) {          /* the first list made: its table is copied once */
        if (!rd64(vt + 16, &fn) || fn != g_base + 0x1713860ull || !g_api->is_mapped(vt - 16, 16)) return;
        for (n = 0; n < 256 && g_api->is_mapped(vt + 8 * n, 8) && g_api->is_function(((const uint64_t*)vt)[n]); ++n) {}
        if (n < 120) {          /* the game's sub menu table has 139 */ g_api->log(g_api, 1, "the creator's lists keep the game's own table: it is shorter than expected (the right stick still moves them)"); g_menu_vt_game = 1; return; }
        g_mvt[0] = ((const uint64_t*)vt)[-2]; g_mvt[1] = ((const uint64_t*)vt)[-1];
        for (fn = 0; fn < n; ++fn) g_mvt[2 + fn] = ((const uint64_t*)vt)[fn];
        g_menu_tick = g_api->make_function(menu_tick, 0);
        if (!g_menu_tick) { g_menu_vt_game = 1; return; }
        g_mvt[2 + 2] = g_menu_tick;
        g_menu_vt_game = vt;
    }
    if (vt == g_menu_vt_game && g_menu_tick) *(volatile uint64_t*)sub = (uint64_t)&g_mvt[2];
}
static uint64_t open_sub(Ctx* c, uint64_t item) {
    static uint8_t args[0x20];
    uint64_t menu = 0, root = 0, sub;
    int i;
    if (!rd64(item + 8, &menu) || !menu) return 0;
    if (rd64(menu + 0x98, &root) && root) menu = root;
    for (i = 0; i < 0x20; ++i) args[i] = 0;
    *(uint64_t*)args = item;
    /* where the sub menu goes (read at 0x1720028): 0 = left of the item, 1 = right of it (the game's own sub menus), anything else =
     * on the item itself; a float at +0xc and one at +0x10 move it sideways and down. On the class change screen each list is put
     * on the one it came from, so that they do not grow across the unit on show. */
    { uint64_t sc = 0; *(uint32_t*)(args + 8) = rd64(g_base + 0x2289410ull, &sc) && sc ? 2 : 1; }
    sub = call(c, g_base + MAKE_SUB, menu, (uint64_t)args, 0, 0);
    if (sub && g_api->is_mapped(sub, 0x110)) menu_own(sub);
    return sub && g_api->is_mapped(sub, 0x110) ? sub : 0;
}

/* the name id of a row without its "MJID_" ("SwordFighter"): what a placeholder's line shows as its base */
static const char* short_name(uint64_t row, char* out, int max) {
    uint64_t q = 0;
    if (!rd64(row + 0x20, &q) || !text_at(q, out, max)) return "?";
    return out[0] == 'M' && out[1] == 'J' && out[2] == 'I' && out[3] == 'D' && out[4] == '_' ? out + 5 : out;
}

/* a class the rank lists offer: of that rank, not the dismounted half of a mounted class, not one of the placeholders */
/* a class offered in a list of classes of a tier: the game's, and for a class line also the classes made here (never the class itself) */
static int offered(uint64_t row, uint8_t rank, int made_too, uint64_t self) {
    char key[JB_KEY_MAX];
    int len = 0;
    if (row == self || *(const uint8_t*)(row + 0x31) != rank || !row_key(row, key, sizeof key)) return 0;
    if (!made_too && key[0] == 'J' && key[4] == 'C' && key[5] == 'u' && key[6] == 's') return 0;
    while (key[len]) len++;
    return !(len > 6 && same_text(key + len - 6, "\xe4\xb8\x8b\xe9\xa6\xac"));
}
static int slot_empty(uint32_t i) { return !g_own[i].base[0] || same_text(g_own[i].base, "JID_\xe7\x84\xa1\xe3\x81\x97"); }
static int slot_dead(uint32_t i) { return !g_own[i].key[0]; }          /* removed in this session: gone from the files, its row stays until the game closes */

/* A mounted class's own dismounted class is not edited by itself (apart from its Class Bonus): it follows the mounted class, as
 * the game's pairs are alike in everything but the Class Bonus, the type and the exam. Done whenever the files are written. */
static uint64_t find_job(const char* key);
static int foot_of(uint32_t slot) {
    uint32_t k;
    if (slot >= g_own_count || !g_own[slot].key[0] || g_own[slot].no_mount || !g_own[slot].dismount[0]) return -1;
    for (k = 0; k < g_own_count; ++k) if (k != slot && g_own[k].no_mount && g_own[k].key[0] && same_text(g_own[k].key, g_own[slot].dismount)) return (int)k;
    return -1;
}
static void foot_follow(void) {
    static const uint8_t kSame[6] = {JB_B_STATS, JB_B_CAP, JB_B_GROW, JB_B_GBONUS, JB_B_WRANK, JB_B_RANK};
    uint32_t i, b, x;
    for (i = 0; i < g_own_count; ++i) {
        const int k = foot_of(i);
        JbEntry *m, *f;
        uint64_t mrow, frow;
        if (k < 0) continue;
        m = &g_own[i]; f = &g_own[k];
        for (b = 0; b < 6; ++b) { f->has[kSame[b]] = m->has[kSame[b]]; for (x = 0; x < 13; ++x) f->val[kSame[b]][x] = m->val[kSame[b]][x]; }
        for (x = 0; x < JB_NAME_MAX; ++x) f->name[x] = m->name[x];
        for (x = 0; x < JB_HELP_MAX; ++x) f->help[x] = m->help[x];
        for (x = 0; x < JB_KEY_MAX; ++x) { f->master[x] = m->master[x]; f->lower[x] = m->lower[x]; f->upper[x] = m->upper[x]; }
        f->sex = m->sex; f->has_sex = m->has_sex; f->cexp[0] = m->cexp[0]; f->cexp[1] = m->cexp[1]; f->has_cexp = m->has_cexp;
        if (m->has_abil) { for (b = 0; b < 3; ++b) for (x = 0; x < JB_KEY_MAX; ++x) f->abil[b][x] = m->abil[b][x]; f->has_abil = 1; f->skills[0] = 0; }
        mrow = find_job(m->key); frow = find_job(f->key);
        if (!mrow || !frow) continue;          /* in the game's table too, where both are there */
        for (b = 0; b < 6; ++b) for (x = 0; x < (uint32_t)kJbBlock[kSame[b]].count * kJbBlock[kSame[b]].size; ++x)
            ((volatile uint8_t*)frow)[kJbBlock[kSame[b]].off + x] = ((const uint8_t*)mrow)[kJbBlock[kSame[b]].off + x];
        *(volatile uint8_t*)(frow + 0x32) = *(const uint8_t*)(mrow + 0x32);
        for (x = 0x48; x < 0x58; ++x) ((volatile uint8_t*)frow)[x] = ((const uint8_t*)mrow)[x];          /* the class line */
        for (x = 0x100; x < 0x110; ++x) ((volatile uint8_t*)frow)[x] = ((const uint8_t*)mrow)[x];        /* the master ability */
        for (x = 0x19c; x < 0x1a0; ++x) ((volatile uint8_t*)frow)[x] = ((const uint8_t*)mrow)[x];        /* points to master */
    }
}
static void save_own(void) {      /* this mod's job.txt, rewritten from g_own */
    static char file[24576];
    char path[1100];
    char* at = file;
    uint32_t i;
    foot_follow();
    at = put_text(at, "# Placeholder classes for Create New Class. Each entry becomes a class at the end of the game's table.\n"
                      "# base = the class the entry is a copy of (JID_\xe7\x84\xa1\xe3\x81\x97 is the game's empty \"none\" class).\n"
                      "# Create New Class rewrites this file when you choose a base in the game.\n\n");
    at = put_text(at, "# You can give an entry a name and a help text of its own (shown after the next start):\n"
                      "#   name = Sky Lancer\n#   help = First line.\\nSecond line.\n"
                      "# A placeholder that gets a name is given a key from it at the next start (JID_Custom01 -> JID_SkyLancer).\n"
                      "#   master_skill = SID_...   the skill for mastering the class (chosen in the game), or none\n"
                      "#   icon = JID_...           the class whose icon it shows (chosen in the game)\n"
                      "# The numbers are written here when you change them in the game:\n"
                      "#   stats, modifiers, caps, growths, growth_bonus = HP,Str,Mag,Dex,Spd,Lck,Def,Res,Cha,sight,move\n"
                      "#   weapon_ranks = sword,lance,axe,bow,gauntlet,black magic,white magic,special (-1 = cannot use)\n"
                      "#   unit_types = foot,cavalry,armor,flying,command   move_type = n   rank = n\n"
                      "#   cc_ranks = the rank needed to change into the class: the 8 weapons, then the 5 unit types (0 = not asked)\n"
                      "#   cc_shared = three weapon numbers (0 sword .. 7 special, 15 = none)   cc_fail = n\n\n");
    for (i = 0; i < g_own_count && at - file < (long long)sizeof file - 1400; ++i) {
        uint32_t bl, x;
        const char* h;
        if (!g_own[i].key[0]) continue;          /* a removed class */
        *at++ = '['; at = put_text(at, g_own[i].key); at = put_text(at, "]\nbase = "); at = put_text(at, g_own[i].base); *at++ = '\n';
        if (g_own[i].name[0]) { at = put_text(at, "name = "); at = put_text(at, g_own[i].name); *at++ = '\n'; }
        if (g_own[i].has_cexp) { at = put_text(at, "class_exp = "); at = put_int(at, g_own[i].cexp[0]); at = put_text(at, ", "); at = put_int(at, g_own[i].cexp[1]); *at++ = '\n'; }
        if (g_own[i].has_sex) { at = put_text(at, "sex = "); *at++ = (char)('0' + g_own[i].sex); *at++ = '\n'; }
        if (g_own[i].has_abil) {
            uint32_t a3;
            at = put_text(at, "abilities = ");
            for (a3 = 0; a3 < 3; ++a3) { if (a3) at = put_text(at, ", "); at = put_text(at, g_own[i].abil[a3][0] ? g_own[i].abil[a3] : "none"); }
            *at++ = '\n';
        }
        if (g_own[i].skills[0]) { at = put_text(at, "skills = "); at = put_text(at, g_own[i].skills); *at++ = '\n'; }
        if (g_own[i].barred[0]) { at = put_text(at, "barred = "); at = put_text(at, g_own[i].barred); *at++ = '\n'; }
        if (g_own[i].dismount[0]) { at = put_text(at, "dismount = "); at = put_text(at, g_own[i].dismount); *at++ = '\n'; }
        if (g_own[i].lower[0]) { at = put_text(at, "lower = "); at = put_text(at, g_own[i].lower); *at++ = '\n'; }
        if (g_own[i].upper[0]) { at = put_text(at, "upper = "); at = put_text(at, g_own[i].upper); *at++ = '\n'; }
        if (g_own[i].no_mount) at = put_text(at, "mount = none\n");
        else if (g_own[i].mount_from[0]) { at = put_text(at, "mount = "); at = put_text(at, g_own[i].mount_from); *at++ = '\n'; }
        if (g_own[i].attr[0]) { at = put_text(at, "attributes = "); at = put_text(at, g_own[i].attr); *at++ = '\n'; }
        if (g_own[i].icon[0]) { at = put_text(at, "icon = "); at = put_text(at, g_own[i].icon); *at++ = '\n'; }
        if (g_own[i].master[0]) { at = put_text(at, "master_skill = "); at = put_text(at, g_own[i].master); *at++ = '\n'; }
        if (g_own[i].help[0]) { at = put_text(at, "help = "); for (h = g_own[i].help; *h; ++h) { if (*h == '\n') { *at++ = '\\'; *at++ = 'n'; } else *at++ = *h; } *at++ = '\n'; }
        for (bl = 0; bl < JB_NBLOCK; ++bl) {
            if (!g_own[i].has[bl]) continue;
            at = put_text(at, kJbBlock[bl].name); at = put_text(at, " = ");
            for (x = 0; x < kJbBlock[bl].count; ++x) {
                const int v = g_own[i].val[bl][x];
                if (x) *at++ = ',';
                if (v < 0) { *at++ = '-'; at = put_int(at, (uint64_t)(-v)); } else at = put_int(at, (uint64_t)v);
            }
            *at++ = '\n';
        }
        *at++ = '\n';
    }
    { char* p2 = put_text(path, g_mod_dir); p2 = put_text(p2, "/job.txt"); *p2 = 0; }
    if (!g_api->write_file(path, file, (uint64_t)(at - file))) g_api->log(g_api, 1, "job.txt could not be written: the choice lasts until the game closes");
}

static void save_looks(void) {      /* this mod's asset.txt, rewritten from g_own_look */
    static char file[32768];
    char path[1100];
    char* at = file;
    uint32_t i, f;
    for (i = 0; i < g_own_count; ++i) {          /* a dismounted class wears what its mounted class was given (not the mount, the animations or the casting) */
        static const uint8_t kWorn[6] = {JB_BODY, JB_MATERIAL, JB_COLORS, JB_SCALE, JB_FOOTSTEP, JB_ACCESSORY};
        const int k = foot_of(i);
        uint32_t w, sx, x;
        if (k < 0) continue;
        for (w = 0; w < 6; ++w) for (sx = 0; sx < 2; ++sx) for (x = 0; x < JB_SRC_MAX; ++x) g_own_look[k].src[sx][kWorn[w]][x] = g_own_look[i].src[sx][kWorn[w]][x];
    }
    at = put_text(at, "# The look of each created class, where it differs from its base class. Written by Create New Class.\n"
                      "# field = value sets it for male and female, field.m / field.f for one of them. A value is\n"
                      "#   [cond,cond]   the asset row with exactly these conditions (the field is copied from it)\n"
                      "#   JID_Class     that class's row matching each of this class's rows\n"
                      "#   a model name  for body, material, footstep, mount, magic\n"
                      "#   #RRGGBB,#RRGGBB,#RRGGBB   for colors: the three outfit colors written out (- = none)\n"
                      "# Fields: body material colors scale anim footstep accessories mount magic\n"
                      "# anim.sword / lance / axe / bow / gauntlet / black / white / special / unarmed = animation names, comma separated:\n"
                      "#   the animations for that weapon kind only (applied while the game runs); absent = the class's own\n"
                      "# cast_hand = W1_014            a model held in the right hand while the class casts black magic\n"
                      "# cast_effect = MagicWP_Thunder the effect every black magic spell of the class uses (both after a restart)\n"
                      "# cast_spells = hand            the class's tome and staff spells (and their glyph) start at its right hand\n\n");
    for (i = 0; i < g_own_count; ++i) {
        int any = 0;
        for (f = 0; f < JB_NFIELD; ++f) any |= g_own_look[i].src[0][f][0] | g_own_look[i].src[1][f][0];
        for (f = 0; f < N_WKIND; ++f) any |= g_wanim[i][0][f][0] | g_wanim[i][1][f][0];
        any |= g_cast[i].hand[0] | g_cast[i].effect[0] | g_cast_spells[i];
        if (!any || !g_own[i].key[0]) continue;
        *at++ = '['; at = put_text(at, g_own[i].key); at = put_text(at, "]\n");
        for (f = 0; f < JB_NFIELD; ++f) {
            const char *m = g_own_look[i].src[0][f], *w = g_own_look[i].src[1][f];
            if (m[0] && same_text(m, w)) { at = put_text(at, kJbFieldName[f]); at = put_text(at, " = "); at = put_text(at, m); *at++ = '\n'; continue; }
            if (m[0]) { at = put_text(at, kJbFieldName[f]); at = put_text(at, ".m = "); at = put_text(at, m); *at++ = '\n'; }
            if (w[0]) { at = put_text(at, kJbFieldName[f]); at = put_text(at, ".f = "); at = put_text(at, w); *at++ = '\n'; }
        }
        if (g_cast[i].hand[0]) { at = put_text(at, "cast_hand = "); at = put_text(at, g_cast[i].hand); *at++ = '\n'; }
        if (g_cast_spells[i]) at = put_text(at, "cast_spells = hand\n");
        if (g_cast[i].effect[0]) { at = put_text(at, "cast_effect = "); at = put_text(at, g_cast[i].effect); *at++ = '\n'; }
        for (f = 0; f < N_WKIND; ++f) {
            const char *m = g_wanim[i][0][f], *w = g_wanim[i][1][f];
            if (m[0] && same_text(m, w)) { at = put_text(at, "anim."); at = put_text(at, kWKindName[f]); at = put_text(at, " = "); at = put_text(at, m); *at++ = '\n'; continue; }
            if (m[0]) { at = put_text(at, "anim."); at = put_text(at, kWKindName[f]); at = put_text(at, ".m = "); at = put_text(at, m); *at++ = '\n'; }
            if (w[0]) { at = put_text(at, "anim."); at = put_text(at, kWKindName[f]); at = put_text(at, ".f = "); at = put_text(at, w); *at++ = '\n'; }
        }
        *at++ = '\n';
    }
    { char* p2 = put_text(path, g_mod_dir); p2 = put_text(p2, "/asset.txt"); *p2 = 0; }
    if (!g_api->write_file(path, file, (uint64_t)(at - file))) g_api->log(g_api, 1, "asset.txt could not be written: the choice lasts until the game closes");
}

/* ---- the loaded asset table ----
 * game::AssetTableFile is a singleton ([[main + 0x216a930]]). It keeps a list of pointers to the asset rows (first at +0x40, end
 * at +0x48), which is what the game itself walks (0x7ea8ec, 0x1842c54): a row is 0x180 bytes, with its key at +8, its index at
 * +0x10 and the number of condition groups at +0x20. A row's fields are read when a model is built, so a change made here shows
 * the next time the unit's model is loaded. The list is read again each time, as the game may load the table anew. */
#define ASSET_GOT 0x216a930ull
#define AROW 0x180u
static uint64_t g_at_list, g_at_first;
static uint32_t g_at_count, g_at_gen;
static uint16_t m16(uint64_t a) { return *(const uint16_t*)a; }
static uint64_t m64(uint64_t a) { return *(const uint64_t*)a; }
static uint64_t at_row(uint32_t i) { return m64(g_at_list + (uint64_t)i * 8); }

static int find_asset_table(void) {
    static int told, told_bad;
    uint64_t p, inst, b, e, r0;
    uint32_t n, i, ok = 0;
    char line[220];
    char* at;
    p = g_api->is_mapped(g_base + ASSET_GOT, 8) ? m64(g_base + ASSET_GOT) : 0;
    inst = p > 0x10000 && g_api->is_mapped(p, 8) ? m64(p) : 0;
    b = inst > 0x10000 && g_api->is_mapped(inst, 0x108) ? m64(inst + 0x40) : 0;
    e = b ? m64(inst + 0x48) : 0;
    n = b > 0x10000 && e > b && e - b < ((uint64_t)1 << 22) ? (uint32_t)((e - b) / 8) : 0;
    if (n && g_api->is_mapped(b, (uint64_t)n * 8)) {
        for (i = 0; i < n; ++i) { const uint64_t r = m64(b + (uint64_t)i * 8); if (r < 0x10000 || !g_api->is_mapped(r, AROW)) break; }
        ok = i == n;
    }
    if (!ok) {
        g_at_list = 0; g_at_count = 0;
        if (!told_bad) {
            told_bad = 1;
            at = put_text(line, "the loaded asset table was not usable: holder "); at = put_hex(at, p); at = put_text(at, ", object "); at = put_hex(at, inst);
            at = put_text(at, ", list "); at = put_hex(at, b); at = put_text(at, " to "); at = put_hex(at, e); *at = 0;
            g_api->log(g_api, 1, line);
        }
        return 0;
    }
    r0 = m64(b);
    if (b != g_at_list || r0 != g_at_first || n != g_at_count) { g_at_gen++; told = 0; }
    g_at_list = b; g_at_first = r0; g_at_count = n;
    if (told) return 1;
    told = 1;
    at = put_text(line, "asset table in memory: "); at = put_int(at, n); at = put_text(at, " rows, list at "); at = put_hex(at, b);
    at = put_text(at, ", row 0 at "); at = put_hex(at, r0); at = put_text(at, ", row 1 at "); at = put_hex(at, n > 1 ? at_row(1) : 0);
    at = put_text(at, ", index words "); at = put_int(at, m16(r0 + 0x10)); *at++ = '/'; at = put_int(at, m16(r0 + 0x12));
    at = put_text(at, " (the class table is at "); at = put_hex(at, g_job_count ? g_jobs[0] : 0); *at++ = ')'; *at = 0;
    TRACE(line);
    for (i = 0; i < n; ++i) {          /* what a row's conditions look like in memory, for the log: the first row that has any */
        const uint64_t r = at_row(i), arr = m64(r + 0x28);
        if (!m16(r + 0x20)) continue;
        at = put_text(line, "first row with conditions: row "); at = put_int(at, i); at = put_text(at, ", "); at = put_int(at, m16(r + 0x20));
        at = put_text(at, " group(s) at "); at = put_hex(at, arr);
        if (arr > 0x10000 && g_api->is_mapped(arr, 16)) {
            const uint64_t ca = m64(arr + 8);
            at = put_text(at, ": "); at = put_int(at, m16(arr)); at = put_text(at, " condition(s) at "); at = put_hex(at, ca);
            if (ca > 0x10000 && g_api->is_mapped(ca, 8)) {
                char t[40];
                at = put_text(at, ", first "); at = put_hex(at, m64(ca));
                if (text_at(m64(ca), t, sizeof t)) { at = put_text(at, " = "); at = put_text(at, t); }
            }
        }
        *at = 0;
        TRACE(line);
        break;
    }
    return 1;
}

static const char* m_text(uint64_t p) { return p > 0x10000 && g_api->is_mapped(p, 1) ? (const char*)p : ""; }
static int m_has(uint64_t ca, uint32_t cn, const char* want) {
    uint32_t j;
    for (j = 0; j < cn; ++j)
        if (same_text(m_text(m64(ca + (uint64_t)j * 8)), want)) return 1;
    return 0;
}
/* the row's first condition group that names `key`; 0 when there is none */
static int m_group(uint64_t r, const char* key, uint64_t* ca, uint32_t* cn) {
    const uint32_t n = m16(r + 0x20);
    const uint64_t arr = m64(r + 0x28);
    uint32_t g;
    if (!n || n > 64 || !g_api->is_mapped(arr, (uint64_t)n * 16)) return 0;
    for (g = 0; g < n; ++g) {
        *cn = m16(arr + (uint64_t)g * 16); *ca = m64(arr + (uint64_t)g * 16 + 8);
        if (*cn && *cn <= 16 && g_api->is_mapped(*ca, (uint64_t)*cn * 8) && (!key || m_has(*ca, *cn, key))) return 1;
    }
    return 0;
}
static int m_own(uint64_t r, const char* key) {          /* every group of the row names the class */
    const uint32_t n = m16(r + 0x20);
    const uint64_t arr = m64(r + 0x28);
    uint32_t g;
    if (!n || n > 64 || !g_api->is_mapped(arr, (uint64_t)n * 16)) return 0;
    for (g = 0; g < n; ++g) {
        const uint32_t cn = m16(arr + (uint64_t)g * 16);
        const uint64_t ca = m64(arr + (uint64_t)g * 16 + 8);
        if (!cn || cn > 16 || !g_api->is_mapped(ca, (uint64_t)cn * 8) || !m_has(ca, cn, key)) return 0;
    }
    return 1;
}
static int m_sex(uint64_t ca, uint32_t cn) { return m_has(ca, cn, "\xe5\xa5\xb3") ? 1 : m_has(ca, cn, "\xe7\x94\xb7") ? 0 : 2; }

/* the class's main row for a sex (the class and that sex and nothing else; or the class alone), 0 when it has none */
static uint64_t main_row(const char* key, int sex) {
    static char seen_key[64][JB_KEY_MAX];          /* each answer is found once for a loaded table */
    static uint64_t seen_row[64][2];
    static uint8_t seen_done[64][2];
    static uint32_t seen_count, seen_gen;
    uint32_t i, cn = 0, e;
    uint64_t ca = 0;
    if (!find_asset_table()) return 0;
    if (seen_gen != g_at_gen) { seen_gen = g_at_gen; seen_count = 0; }          /* the table was loaded anew */
    for (e = 0; e < seen_count && !same_text(seen_key[e], key); ++e) {}
    if (e < seen_count && seen_done[e][sex]) return seen_row[e][sex];
    if (e == seen_count && seen_count < 64) { char* k = put_text(seen_key[e], key); *k = 0; seen_done[e][0] = seen_done[e][1] = 0; seen_count++; }
    if (e < seen_count) { seen_done[e][sex] = 1; seen_row[e][sex] = 0; }
    for (i = 0; i < g_at_count; ++i) {
        const uint64_t r = at_row(i);
        if (!m16(r + 0x20) || !m_group(r, key, &ca, &cn) || !m_own(r, key)) continue;
        if ((cn == 2 && m_sex(ca, cn) == sex) || cn == 1) { if (e < seen_count) seen_row[e][sex] = r; return r; }
    }
    return 0;
}

/* A row's accessories are not read from the row when a look is built. When the table is loaded (0x7f3840) each row gets an
 * object of its own (row +0x18) in which the accessory strings "model(attach point)" are kept taken apart: a vector of 0x48-byte
 * entries at +0x118 (begin) / +0x120 (end), made with 0x7f32a0(entry, string) and 0x7f6910(vector, entry) -> new end, and a byte
 * at +0x14c saying that the row has any (accessory list +0x98, or the lists at +0xa8 and +0x170). That is why a changed list
 * showed only after a restart. Here the vector is made again from the row's list with the game's two functions (the old entries
 * are left behind: a few bytes each). */
#define ACC_PARSE 0x7f32a0ull
#define ACC_PUSH  0x7f6910ull
static const char* m_text0(uint64_t p) { return p > 0x10000 && g_api->is_mapped(p, 1) ? (const char*)p : ""; }
static Ctx* g_ctx;          /* the game thread's context, set by whichever of our functions the game called */
static void acc_sync(uint64_t row) {
    static uint64_t entry[9], vec[3];
    static int told;
    uint64_t d = 0, list;
    uint32_t n, j, k;
    if (!g_ctx || !rd64(row + 0x18, &d) || d < 0x10000 || !g_api->is_mapped(d, 0x150)) return;
    if (!g_api->is_function(g_base + ACC_PARSE) || !g_api->is_function(g_base + ACC_PUSH)) return;
    n = *(const uint16_t*)(row + 0x98); list = *(const uint64_t*)(row + 0xa0);
    if (n > 32 || (n && !g_api->is_mapped(list, (uint64_t)n * 8))) return;
    /* The new entries are made in a vector of our own and handed over when they are complete. The row object's old storage is
     * left alone, entries and all, and never freed: in the user's game the first version, which reused that storage (the game's
     * append frees the old block), crashed after a while of hovering, most likely because a look still being built was reading it. */
    vec[0] = vec[1] = vec[2] = 0;
    for (j = 0; j < n; ++j) {
        const uint64_t text = *(const uint64_t*)(list + (uint64_t)j * 8);
        uint64_t end;
        if (text < 0x10000 || !g_api->is_mapped(text, 1)) continue;
        for (k = 0; k < 9; ++k) entry[k] = 0;
        call(g_ctx, g_base + ACC_PARSE, (uint64_t)entry, text, 0, 0);
        end = call(g_ctx, g_base + ACC_PUSH, (uint64_t)vec, (uint64_t)entry, 0, 0);
        if (end > 0x10000) vec[1] = end;
    }
    if (!vec[0]) { *(volatile uint64_t*)(d + 0x120) = *(const uint64_t*)(d + 0x118); }          /* none: an empty range of the old storage */
    else {
        *(volatile uint64_t*)(d + 0x128) = vec[2];
        *(volatile uint64_t*)(d + 0x118) = vec[0];
        *(volatile uint64_t*)(d + 0x120) = vec[1];
    }
    *(volatile uint8_t*)(d + 0x14c) = (uint8_t)((n || *(const uint16_t*)(row + 0xa8) || *(const uint16_t*)(row + 0x170)) ? 1 : 0);
    if (told < 300) {          /* each change, so that the last one before a fault is known */
        char line[200];
        char* at = put_text(line, "accessories of row "); at = put_int(at, *(const uint16_t*)(row + 0x10)); at = put_text(at, " made again for the game: ");
        at = put_int(at, n); at = put_text(at, " in the list, "); at = put_int(at, (*(const uint64_t*)(d + 0x120) - *(const uint64_t*)(d + 0x118)) / 0x48); at = put_text(at, " entries");
        if (n) { const char* v = m_text0(*(const uint64_t*)list); int len = 0; at = put_text(at, ", first "); while (v[len] && len < 48) *at++ = v[len++]; }
        *at = 0;
        told++;
        TRACE(line);
    }
}
/* The same object keeps the row's animation names ready-made (0x7f4b54): nine vectors of std::string at +0x28 + k * 0x18 (begin,
 * end, capacity), one per weapon kind. A name with '#' goes into vector k with the '#' replaced by '1'..'8' (k = 0..7) or '0'
 * (k = 8); a name without one goes into all nine. They are made again here from the row's list (count at +0x80, names at +0x88),
 * in memory from the game's own heap (so the game may free it), with libc++'s string layout: up to 22 characters in the 24 bytes
 * (first byte = length * 2), longer ones as {capacity | 1, length, pointer}. The old vectors are left behind, as for accessories. */
static uint64_t game_alloc(uint64_t size) {
    uint64_t heap_var = 0, heap = 0, p;
    if (!g_ctx || !rd64(g_base + HEAP_GOT, &heap_var) || !rd64(heap_var, &heap) || !heap) return 0;
    p = call(g_ctx, g_base + ALLOCATE, heap, size, 16, 0);
    return p > 0x10000 && g_api->is_mapped(p, size) ? p : 0;
}
static const char* row_class(uint64_t r);
static void anim_sync(uint64_t row) {
    static int told;
    uint64_t d = 0, list, vec[9];
    uint32_t n, j, k, i, cnt[9], any_over = 0;
    const char* over[9];
    if (!g_ctx || !rd64(row + 0x18, &d) || d < 0x10000 || !g_api->is_mapped(d, 0x150)) return;
    n = *(const uint16_t*)(row + 0x80); list = *(const uint64_t*)(row + 0x88);
    if (n > 24 || (n && !g_api->is_mapped(list, (uint64_t)n * 8))) return;
    {   /* a created class's row: its animations for single weapon kinds */
        const char* cls = row_class(row);
        uint64_t ca = 0;
        uint32_t cn = 0, s2;
        for (k = 0; k < 9; ++k) over[k] = 0;
        for (s2 = 0; cls[0] && s2 < g_own_count; ++s2)
            if (same_text(g_own[s2].key, cls)) {
                const int sex = m_group(row, 0, &ca, &cn) && m_sex(ca, cn) == 1;
                for (k = 0; k < 9; ++k) if (g_wanim[s2][sex][k][0]) { over[k] = g_wanim[s2][sex][k]; any_over = 1; }
                break;
            }
    }
    for (k = 0; k < 9; ++k) {
        cnt[k] = n;
        if (over[k]) { const char* q; cnt[k] = 1; for (q = over[k]; *q; ++q) cnt[k] += *q == ','; }
        vec[k] = cnt[k] ? game_alloc((uint64_t)cnt[k] * 24) : 0;
        if (cnt[k] && !vec[k]) return;
    }
    for (k = 0; k < 9; ++k) {
        uint32_t made = 0;
        const char* next = over[k];
        const uint32_t n = cnt[k];
        for (j = 0; j < n; ++j) {
            const uint64_t text = over[k] ? 0 : *(const uint64_t*)(list + (uint64_t)j * 8);
            const char* t = over[k] ? next : text > 0x10000 && g_api->is_mapped(text, 1) ? (const char*)text : 0;
            volatile uint8_t* e = (volatile uint8_t*)(vec[k] + (uint64_t)made * 24);
            volatile uint8_t* chars;
            uint32_t len = 0, mark = 0xffffffffu;
            if (!t) continue;
            while (t[len] && t[len] != (over[k] ? ',' : 0) && len < 200) { if (t[len] == '#') mark = len; len++; }
            if (over[k]) { next = t + len + (t[len] == ','); if (!len) continue; }
            for (i = 0; i < 24; ++i) e[i] = 0;
            if (len <= 22) { e[0] = (uint8_t)(len * 2); chars = e + 1; }
            else {
                const uint64_t cap = (len | 7) == 0x17 ? 0x1a : (uint64_t)(len | 7) + 1, mem = game_alloc(cap);
                if (!mem) continue;
                *(volatile uint64_t*)e = cap | 1; *(volatile uint64_t*)(e + 8) = len; *(volatile uint64_t*)(e + 16) = mem;
                chars = (volatile uint8_t*)mem;
            }
            for (i = 0; i < len; ++i) chars[i] = (uint8_t)t[i];
            chars[len] = 0;
            if (mark != 0xffffffffu) chars[mark] = (uint8_t)(k < 8 ? '1' + k : '0');
            made++;
        }
        if (!n) { *(volatile uint64_t*)(d + 0x30 + (uint64_t)k * 0x18) = *(const uint64_t*)(d + 0x28 + (uint64_t)k * 0x18); continue; }
        *(volatile uint64_t*)(d + 0x38 + (uint64_t)k * 0x18) = vec[k] + (uint64_t)n * 24;
        *(volatile uint64_t*)(d + 0x28 + (uint64_t)k * 0x18) = vec[k];
        *(volatile uint64_t*)(d + 0x30 + (uint64_t)k * 0x18) = vec[k] + (uint64_t)made * 24;
    }
    *(volatile uint8_t*)(d + 0x14b) = n || any_over ? 1 : 0;
    if (told < 40) {
        char line[200];
        char* at = put_text(line, "animations of row "); at = put_int(at, *(const uint16_t*)(row + 0x10)); at = put_text(at, " made again for the game: ");
        at = put_int(at, n); at = put_text(at, " in the list");
        if (n) { const char* v = m_text0(*(const uint64_t*)list); int len = 0; at = put_text(at, ", first "); while (v[len] && len < 48) *at++ = v[len++]; }
        *at = 0; told++;
        TRACE(line);
    }
}
/* and its bytes that say which kinds of field the row has at all (0x7f5184 on): +0x148 a model name (the strings at +0x30 and
 * +0x40..+0x78), +0x14a the strings at +0xd8, +0xe0, +0xe8. A row that had none of a kind is not asked for it. The bytes for
 * colors (+0x149) and for scale and the rest (+0x14d) are only ever switched on here. */
static int str_set(uint64_t row, uint32_t off) { const uint64_t p = *(const uint64_t*)(row + off); return p > 0x10000 && g_api->is_mapped(p, 1) && *(const char*)p; }
static void flags_sync(uint64_t row, uint32_t f) {
    uint64_t d = 0;
    uint32_t o, any = 0;
    if (!rd64(row + 0x18, &d) || d < 0x10000 || !g_api->is_mapped(d, 0x150)) return;
    any = (uint32_t)str_set(row, 0x30);
    for (o = 0x40; o <= 0x78; o += 8) any |= (uint32_t)str_set(row, o);
    *(volatile uint8_t*)(d + 0x148) = (uint8_t)any;
    *(volatile uint8_t*)(d + 0x14a) = (uint8_t)(str_set(row, 0xd8) | str_set(row, 0xe0) | str_set(row, 0xe8));
    if (f == JB_COLORS) { for (any = 0, o = 0; o < 12; ++o) any |= *(const uint8_t*)(row + 0x108 + o); if (any) *(volatile uint8_t*)(d + 0x149) = 1; }
    if (f == JB_SCALE && *(const uint32_t*)(row + 0xf8)) *(volatile uint8_t*)(d + 0x14d) = 1;
}
static void copy_field(uint64_t to, uint64_t from, uint32_t f) {
    uint32_t q, k;
    for (q = 0; q < 2; ++q)
        for (k = 0; k < kJbField[f][q].len; ++k) ((volatile uint8_t*)to)[kJbField[f][q].off + k] = ((const uint8_t*)from)[kJbField[f][q].off + k];
    if (f == JB_ACCESSORY) acc_sync(to);
    if (f == JB_ANIM) anim_sync(to);
    flags_sync(to, f);
}

/* the value of a field in a row, as its internal name(s) */
static uint8_t g_zero_row[AROW];          /* stands for "(none)" in the value lists: a row with every field empty */
static char* put_value(char* at, uint64_t r, uint32_t f) {
    const uint64_t off = r + kJbField[f][0].off;
    static const char hex[] = "0123456789abcdef";
    uint32_t k;
    if (r == (uint64_t)g_zero_row) return put_text(at, "(none)");
    if (kJbFieldIsString[f]) {
        const char* v = m_text(m64(off));
        return put_text(at, v[0] ? v : "-");
    }
    if (f == JB_ANIM || f == JB_ACCESSORY) {
        const uint32_t n = m16(off);
        const uint64_t p = m64(off + 8);
        if (!n || !g_api->is_mapped(p, 8)) return put_text(at, "-");
        { const char* v = m_text(m64(p)); int len = 0; while (v[len] && len < 40) *at++ = v[len++]; }
        if (n > 1) { at = put_text(at, " +"); at = put_int(at, n - 1); }
        return at;
    }
    if (f == JB_SCALE) {
        const float v = *(const float*)off;
        const uint32_t x = v > 0 && v < 1000 ? (uint32_t)(v * 100.0f + 0.5f) : 0;
        if (!x) return put_text(at, "-");
        at = put_int(at, x / 100); *at++ = '.'; *at++ = (char)('0' + x / 10 % 10); *at++ = (char)('0' + x % 10);
        return at;
    }
    *at++ = '#';
    for (k = 0; k < kJbField[f][0].len; ++k) { const uint8_t b = *(const uint8_t*)(off + k); *at++ = hex[b >> 4]; *at++ = hex[b & 15]; if (k % 4 == 3 && k + 1 < kJbField[f][0].len) *at++ = ' '; }
    return at;
}

/* every different value a field has in the table, each with a row that has it (a class's main row when there is one) */
#define MAX_VALUES 1536
static uint64_t g_val_hash[MAX_VALUES], g_val_row[MAX_VALUES];
static uint8_t g_val_main[MAX_VALUES];
static uint32_t g_value_count;
static uint64_t hash_text(uint64_t h, const char* t) { while (*t) { h = (h ^ (uint8_t)*t++) * 0x100000001b3ull; } return (h ^ 0xff) * 0x100000001b3ull; }
static uint64_t value_hash(uint64_t r, uint32_t f) {
    uint64_t h = 0xcbf29ce484222325ull;
    uint32_t q, k, any = 0;
    if (kJbFieldIsString[f] && f != JB_MOUNT) { const char* v = m_text(m64(r + kJbField[f][0].off)); return v[0] && !same_text(v, "null") ? hash_text(h, v) : 0; }
    if (f == JB_ANIM || f == JB_ACCESSORY) {
        const uint32_t n = m16(r + kJbField[f][0].off);
        const uint64_t p = m64(r + kJbField[f][0].off + 8);
        if (!n || n > 64 || !g_api->is_mapped(p, (uint64_t)n * 8)) return 0;
        for (k = 0; k < n; ++k) h = hash_text(h, m_text(m64(p + (uint64_t)k * 8)));
        return h | 1;
    }
    if (f == JB_MOUNT) {
        const char* v = m_text(m64(r + 0x50));
        if (!v[0] || same_text(v, "null")) return 0;
        h = hash_text(h, v); h = hash_text(h, m_text(m64(r + 0x58))); h = hash_text(h, m_text(m64(r + 0x60)));
        for (k = 0; k < 4; ++k) h = (h ^ *(const uint8_t*)(r + 0x140 + k)) * 0x100000001b3ull;
        return h | 1;
    }
    for (q = 0; q < 2; ++q)
        for (k = 0; k < kJbField[f][q].len; ++k) { const uint8_t b = *(const uint8_t*)(r + kJbField[f][q].off + k); any |= b; h = (h ^ b) * 0x100000001b3ull; }
    return any ? h | 1 : 0;
}
/* The values are offered in groups, since a list of a thousand names tells nothing:
 *   from a class     one line per class (its main rows); taking it takes the class's look for that field as a whole
 *   from a character values of rows that name a character (shown with the character's name)
 *   other            the rest (cutscene and shop presets, situations)
 * g_val_kind / g_val_who say which group a value is in and whose it is (a class row of the game's table, or the condition text). */
enum { K_NONE, K_CLASS, K_CHAR, K_OTHER, K_SKIP };
static uint8_t g_val_kind[MAX_VALUES];
static uint64_t g_val_who[MAX_VALUES];
static uint32_t g_seg_first[4], g_seg_count[4];
static uint8_t g_leaf = TAG_VALUE;          /* what the open list's lines are: rows (TAG_VALUE) or accessory entries (TAG_AVALUE) */
static char g_point[40];                    /* the attach point of the accessory list that is open */
static const char* row_class(uint64_t r);
static uint8_t classify(uint64_t r, uint64_t* who, int any_own) {
    const char* cls = row_class(r);
    const uint32_t n = m16(r + 0x20);
    const uint64_t arr = m64(r + 0x28);
    uint64_t ca = 0;
    uint32_t cn = 0, g, j;
    *who = 0;
    if (cls[0]) {
        const uint64_t job = find_job(cls);
        if (!job || !m_group(r, cls, &ca, &cn)) return K_SKIP;
        if (!any_own && !(cn == 1 || (cn == 2 && m_sex(ca, cn) != 2))) return K_SKIP;      /* generic-soldier and character rows come with the class */
        *who = job;
        return K_CLASS;
    }
    if (n && n <= 64 && g_api->is_mapped(arr, (uint64_t)n * 16))
        for (g = 0; g < n; ++g) {
            cn = m16(arr + (uint64_t)g * 16); ca = m64(arr + (uint64_t)g * 16 + 8);
            if (!cn || cn > 16 || !g_api->is_mapped(ca, (uint64_t)cn * 8)) continue;
            for (j = 0; j < cn; ++j) {
                const char* t = m_text(m64(ca + (uint64_t)j * 8));
                if ((t[0] == 'A' || t[0] == 'P') && t[1] == 'I' && t[2] == 'D' && t[3] == '_' && !same_text(t, "AID_\xe3\x83\xa2\xe3\x83\x96")) { *who = (uint64_t)t; return K_CHAR; }
            }
        }
    return K_OTHER;
}
static void group_values(void) {          /* (none), then the classes, the characters, the rest: each a stretch of the list */
    static uint64_t th[MAX_VALUES], tr[MAX_VALUES], tw[MAX_VALUES];
    static uint8_t tk[MAX_VALUES], tm[MAX_VALUES];
    uint32_t i, kind, n = 0;
    for (i = 0; i < g_value_count; ++i) { th[i] = g_val_hash[i]; tr[i] = g_val_row[i]; tw[i] = g_val_who[i]; tk[i] = g_val_kind[i]; tm[i] = g_val_main[i]; }
    for (kind = K_NONE; kind <= K_OTHER; ++kind) {
        g_seg_first[kind] = n;
        for (i = 0; i < g_value_count; ++i)
            if (tk[i] == kind) { g_val_hash[n] = th[i]; g_val_row[n] = tr[i]; g_val_who[n] = tw[i]; g_val_kind[n] = tk[i]; g_val_main[n] = tm[i]; n++; }
        g_seg_count[kind] = n - g_seg_first[kind];
    }
    g_value_count = n;
}
/* ---- animation sets, in words ----
 * A set's name says what it is: AB_<number><letters>_W<weapon kind>.
 *   number   999 = the generic set, 998 = the generic set of the advanced classes, 300 to 499 = a class (the number of its body
 *            model, CD_<number>), under 200 = a character, 200s = mounts and animals, 500s = monsters
 *   letters  M / F / T = male, female, creature; then A on foot, B armored on foot, C on a horse, D on an armored horse,
 *            E pegasus, F dragon, G the flying camel riders' mount, H chariot, I elephant; a small letter in front marks a
 *            variant (b underworld general, y five years later, w cutscene, x variant, z on the ground)
 * AnimSet.bin has one row per set and weapon kind (its labels): which kinds a set exists for is read from it at start. */
static char g_aset[1400][20];
static uint32_t g_aset_count;
static char g_wfilter;          /* '1'..'8' / '0' while a weapon kind's list is collected: only rows with a set of their own for it */
static void animset_read(const NxModFile* file, void* user) {
    const uint8_t* d = file->data;
    const uint64_t size = file->size;
    uint64_t dsz, np, nl, lt, st, i, n;
    char line[100];
    char* at;
    (void)user;
    if (size < 0x20 || jb_u32(d) != size) return;
    dsz = jb_u32(d + 4); np = jb_u32(d + 8); nl = jb_u32(d + 12);
    lt = 0x20 + dsz + np * 8; st = lt + nl * 8;
    if (st >= size) return;
    for (i = 0; i < nl && g_aset_count < 1400; ++i) {
        const uint64_t name = jb_u32(d + lt + i * 8 + 4);
        const char* key = (const char*)(d + st + name);
        if (st + name + 4 > size || key[0] != 'A' || key[1] != 'B' || key[2] != '_') continue;
        for (n = 0; n < 19 && st + name + n < size && key[n]; ++n) {}
        if (n == 19 || n < 6 || key[n - 3] != '_' || key[n - 2] != 'W') continue;          /* "..._W<digit>" only */
        for (n = 0; key[n]; ++n) g_aset[g_aset_count][n] = key[n];
        g_aset[g_aset_count][n] = 0;
        g_aset_count++;
    }
    at = put_text(line, "AnimSet.bin read: "); at = put_int(at, g_aset_count); at = put_text(at, " animation sets"); *at = 0;
    TRACE(line);
}
static int aset_generic(const char* name) { return name[3] == '9' && name[4] == '9' && (name[5] == '9' || name[5] == '8' || name[5] == '6'); }
/* does the row have a set of its own (not the generic one) that exists for the weapon kind? */
static int anim_has_weapon(uint64_t r, char digit) {
    const uint32_t n = m16(r + 0x80);
    const uint64_t list = m64(r + 0x88);
    uint32_t j, k, x, generic = 0, own_any = 0;
    if (!g_aset_count) return 1;
    if (!n || n > 24 || !g_api->is_mapped(list, (uint64_t)n * 8)) return 0;
    for (j = 0; j < n; ++j) {
        const char* t = m_text(m64(list + (uint64_t)j * 8));
        char name[24];
        for (x = 0; t[x] && x < 23; ++x) name[x] = t[x] == '#' ? digit : t[x];
        name[x] = 0;
        if (x < 6 || x >= 20) continue;
        if (!aset_generic(name)) own_any = 1;
        if (name[x - 1] != digit) continue;          /* a name written out for another kind */
        for (k = 0; k < g_aset_count; ++k) if (same_text(g_aset[k], name)) { if (aset_generic(name)) generic = 1; else return 1; break; }
    }
    return generic && !own_any;          /* a row with nothing but the generic set counts when that exists for the kind */
}
static void collect_values(uint32_t f) {
    /* a look that is a class's own (body, animations, mount) is listed class by class; a value many classes share (material,
     * footsteps, magic, colors, scale) once, under the first class that has it */
    const int per_class = f == JB_BODY || f == JB_ANIM || f == JB_MOUNT;
    uint32_t i, k, c2;
    g_value_count = 0; g_leaf = TAG_VALUE;
    if (!find_asset_table()) return;
    g_val_hash[0] = 1; g_val_row[0] = (uint64_t)g_zero_row; g_val_main[0] = 0; g_val_kind[0] = K_NONE; g_val_who[0] = 0; g_value_count = 1;
    for (i = 0; i < g_at_count; ++i) {
        const uint64_t r = at_row(i), h = value_hash(r, f);
        uint64_t who = 0;
        uint8_t kind;
        if (!h) continue;
        if (f == JB_ANIM && g_wfilter && !anim_has_weapon(r, g_wfilter)) continue;
        kind = classify(r, &who, 0);
        if (kind == K_SKIP) continue;
        if (kind == K_CLASS && per_class) {          /* one line per class, whatever other classes have */
            for (c2 = 1; c2 < g_value_count && !(g_val_kind[c2] == K_CLASS && g_val_who[c2] == who); ++c2) {}
            if (c2 < g_value_count) continue;
            for (k = 1; k < g_value_count && !(g_val_hash[k] == h && g_val_kind[k] != K_CLASS); ++k) {}
        } else
            for (k = 1; k < g_value_count && g_val_hash[k] != h; ++k) {}
        if (k < g_value_count) { if (kind < g_val_kind[k]) { g_val_row[k] = r; g_val_kind[k] = kind; g_val_who[k] = who; } continue; }
        if (g_value_count < MAX_VALUES) { g_val_hash[k] = h; g_val_row[k] = r; g_val_main[k] = 0; g_val_kind[k] = kind; g_val_who[k] = who; g_value_count++; }
    }
    group_values();
}

/* ---- accessories, by where they are attached ----
 * An accessory is "model(attach point)". The sheet's Accessories line opens the attach points the table uses; a point opens the
 * accessories made for it; taking one puts it in the class's list in place of whatever the list had for that point. */
static char g_points[8][40];
static uint32_t g_point_count;
static int point_of(const char* entry, char* out, int max) {
    int n = 0, open = -1, k = 0;
    while (entry[n] && n < 120) { if (entry[n] == '(') open = n; n++; }
    out[0] = 0;
    if (open < 0 || n < 2 || entry[n - 1] != ')') return 0;
    for (n = open + 1; entry[n] && entry[n] != ')' && k < max - 1; ++n) out[k++] = entry[n];
    out[k] = 0;
    return k > 0;
}
static const char* point_label(const char* p) {
    if (same_text(p, "Jnt_C_Head")) return "Head";
    if (same_text(p, "Loc_C_Quiver")) return "Quiver";
    if (same_text(p, "Loc_L_Shield")) return "Shield (left arm)";
    if (same_text(p, "Loc_L_Shield2")) return "Shield (left arm, second)";
    if (same_text(p, "Loc_R_Shield")) return "Shield (right arm)";
    return p;
}
static void collect_points(void) {
    uint32_t i, j, k;
    char pt[40];
    g_point_count = 0;
    if (!find_asset_table()) return;
    for (i = 0; i < g_at_count; ++i) {
        const uint64_t r = at_row(i), list = m64(r + 0xa0);
        const uint32_t n = m16(r + 0x98);
        if (!n || n > 32 || !g_api->is_mapped(list, (uint64_t)n * 8)) continue;
        for (j = 0; j < n; ++j) {
            if (!point_of(m_text(m64(list + (uint64_t)j * 8)), pt, sizeof pt)) continue;
            for (k = 0; k < g_point_count && !same_text(g_points[k], pt); ++k) {}
            if (k == g_point_count && g_point_count < 8) { char* e = put_text(g_points[g_point_count++], pt); *e = 0; }
        }
    }
}
static void collect_point_values(const char* point) {
    uint32_t i, j, k;
    char pt[40];
    { char* e = put_text(g_point, point); *e = 0; }
    g_value_count = 0; g_leaf = TAG_AVALUE;
    if (!find_asset_table()) return;
    g_val_hash[0] = 1; g_val_row[0] = (uint64_t)g_zero_row; g_val_main[0] = 0; g_val_kind[0] = K_NONE; g_val_who[0] = 0; g_value_count = 1;
    for (i = 0; i < g_at_count; ++i) {
        const uint64_t r = at_row(i), list = m64(r + 0xa0);
        const uint32_t n = m16(r + 0x98);
        uint64_t who = 0, ca = 0;
        uint32_t cn = 0;
        uint8_t kind = K_SKIP, mark = 2;
        if (!n || n > 32 || !g_api->is_mapped(list, (uint64_t)n * 8)) continue;
        for (j = 0; j < n; ++j) {
            const char* e = m_text(m64(list + (uint64_t)j * 8));
            uint64_t h;
            if (!point_of(e, pt, sizeof pt) || !same_text(pt, point)) continue;
            if (kind == K_SKIP) {          /* whose row this is: once per row */
                kind = classify(r, &who, 1);
                if (kind == K_SKIP) kind = K_OTHER;
                if (kind == K_CLASS && m_group(r, 0, &ca, &cn)) mark = (uint8_t)(m_sex(ca, cn) | (m_has(ca, cn, "AID_\xe3\x83\xa2\xe3\x83\x96") ? 4 : 0));
            }
            h = hash_text(0xcbf29ce484222325ull, e) | 2;
            for (k = 1; k < g_value_count && g_val_hash[k] != h; ++k) {}
            if (k < g_value_count) { if (kind < g_val_kind[k]) { g_val_kind[k] = kind; g_val_who[k] = who; g_val_main[k] = mark; } continue; }
            if (g_value_count < MAX_VALUES) { g_val_hash[k] = h; g_val_row[k] = (uint64_t)e; g_val_main[k] = mark; g_val_kind[k] = kind; g_val_who[k] = who; g_value_count++; }
        }
    }
    group_values();
}

/* The game sizes a sub menu from its lines when it opens, so a line that grows later ("empty" -> "copy of ...") would not fit until
 * the menu is opened again. Every placeholder line is therefore padded to one width with ideographic spaces (U+3000, about two
 * Latin letters wide), which keeps the menu at the width of its longest possible line from the start. */
#define LINE_UNITS 46
/* Names for the characters the asset table's conditions mention ("AID_..."): Person.bin, read as the game's files are set up,
 * gives each character's asset id (row +0x278) and name id (row +0x20, "MPID_..."); the name itself is asked of the game, in the
 * language it runs in, the way a class's name is (a stand-in item whose GetMid answers the id). */
typedef struct Person { char aid[56]; char mid[40]; } Person;
static Person g_people[1600];
static uint32_t g_people_count;
static const char* g_mid_text;
/* skills likewise: Skill.bin's key ("SID_...") and name id (row +0x20) */
typedef struct SkillName { char sid[52]; char mid[36]; } SkillName;
static SkillName g_skills[2400];
static uint32_t g_skill_count;
static void skill_read(const NxModFile* file, void* user) {
    const uint8_t* d = file->data;
    const uint64_t size = file->size;
    uint64_t dsz, np, nl, lt, st, i;
    char line[120];
    char* at;
    (void)user;
    if (size < 0x20 || jb_u32(d) != size) return;
    dsz = jb_u32(d + 4); np = jb_u32(d + 8); nl = jb_u32(d + 12);
    lt = 0x20 + dsz + np * 8; st = lt + nl * 8;
    if (st >= size) return;
    for (i = 0; i < nl && g_skill_count < 2400; ++i) {
        const uint64_t off = jb_u32(d + lt + i * 8), name = jb_u32(d + lt + i * 8 + 4);
        const char* key = (const char*)(d + st + name);
        uint64_t m, n;
        const char* ms;
        if (st + name + 5 > size || key[0] != 'S' || key[1] != 'I' || key[2] != 'D' || key[3] != '_' || off + 0x28 > dsz) continue;
        m = jb_u64(d + 0x20 + off + 0x20);
        if (m < dsz || 0x20 + m + 2 > size) continue;
        ms = (const char*)(d + 0x20 + m);
        for (n = 0; n < 51 && st + name + n < size && key[n]; ++n) {}
        if (n == 51) continue;
        for (n = 0; key[n]; ++n) g_skills[g_skill_count].sid[n] = key[n];
        g_skills[g_skill_count].sid[n] = 0;
        for (n = 0; n < 35 && 0x20 + m + n < size && ms[n]; ++n) g_skills[g_skill_count].mid[n] = ms[n];
        g_skills[g_skill_count].mid[n] = 0;
        g_skill_count++;
    }
    at = put_text(line, "Skill.bin read: "); at = put_int(at, g_skill_count); at = put_text(at, " skills with a name id"); *at = 0;
    TRACE(line);
}
static void person_read(const NxModFile* file, void* user) {
    const uint8_t* d = file->data;
    const uint64_t size = file->size;
    uint64_t dsz, np, nl, lt, st, i;
    char line[120];
    char* at;
    (void)user;
    if (size < 0x20 || jb_u32(d) != size) return;
    dsz = jb_u32(d + 4); np = jb_u32(d + 8); nl = jb_u32(d + 12);
    lt = 0x20 + dsz + np * 8; st = lt + nl * 8;
    if (st >= size) return;
    for (i = 0; i < nl && g_people_count < 1600; ++i) {
        const uint64_t off = jb_u32(d + lt + i * 8), name = jb_u32(d + lt + i * 8 + 4);
        const char* key = (const char*)(d + st + name);
        uint64_t a, m, k, n;
        const char *as, *ms;
        if (st + name + 5 > size || key[0] != 'P' || key[1] != 'I' || key[2] != 'D' || key[3] != '_' || off + 0x280 > dsz) continue;
        m = jb_u64(d + 0x20 + off + 0x20); a = jb_u64(d + 0x20 + off + 0x278);
        if (m < dsz || a < dsz || 0x20 + m + 2 > size || 0x20 + a + 2 > size) continue;
        ms = (const char*)(d + 0x20 + m); as = (const char*)(d + 0x20 + a);
        for (n = 0; n < 55 && 0x20 + a + n < size && as[n]; ++n) {}
        if (n == 55 || !n) continue;
        for (k = 0; k < g_people_count && !jb_same(g_people[k].aid, as); ++k) {}
        if (k < g_people_count) continue;
        for (n = 0; as[n]; ++n) g_people[g_people_count].aid[n] = as[n];
        g_people[g_people_count].aid[n] = 0;
        for (n = 0; n < 39 && 0x20 + m + n < size && ms[n]; ++n) g_people[g_people_count].mid[n] = ms[n];
        g_people[g_people_count].mid[n] = 0;
        g_people_count++;
    }
    at = put_text(line, "Person.bin read: "); at = put_int(at, g_people_count); at = put_text(at, " characters with an asset id and a name id"); *at = 0;
    TRACE(line);
}
static char* pad_line(char* start, char* at) {
    int units = 0;
    const unsigned char* p;
    for (p = (const unsigned char*)start; p < (const unsigned char*)at; ++p)
        if ((*p & 0xc0) != 0x80) units += *p >= 0xe0 ? 2 : 1;      /* one per character, two for the wide ones */
    for (; units + 2 <= LINE_UNITS && at - start < 88; units += 2) { *at++ = (char)0xe3; *at++ = (char)0x80; *at++ = (char)0x80; }
    return at;
}


/* a class's name as the game shows it: the game's own GetText on a stand-in item whose message id is the class's */
static const char* shown_name(Ctx* c, uint64_t row, char* out, int max) {
    uint64_t shown;
    *(uint64_t*)g_name_item = (uint64_t)&g_vt_cls[2];
    tag_set((uint64_t)g_name_item, TAG_CLASS, 0, 0, 0, row);
    shown = call(c, g_vt_cls[2 + SLOT_TEXT], (uint64_t)g_name_item, 0, 0, 0);
    if (shown && text_at(shown, out, max)) return out;
    return short_name(row, out, max);
}

/* a line of a placeholder's sheet: "Base class: Name (JID_...)", or a look field with its value for male / female as the class's
 * rows in the loaded table have it now */
/* ---- the class's numbers ----
 * Stats and ranks are plain numbers in the class's row of the game's table, read whenever the game needs them: a change shows
 * at once. They are kept in job.txt as whole blocks (jobbin.h) and written over the base's at the next start. */
static const char* const kBlockLabel[JB_NBLOCK] = {"Basic Stats (generic units)", "Class Bonus (every unit)", "Stat caps", "Growth Rate (generic units)",
                                                   "Growth Rate Bonus (every unit)", "Weapon Skill Lv", "Class Type", "Movement type", "Class Tier",
                                                   "Primary and Secondary Skills", "Secondary Skills", "Pass Chance lost per missing Lv"};
static const char* const kGroup[8] = {"Class Info", "Basic Stats", "Skill Exp Bonus", "Abilities", "Certification Exam", "Mounted", "Appearance", "Animations and Magic"};
static const char* const kStatLabel[11] = {"HP", "Str", "Mag", "Dex", "Spd", "Lck", "Def", "Res", "Cha", "Sight", "Move"};
static const char* const kWeaponLabel[8] = {"Sword", "Lance", "Axe", "Bow", "Gauntlets", "Black Magic", "White Magic", "Special"};
static const char* const kTypeLabel[5] = {"Infantry", "Cavalry", "Heavy Armor", "Flying", "Authority"};
/* a skill by its number in the exam's lists: the eight weapons, the five others; 15 = none */
static char* put_skill(char* at, int v) { return v >= 0 && v < 8 ? put_text(at, kWeaponLabel[v]) : v >= 8 && v < 13 ? put_text(at, kTypeLabel[v - 8]) : v == 15 ? put_text(at, "-") : put_int(at, (uint64_t)v); }
static const char* num_label(uint32_t bl, uint32_t x) {
    static const char* const kShare[3] = {"Either-or skill 1", "Either-or skill 2", "Either-or skill 3"};
    return kJbBlock[bl].count == 1 ? kBlockLabel[bl] : bl == JB_B_WRANK ? kWeaponLabel[x % 8] : bl == JB_B_UTYPE ? kTypeLabel[x % 5]
           : bl == JB_B_CCREQ ? (x < 8 ? kWeaponLabel[x] : kTypeLabel[(x - 8) % 5]) : bl == JB_B_CCSHARE ? kShare[x % 3] : kStatLabel[x % 11];
}
static int num_get(uint64_t row, uint32_t bl, uint32_t x) {
    const uint64_t a = row + kJbBlock[bl].off + (uint64_t)x * kJbBlock[bl].size;
    if (kJbBlock[bl].size == 2) return *(const int16_t*)a;
    return bl == JB_B_MOVE || bl == JB_B_RANK ? (int)*(const uint8_t*)a : (int)*(const int8_t*)a;
}
static char* put_num(char* at, int v) { if (v < 0) { *at++ = '-'; return put_int(at, (uint64_t)(-v)); } return put_int(at, (uint64_t)v); }
/* delta = the step; wrap = go round from the top to the bottom (the A button, which can only add) */
static int exp_by_rank_later;
static int num_step(uint8_t slot, uint32_t bl, uint32_t x, int delta, int wrap) {
    const uint64_t row = find_job(g_own[slot].key);
    int v, old;
    uint32_t k;
    if (!row || bl >= JB_NBLOCK || x >= kJbBlock[bl].count) return 0;
    old = num_get(row, bl, x); v = old + delta;
    if (v > kJbBlock[bl].hi) v = wrap ? kJbBlock[bl].lo : kJbBlock[bl].hi;
    if (v < kJbBlock[bl].lo) v = wrap ? kJbBlock[bl].hi : kJbBlock[bl].lo;
    if (bl == JB_B_CCSHARE && (v == 13 || v == 14)) v = delta > 0 ? 15 : 12;          /* thirteen skills, then none */
    if (v == old) return 0;
    if (kJbBlock[bl].size == 2) *(volatile int16_t*)(row + kJbBlock[bl].off + (uint64_t)x * 2) = (int16_t)v;
    else *(volatile uint8_t*)(row + kJbBlock[bl].off + x) = (uint8_t)v;
    for (k = 0; k < kJbBlock[bl].count; ++k) g_own[slot].val[bl][k] = (int16_t)num_get(row, bl, k);
    g_own[slot].has[bl] = 1;
    if (bl == JB_B_MOD && foot_of(slot) >= 0) {          /* the dismounted class keeps its distance from the mounted one */
        const uint32_t fs = (uint32_t)foot_of(slot);
        const uint64_t frow = find_job(g_own[fs].key);
        if (frow) {
            int fv = num_get(frow, bl, x) + (v - old);
            if (fv > kJbBlock[bl].hi) fv = kJbBlock[bl].hi;
            if (fv < kJbBlock[bl].lo) fv = kJbBlock[bl].lo;
            *(volatile int16_t*)(frow + kJbBlock[bl].off + (uint64_t)x * 2) = (int16_t)fv;
            for (k = 0; k < kJbBlock[bl].count; ++k) g_own[fs].val[bl][k] = (int16_t)num_get(frow, bl, k);
            g_own[fs].has[bl] = 1;
        }
    }
    if (bl == JB_B_CCREQ && v <= 0) { uint32_t j; for (j = 0; j < 3; ++j) if (*(const uint8_t*)(row + 0x1b1 + j) == x) { *(volatile uint8_t*)(row + 0x1b1 + j) = 15; g_own[slot].val[JB_B_CCSHARE][j] = 15; g_own[slot].has[JB_B_CCSHARE] = 1; } }
    save_own();
    if (bl == JB_B_RANK) exp_by_rank_later = (int)slot + 1;          /* the class experience follows (done after this, where the table is known) */
    return 1;
}
/* ---- the class type ----
 * A class is of a unit type when its number for it (+0x196: foot, cavalry, armor, flying, command) is not negative; the game shows
 * the types' icons beside the class name. A preset sets the four body types and the movement type (+0x30) the game's own classes
 * of that kind have; a type newly given gets the class's rank as its number, as most of the game's classes have. */
#define N_TYPESET 9
/* The icons beside a class's name are its attribute skills (+0xf0: count, +0xf8: pointer to 16-byte entries that start with the
 * skill id), which also decide what is effective against it. A preset shares the list of a class of the game that has exactly
 * the preset's skills, as the game's own rows share theirs. */
static const char* const kTypeSet[N_TYPESET] = {"Infantry", "Armored Infantry", "Cavalry", "Armored Cavalry", "Fliers", "Flying Cavalry", "Dragon", "Beast", "Fiend"};
static const uint8_t kTypeMask[N_TYPESET] = {1, 1 | 4, 2, 2 | 4, 8, 2 | 8, 0, 0, 0};          /* 0: unit types and movement stay */
static const uint8_t kTypeMove[N_TYPESET] = {1, 3, 5, 6, 8, 7, 0, 0, 0};
static const char* const kTypeAttr[N_TYPESET][2] = {
    {"SID_\xe6\xad\xa9\xe5\x85\xb5", 0}, {"SID_\xe6\xad\xa9\xe5\x85\xb5", "SID_\xe9\x87\x8d\xe8\xa3\x85"}, {"SID_\xe9\xa8\x8e\xe5\x85\xb5", 0},
    {"SID_\xe9\xa8\x8e\xe5\x85\xb5", "SID_\xe9\x87\x8d\xe8\xa3\x85"}, {"SID_\xe9\xa3\x9b\xe8\xa1\x8c", 0}, {"SID_\xe9\xa8\x8e\xe5\x85\xb5", 0},
    {"SID_\xe7\xab\x9c", 0}, {"SID_\xe7\x8d\xa3", 0}, {"SID_\xe5\x86\xa5\xe9\xad\x94", 0}};
static const char* attr_sid(uint64_t job_row, uint32_t x) {
    uint64_t list = 0, s0 = 0;
    if (!job_row || *(const uint16_t*)(job_row + JB_ATTR) <= x || !rd64(job_row + JB_ATTR + 8, &list) || !rd64(list + (uint64_t)x * 16, &s0) || s0 < 0x10000 || !g_api->is_mapped(s0, 1)) return "";
    return (const char*)s0;
}
/* The game keeps, for every class, sets of its skills made when the class table is loaded: the row's +0x18 points at the class's
 * loaded object, with a skill set at +0x240 and one at +0x2a0 (the header picks one by a setting; 0x18db400). A set: +0 / +8 the
 * begin / end of u32 words, one bit per skill index; +0x20 / +0x28 / +0x30 begin / end / room of a u32 list, low 12 bits = the
 * skill's index in the skill table (object at *[main+0x216a8c0], array at +0x40; a skill's key at +8, its kind at +0xfa); +0x58
 * the kinds in the list as bits. The class header draws its icons from the list (0x18db780), so a change of the row's attribute
 * skills shows only once the sets have them too: their attribute entries are replaced by the other class's. */
static int attr_index(uint32_t entry, uint64_t* skill) {      /* 1 when the entry is one of the attribute skills */
    static const char* const kSid[7] = {"SID_\xe6\xad\xa9\xe5\x85\xb5", "SID_\xe9\x87\x8d\xe8\xa3\x85", "SID_\xe9\xa8\x8e\xe5\x85\xb5", "SID_\xe9\xa3\x9b\xe8\xa1\x8c", "SID_\xe7\xab\x9c", "SID_\xe7\x8d\xa3", "SID_\xe5\x86\xa5\xe9\xad\x94"};
    uint64_t p = 0, obj = 0, arr = 0, sk = 0, key = 0;
    char text[48];
    uint32_t w;
    *skill = 0;
    if (!rd64(g_base + 0x216a8c0ull, &p) || !rd64(p, &obj) || !rd64(obj + 0x40, &arr) || !rd64(arr + (uint64_t)(entry & 0xfff) * 8, &sk) || !sk) return -1;
    if (!g_api->is_mapped(sk, 0x100) || !rd64(sk + 8, &key) || !text_at(key, text, sizeof text) || text[0] != 'S' || text[1] != 'I' || text[2] != 'D') return -1;
    *skill = sk;
    for (w = 0; w < 7; ++w) if (same_text(text, kSid[w])) return 1;
    return 0;
}
static int attr_set_swap(uint64_t mine, uint64_t theirs, uint32_t* out_n, uint32_t* in_n) {      /* 1 done, 0 no room, -1 not as expected */
    uint64_t w1 = 0, we = 0, b1 = 0, e1 = 0, c1 = 0, b2 = 0, e2 = 0, sk = 0, mask = 0;
    uint32_t keep[64], drop[16], add[16], n = 0, nd = 0, na = 0, n1, n2, nw, i;
    if (!g_api->is_mapped(mine, 0x60) || !g_api->is_mapped(theirs, 0x60) || mine == theirs) return -1;
    if (!rd64(mine, &w1) || !rd64(mine + 8, &we) || !rd64(mine + 0x20, &b1) || !rd64(mine + 0x28, &e1) || !rd64(mine + 0x30, &c1) || !rd64(theirs + 0x20, &b2) || !rd64(theirs + 0x28, &e2)) return -1;
    n1 = (uint32_t)((e1 - b1) / 4); n2 = (uint32_t)((e2 - b2) / 4); nw = (uint32_t)((we - w1) / 4);
    if (e1 < b1 || e2 < b2 || c1 < e1 || we < w1 || n1 > 48 || n2 > 48 || nw > 128 || (n1 && !g_api->is_mapped(b1, n1 * 4)) || (n2 && !g_api->is_mapped(b2, n2 * 4)) || (nw && !g_api->is_mapped(w1, nw * 4))) return -1;
    for (i = 0; i < n1; ++i) {
        const uint32_t e = *(const uint32_t*)(b1 + (uint64_t)i * 4);
        const int a = attr_index(e, &sk);
        if (a < 0) return -1;
        if (a) { if (nd < 16) drop[nd++] = e & 0xfff; } else { keep[n++] = e; mask |= 1ull << (*(const uint8_t*)(sk + 0xfa) & 63); }
    }
    for (i = 0; i < n2; ++i) {
        const uint32_t e = *(const uint32_t*)(b2 + (uint64_t)i * 4);
        const int a = attr_index(e, &sk);
        if (a < 0) return -1;
        if (a && na < 16) { keep[n++] = e; add[na++] = e & 0xfff; mask |= 1ull << (*(const uint8_t*)(sk + 0xfa) & 63); }
    }
    *out_n = nd; *in_n = na;
    if (!b1 || (uint64_t)n * 4 > c1 - b1) return 0;
    for (i = 0; i < na; ++i) if ((add[i] >> 5) >= nw) return 0;          /* the set's bits do not reach this skill */
    for (i = 0; i < nd; ++i) if ((drop[i] >> 5) < nw) *(volatile uint32_t*)(w1 + (uint64_t)(drop[i] >> 5) * 4) &= ~(1u << (drop[i] & 31));
    for (i = 0; i < na; ++i) *(volatile uint32_t*)(w1 + (uint64_t)(add[i] >> 5) * 4) |= 1u << (add[i] & 31);
    for (i = 0; i < n; ++i) *(volatile uint32_t*)(b1 + (uint64_t)i * 4) = keep[i];
    *(volatile uint64_t*)(mine + 0x28) = b1 + (uint64_t)n * 4;
    *(volatile uint64_t*)(mine + 0x58) = *(const uint64_t*)(mine + 0x58) | mask;          /* a kind bit too many only costs the game a look */
    return 1;
}
/* how many attribute skills the game's first set of a class holds (a mounted class's set also has its mount's); -1 = not readable */
static int attr_set_count(uint64_t row) {
    uint64_t r = 0, b = 0, e = 0, sk = 0;
    uint32_t n, i;
    int c = 0;
    if (!rd64(row + 0x18, &r) || !r || !g_api->is_mapped(r, 0x300) || !rd64(r + 0x260, &b) || !rd64(r + 0x268, &e) || e < b) return -1;
    n = (uint32_t)((e - b) / 4);
    if (n > 48 || (n && !g_api->is_mapped(b, n * 4))) return -1;
    for (i = 0; i < n; ++i) { const int a = attr_index(*(const uint32_t*)(b + (uint64_t)i * 4), &sk); if (a < 0) return -1; c += a; }
    return c;
}
static void attr_live(uint64_t row, uint64_t from, const char* key) {
    static const uint16_t kSet[2] = {0x240, 0x2a0};
    uint64_t rm = 0, rt = 0;
    uint32_t k;
    char line[220];
    char* at = put_text(line, "class "); at = put_text(at, key); at = put_text(at, ": the game's skill sets of the class: ");
    if (!rd64(row + 0x18, &rm) || !rd64(from + 0x18, &rt) || !rm || !rt || !g_api->is_mapped(rm, 0x300) || !g_api->is_mapped(rt, 0x300)) { at = put_text(at, "not found (the change shows after a restart)"); *at = 0; TRACE(line); return; }
    for (k = 0; k < 2; ++k) {
        uint32_t o = 0, in = 0;
        const int r = attr_set_swap(rm + kSet[k], rt + kSet[k], &o, &in);
        if (k) at = put_text(at, "; ");
        at = put_text(at, k ? "second " : "first ");
        if (r < 0) at = put_text(at, "not as expected, left alone");
        else { at = put_text(at, r ? "changed at once, " : "no room (after a restart), "); at = put_int(at, o); at = put_text(at, " out, "); at = put_int(at, in); at = put_text(at, " in"); }
    }
    *at = 0;
    TRACE(line);
}
/* ---- the on-foot partner ----
 * A mounted class's row points at the class its units are on foot (+0x58); the unit stays of the mounted class and the game shows
 * and uses the other one while it is dismounted, so the one link gives both the dismount and the mount command. The partner is a
 * new entry in job.txt: a copy of this one (numbers, name, skill, look) on foot, of the infantry type, with no mount and no
 * on-foot class of its own. A row cannot be added to the loaded table, so it exists from the next start. */
static void save_looks(void);
/* the on-foot class must not wear the mount or ride: its mount and animation choices go back to the base's (a class on foot),
 * and the mounted class shows the mount its unit has equipped, not one chosen as a look */
/* The kinds of class the Base Class list is sorted into, by what the class is in the game: its attribute skills (flying, cavalry,
 * armored) first, then whether magic is its best weapon skill. Every cavalry and flier class of the game is a mounted one. */
enum { N_BCAT = 5 };
static const char* const kBcat[N_BCAT] = {"Infantry", "Magic", "Armored", "Cavalry (mounted)", "Fliers (mounted)"};
static uint32_t class_cat(uint64_t row) {
    uint32_t k, fly = 0, cav = 0, arm = 0;
    int phys = -1, mag = -1;
    for (k = 0; k < *(const uint16_t*)(row + JB_ATTR) && k < 8; ++k) {
        const char* sid = attr_sid(row, k);
        if (same_text(sid, "SID_\xe9\xa3\x9b\xe8\xa1\x8c")) fly = 1;
        else if (same_text(sid, "SID_\xe9\xa8\x8e\xe5\x85\xb5")) cav = 1;
        else if (same_text(sid, "SID_\xe9\x87\x8d\xe8\xa3\x85")) arm = 1;
    }
    if (fly) return 4;
    if (cav) return 3;
    if (arm) return 2;
    for (k = 0; k < 8; ++k) { const int v = num_get(row, JB_B_WRANK, k); if (k == 5 || k == 6) { if (v > mag) mag = v; } else if (k < 5 && v > phys) phys = v; }
    return mag >= 0 && mag >= phys ? 1 : 0;
}
/* a class the game treats as mounted: it has an on-foot class (+0x58) and a mount (+0x68) */
static int row_mounted(uint64_t row) { return row && *(const uint64_t*)(row + 0x58) && *(const uint64_t*)(row + 0x68); }
static void foot_looks(uint32_t mounted, uint32_t foot) {
    uint32_t sx, k;
    /* the game's pairs both have the mount / dismount ability (SID_乗り降り); the light cavalry's list is that skill alone */
    if (g_own[mounted].has_abil) {          /* abilities of its own: mount / dismount must be one of the three, and the on-foot class has the same */
        uint32_t a3, have = 0, free3 = 2;
        for (a3 = 0; a3 < 3; ++a3) { if (same_text(g_own[mounted].abil[a3], "SID_\xe4\xb9\x97\xe3\x82\x8a\xe9\x99\x8d\xe3\x82\x8a")) have = 1; }
        for (a3 = 3; a3-- > 0;) if (!g_own[mounted].abil[a3][0]) free3 = a3;
        if (!have) { char* a2 = put_text(g_own[mounted].abil[free3], "SID_\xe4\xb9\x97\xe3\x82\x8a\xe9\x99\x8d\xe3\x82\x8a"); *a2 = 0; }
        for (a3 = 0; a3 < 3; ++a3) { char* a2 = put_text(g_own[foot].abil[a3], g_own[mounted].abil[a3]); *a2 = 0; }
        g_own[foot].has_abil = 1; g_own[foot].skills[0] = 0;
    } else if (row_mounted(find_job(g_own[mounted].base)) && !g_own[mounted].skills[0]) {
        g_own[foot].skills[0] = 0;          /* a mounted base: both keep their bases' abilities, which have mount / dismount as the game made them */
    } else {
        if (!g_own[mounted].skills[0]) { char* a2 = put_text(g_own[mounted].skills, "JID_\xe8\xbb\xbd\xe9\xa8\x8e\xe5\x85\xb5"); *a2 = 0; }
        { char* a2 = put_text(g_own[foot].skills, g_own[mounted].skills); *a2 = 0; }
    }
    { char* a2 = put_text(g_own[foot].barred, "none"); *a2 = 0; }
    for (sx = 0; sx < 2; ++sx) {
        g_own_look[foot].src[sx][JB_MOUNT][0] = 0;
        g_own_look[foot].src[sx][JB_ANIM][0] = 0;
        g_own_look[mounted].src[sx][JB_MOUNT][0] = 0;
        for (k = 0; k < N_WKIND; ++k) g_wanim[foot][sx][k][0] = 0;
    }
    g_cast_spells[foot] = 0; g_cast[foot].hand[0] = 0; g_cast[foot].effect[0] = 0;          /* on foot it casts as the game does: no held model, spells and glyph at the casting point */
}
static int gen_dismount(uint8_t slot) {      /* 1 made, 0 not (logged) */
    static const char kFoot[] = "\xe4\xb8\x8b\xe9\xa6\xac";          /* 下馬, as the game's own pairs are named */
    const uint64_t row = find_job(g_own[slot].key);
    JbEntry* e;
    char key[JB_KEY_MAX + 8], fkey[JB_KEY_MAX + 8], line[240];
    char* at;
    uint32_t n = 0, k, f, s2, fn = 0, bl, x;
    uint64_t brow, frow, own_foot;
    int mounted_base, fresh;
    at = put_text(key, g_own[slot].key); at = put_text(at, kFoot); *at = 0;
    while (key[n]) n++;
    at = put_text(line, "class "); at = put_text(at, g_own[slot].key); at = put_text(at, ": dismounted class ");
    for (k = 0; k < g_own_count && !same_text(g_own[k].key, key); ++k) {}
    fresh = k == g_own_count;
    if (!row || n >= JB_KEY_MAX || (fresh && g_own_count >= MAX_SLOTS) || g_own[slot].no_mount) {
        at = put_text(at, !row ? "not made: the class is not loaded yet" : g_own[slot].no_mount ? "not made: this is a dismounted class" : "not made: no room for another entry");
        *at = 0; g_api->log(g_api, 2, line);
        return 0;
    }
    {   /* the base's own on-foot class, when the base is a mounted class */
        char* f2 = put_text(fkey, g_own[slot].base); f2 = put_text(f2, kFoot); *f2 = 0;
        while (fkey[fn]) fn++;
        brow = find_job(g_own[slot].base); frow = fn < JB_KEY_MAX ? find_job(fkey) : 0;
        mounted_base = row_mounted(brow) && frow;
    }
    if (fresh && !mounted_base) {          /* a new pair needs a mounted base */
        at = put_text(at, "not made: the Base Class is not a mounted class (choose a mounted Base Class first)");
        *at = 0; g_api->log(g_api, 2, line);
        return 0;
    }
    /* a pair made earlier from a class on foot: the game reads the mount of a class that has an on-foot class (0x96bbbc), so one
     * without a mount takes that of the class whose picture, type or numbers it has, whichever is mounted */
    if (!mounted_base && !*(const uint64_t*)(row + 0x68) && !g_own[slot].mount_from[0]) {
        const char* const cand[3] = {g_own[slot].icon, g_own[slot].attr, g_own[slot].base};
        uint32_t c2;
        for (c2 = 0; c2 < 3 && !g_own[slot].mount_from[0]; ++c2) {
            const uint64_t r2 = cand[c2][0] ? find_job(cand[c2]) : 0;
            if (r2 && *(const uint64_t*)(r2 + 0x68)) { char* a2 = put_text(g_own[slot].mount_from, cand[c2]); *a2 = 0; }
        }
        if (!g_own[slot].mount_from[0]) {
            at = put_text(at, "not made: the class has no mount, and neither has the class of its picture, type or base");
            *at = 0; g_api->log(g_api, 2, line);
            return 0;
        }
    }
    /* The entry is written anew each time (the first time, and at "Regenerate"): a copy of the mounted class's entry, then what a
     * dismounted class has of its own. */
    s2 = fresh ? g_own_count : k;
    e = &g_own[s2];
    *e = g_own[slot];
    for (k = 0; k <= n; ++k) e->key[k] = key[k];
    if (mounted_base) {
        /* a copy of the base's on-foot class (its type, movement, attribute skills, icon, look and animations are that class's),
         * with this class's own numbers on top. The game's pairs differ in the Class Bonus (the on-foot class loses what the
         * mount gives): the same difference is taken off here. The exam is asked of the mounted class only. */
        for (k = 0; k <= fn; ++k) e->base[k] = fkey[k];
        e->has[JB_B_UTYPE] = e->has[JB_B_MOVE] = e->has[JB_B_CCREQ] = e->has[JB_B_CCSHARE] = e->has[JB_B_CCFAIL] = 0;
        if (e->has[JB_B_MOD]) for (k = 0; k < kJbBlock[JB_B_MOD].count; ++k) {
            int v = e->val[JB_B_MOD][k] - num_get(brow, JB_B_MOD, k) + num_get(frow, JB_B_MOD, k);
            e->val[JB_B_MOD][k] = (int16_t)(v < kJbBlock[JB_B_MOD].lo ? kJbBlock[JB_B_MOD].lo : v > kJbBlock[JB_B_MOD].hi ? kJbBlock[JB_B_MOD].hi : v);
        }
        e->attr[0] = 0;
        if (e->icon[0]) {          /* another class's icon: its on-foot one when it has one */
            char ik[JB_KEY_MAX + 8];
            char* i2 = put_text(ik, e->icon);
            i2 = put_text(i2, kFoot); *i2 = 0;
            if ((uint64_t)(i2 - ik) < JB_KEY_MAX && find_job(ik)) { i2 = put_text(e->icon, ik); *i2 = 0; }
        }
    } else {          /* an earlier pair, base on foot: the same class as infantry, movement type 1 */
        const int tier = num_get(row, JB_B_RANK, 0);
        for (k = 0; k < 5; ++k) e->val[JB_B_UTYPE][k] = (int16_t)(k == 0 ? tier : k == 4 ? num_get(row, JB_B_UTYPE, 4) : -1);
        e->val[JB_B_MOVE][0] = 1;
        e->has[JB_B_UTYPE] = e->has[JB_B_MOVE] = 1;
        { char* a2 = put_text(e->attr, "JID_\xe9\x97\x98\xe5\xa3\xab"); *a2 = 0; }          /* the fighter's attribute skills: foot only */
    }
    { char* a2 = put_text(e->dismount, "none"); *a2 = 0; }
    e->no_mount = 1; e->mount_from[0] = 0;
    { char* a2 = put_text(g_own[slot].dismount, key); *a2 = 0; }
    g_own_look[s2] = g_own_look[slot];
    for (k = 0; k < 2; ++k) for (f = 0; f < N_WKIND; ++f) { uint32_t c2 = 0; do g_wanim[s2][k][f][c2] = g_wanim[slot][k][f][c2]; while (g_wanim[slot][k][f][c2++]); }
    g_cast[s2] = g_cast[slot]; g_cast_spells[s2] = g_cast_spells[slot];
    foot_looks(slot, s2);
    if (fresh) g_own_count++;
    /* the numbers of a dismounted class that is loaded change at once: its own where it has them, else those of the on-foot
     * class it is a copy of (its look, type icons and animations are made from the files at the next start) */
    own_foot = fresh ? 0 : find_job(key);
    for (bl = 0; own_foot && bl < JB_NBLOCK; ++bl) {
        if (!e->has[bl] && !mounted_base) continue;
        for (x = 0; x < kJbBlock[bl].count; ++x) {
            const int v = e->has[bl] ? e->val[bl][x] : num_get(frow, bl, x);
            if (kJbBlock[bl].size == 2) *(volatile int16_t*)(own_foot + kJbBlock[bl].off + (uint64_t)x * 2) = (int16_t)v;
            else *(volatile uint8_t*)(own_foot + kJbBlock[bl].off + x) = (uint8_t)v;
        }
    }
    save_own(); save_looks();
    at = put_text(at, key);
    at = put_text(at, fresh ? " written to job.txt; both classes are linked from the next start" : " made again from the mounted class as it is now; numbers at once, look and type from the next start");
    *at = 0;
    g_api->log(g_api, 2, line);
    return 1;
}
/* ---- abilities and barred characters ----
 * Abilities: the class's owned skills (+0x90: count, +0x98: 16-byte entries that start with the skill id). Up to three, each one
 * of the skills the game's classes have as abilities; written as a list of the class's own at the next start (jobbin.h).
 * Barred characters (+0x1e8): the list of a class of the game, shared; at once. */
#define MAX_ABIL 128
static char g_abil[MAX_ABIL][JB_KEY_MAX];
static uint32_t g_abil_count;
static uint64_t g_bar[24];
static uint32_t g_bar_count;
static const char* own_sid(uint64_t row, uint32_t x) {
    uint64_t list = 0, s0 = 0;
    if (!row || *(const uint16_t*)(row + 0x90) <= x || !rd64(row + 0x98, &list) || !rd64(list + (uint64_t)x * 16, &s0) || s0 < 0x10000 || !g_api->is_mapped(s0, 1)) return "";
    return (const char*)s0;
}
static void abil_collect(uint8_t slot) {
    int i;
    uint32_t x, k;
    char key[JB_KEY_MAX];
    g_abil_count = 0; g_bar_count = 0;
    for (i = 0; i < g_job_count; ++i) {
        const uint64_t r = g_jobs[i];
        if (!row_key(r, key, sizeof key)) continue;
        for (k = 0; k < g_own_count && !same_text(g_own[k].key, key); ++k) {}
        if (k < g_own_count) continue;          /* the game's classes only: job.txt names them, and they are there at the start */
        for (x = 0; x < *(const uint16_t*)(r + 0x90) && x < 8; ++x) {
            const char* sid = own_sid(r, x);
            uint32_t c2 = 0;
            if (!sid[0]) continue;
            for (k = 0; k < g_abil_count && !same_text(g_abil[k], sid); ++k) {}
            if (k < g_abil_count || g_abil_count >= MAX_ABIL) continue;
            while (sid[c2] && c2 < JB_KEY_MAX - 1) { g_abil[g_abil_count][c2] = sid[c2]; c2++; }
            g_abil[g_abil_count++][c2] = 0;
        }
        if (*(const uint16_t*)(r + 0x1e8)) {
            for (k = 0; k < g_bar_count && *(const uint64_t*)(g_bar[k] + 0x1f0) != *(const uint64_t*)(r + 0x1f0); ++k) {}
            if (k == g_bar_count && g_bar_count < 24) g_bar[g_bar_count++] = r;
        }
    }
    if (!g_own[slot].has_abil) {          /* the three slots start as what the class has now */
        const uint64_t row = find_job(g_own[slot].key);
        for (x = 0; x < 3; ++x) { char* a2 = put_text(g_own[slot].abil[x], own_sid(row, x)); *a2 = 0; }
    }
}
static const char* const kSexWord[3] = {"Anyone", "Male only", "Female only"};
static void type_take(uint8_t slot, uint32_t set) {
    const uint64_t row = find_job(g_own[slot].key);
    uint64_t from = 0, loose = 0;
    uint32_t k;
    int rank, i;
    char line[200];
    char* at;
    if (!row || set >= N_TYPESET) return;
    for (i = 0; i < g_job_count && !from; ++i) {          /* a class of the game with exactly these attribute skills */
        const uint64_t r = g_jobs[i];
        const uint32_t want = kTypeAttr[set][1] ? 2 : 1;
        char key[JB_KEY_MAX];
        if (r == row || *(const uint16_t*)(r + JB_ATTR) != want || !same_text(attr_sid(r, 0), kTypeAttr[set][0])) continue;
        if (want == 2 && !same_text(attr_sid(r, 1), kTypeAttr[set][1])) continue;
        if (!row_key(r, key, sizeof key)) continue;
        for (k = 0; k < g_own_count && !same_text(g_own[k].key, key); ++k) {}
        if (k < g_own_count) continue;          /* not another created class: job.txt names it, and it must be there at the start */
        if (attr_set_count(r) == (int)(set == 5 ? 2 : want)) from = r;          /* flying cavalry: cavalry, with a flying mount's type on top */          /* and whose loaded set has no more than these (no mount's types on top) */
        else if (!loose) loose = r;
    }
    if (!from) from = loose;
    if (from) {
        for (k = 0; k < 16; ++k) ((volatile uint8_t*)row)[JB_ATTR + k] = ((const uint8_t*)from)[JB_ATTR + k];
        row_key(from, g_own[slot].attr, sizeof g_own[slot].attr);
        attr_live(row, from, g_own[slot].key);
    }
    at = put_text(line, "class "); at = put_text(at, g_own[slot].key); at = put_text(at, ": type "); at = put_text(at, kTypeSet[set]);
    at = put_text(at, from ? ", attribute skills of " : ", no class found with its attribute skills"); if (from) at = put_text(at, g_own[slot].attr); *at = 0;
    TRACE(line);
    if (kTypeMask[set]) {
        rank = num_get(row, JB_B_RANK, 0);
        for (k = 0; k < 4; ++k) {
            const int old = num_get(row, JB_B_UTYPE, k);
            *(volatile uint8_t*)(row + kJbBlock[JB_B_UTYPE].off + k) = (uint8_t)((kTypeMask[set] >> k & 1) ? (old >= 0 ? old : rank) : -1);
        }
        *(volatile uint8_t*)(row + kJbBlock[JB_B_MOVE].off) = kTypeMove[set];
        for (k = 0; k < 5; ++k) g_own[slot].val[JB_B_UTYPE][k] = (int16_t)num_get(row, JB_B_UTYPE, k);
        g_own[slot].val[JB_B_MOVE][0] = (int16_t)num_get(row, JB_B_MOVE, 0);
        g_own[slot].has[JB_B_UTYPE] = g_own[slot].has[JB_B_MOVE] = 1;
    }
    save_own();
}
/* ---- the mastery skill ----
 * A class's row lists the skill a unit gets for mastering it (+0x100: count, +0x108: pointer to the skill ids, "SID_..."). The
 * creator offers the mastery skills the game's classes have; taking one makes the class's list that class's (the two rows then
 * share it, as the game's own rows do). */
static int game_text(Ctx* c, const char* mid, char* out, int max);
static const char* master_sid(uint64_t job_row) {
    uint64_t list = 0, s0 = 0;
    if (!job_row || *(const uint16_t*)(job_row + JB_MASTER) < 1 || !rd64(job_row + JB_MASTER + 8, &list) || !rd64(list, &s0) || s0 < 0x10000 || !g_api->is_mapped(s0, 1)) return "";
    return (const char*)s0;
}
static const char* skill_name(Ctx* c, const char* sid, char* out, int max) {
    uint32_t k;
    if (!sid[0]) return "(none)";
    for (k = 0; k < g_skill_count; ++k)
        if (same_text(g_skills[k].sid, sid)) { if (game_text(c, g_skills[k].mid, out, max)) return out; break; }
    return sid;
}
static void take_master(uint8_t slot, uint64_t from) {          /* from = a class row, or the empty row for none */
    const uint64_t row = find_job(g_own[slot].key);
    const char* sid = from == (uint64_t)g_zero_row ? "" : master_sid(from);
    char line[200];
    char* at;
    uint32_t k;
    if (!row) return;
    if (sid[0]) for (k = 0; k < 16; ++k) ((volatile uint8_t*)row)[JB_MASTER + k] = ((const uint8_t*)from)[JB_MASTER + k];
    else { *(volatile uint16_t*)(row + JB_MASTER) = 0; *(volatile uint64_t*)(row + JB_MASTER + 8) = 0; }
    at = put_text(g_own[slot].master, sid[0] ? sid : "none"); *at = 0;
    save_own();
    at = put_text(line, "class "); at = put_text(at, g_own[slot].key); at = put_text(at, ": mastery skill = "); at = put_text(at, g_own[slot].master); *at = 0;
    TRACE(line);
}
/* The numbers are changed on a list screen of the game's own kind (the runtime's mod interface, version 4): the pop-up menus do not
 * pass left / right to their lines (seen in the user's game), that screen does. One block of numbers per screen; movement type and
 * rank share one. Left / right: one down / up. Confirm: five up (one for ranks and types), going round at the top. */
/* ---- choices made from a list ----
 * A line whose value is one of a few (gender, an ability, the barred characters, where spells start) opens a list of them; the
 * one in use is marked. `what`: 0 gender, 1-3 an ability slot, 4 barred characters, 5 where spells start. Item 0 of the ability
 * and barred lists is "none". A list longer than a sub menu shows is split into pages. */
enum { PICK_SEX = 0, PICK_BAR = 4, PICK_CAST = 5 };
static uint32_t pick_count(uint32_t what) { return what == PICK_SEX ? 3 : what == PICK_CAST ? 2 : what == PICK_BAR ? g_bar_count + 1 : g_abil_count + 1; }
static uint32_t pick_cur(uint8_t slot, uint32_t what) {
    const uint64_t row = find_job(g_own[slot].key);
    uint32_t k;
    if (what == PICK_SEX) return row ? *(const uint8_t*)(row + 0x32) % 3u : 0;
    if (what == PICK_CAST) return g_cast_spells[slot] ? 1 : 0;
    if (what == PICK_BAR) {
        if (!row || !*(const uint16_t*)(row + 0x1e8)) return 0;
        for (k = 0; k < g_bar_count; ++k) if (*(const uint64_t*)(g_bar[k] + 0x1f0) == *(const uint64_t*)(row + 0x1f0)) return k + 1;
        return 0xffff;
    }
    if (!g_own[slot].abil[what - 1][0]) return 0;
    for (k = 0; k < g_abil_count; ++k) if (same_text(g_abil[k], g_own[slot].abil[what - 1])) return k + 1;
    return 0xffff;
}
static char* pick_name(Ctx* c, char* at, uint32_t what, uint32_t idx) {
    char name[64];
    if (what == PICK_SEX) return put_text(at, kSexWord[idx % 3]);
    if (what == PICK_CAST) return put_text(at, idx ? "The right hand" : "The casting point (as the game)");
    if (!idx) return put_text(at, "(none)");
    if (what == PICK_BAR) {
        if (idx > g_bar_count) return put_text(at, "?");
        at = put_text(at, "As "); at = put_text(at, shown_name(c, g_bar[idx - 1], name, sizeof name));
        at = put_text(at, " ("); at = put_int(at, *(const uint16_t*)(g_bar[idx - 1] + 0x1e8)); return put_text(at, " characters)");
    }
    return idx <= g_abil_count ? put_text(at, skill_name(c, g_abil[idx - 1], name, sizeof name)) : put_text(at, "?");
}
static void pick_take(uint8_t slot, uint32_t what, uint32_t idx) {
    const uint64_t row = find_job(g_own[slot].key);
    char* a2;
    if (what == PICK_CAST) { g_cast_spells[slot] = idx ? 1 : 0; save_looks(); return; }
    if (!row) return;
    if (what == PICK_SEX) {
        g_own[slot].sex = (int8_t)(idx % 3); g_own[slot].has_sex = 1;
        *(volatile uint8_t*)(row + 0x32) = (uint8_t)g_own[slot].sex;
    } else if (what == PICK_BAR) {
        if (idx && idx <= g_bar_count) {
            uint32_t b2;
            for (b2 = 0; b2 < 16; ++b2) ((volatile uint8_t*)row)[0x1e8 + b2] = ((const uint8_t*)g_bar[idx - 1])[0x1e8 + b2];
            row_key(g_bar[idx - 1], g_own[slot].barred, sizeof g_own[slot].barred);
        } else {
            *(volatile uint16_t*)(row + 0x1e8) = 0; *(volatile uint64_t*)(row + 0x1f0) = 0;
            a2 = put_text(g_own[slot].barred, "none"); *a2 = 0;
        }
    } else {
        a2 = put_text(g_own[slot].abil[what - 1], idx && idx <= g_abil_count ? g_abil[idx - 1] : ""); *a2 = 0;
        g_own[slot].has_abil = 1;
    }
    save_own();
}
static uint8_t g_num_slot, g_num_block;          /* what the open screen shows; block JB_NBLOCK = movement type and rank */
static NxModList g_num_list;
static void copy_out(char* out, uint64_t n, const char* t) { uint64_t k = 0; if (!n) return; while (t[k] && k + 1 < n) { out[k] = t[k]; k++; } out[k] = 0; }
#define NUM_SKILLS (JB_NBLOCK + 1)          /* the thirteen skills on one screen: the eight weapons, then the five others */
static uint32_t num_row_block(int row) { return g_num_block == JB_NBLOCK ? (row ? JB_B_MOVE : JB_B_RANK) : g_num_block == NUM_SKILLS ? (row < 8 ? JB_B_WRANK : JB_B_UTYPE) : g_num_block; }
static uint32_t num_row_elem(int row) { return g_num_block == JB_NBLOCK ? 0 : g_num_block == NUM_SKILLS && row >= 8 ? (uint32_t)row - 8 : (uint32_t)row; }
/* The exam (traced at 0x96a2e0 and 0x15bb8fc; the user's test named the headings): a skill with a required Skill Lv is a Primary
 * Skill - every one is needed - unless it is one of the three in the row's list at +0x1b1, which makes it a Secondary Skill: of
 * those only the one the unit is closest in counts. */
static int share_find(uint64_t row, uint32_t x) { uint32_t k; for (k = 0; k < 3; ++k) if (*(const uint8_t*)(row + 0x1b1 + k) == x) return (int)k; return -1; }
static void share_set(uint8_t slot, uint64_t row, uint32_t k, uint32_t v) {
    uint32_t j;
    *(volatile uint8_t*)(row + 0x1b1 + k) = (uint8_t)v;
    for (j = 0; j < 3; ++j) g_own[slot].val[JB_B_CCSHARE][j] = (int16_t)*(const uint8_t*)(row + 0x1b1 + j);
    g_own[slot].has[JB_B_CCSHARE] = 1;
}
static void numl_label(int row, char* out, uint64_t n, void* user) {
    const uint32_t bl = num_row_block(row), x = num_row_elem(row);
    (void)user;
    copy_out(out, n, num_label(bl, x));
}
static void numl_value(int row, char* out, uint64_t n, void* user) {
    const uint32_t bl = num_row_block(row), x = num_row_elem(row);
    const uint64_t job = find_job(g_own[g_num_slot].key);
    char t[40];
    char* e = t;
    const int v = job ? num_get(job, bl, x) : 0;
    (void)user;
    if (v < 0 && (bl == JB_B_WRANK || bl == JB_B_UTYPE)) e = put_text(e, "-"); else if (bl == JB_B_CCSHARE) e = put_skill(e, v);
    else if (bl == JB_B_CCREQ) { if (v <= 0) e = put_text(e, "-"); else { e = put_num(e, v); e = put_text(e, job && share_find(job, x) >= 0 ? "  Secondary" : "  Primary"); } }
    else e = put_num(e, v);
    *e = 0;
    copy_out(out, n, t);
}
static void numl_help(int row, char* out, uint64_t n, void* user) {
    static const char* const kHelp[JB_NBLOCK] = {
        "Basic Stats of units whose character has none of its own (generic soldiers).", "Added to the stats of every unit in this class (for a mounted class: while mounted).",
        "The highest each stat can reach in this class.", "Growth Rate of units whose character has none of its own (generic soldiers).",
        "Added to the Growth Rate of every unit in this class.", "Skill Exp Bonus of the class in this weapon. - means it cannot use the weapon.",
        "Skill Exp Bonus of the class in this skill. - means it is not that kind of class.", "How the class moves across terrain (the game's own number).",
        "Class Tier, 1 to 5. Points to Master follow it.",
        "Skill Lv needed for the exam (- = not asked). Primary: always needed. Secondary: of up to three, only the unit's best counts.",
        "Of the skills listed here only the one the unit is closest in counts. Every other required skill counts in full.",
        "Pass Chance = 100 - this x the missing Skill Lv. The game's classes have 30, or 15."};
    char t[240];
    char* e = put_text(t, kHelp[num_row_block(row)]);
    (void)user;
    e = put_text(e, num_row_block(row) == JB_B_CCREQ ? "  Left / right: the level.  Confirm: Primary / Secondary." : kJbBlock[num_row_block(row)].size == 2 ? "  Left / right: 1.  Confirm: +5." : "  Left / right: 1.");
    *e = 0;
    copy_out(out, n, t);
}
static int numl_side(Ctx* c, int row, int dir, void* user) { (void)user; g_ctx = c; return num_step(g_num_slot, num_row_block(row), num_row_elem(row), dir, 0); }
static uint32_t numl_pick(Ctx* c, int row, void* user) {
    const uint32_t bl = num_row_block(row);
    (void)user; g_ctx = c;
    if (bl == JB_B_CCREQ) {          /* Primary <-> Secondary, for a skill that is asked for; three Secondary Skills at most */
        const uint64_t job = find_job(g_own[g_num_slot].key);
        const uint32_t x = num_row_elem(row);
        int k;
        if (!job || num_get(job, bl, x) <= 0) return 13;
        k = share_find(job, x);
        if (k >= 0) share_set(g_num_slot, job, (uint32_t)k, 15);
        else if ((k = share_find(job, 15)) >= 0) share_set(g_num_slot, job, (uint32_t)k, x);
        else return 13;
        save_own();
        return 13;
    }
    num_step(g_num_slot, bl, num_row_elem(row), kJbBlock[bl].size == 2 ? 5 : 1, 1);
    return 13;          /* the list stays; the row's value is drawn again */
}
static int open_numbers(Ctx* c, uint64_t item, uint8_t slot, uint8_t block) {
    static int told;
    if (g_api->version < 4 || g_api->size < sizeof(NxModApi) || !g_api->open_list) {
        if (!told) { told = 1; g_api->log(g_api, 1, "the numbers need the game's list screen (mod interface version 4): this build of the game does not have it"); }
        return 0;
    }
    g_num_slot = slot; g_num_block = block;
    g_num_list.size = sizeof g_num_list; g_num_list.pages = 1;
    g_num_list.titles[0] = block == NUM_SKILLS ? "Skill Exp Bonus" : block == JB_NBLOCK ? "Class Tier and Movement" : block == JB_B_STATS ? "Basic Stats" : block == JB_B_MOD ? (g_own[slot].no_mount ? "Dismounted Class Bonus" : "Class Bonus") : block == JB_B_CAP ? "Stat caps"
                           : block == JB_B_GROW ? "Growth Rate" : block == JB_B_GBONUS ? "Growth Rate Bonus" : block == JB_B_WRANK ? "Weapon Skill Lv" : block == JB_B_CCREQ ? "Exam: Primary and Secondary Skills"
                           : block == JB_B_CCSHARE ? "Exam: Either-or skills" : block == JB_B_CCFAIL ? "Exam: Pass Chance" : "Skill Lv";
    g_num_list.titles[1] = g_num_list.titles[2] = "";
    g_num_list.count = block == NUM_SKILLS ? 13 : block == JB_NBLOCK ? 2 : kJbBlock[block].count;
    g_num_list.user = 0; g_num_list.page = 0;
    g_num_list.label = numl_label; g_num_list.value = numl_value; g_num_list.help = numl_help;
    g_num_list.pick = numl_pick; g_num_list.back = 0; g_num_list.side = numl_side;
    if (!g_api->open_list(c, item, &g_num_list)) { g_api->log(g_api, 1, "the game's list screen could not be opened for the numbers"); return 0; }
    if (!told) { told = 1; TRACE("numbers: the game's list screen is open (left / right change a value, confirm adds five)"); }
    return 1;
}
/* the game's text for a message id; 0 when it has none */
static int game_text(Ctx* c, const char* mid, char* out, int max) {
    uint64_t shown;
    *(uint64_t*)g_name_item = (uint64_t)&g_vt_cls[2];
    g_mid_text = mid;
    shown = call(c, g_vt_cls[2 + SLOT_TEXT], (uint64_t)g_name_item, 0, 0, 0);
    g_mid_text = 0;
    return shown && text_at(shown, out, max) && out[0] && !same_text(out, mid);
}
static int person_name(Ctx* c, const char* aid, char* out, int max) {
    uint32_t k;
    for (k = 0; k < g_people_count; ++k)
        if (same_text(g_people[k].aid, aid)) return game_text(c, g_people[k].mid, out, max);
    return 0;
}

/* ---- outfit colors ----
 * An asset row has three outfit colors (+0x108, +0x10c, +0x110: red, green, blue and a fourth byte that is 255 where the color is
 * set and 0 where the row has none). They can be taken from a class or character like the other fields, or made here: from a
 * list of color families and shades (the unit on the class change screen wears the shade under the cursor), or exactly, by hue,
 * saturation, brightness or red, green, blue on the game's list screen. Made colors are saved as "#RRGGBB,#RRGGBB,-". */
#define N_FAMILY 10
static const char* const kFamily[N_FAMILY] = {"Red", "Orange", "Yellow", "Green", "Teal", "Blue", "Purple", "Pink", "Brown", "White, grey, black"};
static const uint16_t kFamilyHue[N_FAMILY] = {0, 28, 52, 120, 172, 216, 272, 322, 26, 0};
static const char* const kShade[12] = {"Pale", "Light", "Soft", "Vivid", "Muted light", "Medium", "Strong", "Dusty", "Deep", "Rich dark", "Dark muted", "Darkest"};
static const uint8_t kShadeSV[12][2] = {{25, 100}, {50, 100}, {75, 100}, {100, 100}, {45, 82}, {70, 82}, {100, 80}, {45, 58}, {80, 58}, {100, 52}, {55, 32}, {100, 28}};
static const uint8_t kBrownSV[12][2] = {{30, 82}, {40, 74}, {50, 68}, {58, 62}, {62, 55}, {66, 48}, {70, 42}, {72, 36}, {75, 30}, {78, 24}, {45, 45}, {35, 60}};
static const uint8_t kGreyV[12] = {100, 92, 84, 75, 66, 56, 46, 37, 28, 19, 10, 0};
static uint32_t hsv_rgb(uint32_t h, uint32_t sat, uint32_t val) {          /* hue 0..359, the others 0..100 -> 0xRRGGBB */
    const uint32_t V = (val * 255 + 50) / 100, f = h % 60;
    const uint32_t p = V * (100 - sat) / 100, q = V * (6000 - sat * f) / 6000, u = V * (6000 - sat * (60 - f)) / 6000;
    switch (h / 60 % 6) {
        case 0: return V << 16 | u << 8 | p;
        case 1: return q << 16 | V << 8 | p;
        case 2: return p << 16 | V << 8 | u;
        case 3: return p << 16 | q << 8 | V;
        case 4: return u << 16 | p << 8 | V;
        default: return V << 16 | p << 8 | q;
    }
}
static void rgb_hsv(uint32_t rgb, uint32_t* h, uint32_t* sat, uint32_t* val) {          /* the hue is left as it is for a grey */
    const int r = (int)(rgb >> 16 & 255), g = (int)(rgb >> 8 & 255), b = (int)(rgb & 255);
    const int mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b), d = mx - mn;
    int hh;
    *val = (uint32_t)(mx * 100 + 127) / 255;
    *sat = mx ? (uint32_t)(d * 100 + mx / 2) / (uint32_t)mx : 0;
    if (!d) return;
    if (mx == r) hh = 60 * (g - b) / d; else if (mx == g) hh = 120 + 60 * (b - r) / d; else hh = 240 + 60 * (r - g) / d;
    *h = (uint32_t)((hh % 360 + 360) % 360);
}
static uint32_t shade_rgb(uint32_t family, uint32_t shade) {
    if (family == N_FAMILY - 1) return hsv_rgb(0, 0, kGreyV[shade % 12]);
    if (family == 8) return hsv_rgb(kFamilyHue[8], kBrownSV[shade % 12][0], kBrownSV[shade % 12][1]);
    return hsv_rgb(kFamilyHue[family % N_FAMILY], kShadeSV[shade % 12][0], kShadeSV[shade % 12][1]);
}
static char* put_rgb(char* at, uint32_t rgb) {
    static const char hex[] = "0123456789ABCDEF";
    int k;
    *at++ = '#';
    for (k = 20; k >= 0; k -= 4) *at++ = hex[rgb >> k & 15];
    return at;
}
static uint64_t main_row(const char* key, int sex);
/* the class's three colors as they are now (its male main row, or its female one) */
static void cur_colors(uint8_t slot, uint8_t* out) {
    const uint64_t m = main_row(g_own[slot].key, 0), w = main_row(g_own[slot].key, 1), r = m ? m : w;
    uint32_t k;
    for (k = 0; k < 12; ++k) out[k] = r ? *(const uint8_t*)(r + 0x108 + k) : 0;
}
#define COL_NONE ((uint64_t)1 << 24)          /* in a shade line's tag: "no color" */
static struct { uint8_t slot, idx, dirty; uint8_t col[12]; uint32_t h, s, v; } g_col;          /* the color on the list screen */
static const char* field_text(Ctx* c, uint8_t slot, uint8_t field) {
    static char extra[MAX_SLOTS][4][160];
    char* start = field == FIELD_NAME ? extra[slot][0] : field == FIELD_LOWER ? extra[slot][2] : field == FIELD_UPPER ? extra[slot][3] : g_field_text[slot][field == FIELD_BASE ? JB_NFIELD : field];
    char* at = start;
    char name[64];
    if (field == FIELD_BASE) {
        const uint64_t base = find_job(g_own[slot].base);
        at = put_text(at, "Base Class: ");
        if (base && *(const uint8_t*)(base + 0x31) >= 1) { at = put_text(at, shown_name(c, base, name, sizeof name)); at = put_text(at, " ("); at = put_text(at, g_own[slot].base); *at++ = ')'; }
        else at = put_text(at, "none");
        if (g_fresh[slot]) at = put_text(at, " (restart for its look)");
    } else if (field == FIELD_NAME) {
        const uint64_t row = find_job(g_own[slot].key);
        at = put_text(at, "Name: ");
        if (row) at = put_text(at, shown_name(c, row, name, sizeof name));
        if (g_own[slot].name[0] && row && !same_text(name, g_own[slot].name)) { at = put_text(at, " -> "); at = put_text(at, g_own[slot].name); at = put_text(at, " after a restart"); }
        else if (!g_own[slot].name[0]) at = put_text(at, " (the base's)");
    } else if (field == FIELD_LOWER || field == FIELD_UPPER) {
        const uint64_t row = find_job(g_own[slot].key);
        const uint64_t to = row ? *(const uint64_t*)(row + (field == FIELD_UPPER ? 0x50 : 0x48)) : 0;
        at = put_text(at, field == FIELD_UPPER ? "Class Line, next: " : "Class Line, previous: ");
        at = put_text(at, to > 0x10000 && g_api->is_mapped(to, ROW_BYTES) && *(const uint8_t*)(to + 0x31) >= 1 ? shown_name(c, to, name, sizeof name) : "(none)");
    } else {
        const uint64_t m = main_row(g_own[slot].key, 0), w = main_row(g_own[slot].key, 1);
        at = put_text(at, kFieldLabel[field]); at = put_text(at, ": ");
        if (!m && !w) at = put_text(at, "(after a restart)");
        else if (m && w && value_hash(m, field) != value_hash(w, field)) { at = put_value(at, m, field); at = put_text(at, " / "); at = put_value(at, w, field); }
        else at = put_value(at, m ? m : w, field);
    }
    at = pad_line(start, at);
    *at = 0;
    return start;
}

static uint8_t g_top[MAX_SLOTS];          /* the classes the creator's first list shows: those made, then one empty placeholder ("New class") */
static uint32_t g_top_count;
static char g_item_text[PAGE][176];      /* the lines of the value list or range list that is open (one buffer per position) */

/* the class a body / animation number belongs to (CD_<number> is the body of one of its main rows); 0 when none */
static uint64_t class_of_number(uint32_t num) {
    static uint32_t gen = 0xffffffffu, count;
    static uint16_t nums[2 * MAX_JOBS];
    static uint64_t rows[2 * MAX_JOBS];
    uint32_t i;
    if (!find_asset_table()) return 0;
    if (gen != g_at_gen) {
        char key[JB_KEY_MAX];
        int s2;
        gen = g_at_gen; count = 0;
        if (!g_job_count) collect_jobs();
        for (i = 0; i < (uint32_t)g_job_count; ++i) {
            if (!row_key(g_jobs[i], key, sizeof key)) continue;
            for (s2 = 0; s2 < 2; ++s2) {
                const uint64_t r = main_row(key, s2);
                const char* b = r ? m_text(m64(r + 0x30)) : "";
                uint32_t v = 0, x;
                if (b[0] != 'C' || b[1] != 'D' || b[2] != '_') continue;
                for (x = 3; b[x] >= '0' && b[x] <= '9'; ++x) v = v * 10 + (uint32_t)(b[x] - '0');
                if (v && v < 65536 && count < 2 * MAX_JOBS) { nums[count] = (uint16_t)v; rows[count++] = g_jobs[i]; }
            }
        }
    }
    for (i = 0; i < count; ++i) if (nums[i] == num) return rows[i];
    return 0;
}
/* "AB_382MA_W#" -> "Swordmaster, male, on foot" */
static char* put_set(Ctx* c, char* at, const char* name) {
    static const char* const kType[9] = {", on foot", ", armored", ", on horse", ", on armored horse", ", on pegasus", ", on dragon", ", on flying mount", ", on chariot", ", on elephant"};
    uint32_t num = 0, x = 3;
    char pre = 0, sex, type, cls[64];
    uint64_t job;
    if (name[0] != 'A' || name[1] != 'B' || name[2] != '_') return put_text(at, name);
    while (name[x] >= '0' && name[x] <= '9') num = num * 10 + (uint32_t)(name[x++] - '0');
    if (name[x] >= 'a' && name[x] <= 'z') pre = name[x++];
    sex = name[x] ? name[x++] : 0; type = name[x] && name[x] != '_' ? name[x] : 0;
    job = num >= 300 && num < 500 ? class_of_number(num) : 0;
    if (num == 999) at = put_text(at, "Generic");
    else if (num == 998) at = put_text(at, "Generic (advanced classes)");
    else if (job) at = put_text(at, shown_name(c, job, cls, sizeof cls));
    else if (num < 200) { at = put_text(at, "Character set "); at = put_int(at, num); }
    else if (num < 300) { at = put_text(at, "Mount or animal "); at = put_int(at, num); }
    else if (num >= 500 && num < 900) { at = put_text(at, "Monster "); at = put_int(at, num); }
    else { at = put_text(at, "Set "); at = put_int(at, num); }
    at = put_text(at, sex == 'M' ? ", male" : sex == 'F' ? ", female" : "");
    if (type >= 'A' && type <= 'I') at = put_text(at, kType[type - 'A']);
    { uint32_t y; for (y = x; name[y]; ++y) if (name[y] == '_' && name[y + 1] == 'W' && name[y + 2] >= '0' && name[y + 2] <= '8') { at = put_text(at, ", "); at = put_text(at, kWKindLabel[name[y + 2] == '0' ? 8 : name[y + 2] - '1']); break; } }
    at = put_text(at, pre == 'b' ? " (underworld)" : pre == 'y' ? " (five years later)" : pre == 'w' ? " (cutscene)" : pre == 'x' ? " (variant)" : pre == 'z' ? " (on the ground)" : "");
    return at;
}
static int row_own_set(uint64_t row) {          /* has the row a set that is not the generic one? */
    const uint32_t cnt = m16(row + 0x80);
    const uint64_t list = m64(row + 0x88);
    uint32_t j;
    if (!cnt || cnt > 24 || !g_api->is_mapped(list, (uint64_t)cnt * 8)) return 0;
    for (j = 0; j < cnt; ++j) { const char* t = m_text(m64(list + (uint64_t)j * 8)); if (t[0] && t[1] && t[2] && t[3] && t[4] && t[5] && !aset_generic(t)) return 1; }
    return 0;
}
/* a list of set names (a row's, or a comma separated text): its own set in words, "+ generic" when it also has the generic one */
static char* put_sets(Ctx* c, char* at, uint64_t row, const char* text) {
    char names[8][24];
    uint32_t n = 0, j, x, own = 0xffffffffu, generic = 0;
    if (row) {
        const uint32_t cnt = m16(row + 0x80);
        const uint64_t list = m64(row + 0x88);
        if (cnt && cnt <= 24 && g_api->is_mapped(list, (uint64_t)cnt * 8))
            for (j = 0; j < cnt && n < 8; ++j) { const char* t = m_text(m64(list + (uint64_t)j * 8)); for (x = 0; t[x] && x < 23; ++x) names[n][x] = t[x]; names[n][x] = 0; if (x) n++; }
    } else if (text)
        while (*text && n < 8) { for (x = 0; *text && *text != ',' && x < 23; ++x) names[n][x] = *text++; names[n][x] = 0; while (*text && *text != ',') text++; if (*text == ',') text++; if (x) n++; }
    if (!n) return put_text(at, "-");
    for (j = 0; j < n; ++j) { if (aset_generic(names[j])) generic++; else own = j; }          /* the last set of its own: the most particular */
    if (own == 0xffffffffu) {          /* only generic ones: the one for every weapon kind, when there is one */
        uint32_t pick = 0, y;
        for (j = 0; j < n; ++j) for (y = 0; names[j][y]; ++y) if (names[j][y] == '#') pick = j;
        at = put_set(c, at, names[pick]);
        at = put_text(at, "  ["); { const char* nm = names[pick]; uint32_t us = 0; for (y = 0; nm[y]; ++y) { if (nm[y] == '_' && ++us == 2) break; *at++ = nm[y]; } }
        *at++ = ']';
        return at;
    }
    at = put_set(c, at, names[own]);
    if (own != 0xffffffffu && generic) at = put_text(at, " + generic");
    if (own != 0xffffffffu && n - generic > 1) { at = put_text(at, " +"); at = put_int(at, n - generic - 1); }
    at = put_text(at, "  [");
    { const char* nm = names[own == 0xffffffffu ? 0 : own]; uint32_t y = 0, us = 0; while (nm[y]) { if (nm[y] == '_' && ++us == 2) break; *at++ = nm[y++]; } }
    *at++ = ']';
    return at;
}

static void dyn_text(Ctx* c, void* user) {
    const Tag* t = tag_of(c->x[0]);
    (void)user;
    if (t && t->kind == TAG_KLINE && t->slot < g_own_count) {
        char* start = g_item_text[t->rank % PAGE];
        char* at = start;
        if (t->rank == 3) at = put_text(at, "From a class or character");
        else if (t->rank < 2) {
            const char* cur = t->rank ? g_cast[t->slot].effect : g_cast[t->slot].hand;
            uint32_t o;
            at = put_text(at, t->rank ? "Black magic effect*: " : "Held while casting*: ");
            for (o = 0; o < kCastOptN[t->rank] && !jb_same(cur, kCastOpt[t->rank][o]); ++o) {}
            at = put_text(at, o < kCastOptN[t->rank] ? kCastOptLabel[t->rank][o] : cur);
        } else { at = put_text(at, "Spells start at*: "); at = pick_name(c, at, PICK_CAST, g_cast_spells[t->slot] ? 1 : 0); }
        at = pad_line(start, at); *at = 0;
        c->x[0] = (uint64_t)start;
        return;
    }
    if (t && t->kind == TAG_KVAL) {
        char* start = g_item_text[t->rank % PAGE];
        char* at = put_text(start, kCastOptLabel[t->field & 1][t->row % kCastOptN[t->field & 1]]);
        if (t->row) { at = put_text(at, " ("); at = put_text(at, kCastOpt[t->field & 1][t->row % kCastOptN[t->field & 1]]); at = put_text(at, ")"); }
        *at = 0;
        c->x[0] = (uint64_t)start;
        return;
    }
    if (t && t->kind == TAG_FIELD && t->field != FIELD_SKILL && t->field != FIELD_ICON && t->slot < g_own_count) { c->x[0] = (uint64_t)field_text(c, t->slot, t->field); return; }
    if (t && t->kind == TAG_FIELD && t->field == FIELD_ICON && t->slot < g_own_count) {
        char* start = g_item_text[t->rank % PAGE];
        char name[64];
        const uint64_t from = find_job(g_own[t->slot].icon[0] ? g_own[t->slot].icon : g_own[t->slot].base);
        char* at = put_text(start, "Class Icon: ");
        at = put_text(at, from && *(const uint8_t*)(from + 0x31) >= 1 ? shown_name(c, from, name, sizeof name) : "-");
        if (!g_own[t->slot].icon[0]) at = put_text(at, " (the base's)");
        at = pad_line(start, at); *at = 0;
        c->x[0] = (uint64_t)start;
        return;
    }
    if (t && t->kind >= TAG_CTOP && t->kind <= TAG_CSHADE && t->slot < g_own_count) {
        char* start = g_item_text[t->rank % PAGE];
        char* at = start;
        const uint32_t idx = (uint32_t)(t->row >> 32) % 3;
        if (t->kind == TAG_CTOP) at = put_text(at, "From a class or character");
        else if (t->kind == TAG_CSLOT) {
            uint8_t col[12];
            cur_colors(t->slot, col);
            at = put_text(at, "Color "); at = put_int(at, idx + 1); at = put_text(at, ": ");
            if (col[idx * 4 + 3]) at = put_rgb(at, (uint32_t)col[idx * 4] << 16 | (uint32_t)col[idx * 4 + 1] << 8 | col[idx * 4 + 2]); else at = put_text(at, "(none)");
            at = pad_line(start, at);
        } else if (t->kind == TAG_CEXACT) at = put_text(at, "Exact color (hue, red, green, blue ...)");
        else if (t->kind == TAG_CHUE) at = put_text(at, kFamily[(uint32_t)t->row % N_FAMILY]);
        else if (t->row & COL_NONE) at = put_text(at, "(none)");
        else {
            at = put_text(at, (t->field == N_FAMILY - 1) ? ((t->rank % 12) == 0 ? "White" : (t->rank % 12) == 11 ? "Black" : "Grey") : kShade[t->rank % 12]);
            at = put_text(at, "  "); at = put_rgb(at, (uint32_t)t->row & 0xffffff);
        }
        *at = 0;
        c->x[0] = (uint64_t)start;
        return;
    }
    if (t && t->kind == TAG_FIELD && t->field == FIELD_SKILL && t->slot < g_own_count) {
        char* start = g_item_text[t->rank % PAGE];
        char name[64];
        char* at = put_text(start, "Master Ability: ");
        at = put_text(at, skill_name(c, master_sid(find_job(g_own[t->slot].key)), name, sizeof name));
        at = pad_line(start, at); *at = 0;
        c->x[0] = (uint64_t)start;
        return;
    }
    if (t && t->kind == TAG_SKILL) {          /* "Great Counter (Guardian)" */
        char* at = g_item_text[t->rank % PAGE];
        char name[64];
        if (t->row == (uint64_t)g_zero_row) at = put_text(at, "(none)");
        else {
            at = put_text(at, skill_name(c, master_sid(t->row), name, sizeof name));
            at = put_text(at, " ("); at = put_text(at, shown_name(c, t->row, name, sizeof name)); *at++ = ')';
        }
        *at = 0;
        c->x[0] = (uint64_t)g_item_text[t->rank % PAGE];
        return;
    }
    if (t && t->kind == TAG_TYPE) {
        c->x[0] = (uint64_t)(t->row < N_TYPESET ? kTypeSet[t->row] : t->row == N_TYPESET ? "Infantry, Cavalry, Heavy Armor, Flying Skill Lv"
                             : t->row == N_TYPESET + 2 ? "Gender, abilities, barred characters"
                             : t->slot >= g_own_count ? "?" : g_own[t->slot].no_mount ? "This is a dismounted class"
                             : g_own[t->slot].dismount[0] && !same_text(g_own[t->slot].dismount, "none") ? "Regenerate dismounted class (look after a restart)"
                             : row_mounted(find_job(g_own[t->slot].base)) ? "Generate dismounted class (after a restart)" : "Dismounted class: needs a mounted Base Class");
        return;
    }
    if (t && t->kind == TAG_BLOCK && t->field == JB_NBLOCK) {
        char* start = g_item_text[t->rank % PAGE];
        char* at = put_text(start, "Class Tier and Movement");
        at = pad_line(start, at); *at = 0;
        c->x[0] = (uint64_t)start;
        return;
    }
    if (t && (t->kind == TAG_BLOCK || t->kind == TAG_NUM) && t->slot < g_own_count && t->field < JB_NBLOCK) {
        const uint64_t row = find_job(g_own[t->slot].key);
        const uint32_t bl = t->field, x = (uint32_t)t->row;
        char* start = g_item_text[t->rank % PAGE];
        char* at = start;
        if (t->kind == TAG_BLOCK) {
            at = put_text(at, bl == JB_B_MOD && g_own[t->slot].no_mount ? "Dismounted Class Bonus" : kBlockLabel[bl]);
            if (bl == JB_B_UTYPE && row) {          /* the types the class is of */
                uint32_t k, any = 0;
                at = put_text(at, ": ");
                for (k = 0; k < *(const uint16_t*)(row + JB_ATTR) && k < 4; ++k) {
                    static const char* const kAttrWord[7] = {"Infantry", "Armored", "Cavalry", "Fliers", "Dragon", "Beast", "Fiend"};
                    static const char* const kAttrSid[7] = {"SID_\xe6\xad\xa9\xe5\x85\xb5", "SID_\xe9\x87\x8d\xe8\xa3\x85", "SID_\xe9\xa8\x8e\xe5\x85\xb5", "SID_\xe9\xa3\x9b\xe8\xa1\x8c", "SID_\xe7\xab\x9c", "SID_\xe7\x8d\xa3", "SID_\xe5\x86\xa5\xe9\xad\x94"};
                    const char* sid = attr_sid(row, k);
                    uint32_t w;
                    for (w = 0; w < 7 && !same_text(sid, kAttrSid[w]); ++w) {}
                    if (any++) at = put_text(at, ", ");
                    at = put_text(at, w < 7 ? kAttrWord[w] : "?");
                }
                if (!any) at = put_text(at, "(none)");
            }
            if (kJbBlock[bl].count == 1 && row) { at = put_text(at, ": "); at = put_num(at, num_get(row, bl, 0)); }
            if (bl == JB_B_CCSHARE && row) {
                uint32_t k, any = 0;
                at = put_text(at, ": ");
                for (k = 0; k < 3; ++k) { const int v = num_get(row, bl, k); if (v == 15) continue; if (any++) at = put_text(at, " or "); at = put_skill(at, v); }
                if (!any) at = put_text(at, "(none)");
            }
        } else {
            const int v = row ? num_get(row, bl, x) : 0;
            at = put_text(at, num_label(bl, x));
            at = put_text(at, ": ");
            if (v < 0 && (bl == JB_B_WRANK || bl == JB_B_UTYPE)) at = put_text(at, "-"); else at = put_num(at, v);
        }
        at = pad_line(start, at);
        *at = 0;
        c->x[0] = (uint64_t)start;
        return;
    }
    if (t && t->kind == TAG_CAT) {
        const uint32_t first = (uint32_t)t->row, count = (uint32_t)(t->row >> 32);
        const uint8_t kind = first < g_value_count ? g_val_kind[first] : K_OTHER;
        char* at = put_text(g_item_text[t->rank % PAGE], kind == K_CLASS ? "From a class (" : kind == K_CHAR ? "From a character (" : "Other (");
        at = put_int(at, count); *at++ = ')'; *at = 0;
        c->x[0] = (uint64_t)g_item_text[t->rank % PAGE];
        return;
    }
    if (t && t->kind == TAG_POINT) {          /* "Head: AC_382a" */
        char* at = put_text(g_item_text[t->rank % PAGE], point_label(t->row < g_point_count ? g_points[t->row] : "?"));
        const uint64_t m = main_row(g_own[t->slot].key, 0), w = main_row(g_own[t->slot].key, 1), r = m ? m : w;
        const char* cur = "-";
        char pt[40];
        if (r && m16(r + 0x98) <= 32 && g_api->is_mapped(m64(r + 0xa0), (uint64_t)m16(r + 0x98) * 8)) {
            uint32_t j2;
            for (j2 = 0; j2 < m16(r + 0x98); ++j2) {
                const char* e = m_text(m64(m64(r + 0xa0) + (uint64_t)j2 * 8));
                if (t->row < g_point_count && point_of(e, pt, sizeof pt) && same_text(pt, g_points[t->row])) cur = e;
            }
        }
        at = put_text(at, ": "); { int len = 0; while (cur[len] && cur[len] != '(' && len < 48) *at++ = cur[len++]; }
        at = pad_line(g_item_text[t->rank % PAGE], at);
        *at = 0;
        c->x[0] = (uint64_t)g_item_text[t->rank % PAGE];
        return;
    }
    if (t && t->kind == TAG_RANGE) {          /* "Gladiator - Rogue", or "25 - 36: CD_320# ..." */
        const uint32_t first = (uint32_t)t->row, count = (uint32_t)(t->row >> 32);
        char* at = g_item_text[t->rank % PAGE];
        char name[64];
        if (first < g_value_count && g_val_kind[first] == K_CLASS && first + count <= g_value_count && count) {
            at = put_text(at, shown_name(c, g_val_who[first], name, sizeof name)); at = put_text(at, " - ");
            at = put_text(at, shown_name(c, g_val_who[first + count - 1], name, sizeof name));
        } else {
            at = put_int(at, first + 1); at = put_text(at, " - "); at = put_int(at, first + count);
            if (first < g_value_count) {
                at = put_text(at, ": ");
                if (g_leaf == TAG_SKILL) at = put_text(at, g_val_row[first] == (uint64_t)g_zero_row ? "(none)" : skill_name(c, master_sid(g_val_row[first]), name, sizeof name));
                else if (g_leaf == TAG_AVALUE) { const char* e = g_val_row[first] == (uint64_t)g_zero_row ? "(none)" : m_text(g_val_row[first]); int len = 0; while (e[len] && e[len] != '(' && len < 40) *at++ = e[len++]; }
                else at = put_value(at, g_val_row[first], t->field);
                at = put_text(at, " ...");
            }
        }
        *at = 0;
        c->x[0] = (uint64_t)g_item_text[t->rank % PAGE];
        return;
    }
    if (t && t->kind == TAG_WSRC) {
        const uint32_t src = (uint32_t)(t->row & 0xff) % N_WKIND, k = (uint32_t)(t->row >> 8 & 0xff) % N_WKIND;
        char* at = put_text(g_item_text[t->rank % PAGE], kWKindLabel[src]);
        at = put_text(at, src == k ? " animations (this weapon)" : " animations");
        *at = 0;
        c->x[0] = (uint64_t)g_item_text[t->rank % PAGE];
        return;
    }
    if (t && (t->kind == TAG_WTOP || t->kind == TAG_WSLOT) && t->slot < g_own_count) {
        char* start = g_item_text[t->rank % PAGE];
        char* at = start;
        if (t->kind == TAG_WTOP) {
            const uint64_t m = main_row(g_own[t->slot].key, 0), w = main_row(g_own[t->slot].key, 1);
            at = put_text(at, "Base: "); at = m || w ? put_sets(c, at, m ? m : w, 0) : put_text(at, "-");
        } else {
            const uint32_t k = (uint32_t)t->row % N_WKIND;
            const char* v = g_wanim[t->slot][0][k][0] ? g_wanim[t->slot][0][k] : g_wanim[t->slot][1][k];
            uint32_t len = 0, more = 0;
            at = put_text(at, kWKindLabel[k]); at = put_text(at, ": ");
            (void)len; (void)more;
            if (!v[0]) at = put_text(at, "(base)"); else at = put_sets(c, at, 0, v);
        }
        at = pad_line(start, at); *at = 0;
        c->x[0] = (uint64_t)start;
        return;
    }
    if (t && (t->kind == TAG_VALUE || t->kind == TAG_AVALUE || t->kind == TAG_WVALUE)) {          /* whose it is, then the internal name */
        char* at = g_item_text[t->rank % PAGE];
        char name[64];
        uint32_t k;
        for (k = 0; k < g_value_count && g_val_row[k] != t->row; ++k) {}
        if (t->row == (uint64_t)g_zero_row) at = put_text(at, t->kind == TAG_WVALUE ? "(as the base)" : t->kind == TAG_AVALUE ? "(none here)" : t->field == JB_ACCESSORY ? "(none: remove all)" : "(none)");
        else {
            if (k < g_value_count && g_val_kind[k] == K_CLASS && g_val_who[k]) {
                at = put_text(at, shown_name(c, g_val_who[k], name, sizeof name));
                if (t->kind == TAG_AVALUE) {
                    const uint8_t mk = g_val_main[k];
                    at = put_text(at, (mk & 3) == 1 ? " (F" : (mk & 3) == 0 ? " (M" : " (");
                    at = put_text(at, (mk & 4) ? ((mk & 3) == 2 ? "generic)" : ", generic)") : ")");
                    if (at[-2] == '(') at -= 3;
                }
                at = put_text(at, ": ");
            } else if (k < g_value_count && g_val_kind[k] == K_CHAR && g_val_who[k] && person_name(c, (const char*)g_val_who[k], name, sizeof name)) {
                at = put_text(at, name); at = put_text(at, ": ");
            }
            if (t->kind == TAG_AVALUE) { const char* e = m_text(t->row); int len = 0; while (e[len] && e[len] != '(' && len < 48) *at++ = e[len++]; }
            else if (t->field == JB_ANIM) {          /* animations in words; a class's own line needs no class named twice */
                if (k < g_value_count && g_val_kind[k] == K_CLASS && g_val_who[k] && row_own_set(t->row)) at = g_item_text[t->rank % PAGE];          /* its own set names the class already */
                at = put_sets(c, at, t->row, 0);
            }
            else at = put_value(at, t->row, t->field);
        }
        *at = 0;
        c->x[0] = (uint64_t)g_item_text[t->rank % PAGE];
        return;
    }
    if (t && t->kind == TAG_ABIL && t->slot < g_own_count) {
        char* start = g_item_text[t->rank % PAGE];
        char* at = start;
        const uint32_t what = t->field;
        if (what <= PICK_BAR) {
            const uint32_t cur = pick_cur(t->slot, what);
            if (what == PICK_SEX) at = put_text(at, "Gender: ");
            else if (what == PICK_BAR) at = put_text(at, "Barred characters: ");
            else { at = put_text(at, "Class Ability "); at = put_int(at, what); at = put_text(at, ": "); }
            if (cur == 0xffff) at = put_text(at, what == PICK_BAR ? "the base's" : g_own[t->slot].abil[what - 1]);
            else at = pick_name(c, at, what, cur);
        } else at = put_text(at, "Abilities take effect after a restart");
        at = pad_line(start, at); *at = 0;
        c->x[0] = (uint64_t)start;
        return;
    }
    if (t && t->kind == TAG_PICK && t->slot < g_own_count) {          /* a choice; the one in use is marked */
        char* start = g_item_text[t->rank % PAGE];
        char* at = pick_name(c, start, t->field, (uint32_t)t->row);
        if (pick_cur(t->slot, t->field) == (uint32_t)t->row) at = put_text(at, "  (current)");
        at = pad_line(start, at); *at = 0;
        c->x[0] = (uint64_t)start;
        return;
    }
    if (t && t->kind == TAG_PPAGE) {          /* a page of a long list: its first and last choice */
        static char page_text[16][150];
        char* start = page_text[t->rank % 16];
        const uint32_t first = (uint32_t)t->row, count = pick_count(t->field);
        char* at = pick_name(c, start, t->field, first);
        at = put_text(at, "  ...  ");
        at = pick_name(c, at, t->field, first + PAGE <= count ? first + PAGE - 1 : count - 1);
        *at = 0;
        c->x[0] = (uint64_t)start;
        return;
    }
    if (t && t->kind == TAG_GROUP) { c->x[0] = (uint64_t)kGroup[t->row % 8]; return; }
    if (t && t->kind == TAG_REMOVE) { c->x[0] = (uint64_t)"Remove this class"; return; }
    if (t && t->kind == TAG_CLASS) { c->x[0] = (uint64_t)"(none)"; return; }          /* the only class line drawn here: no class */
    if (t && t->kind == TAG_SPAGE) {
        static char sp_text[8][40];
        char* start = sp_text[t->rank % 8];
        char* at = put_text(start, "Classes ");
        at = put_int(at, t->row + 1); at = put_text(at, " - "); at = put_int(at, t->row + PAGE <= g_top_count ? t->row + PAGE : g_top_count); *at = 0;
        c->x[0] = (uint64_t)start;
        return;
    }
    if (t && t->kind == TAG_NAME && t->slot < g_own_count) {
        char* start;
        char* at;
        uint32_t k;
        if (!t->rank) { c->x[0] = (uint64_t)field_text(c, t->slot, FIELD_NAME); return; }
        start = g_item_text[1]; at = put_text(start, "Description: ");
        if (!g_own[t->slot].help[0]) at = put_text(at, g_own[t->slot].name[0] ? "(none)" : "(the base's)");
        for (k = 0; g_own[t->slot].help[k] && k < 110; ++k) *at++ = g_own[t->slot].help[k] == '\n' ? ' ' : g_own[t->slot].help[k];
        while (k && k < 113 && (g_own[t->slot].help[k] & 0xc0) == 0x80) *at++ = g_own[t->slot].help[k++];          /* not in the middle of a character */
        if (g_own[t->slot].help[k]) at = put_text(at, "...");
        at = pad_line(start, at); *at = 0;
        c->x[0] = (uint64_t)start;
        return;
    }
    if (t && t->kind == TAG_BCAT) {          /* "Cavalry (mounted): 17 classes" */
        static char cat_text[N_BCAT][48];
        const uint32_t cat = (uint32_t)(t->row % N_BCAT);
        char* at = put_text(cat_text[cat], kBcat[cat]);
        int k, count = 0;
        for (k = 0; k < g_job_count; ++k) count += *(const uint8_t*)(g_jobs[k] + 0x31) >= 1 && offered(g_jobs[k], *(const uint8_t*)(g_jobs[k] + 0x31), 0, 0) && class_cat(g_jobs[k]) == cat;
        at = put_text(at, ": "); at = put_int(at, (uint64_t)count); at = put_text(at, " classes"); *at = 0;
        c->x[0] = (uint64_t)cat_text[cat];
        return;
    }
    if (t && t->kind == TAG_RANK) {
        const uint32_t page = (uint32_t)(t->row & 0xff), pages = (uint32_t)(t->row >> 8 & 0xff);
        char* at = put_text(g_rank_text[(t->rank - 1) % 5][page % MAX_PAGES], "Class Tier ");
        at = put_int(at, t->rank);
        if (pages > 1) { at = put_text(at, " ("); at = put_int(at, page + 1); at = put_text(at, " of "); at = put_int(at, pages); *at++ = ')'; }
        *at = 0;
        c->x[0] = (uint64_t)g_rank_text[(t->rank - 1) % 5][page % MAX_PAGES];
        return;
    }
    if (t && t->kind == TAG_SLOT && t->slot < g_own_count) {
        char name[64];
        char* at = g_slot_text[t->slot];
        const uint64_t row = find_job(g_own[t->slot].key), base = find_job(g_own[t->slot].base);
        if (slot_dead(t->slot)) at = put_text(at, "Removed (gone after a restart)");
        else if (!row) { at = put_text(at, g_own[t->slot].key); at = put_text(at, ": not loaded (restart the game)"); }
        else if (g_own[t->slot].no_mount) at = put_text(at, "Dismounted class: settings");
        else if (slot_empty(t->slot)) at = put_text(at, "New class");
        else {
            at = put_text(at, shown_name(c, row, name, sizeof name));
            if (base && *(const uint8_t*)(base + 0x31) >= 1) { at = put_text(at, " (copy of "); at = put_text(at, shown_name(c, base, name, sizeof name)); *at++ = ')'; }
        }
        at = pad_line(g_slot_text[t->slot], at);
        *at = 0;
        c->x[0] = (uint64_t)g_slot_text[t->slot];
        return;
    }
    c->x[0] = (uint64_t)"?";
}

static void cls_mid(Ctx* c, void* user) {      /* GetMid of a class item: the class's name id, which the game's GetText looks up */
    const Tag* t = tag_of(c->x[0]);
    uint64_t q = 0;
    (void)user;
    if (g_mid_text) { c->x[0] = (uint64_t)g_mid_text; return; }
    c->x[0] = t && t->kind == TAG_CLASS && rd64(t->row + 0x20, &q) ? q : 0;
}

/* a stretch of the value list: its values when they fit one list, else smaller stretches */
static int open_values(Ctx* c, uint64_t item, uint8_t slot, uint8_t field, uint32_t first, uint32_t count) {
    const uint64_t sub = open_sub(c, item);
    uint32_t i, step = PAGE;
    if (!sub) return 0;
    if (count <= PAGE) {
        for (i = 0; i < count; ++i) {
            const uint64_t it = new_item(c, sub, g_vt_dyn);
            if (it) tag_set(it, g_leaf, slot, (uint8_t)i, field, g_val_row[first + i]);
        }
        return 1;
    }
    while ((uint64_t)step * PAGE < count) step *= PAGE;
    for (i = 0; i * step < count; ++i) {
        const uint32_t n = count - i * step < step ? count - i * step : step;
        const uint64_t it = new_item(c, sub, g_vt_dyn);
        if (it) tag_set(it, TAG_RANGE, slot, (uint8_t)i, field, (uint64_t)(first + i * step) | (uint64_t)n << 32);
    }
    return 1;
}

/* ---- preview on the class change screen ----
 * While the game's class change screen (facility::ProcClassExaminerScene, instance at [main + 0x2289410], 0 when it is not open)
 * shows its list of classes, the unit on screen wears the class under the cursor: the list item's OnSelect (0x16e7d30) writes the
 * class row's index and rank into the screen's shared settings ([main + 0x2234120] +0x14 and +0x10) and sets the scene's byte
 * +0xf1, and the scene builds the look again on its next tick (0x149d700). The same writes are made here, so the unit wears a
 * created class, and wears it again after a look field of it was changed. What the game's list had chosen is put back when the
 * creator is left. */
#define SCENE_VAR 0x2289410ull
#define PARAM_VAR 0x2234120ull
static uint64_t g_prev_scene;
static uint32_t g_prev_idx, g_prev_rank, g_prev_logged;
static int scene_objs(uint64_t* scene, uint64_t* par) {
    uint64_t sc = 0, pa = 0;
    if (!rd64(g_base + SCENE_VAR, &sc) || sc < 0x10000 || !g_api->is_mapped(sc, 0x200)) { g_prev_scene = 0; return 0; }
    if (!rd64(g_base + PARAM_VAR, &pa) || pa < 0x10000 || !g_api->is_mapped(pa, 0x48)) { g_prev_scene = 0; return 0; }
    *scene = sc; *par = pa;
    return 1;
}
/* What the game's list item does besides the two writes (0x16e7dd0): it hands the scene's waiting object (scene +0xf8) a small
 * function object, std::function<void()> made of {table 0x207f0f8, the scene} with its own address at +0x20, through
 * 0x19878e0(object, 0, &function). Setting the byte alone did not make the scene build the look again in the user's game, so
 * this is done as well. */
#define SCENE_WAIT 0x19878e0ull
#define SCENE_WAIT_FN 0x207f0f8ull
static void scene_kick(Ctx* c, uint64_t sc) {
    static uint64_t fn[5];
    static int told;
    uint64_t obj = 0, destroy = 0;
    *(volatile uint8_t*)(sc + 0xf1) = 1;
    if (!c || !rd64(sc + 0xf8, &obj) || obj < 0x10000 || !g_api->is_function(g_base + SCENE_WAIT)) {
        if (!told) { told = 1; g_api->log(g_api, 1, "class change screen: the scene has no waiting object: the look is asked for by the flag alone"); }
        return;
    }
    fn[0] = g_base + SCENE_WAIT_FN; fn[1] = sc; fn[2] = 0; fn[3] = 0; fn[4] = (uint64_t)fn;
    call(c, g_base + SCENE_WAIT, obj, 0, (uint64_t)fn, 0);
    if (rd64(g_base + SCENE_WAIT_FN + 0x20, &destroy) && g_api->is_function(destroy)) call(c, destroy, (uint64_t)fn, 0, 0, 0);
    if (!told) { told = 1; TRACE("class change screen: the look is asked for as the game's list does it (flag and the scene's waiting object)"); }
}
/* In the user's game a request for the class that is already on show changed nothing (the look followed only after the creator
 * was left). So when the class asked for is the one on show, the screen is first asked for no class (index -1, what the game's
 * list writes on an empty line: the unit in its own class) and for the class again once that request was taken (scene_frame).
 * The first requests are followed in the log for a second: the scene's flags, its step (the label the tick checks: 2, 4 or 9)
 * and the waiting object's busy byte. */
static struct { int stage; uint32_t idx, rank, frames; uint64_t sc; } g_bounce;
static uint32_t g_diag_frame, g_diag_n;
static uint32_t scene_label(uint64_t sc) {
    uint64_t b = 0, t = 0;
    uint32_t n = 0;
    if (!rd64(sc + 8, &b) || !rd64(sc + 0x10, &t) || b < 0x10000) return 0xffff;
    for (; t >= b && n < 32; t -= 0x40, ++n)
        if (g_api->is_mapped(t, 0x40) && *(const uint8_t*)t == 2) return *(const uint32_t*)(t + 0x20);
    return 0xfffe;
}
static void scene_note(const char* what, uint64_t sc, uint64_t pa) {
    char line[220];
    uint64_t obj = 0;
    char* at = put_text(line, "class change screen: "); at = put_text(at, what);
    at = put_text(at, ": class index "); at = put_int(at, *(const uint32_t*)(pa + 0x14)); at = put_text(at, ", flags "); at = put_int(at, *(const uint8_t*)(sc + 0xf1));
    *at++ = '/'; at = put_int(at, *(const uint8_t*)(sc + 0xf2)); at = put_text(at, ", step "); at = put_int(at, scene_label(sc));
    if (rd64(sc + 0xf8, &obj) && obj > 0x10000 && g_api->is_mapped(obj, 0x90)) { at = put_text(at, ", waiting object busy "); at = put_int(at, *(const uint8_t*)(obj + 0x88)); }
    *at = 0;
    TRACE(line);
}
static void preview_row(Ctx* c, uint64_t job_row) {
    uint64_t sc = 0, pa = 0;
    char line[200], key[JB_KEY_MAX];
    char* at;
    if (!job_row || !scene_objs(&sc, &pa)) return;
    if (g_prev_scene != sc) { g_prev_scene = sc; g_prev_idx = *(const uint32_t*)(pa + 0x14); g_prev_rank = *(const uint32_t*)(pa + 0x10); }
    g_bounce.stage = 0;
    if (*(const uint32_t*)(pa + 0x14) == *(const uint16_t*)(job_row + 0x10)) {
        g_bounce.stage = 1; g_bounce.idx = *(const uint16_t*)(job_row + 0x10); g_bounce.rank = *(const uint8_t*)(job_row + 0x31); g_bounce.frames = 0; g_bounce.sc = sc;
        *(volatile uint32_t*)(pa + 0x14) = 0xffffffffu;
    } else {
        *(volatile uint32_t*)(pa + 0x14) = *(const uint16_t*)(job_row + 0x10);
        *(volatile uint32_t*)(pa + 0x10) = *(const uint8_t*)(job_row + 0x31);
    }
    scene_kick(c, sc);
    if (g_diag_n < 6 && !g_diag_frame) { g_diag_n++; g_diag_frame = 1; scene_note(g_bounce.stage ? "asked (no class first)" : "asked", sc, pa); }
    if (g_prev_logged < 12) {
        g_prev_logged++;
        key[0] = 0; row_key(job_row, key, sizeof key);
        at = put_text(line, "class change screen: preview of "); at = put_text(at, key); at = put_text(at, " asked (scene at "); at = put_hex(at, sc);
        at = put_text(at, ", class index "); at = put_int(at, *(const uint16_t*)(job_row + 0x10)); at = put_text(at, ", the list had ");
        at = put_int(at, g_prev_idx); *at++ = ')'; *at = 0;
        TRACE(line);
    }
}
static void preview_end(Ctx* c) {
    uint64_t sc = 0, pa = 0;
    const uint64_t was = g_prev_scene;
    g_bounce.stage = 0;
    if (!was || !scene_objs(&sc, &pa) || sc != was) { g_prev_scene = 0; return; }
    g_prev_scene = 0;
    *(volatile uint32_t*)(pa + 0x14) = g_prev_idx;
    *(volatile uint32_t*)(pa + 0x10) = g_prev_rank;
    scene_kick(c, sc);
}

/* a value under the cursor is put into the class's rows for as long as the cursor is on it: what the rows had is kept and put
 * back when the cursor moves on or the list is left; choosing the value keeps it */
/* ---- animations for one weapon kind ----
 * Chosen like the class's animations (a class, a character or another row of the table), but for one of the nine weapon kinds:
 * the names of that row's list are kept for the kind (for male and female rows separately when a whole class is taken) and
 * anim_sync puts them into that kind's ready-made list instead of the class's own. The asset table's file has no place for this
 * (a row has one list for all kinds), so it lives in asset.txt as "anim.<kind> = names" and is put in place while the game runs. */
static int g_app_redo;          /* a new base class was copied in: its appearance rows are seen to again */
static uint8_t g_wsel;          /* the weapon kind whose list of values is open */
static uint8_t g_wsrc;          /* the weapon kind whose animations are offered for it (the same, or another's) */
static struct { int on; uint8_t slot, k; char bak[2][JB_SRC_MAX]; } g_wpend;
static uint32_t wanim_resync(uint8_t slot) {
    uint32_t i, done = 0;
    uint64_t d = 0;
    if (!find_asset_table()) return 0;
    for (i = 0; i < g_at_count; ++i) {
        const uint64_t r = at_row(i);
        if (!m16(r + 0x20) || !m16(r + 0x80) || !m_own(r, g_own[slot].key)) continue;
        if (!rd64(r + 0x18, &d) || d < 0x10000 || !g_api->is_mapped(d, 0x150)) continue;
        anim_sync(r);
        done++;
    }
    return done;
}
static void wanim_names(uint64_t r, char* out, char digit) {          /* the row's animation names for one weapon kind, comma separated */
    const uint32_t n = r ? m16(r + 0x80) : 0;
    const uint64_t list = r ? m64(r + 0x88) : 0;
    uint32_t j, used = 0;
    out[0] = 0;
    if (!n || n > 24 || !g_api->is_mapped(list, (uint64_t)n * 8)) return;
    for (j = 0; j < n; ++j) {
        const char* t = m_text(m64(list + (uint64_t)j * 8));
        uint32_t len = 0;
        while (t[len]) len++;
        if (!len || used + len + 2 >= JB_SRC_MAX) continue;
        if (used) out[used++] = ',';
        for (len = 0; t[len]; ++len) out[used++] = t[len] == '#' ? digit : t[len];
        out[used] = 0;
    }
}
static void wanim_set(uint8_t slot, uint32_t k, uint64_t from) {
    const char* cls = from == (uint64_t)g_zero_row ? "" : row_class(from);
    uint64_t ca = 0;
    uint32_t cn = 0;
    int s2, sex = 2;
    if (k >= N_WKIND) return;
    if (from == (uint64_t)g_zero_row) { g_wanim[slot][0][k][0] = g_wanim[slot][1][k][0] = 0; return; }
    if (m_group(from, 0, &ca, &cn)) sex = m_sex(ca, cn);
    for (s2 = 0; s2 < 2; ++s2) {
        uint64_t r = from;
        if (cls[0]) { const uint64_t mr = main_row(cls, s2); if (mr && m16(mr + 0x80)) r = mr; }
        else if (sex != 2 && sex != s2) continue;
        wanim_names(r, g_wanim[slot][s2][k], (char)(g_wsrc < 8 ? '1' + g_wsrc : '0'));
    }
}
static void wpend_drop(int restore) {
    int s2, x;
    if (!g_wpend.on) return;
    g_wpend.on = 0;
    if (!restore) return;
    for (s2 = 0; s2 < 2; ++s2) { for (x = 0; g_wpend.bak[s2][x]; ++x) g_wanim[g_wpend.slot][s2][g_wpend.k][x] = g_wpend.bak[s2][x]; g_wanim[g_wpend.slot][s2][g_wpend.k][x] = 0; }
    wanim_resync(g_wpend.slot);
}
static void wanim_try(uint8_t slot, uint32_t k, uint64_t from) {          /* under the cursor: kept until the cursor moves on */
    int s2, x;
    if (k >= N_WKIND) return;
    g_wpend.slot = slot; g_wpend.k = (uint8_t)k;
    for (s2 = 0; s2 < 2; ++s2) { for (x = 0; g_wanim[slot][s2][k][x]; ++x) g_wpend.bak[s2][x] = g_wanim[slot][s2][k][x]; g_wpend.bak[s2][x] = 0; }
    g_wpend.on = 1;
    wanim_set(slot, k, from);
    wanim_resync(slot);
}
static void wanim_take(uint8_t slot, uint32_t k, uint64_t from) {
    char line[260];
    char* at;
    uint32_t rows;
    if (k >= N_WKIND) return;
    wanim_set(slot, k, from);
    rows = wanim_resync(slot);
    save_looks();
    at = put_text(line, "class "); at = put_text(at, g_own[slot].key); at = put_text(at, ": animations for "); at = put_text(at, kWKindName[k]); at = put_text(at, " = ");
    at = put_text(at, g_wanim[slot][0][k][0] ? g_wanim[slot][0][k] : g_wanim[slot][1][k][0] ? g_wanim[slot][1][k] : "(the class's own)");
    at = put_text(at, ", "); at = put_int(at, rows); at = put_text(at, " rows"); *at = 0;
    TRACE(line);
}
/* after a start (or when the table was loaded anew): the saved choices are put in place once the rows are there */
static void wanim_startup(void) {
    static uint32_t done_gen = 0xffffffffu;
    uint32_t s, k, any = 0, rows = 0;
    if (!find_asset_table() || g_at_gen == done_gen) return;
    for (s = 0; s < g_own_count; ++s) {
        uint32_t has = 0;
        for (k = 0; k < N_WKIND; ++k) has |= (uint32_t)(g_wanim[s][0][k][0] | g_wanim[s][1][k][0]);
        if (!has) continue;
        any = 1; rows += wanim_resync((uint8_t)s);
    }
    if (!any || rows) done_gen = g_at_gen;
    if (rows) { char line[120]; char* at = put_text(line, "animations for single weapon kinds put in place: "); at = put_int(at, rows); at = put_text(at, " rows"); *at = 0; TRACE(line); }
}

#define MAX_TARGETS 128
static struct { int on; uint8_t field; uint32_t gen, n; uint64_t to[MAX_TARGETS]; } g_pend;
static uint8_t g_pend_bak[MAX_TARGETS][AROW];
static void pend_drop(int restore) {
    uint32_t i;
    wpend_drop(restore);
    if (!g_pend.on) return;
    g_pend.on = 0;
    if (!restore || !find_asset_table() || g_pend.gen != g_at_gen) return;
    for (i = 0; i < g_pend.n; ++i) copy_field(g_pend.to[i], (uint64_t)g_pend_bak[i], g_pend.field);
}

/* the class a row is one of the own rows of (every group names it), "" when it is not such a row */
static const char* row_class(uint64_t r) {
    uint64_t ca = 0;
    uint32_t cn = 0, j;
    if (r == (uint64_t)g_zero_row || !m16(r + 0x20) || !m_group(r, 0, &ca, &cn)) return "";
    for (j = 0; j < cn; ++j) {
        const char* t = m_text(m64(ca + (uint64_t)j * 8));
        if (jb_is_class(t)) return m_own(r, t) ? t : "";
    }
    return "";
}
/* Which of the class's rows a chosen value goes to, and from which row each takes it.
 *  - Every field but the body model: the class's main rows (the class alone, or the class and a sex), of the value's sex.
 *  - The body model: every own row of the class that has a body model. Besides the main rows a class has rows for its generic
 *    soldiers and often for named characters (other body builds), which the main rows do not reach.
 *    When the value comes from a class's own row, the whole class is taken: each row gets the body of that class's row with the
 *    same other conditions (male for male, generic for generic, ...), or of its main row of the same sex when it has no such
 *    row. Otherwise each row of the value's sex gets the value itself.
 * record = keep what the rows had, to take the value back (a value under the cursor). Returns the rows changed. */
static uint64_t g_src_rows[MAX_TARGETS];
static uint32_t apply_value(uint8_t slot, uint8_t field, uint64_t from, int record) {
    const char* key = g_own[slot].key;
    char cls[JB_KEY_MAX];
    uint32_t i, k, j, n_src = 0, done = 0, cn = 0, cn2 = 0;
    uint64_t ca = 0, ca2 = 0;
    int sex = 2;
    if (!find_asset_table()) return 0;
    if (from != (uint64_t)g_zero_row && m_group(from, 0, &ca, &cn)) sex = m_sex(ca, cn);
    if (record) { g_pend.on = 0; g_pend.field = field; g_pend.gen = g_at_gen; g_pend.n = 0; }
    { const char* c0 = row_class(from); char* e = put_text(cls, c0); *e = 0; }
    if (same_text(cls, key)) cls[0] = 0;
    if (cls[0])
        for (i = 0; i < g_at_count && n_src < MAX_TARGETS; ++i) { const uint64_t r = at_row(i); if (m16(r + 0x20) && m_own(r, cls) && (field != JB_BODY || str_set(r, 0x30))) g_src_rows[n_src++] = r; }
    for (i = 0; i < g_at_count; ++i) {
        const uint64_t to = at_row(i);
        uint64_t src = from;
        int ts;
        if (!m16(to + 0x20) || !m_group(to, key, &ca, &cn) || !m_own(to, key)) continue;
        ts = m_sex(ca, cn);
        if (field != JB_BODY ? !(cn == 1 || (cn == 2 && ts != 2)) : !str_set(to, 0x30)) continue;
        if (cls[0]) {
            uint64_t same = 0, main_of_sex = 0;
            for (k = 0; k < n_src && !same; ++k) {
                const uint64_t r2 = g_src_rows[k];
                if (!m_group(r2, cls, &ca2, &cn2)) continue;
                if ((cn2 == 1 || (cn2 == 2 && m_sex(ca2, cn2) != 2)) && (m_sex(ca2, cn2) == ts || m_sex(ca2, cn2) == 2 || ts == 2) && !main_of_sex) main_of_sex = r2;
                if (cn2 != cn) continue;
                for (j = 0; j < cn; ++j) { const char* t = m_text(m64(ca + (uint64_t)j * 8)); if (!same_text(t, key) && !m_has(ca2, cn2, t)) break; }
                if (j == cn) same = r2;
            }
            src = same ? same : main_of_sex;
            if (!src) continue;
        } else if (!(sex == 2 || ts == 2 || sex == ts)) continue;
        if (src == to) continue;
        if (record && g_pend.n < MAX_TARGETS) {
            for (j = 0; j < AROW; ++j) g_pend_bak[g_pend.n][j] = ((const uint8_t*)to)[j];
            g_pend.to[g_pend.n++] = to; g_pend.on = 1;
        }
        copy_field(to, src, field);
        done++;
    }
    return done;
}

/* the first list of a field: short lists as they are; long ones as "(none)" and the groups */
static int open_top(Ctx* c, uint64_t item, uint8_t slot, uint8_t field) {
    uint64_t sub;
    uint32_t kind, pos = 1;
    if (g_value_count <= PAGE) return open_values(c, item, slot, field, 0, g_value_count);
    if (!(sub = open_sub(c, item))) return 0;
    { const uint64_t it = new_item(c, sub, g_vt_dyn); if (it) tag_set(it, g_leaf, slot, 0, field, g_val_row[0]); }
    for (kind = K_CLASS; kind <= K_OTHER; ++kind) {
        uint64_t it;
        if (!g_seg_count[kind]) continue;
        it = new_item(c, sub, g_vt_dyn);
        if (it) tag_set(it, TAG_CAT, slot, (uint8_t)pos, field, (uint64_t)g_seg_first[kind] | (uint64_t)g_seg_count[kind] << 32);
        pos++;
    }
    return 1;
}
/* An accessory for the open attach point (0 = none there) goes into the class's main rows: their list keeps what it has for
 * the other points. The new list is an array of pointers to the game's own strings, in memory from the game's heap. */
static uint8_t g_fake_row[AROW];
static uint32_t apply_acc(uint8_t slot, uint64_t entry, int record) {
    uint64_t prev = 0;
    uint32_t done = 0, j, k;
    int s2;
    char pt[40];
    if (!find_asset_table()) return 0;
    if (record) { g_pend.on = 0; g_pend.field = JB_ACCESSORY; g_pend.gen = g_at_gen; g_pend.n = 0; }
    if (entry == (uint64_t)g_zero_row) entry = 0;
    for (s2 = 0; s2 < 2; ++s2) {
        const uint64_t to = main_row(g_own[slot].key, s2);
        uint64_t list, arr = 0;
        uint32_t n, keep = 0, newn;
        if (!to || to == prev) continue;
        prev = to;
        n = m16(to + 0x98); list = m64(to + 0xa0);
        if (n > 32 || (n && !g_api->is_mapped(list, (uint64_t)n * 8))) n = 0;
        for (j = 0; j < n; ++j) if (!(point_of(m_text(m64(list + (uint64_t)j * 8)), pt, sizeof pt) && same_text(pt, g_point))) keep++;
        newn = keep + (entry ? 1 : 0);
        if (newn && !(arr = game_alloc((uint64_t)newn * 8))) continue;
        for (j = 0, k = 0; j < n; ++j)
            if (!(point_of(m_text(m64(list + (uint64_t)j * 8)), pt, sizeof pt) && same_text(pt, g_point))) *(volatile uint64_t*)(arr + (uint64_t)k++ * 8) = m64(list + (uint64_t)j * 8);
        if (entry) *(volatile uint64_t*)(arr + (uint64_t)k * 8) = entry;
        for (j = 0; j < 16; ++j) g_fake_row[0x98 + j] = ((const uint8_t*)to)[0x98 + j];
        g_fake_row[0x98] = (uint8_t)newn; g_fake_row[0x99] = 0;
        *(uint64_t*)(g_fake_row + 0xa0) = arr;
        if (record && g_pend.n < MAX_TARGETS) {
            for (j = 0; j < AROW; ++j) g_pend_bak[g_pend.n][j] = ((const uint8_t*)to)[j];
            g_pend.to[g_pend.n++] = to; g_pend.on = 1;
        }
        copy_field(to, (uint64_t)g_fake_row, JB_ACCESSORY);
        done++;
    }
    return done;
}
static void take_acc(uint8_t slot, uint64_t entry) {
    int s2;
    uint32_t j;
    char line[260];
    char* at;
    apply_acc(slot, entry, 0);
    for (s2 = 0; s2 < 2; ++s2) {          /* for the next start: the list written out */
        const uint64_t to = main_row(g_own[slot].key, s2);
        char* w = g_own_look[slot].src[s2][JB_ACCESSORY];
        char* e = w;
        uint32_t n;
        if (!to) continue;
        n = m16(to + 0x98);
        if (!n || n > 32 || !g_api->is_mapped(m64(to + 0xa0), (uint64_t)n * 8)) { e = put_text(w, "none"); *e = 0; continue; }
        *e++ = '{';
        for (j = 0; j < n; ++j) {
            const char* t = m_text(m64(m64(to + 0xa0) + (uint64_t)j * 8));
            uint32_t len = 0;
            while (t[len]) len++;
            if ((uint32_t)(e - w) + len + 3 >= JB_SRC_MAX) break;
            if (j) *e++ = ',';
            e = put_text(e, t);
        }
        *e++ = '}'; *e = 0;
    }
    save_looks();
    at = put_text(line, "class "); at = put_text(at, g_own[slot].key); at = put_text(at, ": accessory for "); at = put_text(at, g_point); at = put_text(at, " = ");
    at = put_text(at, entry && entry != (uint64_t)g_zero_row ? m_text(entry) : "(none)"); at = put_text(at, ", saved as "); at = put_text(at, g_own_look[slot].src[0][JB_ACCESSORY]); *at = 0;
    TRACE(line);
}

/* the class's rows take a value for good: applied, and written to asset.txt for the next start */
static void take_value(uint8_t slot, uint8_t field, uint64_t from) {
    uint64_t ca = 0;
    uint32_t cn = 0, j, rows;
    int sex = 2, s2;
    char ref[JB_SRC_MAX + 80];
    char* at = ref;
    char line[240];
    const char* cls = row_class(from);
    if (from != (uint64_t)g_zero_row && m_group(from, 0, &ca, &cn)) sex = m_sex(ca, cn);
    rows = apply_value(slot, field, from, 0);
    /* for the next start: "none", the class (a body taken from a class), the row by its conditions, or the model's name when the
     * row has no conditions to name it by */
    if (from == (uint64_t)g_zero_row) at = put_text(at, "none");
    else if (cls[0] && !same_text(cls, g_own[slot].key)) { at = put_text(at, cls); sex = 2; }
    else if (cn) {
        *at++ = '[';
        for (j = 0; j < cn && at - ref < JB_SRC_MAX; ++j) { if (j) *at++ = ','; at = put_text(at, m_text(m64(ca + (uint64_t)j * 8))); }
        *at++ = ']';
    }
    *at = 0;
    if (!ref[0] || at - ref >= JB_SRC_MAX) {
        ref[0] = 0;
        if (kJbFieldIsString[field]) { const char* v = m_text(m64(from + kJbField[field][0].off)); int n = 0; while (v[n] && n < JB_SRC_MAX - 1) { ref[n] = v[n]; n++; } ref[n] = 0; }
    }
    if (ref[0]) {
        for (s2 = 0; s2 < 2; ++s2)
            if (sex == 2 || sex == s2) { int n = 0; while (ref[n]) { g_own_look[slot].src[s2][field][n] = ref[n]; n++; } g_own_look[slot].src[s2][field][n] = 0; }
        save_looks();
    }
    at = put_text(line, "class "); at = put_text(at, g_own[slot].key); at = put_text(at, ": "); at = put_text(at, kJbFieldName[field]);
    at = put_text(at, sex == 1 ? " (female) = " : sex == 0 ? " (male) = " : " = "); at = put_value(at, from, field);
    at = put_text(at, ", "); at = put_int(at, rows); at = put_text(at, " rows");
    if (ref[0]) { at = put_text(at, ", saved as "); at = put_text(at, ref); } else at = put_text(at, " (this session only: the row has no conditions to save it by)");
    *at = 0;
    TRACE(line);
}

/* a new base class: the placeholder's rows in the loaded asset table become copies of the base's matching rows */
static int take_base_look(const char* key, const char* base) {
    uint32_t i, k, cn = 0, cn2 = 0, j, done = 0;
    uint64_t ca = 0, ca2 = 0;
    if (!find_asset_table()) return 0;
    for (i = 0; i < g_at_count; ++i) {
        const uint64_t r = at_row(i);
        if (!m16(r + 0x20) || !m_group(r, key, &ca, &cn) || !m_own(r, key)) continue;
        for (k = 0; k < g_at_count; ++k) {          /* the base's own row with the same other conditions */
            const uint64_t r2 = at_row(k);
            if (!m16(r2 + 0x20) || !m_group(r2, base, &ca2, &cn2) || cn2 != cn || !m_own(r2, base)) continue;
            for (j = 0; j < cn; ++j) { const char* t = m_text(m64(ca + (uint64_t)j * 8)); if (!same_text(t, key) && !m_has(ca2, cn2, t)) break; }
            if (j < cn) continue;
            for (j = 0x30; j < AROW; ++j) ((volatile uint8_t*)r)[j] = ((const uint8_t*)r2)[j];
            acc_sync(r); anim_sync(r); flags_sync(r, JB_NFIELD);
            done++;
            break;
        }
    }
    return (int)done;
}

/* the three colors go into the class's main rows and into every other own row that has colors (its rows for 2D portraits and
 * for named characters); record = keep what the rows had (a shade under the cursor) */
static uint32_t apply_colors(uint8_t slot, const uint8_t* col, int record) {
    const char* key = g_own[slot].key;
    uint32_t i, j, done = 0, cn = 0;
    uint64_t ca = 0;
    if (!find_asset_table()) return 0;
    if (record) { g_pend.on = 0; g_pend.field = JB_COLORS; g_pend.gen = g_at_gen; g_pend.n = 0; }
    for (i = 0; i < g_at_count; ++i) {
        const uint64_t to = at_row(i);
        uint32_t any = 0;
        if (!m16(to + 0x20) || !m_group(to, key, &ca, &cn) || !m_own(to, key)) continue;
        for (j = 0; j < 12; ++j) any |= *(const uint8_t*)(to + 0x108 + j);
        if (!(cn == 1 || (cn == 2 && m_sex(ca, cn) != 2)) && !any) continue;
        if (record && g_pend.n < MAX_TARGETS) {
            for (j = 0; j < AROW; ++j) g_pend_bak[g_pend.n][j] = ((const uint8_t*)to)[j];
            g_pend.to[g_pend.n++] = to; g_pend.on = 1;
        }
        for (j = 0; j < 12; ++j) *(volatile uint8_t*)(to + 0x108 + j) = col[j];
        flags_sync(to, JB_COLORS);
        done++;
    }
    return done;
}
static void colors_text(const uint8_t* col, char* out) {          /* "#RRGGBB,#RRGGBB,-" */
    uint32_t k;
    for (k = 0; k < 3; ++k) {
        if (k) *out++ = ',';
        if (col[k * 4 + 3]) out = put_rgb(out, (uint32_t)col[k * 4] << 16 | (uint32_t)col[k * 4 + 1] << 8 | col[k * 4 + 2]); else *out++ = '-';
    }
    *out = 0;
}
static void take_colors(uint8_t slot, const uint8_t* col) {
    char ref[40], line[200];
    char* at;
    int s2, n;
    const uint32_t rows = apply_colors(slot, col, 0);
    colors_text(col, ref);
    for (s2 = 0; s2 < 2; ++s2) { for (n = 0; ref[n]; ++n) g_own_look[slot].src[s2][JB_COLORS][n] = ref[n]; g_own_look[slot].src[s2][JB_COLORS][n] = 0; }
    save_looks();
    at = put_text(line, "class "); at = put_text(at, g_own[slot].key); at = put_text(at, ": colors = "); at = put_text(at, ref);
    at = put_text(at, ", "); at = put_int(at, rows); at = put_text(at, " rows"); *at = 0;
    TRACE(line);
}
/* the color of a shade line put into place idx of the class's colors as they are */
static void shade_colors(uint8_t slot, uint64_t tag_row, uint8_t* col) {
    const uint32_t idx = (uint32_t)(tag_row >> 32) % 3, rgb = (uint32_t)tag_row & 0xffffff;
    cur_colors(slot, col);
    if (tag_row & COL_NONE) { col[idx * 4] = col[idx * 4 + 1] = col[idx * 4 + 2] = col[idx * 4 + 3] = 0; return; }
    col[idx * 4] = (uint8_t)(rgb >> 16); col[idx * 4 + 1] = (uint8_t)(rgb >> 8); col[idx * 4 + 2] = (uint8_t)rgb; col[idx * 4 + 3] = 255;
}
/* the exact color, on the game's list screen: hue, saturation, brightness, red, green, blue. Every change goes into the rows at
 * once (and the unit on the class change screen is built again); the color is saved when the screen is left or a line confirmed. */
static NxModList g_col_list;
static uint32_t col_rgb(void) { const uint8_t* p = g_col.col + g_col.idx * 4; return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }
static void col_set(uint32_t rgb) { uint8_t* p = g_col.col + g_col.idx * 4; p[0] = (uint8_t)(rgb >> 16); p[1] = (uint8_t)(rgb >> 8); p[2] = (uint8_t)rgb; p[3] = 255; }
static void coll_label(int row, char* out, uint64_t n, void* user) {
    static const char* const k[6] = {"Hue", "Saturation", "Brightness", "Red", "Green", "Blue"};
    (void)user; copy_out(out, n, k[(uint32_t)row % 6]);
}
static void coll_value(int row, char* out, uint64_t n, void* user) {
    const uint32_t rgb = col_rgb();
    char t[16];
    char* e = put_int(t, row == 0 ? g_col.h : row == 1 ? g_col.s : row == 2 ? g_col.v : rgb >> (16 - 8 * ((uint32_t)row - 3) % 24) & 255);
    (void)user; *e = 0; copy_out(out, n, t);
}
static void coll_help(int row, char* out, uint64_t n, void* user) {
    char t[160];
    char* e = put_text(t, "Color "); 
    (void)user;
    e = put_int(e, (uint64_t)g_col.idx + 1); e = put_text(e, " is "); e = put_rgb(e, col_rgb());
    e = put_text(e, row == 0 ? ".  Hue 0 to 359.  Left / right: 1.  Confirm: +15." : row < 3 ? ".  0 to 100.  Left / right: 1.  Confirm: +10." : ".  0 to 255.  Left / right: 1.  Confirm: +16.");
    *e = 0; copy_out(out, n, t);
}
static int col_step(Ctx* c, int row, int delta, int wrap) {
    const uint32_t max = row == 0 ? 359 : row < 3 ? 100 : 255, rgb = col_rgb();
    const uint32_t sh = 16 - 8 * ((uint32_t)(row < 3 ? 3 : row) - 3);
    int v = (int)(row == 0 ? g_col.h : row == 1 ? g_col.s : row == 2 ? g_col.v : (rgb >> sh & 255));
    const int was = v;
    v += delta;
    if (row == 0 || wrap) { const int span = (int)max + 1; v = (v % span + span) % span; }
    else { if (v < 0) v = 0; if (v > (int)max) v = (int)max; }
    if (v == was && g_col.col[g_col.idx * 4 + 3]) return 0;
    if (row < 3) {
        if (row == 0) g_col.h = (uint32_t)v; else if (row == 1) g_col.s = (uint32_t)v; else g_col.v = (uint32_t)v;
        col_set(hsv_rgb(g_col.h, g_col.s, g_col.v));
    } else {
        col_set((rgb & ~((uint32_t)255 << sh)) | (uint32_t)v << sh);
        rgb_hsv(col_rgb(), &g_col.h, &g_col.s, &g_col.v);
    }
    g_col.dirty = 1;
    apply_colors(g_col.slot, g_col.col, 0);
    preview_row(c, find_job(g_own[g_col.slot].key));
    return 1;
}
static int coll_side(Ctx* c, int row, int dir, void* user) { (void)user; g_ctx = c; return col_step(c, row, dir, 0); }
static uint32_t coll_pick(Ctx* c, int row, void* user) {
    (void)user; g_ctx = c;
    col_step(c, row, row == 0 ? 15 : row < 3 ? 10 : 16, 1);
    if (g_col.dirty) { g_col.dirty = 0; take_colors(g_col.slot, g_col.col); }
    return 13;
}
static void coll_back(void* user) { (void)user; if (g_col.dirty) { g_col.dirty = 0; take_colors(g_col.slot, g_col.col); } }
static int open_colors(Ctx* c, uint64_t item, uint8_t slot, uint8_t idx) {
    static int told;
    static char title[40];
    char* e;
    if (g_api->version < 4 || g_api->size < sizeof(NxModApi) || !g_api->open_list) {
        if (!told) { told = 1; g_api->log(g_api, 1, "the exact color needs the game's list screen (mod interface version 4): this build of the game does not have it"); }
        return 0;
    }
    g_col.slot = slot; g_col.idx = (uint8_t)(idx % 3); g_col.dirty = 0;
    cur_colors(slot, g_col.col);
    g_col.h = 0; g_col.s = 0; g_col.v = 50;
    if (g_col.col[g_col.idx * 4 + 3]) rgb_hsv(col_rgb(), &g_col.h, &g_col.s, &g_col.v);
    e = put_text(title, "Outfit color "); e = put_int(e, (uint64_t)g_col.idx + 1); *e = 0;
    g_col_list.size = sizeof g_col_list; g_col_list.pages = 1;
    g_col_list.titles[0] = title; g_col_list.titles[1] = g_col_list.titles[2] = "";
    g_col_list.count = 6; g_col_list.user = 0; g_col_list.page = 0;
    g_col_list.label = coll_label; g_col_list.value = coll_value; g_col_list.help = coll_help;
    g_col_list.pick = coll_pick; g_col_list.back = coll_back; g_col_list.side = coll_side;
    if (!g_api->open_list(c, item, &g_col_list)) { g_api->log(g_api, 1, "the game's list screen could not be opened for the color"); return 0; }
    return 1;
}
/* the icon of another class: the placeholder's icon name becomes that class's (the game's own string) */
static void take_icon(uint8_t slot, uint64_t from) {
    const uint64_t row = find_job(g_own[slot].key);
    char line[200];
    char* at;
    if (!row || !row_key(from, g_own[slot].icon, sizeof g_own[slot].icon)) return;
    *(volatile uint64_t*)(row + JB_ICON) = *(const uint64_t*)(from + JB_ICON);
    save_own();
    at = put_text(line, "class "); at = put_text(at, g_own[slot].key); at = put_text(at, ": icon of "); at = put_text(at, g_own[slot].icon);
    at = put_text(at, " ("); at = put_text(at, m_text(*(const uint64_t*)(from + JB_ICON))); *at++ = ')'; *at = 0;
    TRACE(line);
}

/* A placeholder (or a class made earlier) becomes a copy of a class of the game. */
static void base_take(Ctx* c, uint8_t slot, uint64_t from, uint64_t item) {
    const uint64_t row = find_job(g_own[slot].key);
    uint32_t o, f;
    int rows, i;
    char line[200], key[JB_KEY_MAX];
    char* at;
    if (!row || !row_key(from, key, sizeof key)) return;
        /* the placeholder becomes a copy of the class: everything but its own key hash, key, index and the loader's pointer */
        {
            const uint64_t own_name = *(const uint64_t*)(row + 0x20), own_help = *(const uint64_t*)(row + 0x28);
            uint32_t bl;
            for (o = 0x20; o < ROW_BYTES; ++o) ((volatile uint8_t*)row)[o] = ((const uint8_t*)from)[o];
            if (g_own[slot].name[0]) { *(volatile uint64_t*)(row + 0x20) = own_name; *(volatile uint64_t*)(row + 0x28) = own_help; }
            for (bl = 0; bl < JB_NBLOCK; ++bl) g_own[slot].has[bl] = 0;          /* the numbers start again as the new base's */
            g_own[slot].master[0] = 0; g_own[slot].icon[0] = 0; g_own[slot].attr[0] = 0; g_own[slot].dismount[0] = 0; g_own[slot].lower[0] = 0; g_own[slot].upper[0] = 0; g_own[slot].no_mount = 0; g_own[slot].mount_from[0] = 0; g_own[slot].skills[0] = 0; g_own[slot].barred[0] = 0; g_own[slot].has_sex = 0; g_own[slot].has_cexp = 0; g_own[slot].has_abil = 0;
        }
        for (i = 0; key[i]; ++i) g_own[slot].base[i] = key[i];
        g_own[slot].base[i] = 0;
        save_own();
        /* its look starts again as the new base's: the choices made for the old one are dropped */
        for (f = 0; f < JB_NFIELD; ++f) g_own_look[slot].src[0][f][0] = g_own_look[slot].src[1][f][0] = 0;
        for (f = 0; f < N_WKIND; ++f) g_wanim[slot][0][f][0] = g_wanim[slot][1][f][0] = 0;
        save_looks();
        rows = take_base_look(g_own[slot].key, key);
        g_app_redo = 1;
        g_fresh[slot] = rows == 0;
        preview_row(c, row);
        call(c, g_vt[2 + SLOT_DECIDE_SE], item, 0, 0, 0);
        at = put_text(line, "class "); at = put_text(at, g_own[slot].key); at = put_text(at, " is now a copy of "); at = put_text(at, key);
        at = put_text(at, ", "); at = put_int(at, (uint64_t)rows); at = put_text(at, " asset rows updated"); *at = 0;
        g_api->log(g_api, 2, line);
}
/* A class is removed: its entry (and its dismounted class's) leaves job.txt and asset.txt at once; the rows stay in the game's
 * table until the game closes. A removed entry keeps its place in the lists of this session with an empty key. */
static void remove_class(uint8_t slot) {
    char line[200];
    char* at = put_text(line, "class "); 
    uint32_t k;
    at = put_text(at, g_own[slot].key); at = put_text(at, " removed from job.txt and asset.txt");
    for (k = 0; k < g_own_count; ++k)
        if (k != slot && g_own[k].no_mount && g_own[slot].dismount[0] && same_text(g_own[k].key, g_own[slot].dismount)) { at = put_text(at, ", with its dismounted class "); at = put_text(at, g_own[k].key); g_own[k].key[0] = 0; }
    at = put_text(at, "; both are gone from the game after a restart"); *at = 0;
    g_own[slot].key[0] = 0;
    save_own(); save_looks();
    g_api->log(g_api, 2, line);
}
/* ---- the game's own yes / no prompt ----
 * 0x878e20(parent menu, &message id, &yes, &no, &back) with libc++ function objects (0x20 bytes of storage and a pointer to
 * themselves at +0x20; the first word is a table: slots 2 and 3 copy the object and write the table, slot 6 is the call). "No"
 * is the game's own from its reset prompt; "yes" is a copy of that prompt's table whose call and copies are this mod's. The
 * prompt's Yes item (table 0x20b0530) holds the function at +0x80 and calls it with a pointer to itself; the game's answers 2.
 * The menu the prompt was asked from answers 12 and stands still until the prompt, its newest child, is gone. */
#define PROMPT_FN 0x878e20ull
#define PROMPT_YES_VT 0x20bad10ull
#define PROMPT_NO_VT 0x20bad90ull
#define PROMPT_CHOICE_VT 0x20b0530ull
static uint64_t g_yvt[2 + 9], g_yes_new, g_yes_at;
static int g_prompt_ok;
static uint32_t g_frame, g_ask_frame, g_ask_b;
static uint64_t g_ask_menu, g_ask_menu_vt, g_ask_child;          /* a prompt is up over this menu */
static struct { int kind; uint8_t slot; uint64_t row, item; } g_ask;          /* what Yes does: 1 make the class, 2 remove it */
static uint64_t pick_done(Ctx* c, uint64_t item);
/* After a class was made from "New class", the lists it was chosen from (classes of a tier, tiers, kinds) are closed one after
 * another and the new class's sheet is opened from its line, so that it can be changed at once. Done over a few frames from the
 * frame hook, the way the creator itself is opened: the menu handler 0x17141e0(menu, 3) closes a menu as B does, and the line's
 * own A handler followed by the handler's answer 10 opens the sheet. menu[0..2] = those lists, menu[3] = the list of classes. */
static struct { int stage; uint32_t at; uint64_t menu[4], vt[4], top; uint8_t slot; } g_adv;
static int g_adv_open;          /* the mod itself is pressing A on the new class's line */
static void adv_begin(uint64_t item, uint8_t slot) {
    uint64_t cur = item;
    const Tag* t;
    int k;
    g_adv.stage = 0;
    for (k = 0; k < 4; ++k) {
        uint64_t m = 0, vt = 0, a = 0, avt = 0;
        if (!rd64(cur + 8, &m) || m < 0x10000 || !g_api->is_mapped(m, 0x440) || !rd64(m, &vt)) return;
        g_adv.menu[k] = m; g_adv.vt[k] = vt;
        if (k == 3) break;
        if (!rd64(m + 0x428, &a) || a < 0x10000 || !g_api->is_mapped(a, ITEM_BYTES) || !rd64(a, &avt) || avt != (uint64_t)&g_vt_dyn[2]) return;
        cur = a;
    }
    t = tag_of(cur);
    if (!t || t->kind != TAG_SLOT || t->slot != slot) return;          /* not chosen by way of "New class" */
    g_adv.top = cur; g_adv.slot = slot; g_adv.at = g_frame; g_adv.stage = 1;
}
static void yes_invoke(Ctx* c, void* user) {
    const uint64_t self = c->x[0];
    uint64_t item = 0, vt = 0, fn = 0;
    int kind;
    (void)user; g_ctx = c;
    if (!rd64(c->x[1], &item) || !item || !rd64(item, &vt) || vt != g_base + PROMPT_CHOICE_VT || !rd64(item + 0x80, &fn) || fn != self || g_frame - g_ask_frame < 15 || !g_ask.kind) {
        g_api->log(g_api, 1, "prompt: Yes was called too early or not by the prompt's own Yes: nothing done");
        c->x[0] = 0;
        return;
    }
    kind = g_ask.kind; g_ask.kind = 0;
    if (g_ask.slot < g_own_count) {
        if (kind == 1) { base_take(c, g_ask.slot, g_ask.row, g_ask.item); adv_begin(g_ask.item, g_ask.slot); }
        else remove_class(g_ask.slot);
        pick_done(c, g_ask.item);          /* the lines it was asked from show the result */
    }
    c->x[0] = 2;
}
static void yes_clone_new(Ctx* c, void* user) {
    const uint64_t r = call(c, g_yes_new, c->x[0], c->x[1], c->x[2], c->x[3]);
    (void)user;
    if (r > 0x10000 && g_api->is_mapped(r, 16)) *(volatile uint64_t*)r = (uint64_t)&g_yvt[2];
    c->x[0] = r;
}
static void yes_clone_at(Ctx* c, void* user) {
    const uint64_t to = c->x[1], r = call(c, g_yes_at, c->x[0], c->x[1], c->x[2], c->x[3]);
    (void)user;
    if (to > 0x10000 && g_api->is_mapped(to, 16)) *(volatile uint64_t*)to = (uint64_t)&g_yvt[2];
    c->x[0] = r;
}
static void prompt_setup(void) {
    uint32_t i;
    uint64_t no_call = 0;
    g_prompt_ok = 0;
    if (!g_api->is_function(g_base + PROMPT_FN) || !g_api->is_mapped(g_base + PROMPT_YES_VT - 16, sizeof g_yvt) || !g_api->is_mapped(g_base + PROMPT_NO_VT, 9 * 8)) return;
    for (i = 0; i < 2 + 9; ++i) g_yvt[i] = ((const uint64_t*)(g_base + PROMPT_YES_VT - 16))[i];
    for (i = 0; i < 9; ++i) if (!g_api->is_function(g_yvt[2 + i])) return;
    if (!rd64(g_base + PROMPT_NO_VT + 6 * 8, &no_call) || !g_api->is_function(no_call)) return;
    g_yes_new = g_yvt[2 + 2]; g_yes_at = g_yvt[2 + 3];
    g_yvt[2 + 6] = g_api->make_function(yes_invoke, 0);
    g_yvt[2 + 2] = g_api->make_function(yes_clone_new, 0);
    g_yvt[2 + 3] = g_api->make_function(yes_clone_at, 0);
    g_prompt_ok = g_yvt[2 + 6] && g_yvt[2 + 2] && g_yvt[2 + 3];
    g_api->log(g_api, g_prompt_ok ? 2 : 1, g_prompt_ok ? "prompt: the game's yes / no box is used when a class is made or removed" : "prompt: the runtime had no room for its functions: classes are made without asking, none can be removed");
}
static int prompt_ask(Ctx* c, uint64_t item, const char* mid) {
    static uint64_t ask, yes[6], no[6], none[6];
    uint64_t menu = 0, vt = 0, before = 0, after = 0, args[5];
    uint32_t i;
    if (!g_prompt_ok || !rd64(item + 8, &menu) || menu < 0x10000 || !g_api->is_mapped(menu, 0x170) || !rd64(menu, &vt)) return 0;
    for (i = 0; i < 6; ++i) yes[i] = no[i] = none[i] = 0;
    ask = (uint64_t)mid;
    yes[0] = (uint64_t)&g_yvt[2]; yes[1] = menu; yes[4] = (uint64_t)yes;
    no[0] = g_base + PROMPT_NO_VT; no[4] = (uint64_t)no;
    rd64(menu + 0x28, &before);
    g_ask_frame = g_frame;
    args[0] = menu; args[1] = (uint64_t)&ask; args[2] = (uint64_t)yes; args[3] = (uint64_t)no; args[4] = (uint64_t)none;
    g_api->call_guest(c, g_base + PROMPT_FN, args, 5);
    rd64(menu + 0x28, &after);
    if (after && after != before) { g_ask_menu = menu; g_ask_menu_vt = vt; g_ask_child = after; g_ask_b = 0; }          /* its process: the menu's buttons rest until it is gone */
    else g_ask_menu = 0;
    return 1;
}
/* every frame: the prompt is gone when it is no longer the menu's newest child; a menu left at "a dialog is up" gets its directions back */
static void prompt_watch(void) {
    uint64_t vt = 0, child = 0;
    if (!g_ask_menu) return;
    if (!g_api->is_mapped(g_ask_menu, 0x170) || !rd64(g_ask_menu, &vt) || vt != g_ask_menu_vt) { g_ask_menu = 0; return; }          /* the menu itself is gone */
    if (rd64(g_ask_menu + 0x28, &child) && child == g_ask_child) return;
    if (g_frame - g_ask_frame < 3) return;
    if (*(const uint8_t*)(g_ask_menu + 0x160) == 12) *(volatile uint8_t*)(g_ask_menu + 0x160) = 0;
    g_ask_menu = 0; g_ask.kind = 0;
}
/* ---- typing: the game's own keyboard ----
 * 0xcfa100(object, &request) is the game's wrapper of nn::swkbd (used where the player names a character): object = the input
 * object + 0x2a8, whose text (a libc++ string at +8) holds the answer; request = {initial text, length, header, length, sub text,
 * length, guide, length, +0x40 a mode byte, +0x44 minimum, +0x48 maximum length, +0x4c a flag (1), +0x50 the keyboard kind (0)}.
 * It answers 0 when the text was confirmed. It returns when the player is done: the game stands still meanwhile, as it does there. */
#define KBD_SHOW 0xcfa100ull
#define KBD_INPUT_GOT 0x216a4b0ull
static int kbd_ask(Ctx* c, const char* header, const char* initial, uint32_t max_chars, char* out, uint32_t out_max) {
    static uint64_t req[12];
    uint64_t p = 0, in = 0, text = 0, n = 0;
    uint32_t k, hl = 0, il = 0, len = 0;
    uint8_t b0;
    if (!g_api->is_function(g_base + KBD_SHOW) || !rd64(g_base + KBD_INPUT_GOT, &p) || !rd64(p, &in) || in < 0x10000 || !g_api->is_mapped(in + 0x2a8, 0x20)) {
        g_api->log(g_api, 1, "the game's keyboard was not found: names are typed in job.txt (name = ..., help = ...)");
        return 0;
    }
    while (header[hl]) hl++;
    while (initial[il]) il++;
    for (k = 0; k < 12; ++k) req[k] = 0;
    req[0] = (uint64_t)initial; req[1] = il; req[2] = (uint64_t)header; req[3] = hl;
    ((uint32_t*)req)[0x48 / 4] = max_chars; ((uint8_t*)req)[0x4c] = 1;
    if ((uint32_t)call(c, g_base + KBD_SHOW, in + 0x2a8, (uint64_t)req, 0, 0)) return 0;          /* cancelled */
    b0 = *(const uint8_t*)(in + 0x2b0);
    if (b0 & 1) { rd64(in + 0x2b8, &n); rd64(in + 0x2c0, &text); } else { n = b0 >> 1; text = in + 0x2b1; }
    if (!n || n > 4096 || text < 0x10000 || !g_api->is_mapped(text, n)) return 0;
    for (k = 0; k < n && len + 1 < out_max; ++k) {
        uint8_t ch = *(const uint8_t*)(text + k);
        if (ch < 0x20) ch = ' ';          /* one line: job.txt keeps a setting on one */
        if (!len && ch == ' ') continue;
        out[len++] = (char)ch;
    }
    while (len && ((uint8_t)out[len - 1] & 0xc0) == 0x80 && k < n) len--;          /* cut short: not in the middle of a character */
    if (len && k < n && ((uint8_t)out[len - 1] & 0xc0) == 0xc0) len--;
    while (len && out[len - 1] == ' ') len--;
    out[len] = 0;
    return len > 0;
}
/* A final choice was made: the list it was made from closes (answer 3, as B gives: the handler 0x17141e0 closes that menu only)
 * and the lines it was opened from are drawn again, so that they show the new value. The line a sub menu was opened from is at
 * the sub menu's +0x428; drawing is the item's slot 55 (0x171ea10: it asks the item for its text again). */
#define SLOT_REDRAW 55
static uint64_t pick_done(Ctx* c, uint64_t item) {
    int depth;
    for (depth = 0; depth < 6; ++depth) {
        uint64_t menu = 0, anchor = 0, vt = 0;
        if (!rd64(item + 8, &menu) || menu < 0x10000 || !g_api->is_mapped(menu, 0x440) || !rd64(menu + 0x428, &anchor) || anchor < 0x10000 ||
            !g_api->is_mapped(anchor, ITEM_BYTES) || !rd64(anchor, &vt)) break;
        if (vt != (uint64_t)&g_vt_dyn[2] && vt != (uint64_t)&g_vt_cls[2]) break;          /* ours only */
        call(c, g_vt_dyn[2 + SLOT_REDRAW], anchor, 0, 0, 0);
        item = anchor;
    }
    return 3;
}
static void dyn_acall(Ctx* c, void* user) {
    const uint64_t item = c->x[0];
    const Tag* t = tag_of(item);
    uint64_t sub;
    int i;
    char line[200], key[JB_KEY_MAX];
    char* at;
    (void)user; g_ctx = c;
    if (g_ask_menu || (g_adv.stage && !g_adv_open)) { c->x[0] = 0; return; }          /* a prompt is up: A belongs to it; or the lists are closing by themselves */
    if (!t || !g_api->is_mapped(item, ITEM_BYTES) || *(const uint8_t*)(item + 0x52) != 1) { c->x[0] = 1; return; }
    if (t->kind == TAG_SPAGE) {          /* a page of the creator's first list */
        uint32_t k;
        if (!(sub = open_sub(c, item))) { c->x[0] = 1; return; }
        for (k = (uint32_t)t->row; k < g_top_count && k < (uint32_t)t->row + PAGE; ++k) { const uint64_t it = new_item(c, sub, g_vt_dyn); if (it) tag_set(it, TAG_SLOT, g_top[k], 0, 0, 0); }
        c->x[0] = 10;
        return;
    }
    if (t->kind == TAG_SLOT && t->slot < g_own_count && !slot_dead(t->slot) && !(slot_empty(t->slot) && !g_own[t->slot].no_mount)) {          /* a class: its sheet */
        const uint8_t slot = t->slot;
        const int foot = g_own[slot].no_mount;          /* a dismounted class: its base and its removal belong to the mounted one */
        if (!find_job(g_own[slot].key) || !(sub = open_sub(c, item))) { c->x[0] = 1; return; }
        at = put_text(line, "sheet of "); at = put_text(at, g_own[slot].key); at = put_text(at, ": its male row in the asset table at ");
        at = put_hex(at, main_row(g_own[slot].key, 0)); at = put_text(at, ", female at "); at = put_hex(at, main_row(g_own[slot].key, 1)); *at = 0;
        TRACE(line);
        if (!foot) { const uint64_t it = new_item(c, sub, g_vt_dyn); if (it) tag_set(it, TAG_FIELD, slot, 0, FIELD_BASE, 0); }
        { const uint64_t it = new_item(c, sub, g_vt_dyn); if (it) tag_set(it, TAG_FIELD, slot, 0, FIELD_NAME, 0); }
        for (i = 0; i < 8; ++i) { const uint64_t it = new_item(c, sub, g_vt_dyn); if (it) tag_set(it, TAG_GROUP, slot, (uint8_t)i, 0, (uint64_t)i); }
        if (!foot) { const uint64_t it = new_item(c, sub, g_vt_dyn); if (it) tag_set(it, TAG_REMOVE, slot, 0, 0, 0); }
        c->x[0] = 10;
        return;
    }
    if (t->kind == TAG_SLOT && (t->slot >= g_own_count || slot_dead(t->slot))) { c->x[0] = 1; return; }
    if (t->kind == TAG_KLINE && t->slot < g_own_count) {
        if (t->rank == 3) {          /* the effect row of a class or character, like the other fields */
            collect_values(JB_MAGIC);
            c->x[0] = g_value_count && open_top(c, item, t->slot, JB_MAGIC) ? 10 : 1;
            return;
        }
        if (t->rank < 2) {
            if (!(sub = open_sub(c, item))) { c->x[0] = 1; return; }
            for (i = 0; i < kCastOptN[t->rank]; ++i) { const uint64_t it = new_item(c, sub, g_vt_dyn); if (it) tag_set(it, TAG_KVAL, t->slot, (uint8_t)i, t->rank, (uint64_t)i); }
            c->x[0] = 10;
            return;
        }
        if (!(sub = open_sub(c, item))) { c->x[0] = 1; return; }          /* where spells start: a list of the two */
        for (i = 0; i < 2; ++i) { const uint64_t it = new_item(c, sub, g_vt_dyn); if (it) tag_set(it, TAG_PICK, t->slot, (uint8_t)i, PICK_CAST, (uint64_t)i); }
        c->x[0] = 10;
        return;
    }
    if (t->kind == TAG_KVAL && t->slot < g_own_count) {
        char* to = (t->field & 1) ? g_cast[t->slot].effect : g_cast[t->slot].hand;
        char* e = put_text(to, kCastOpt[t->field & 1][t->row % kCastOptN[t->field & 1]]);
        *e = 0;
        save_looks();
        call(c, g_vt[2 + SLOT_DECIDE_SE], item, 0, 0, 0);
        c->x[0] = pick_done(c, item);
        return;
    }
    if (t->kind == TAG_FIELD && t->field == FIELD_NAME && t->slot < g_own_count) {          /* the name and the description, each typed on the game's keyboard */
        if (!(sub = open_sub(c, item))) { c->x[0] = 1; return; }
        for (i = 0; i < 2; ++i) { const uint64_t it = new_item(c, sub, g_vt_dyn); if (it) tag_set(it, TAG_NAME, t->slot, (uint8_t)i, 0, 0); }
        c->x[0] = 10;
        return;
    }
    if (t->kind == TAG_NAME && t->slot < g_own_count) {
        static char typed[JB_HELP_MAX];
        const int desc = t->rank == 1;
        char* to = desc ? g_own[t->slot].help : g_own[t->slot].name;
        if (!kbd_ask(c, desc ? "Class description" : "Class name", to, desc ? 90 : 24, typed, desc ? JB_HELP_MAX : JB_NAME_MAX)) { c->x[0] = 0; return; }
        { char* e2 = put_text(to, typed); *e2 = 0; }
        save_own();
        call(c, g_vt[2 + SLOT_DECIDE_SE], item, 0, 0, 0);
        call(c, g_vt_dyn[2 + SLOT_REDRAW], item, 0, 0, 0);
        pick_done(c, item);          /* the lines above show it too */
        c->x[0] = 0;
        return;
    }
    if (t->kind == TAG_REMOVE && t->slot < g_own_count) {          /* asked on the game's own prompt: nothing is removed without a Yes */
        if (slot_dead(t->slot)) { c->x[0] = 1; return; }
        g_ask.kind = 2; g_ask.slot = t->slot; g_ask.row = 0; g_ask.item = item;
        if (!prompt_ask(c, item, "MCNC_Remove")) { g_ask.kind = 0; g_api->log(g_api, 1, "the game's prompt is not available: a class is not removed without it (take its entry out of job.txt and asset.txt by hand)"); c->x[0] = 1; return; }
        c->x[0] = 12;
        return;
    }
    if (t->kind == TAG_GROUP && t->slot < g_own_count) {          /* a group of the class sheet, named as the game names it */
        const uint8_t sl = t->slot;
        const uint32_t g = (uint32_t)t->row;
        uint8_t n = 0;
        if (!find_job(g_own[sl].key)) { c->x[0] = 1; return; }
        if (g == 2) { c->x[0] = open_numbers(c, item, sl, NUM_SKILLS) ? 9 : 1; return; }          /* Skill Exp Bonus: the thirteen skills on one screen */
        if (!(sub = open_sub(c, item))) { c->x[0] = 1; return; }
#define LINE(KIND, FIELD, ROW) { const uint64_t it = new_item(c, sub, g_vt_dyn); if (it) tag_set(it, KIND, sl, n, FIELD, ROW); n++; }
        if (g == 0) {          /* Class Info */
            LINE(TAG_BLOCK, JB_NBLOCK, 0) LINE(TAG_BLOCK, JB_B_UTYPE, 0) LINE(TAG_ABIL, 0, 0) LINE(TAG_FIELD, FIELD_ICON, 0)
            LINE(TAG_FIELD, FIELD_LOWER, 0) LINE(TAG_FIELD, FIELD_UPPER, 0)
        } else if (g == 1) {          /* Basic Stats (caps are 99 in every class of the game: left out) */
            LINE(TAG_BLOCK, JB_B_STATS, 0) LINE(TAG_BLOCK, JB_B_MOD, 0) LINE(TAG_BLOCK, JB_B_GROW, 0) LINE(TAG_BLOCK, JB_B_GBONUS, 0)
        } else if (g == 3) {          /* Abilities */
            abil_collect(sl);
            LINE(TAG_ABIL, 1, 0) LINE(TAG_ABIL, 2, 0) LINE(TAG_ABIL, 3, 0) LINE(TAG_FIELD, FIELD_SKILL, 0) LINE(TAG_ABIL, 5, 0)
        } else if (g == 4) {          /* Certification Exam */
            abil_collect(sl);
            LINE(TAG_BLOCK, JB_B_CCREQ, 0) LINE(TAG_BLOCK, JB_B_CCFAIL, 0) LINE(TAG_ABIL, 4, 0)
        } else if (g == 5) {          /* Mounted */
            LINE(TAG_TYPE, JB_B_UTYPE, N_TYPESET + 1)
            if (row_mounted(find_job(g_own[sl].key))) LINE(TAG_FIELD, JB_MOUNT, 0)
            if (foot_of(sl) >= 0 && find_job(g_own[foot_of(sl)].key)) {          /* its dismounted class: only the Class Bonus is its own */
                const uint64_t it = new_item(c, sub, g_vt_dyn);
                if (it) tag_set(it, TAG_BLOCK, (uint8_t)foot_of(sl), n, JB_B_MOD, 0);
                n++;
            }
        } else if (g == 6) {          /* Appearance */
            LINE(TAG_FIELD, JB_BODY, 0) LINE(TAG_FIELD, JB_MATERIAL, 0) LINE(TAG_FIELD, JB_COLORS, 0) LINE(TAG_FIELD, JB_SCALE, 0) LINE(TAG_FIELD, JB_ACCESSORY, 0) LINE(TAG_FIELD, JB_FOOTSTEP, 0)
        } else {          /* Animations and Magic */
            LINE(TAG_FIELD, JB_ANIM, 0) LINE(TAG_FIELD, JB_MAGIC, 0)
        }
#undef LINE
        c->x[0] = 10;
        return;
    }
    if (t->kind == TAG_FIELD && t->field == FIELD_SKILL) {          /* the mastery skills the game's classes have, each once */
        uint32_t k, j2;
        collect_jobs();
        g_value_count = 0; g_leaf = TAG_SKILL;
        g_val_row[0] = (uint64_t)g_zero_row; g_val_kind[0] = K_NONE; g_val_who[0] = 0; g_val_hash[0] = 1; g_value_count = 1;
        for (k = 0; k < (uint32_t)g_job_count && g_value_count < MAX_VALUES; ++k) {
            const char* sid = master_sid(g_jobs[k]);
            if (!sid[0] || g_jobs[k] == find_job(g_own[t->slot].key)) continue;
            for (j2 = 1; j2 < g_value_count && !same_text(master_sid(g_val_row[j2]), sid); ++j2) {}
            if (j2 < g_value_count) continue;
            g_val_row[g_value_count] = g_jobs[k]; g_val_kind[g_value_count] = K_OTHER; g_val_who[g_value_count] = 0; g_val_hash[g_value_count] = 2; g_value_count++;
        }
        c->x[0] = open_values(c, item, t->slot, FIELD_SKILL, 0, g_value_count) ? 10 : 1;
        return;
    }
    if (t->kind == TAG_SKILL && t->slot < g_own_count) {
        take_master(t->slot, t->row);
        call(c, g_vt[2 + SLOT_DECIDE_SE], item, 0, 0, 0);
        c->x[0] = pick_done(c, item);
        return;
    }
    if (t->kind == TAG_BLOCK && t->field == JB_B_UTYPE) {          /* a kind of class to be, or each type's number */
        if (!find_job(g_own[t->slot].key) || !(sub = open_sub(c, item))) { c->x[0] = 1; return; }
        for (i = 0; i < N_TYPESET; ++i) { const uint64_t it = new_item(c, sub, g_vt_dyn); if (it) tag_set(it, TAG_TYPE, t->slot, (uint8_t)i, JB_B_UTYPE, (uint64_t)i); }
        c->x[0] = 10;
        return;
    }
    if (t->kind == TAG_TYPE && t->slot < g_own_count) {
        if (t->row == N_TYPESET) { c->x[0] = open_numbers(c, item, t->slot, JB_B_UTYPE) ? 9 : 1; return; }
        if (t->row == N_TYPESET + 2) {          /* gender, the three abilities, the barred characters */
            if (!find_job(g_own[t->slot].key) || !(sub = open_sub(c, item))) { c->x[0] = 1; return; }
            abil_collect(t->slot);
            for (i = 0; i <= 5; ++i) { const uint64_t it = new_item(c, sub, g_vt_dyn); if (it) tag_set(it, TAG_ABIL, t->slot, (uint8_t)i, (uint8_t)i, 0); }
            c->x[0] = 10;
            return;
        }
        if (t->row > N_TYPESET) {          /* the on-foot partner, written to job.txt for the next start */
            const int made = gen_dismount(t->slot);
            if (made) { call(c, g_vt[2 + SLOT_DECIDE_SE], item, 0, 0, 0); call(c, g_vt_dyn[2 + SLOT_REDRAW], item, 0, 0, 0); }          /* the line says "made" now */
            c->x[0] = made ? 0 : 1;
            return;
        }
        type_take(t->slot, (uint32_t)t->row);
        call(c, g_vt[2 + SLOT_DECIDE_SE], item, 0, 0, 0);
        c->x[0] = pick_done(c, item);
        return;
    }
    if ((t->kind == TAG_ABIL && t->slot < g_own_count && t->field <= PICK_BAR) || (t->kind == TAG_PPAGE && t->slot < g_own_count)) {          /* the list of choices, or one page of it */
        const uint32_t what = t->field;
        uint32_t count, first = 0, last, k;
        if (!find_job(g_own[t->slot].key)) { c->x[0] = 1; return; }
        if (t->kind == TAG_ABIL && what != PICK_SEX) abil_collect(t->slot);
        count = pick_count(what); last = count;
        if (t->kind == TAG_PPAGE) { first = (uint32_t)t->row; last = first + PAGE < count ? first + PAGE : count; }
        if (!(sub = open_sub(c, item))) { c->x[0] = 1; return; }
        if (t->kind == TAG_ABIL && count > PAGE) {
            for (k = 0; k * PAGE < count && k < 16; ++k) { const uint64_t it = new_item(c, sub, g_vt_dyn); if (it) tag_set(it, TAG_PPAGE, t->slot, (uint8_t)k, (uint8_t)what, (uint64_t)k * PAGE); }
        } else {
            for (k = first; k < last; ++k) { const uint64_t it = new_item(c, sub, g_vt_dyn); if (it) tag_set(it, TAG_PICK, t->slot, (uint8_t)(k - first), (uint8_t)what, (uint64_t)k); }
        }
        c->x[0] = 10;
        return;
    }
    if (t->kind == TAG_ABIL) { c->x[0] = 1; return; }          /* the note */
    if (t->kind == TAG_PICK && t->slot < g_own_count) {
        pick_take(t->slot, t->field, (uint32_t)t->row);
        call(c, g_vt[2 + SLOT_DECIDE_SE], item, 0, 0, 0);
        c->x[0] = pick_done(c, item);
        return;
    }
    if (t->kind == TAG_BLOCK && t->field <= JB_NBLOCK) {          /* its numbers, on the game's list screen */
        c->x[0] = open_numbers(c, item, t->slot, t->field) ? 9 : 1;
        return;
    }
    if (t->kind == TAG_WTOP || t->kind == TAG_WSLOT) {          /* the animations of a class, character or row: for every kind, or for one */
        collect_values(JB_ANIM);
        if (t->kind == TAG_WSLOT) {          /* as the base, or the animations of a weapon kind: this one's or another's */
            const uint32_t k = (uint32_t)(t->row % N_WKIND);
            if (!(sub = open_sub(c, item))) { c->x[0] = 1; return; }
            g_wsel = (uint8_t)k;
            { const uint64_t it = new_item(c, sub, g_vt_dyn); if (it) tag_set(it, TAG_WVALUE, t->slot, 0, JB_ANIM, (uint64_t)g_zero_row); }
            for (i = 0; i < N_WKIND; ++i) {
                const uint32_t src = i == 0 ? k : (uint32_t)(i <= (int)k ? i - 1 : i);          /* this weapon first, then the others */
                const uint64_t it = new_item(c, sub, g_vt_dyn);
                if (it) tag_set(it, TAG_WSRC, t->slot, (uint8_t)(i + 1), JB_ANIM, (uint64_t)src | (uint64_t)k << 8);
            }
            c->x[0] = 10;
            return;
        }
        c->x[0] = g_value_count && open_top(c, item, t->slot, JB_ANIM) ? 10 : 1;
        return;
    }
    if (t->kind == TAG_WSRC) {          /* only what has animations of its own for the chosen kind */
        g_wsrc = (uint8_t)(t->row & 0xff) % N_WKIND; g_wsel = (uint8_t)(t->row >> 8 & 0xff) % N_WKIND;
        g_wfilter = (char)(g_wsrc < 8 ? '1' + g_wsrc : '0'); collect_values(JB_ANIM); g_wfilter = 0;
        g_leaf = TAG_WVALUE;
        c->x[0] = g_value_count && open_top(c, item, t->slot, JB_ANIM) ? 10 : 1;
        return;
    }
    if (t->kind == TAG_WVALUE && t->slot < g_own_count) {
        pend_drop(1);
        wanim_take(t->slot, g_wsel, t->row);
        preview_row(c, find_job(g_own[t->slot].key));
        call(c, g_vt[2 + SLOT_DECIDE_SE], item, 0, 0, 0);
        c->x[0] = pick_done(c, item);
        return;
    }
    if (t->kind == TAG_CTOP) {          /* the colors of a class or character, like the other fields */
        collect_values(JB_COLORS);
        c->x[0] = g_value_count && open_top(c, item, t->slot, JB_COLORS) ? 10 : 1;
        return;
    }
    if (t->kind == TAG_CSLOT) {          /* one of the three colors: exactly, none, or a family of shades */
        const uint64_t idx = (t->row >> 32) % 3;
        if (!(sub = open_sub(c, item))) { c->x[0] = 1; return; }
        { const uint64_t it = new_item(c, sub, g_vt_dyn); if (it) tag_set(it, TAG_CEXACT, t->slot, 0, 0, idx << 32); }
        { const uint64_t it = new_item(c, sub, g_vt_dyn); if (it) tag_set(it, TAG_CSHADE, t->slot, 1, 0, idx << 32 | COL_NONE); }
        for (i = 0; i < N_FAMILY; ++i) { const uint64_t it = new_item(c, sub, g_vt_dyn); if (it) tag_set(it, TAG_CHUE, t->slot, (uint8_t)(i + 2), 0, idx << 32 | (uint64_t)i); }
        c->x[0] = 10;
        return;
    }
    if (t->kind == TAG_CEXACT) { pend_drop(1); c->x[0] = open_colors(c, item, t->slot, (uint8_t)(t->row >> 32)) ? 9 : 1; return; }
    if (t->kind == TAG_CHUE) {
        const uint64_t idx = (t->row >> 32) % 3;
        const uint32_t fam = (uint32_t)t->row % N_FAMILY;
        if (!(sub = open_sub(c, item))) { c->x[0] = 1; return; }
        for (i = 0; i < 12; ++i) { const uint64_t it = new_item(c, sub, g_vt_dyn); if (it) tag_set(it, TAG_CSHADE, t->slot, (uint8_t)i, (uint8_t)fam, idx << 32 | shade_rgb(fam, (uint32_t)i)); }
        c->x[0] = 10;
        return;
    }
    if (t->kind == TAG_CSHADE && t->slot < g_own_count) {
        uint8_t col[12];
        pend_drop(1);
        shade_colors(t->slot, t->row, col);
        take_colors(t->slot, col);
        preview_row(c, find_job(g_own[t->slot].key));
        call(c, g_vt[2 + SLOT_DECIDE_SE], item, 0, 0, 0);
        c->x[0] = pick_done(c, item);
        return;
    }
    if (t->kind == TAG_FIELD && t->field < JB_NFIELD) {          /* a look field: every value the table has for it */
        const uint8_t slot = t->slot, field = t->field;
        if (!main_row(g_own[slot].key, 0) && !main_row(g_own[slot].key, 1)) { c->x[0] = 1; return; }      /* no rows of its own until a restart */
        if (field == JB_ANIM) {          /* the class's own list, then one line per weapon kind */
            if (!(sub = open_sub(c, item))) { c->x[0] = 1; return; }
            { const uint64_t it = new_item(c, sub, g_vt_dyn); if (it) tag_set(it, TAG_WTOP, slot, 0, field, 0); }
            for (i = 0; i < N_WKIND; ++i) { const uint64_t it = new_item(c, sub, g_vt_dyn); if (it) tag_set(it, TAG_WSLOT, slot, (uint8_t)(i + 1), field, (uint64_t)i); }
            c->x[0] = 10;
            return;
        }
        if (field == JB_MAGIC) {          /* from a class or character, then what the class holds and shows while casting (read at the next start) */
            if (!(sub = open_sub(c, item))) { c->x[0] = 1; return; }
            { const uint64_t it = new_item(c, sub, g_vt_dyn); if (it) tag_set(it, TAG_KLINE, slot, 3, field, 0); }
            for (i = 0; i < 3; ++i) { const uint64_t it = new_item(c, sub, g_vt_dyn); if (it) tag_set(it, TAG_KLINE, slot, (uint8_t)i, field, 0); }
            c->x[0] = 10;
            return;
        }
        if (field == JB_COLORS) {          /* from a class or character, or each of the three colors made here */
            if (!(sub = open_sub(c, item))) { c->x[0] = 1; return; }
            { const uint64_t it = new_item(c, sub, g_vt_dyn); if (it) tag_set(it, TAG_CTOP, slot, 0, field, 0); }
            for (i = 0; i < 3; ++i) { const uint64_t it = new_item(c, sub, g_vt_dyn); if (it) tag_set(it, TAG_CSLOT, slot, (uint8_t)(i + 1), field, (uint64_t)i << 32); }
            c->x[0] = 10;
            return;
        }
        if (field == JB_ACCESSORY) {          /* remove all, then one line per attach point */
            uint32_t pi;
            collect_points();
            if (!(sub = open_sub(c, item))) { c->x[0] = 1; return; }
            g_leaf = TAG_VALUE;
            { const uint64_t it = new_item(c, sub, g_vt_dyn); if (it) tag_set(it, TAG_VALUE, slot, 0, field, (uint64_t)g_zero_row); }
            for (pi = 0; pi < g_point_count; ++pi) { const uint64_t it = new_item(c, sub, g_vt_dyn); if (it) tag_set(it, TAG_POINT, slot, (uint8_t)(pi + 1), field, pi); }
            c->x[0] = 10;
            return;
        }
        collect_values(field);
        at = put_text(line, kJbFieldName[field]); at = put_text(at, ": "); at = put_int(at, g_value_count - 1); at = put_text(at, " lines: "); at = put_int(at, g_seg_count[K_CLASS]);
        at = put_text(at, " classes, "); at = put_int(at, g_seg_count[K_CHAR]); at = put_text(at, " from characters, "); at = put_int(at, g_seg_count[K_OTHER]); at = put_text(at, " other"); *at = 0;
        TRACE(line);
        c->x[0] = g_value_count && open_top(c, item, slot, field) ? 10 : 1;
        return;
    }
    if (t->kind == TAG_POINT && t->row < g_point_count) {
        collect_point_values(g_points[t->row]);
        at = put_text(line, "accessories for "); at = put_text(at, g_point); at = put_text(at, ": "); at = put_int(at, g_value_count - 1); at = put_text(at, " lines: "); at = put_int(at, g_seg_count[K_CLASS]);
        at = put_text(at, " from classes, "); at = put_int(at, g_seg_count[K_CHAR]); at = put_text(at, " from characters, "); at = put_int(at, g_seg_count[K_OTHER]); at = put_text(at, " other"); *at = 0;
        TRACE(line);
        c->x[0] = open_top(c, item, t->slot, t->field) ? 10 : 1;
        return;
    }
    if (t->kind == TAG_CAT) {
        c->x[0] = open_values(c, item, t->slot, t->field, (uint32_t)t->row, (uint32_t)(t->row >> 32)) ? 10 : 1;
        return;
    }
    if (t->kind == TAG_AVALUE && t->slot < g_own_count) {
        pend_drop(1);
        take_acc(t->slot, t->row);
        preview_row(c, find_job(g_own[t->slot].key));
        call(c, g_vt[2 + SLOT_DECIDE_SE], item, 0, 0, 0);
        c->x[0] = pick_done(c, item);
        return;
    }
    if (t->kind == TAG_RANGE) {
        c->x[0] = open_values(c, item, t->slot, t->field, (uint32_t)t->row, (uint32_t)(t->row >> 32)) ? 10 : 1;
        return;
    }
    if (t->kind == TAG_VALUE && t->slot < g_own_count && t->field < JB_NFIELD) {
        pend_drop(1);          /* the rows as they were, so that the choice is made from a clean state */
        take_value(t->slot, t->field, t->row);
        preview_row(c, find_job(g_own[t->slot].key));
        call(c, g_vt[2 + SLOT_DECIDE_SE], item, 0, 0, 0);
        c->x[0] = pick_done(c, item);
        return;
    }
    if (t->kind == TAG_FIELD || (t->kind == TAG_SLOT && t->slot < g_own_count)) {         /* the base class line (or "New class"): choose the class, by kind and tier */
        const uint8_t slot = t->slot;
        const int line_field = t->kind == TAG_FIELD && (t->field == FIELD_LOWER || t->field == FIELD_UPPER);
        if (!find_job(g_own[slot].key)) { c->x[0] = 1; return; }
        if (!(sub = open_sub(c, item))) { c->x[0] = 1; return; }
        /* where a class is made: say first what it means for the save */
        if (t->kind == TAG_SLOT || t->field == FIELD_BASE) {          /* a base: the kind of class first, so that the class starts with that kind's data */
            for (i = 0; i < N_BCAT; ++i) { const uint64_t it = new_item(c, sub, g_vt_dyn); if (it) tag_set(it, TAG_BCAT, slot, (uint8_t)i, FIELD_BASE, (uint64_t)i); }
            c->x[0] = 10;
            return;
        }
        if (line_field) { const uint64_t it = new_item(c, sub, g_vt_dyn); if (it) tag_set(it, TAG_CLASS, slot, 0, t->field, 0); }          /* no class */
        for (i = 1; i <= 5; ++i) {
            int count = 0, pages, pg, k;
            for (k = 0; k < g_job_count; ++k) count += offered(g_jobs[k], (uint8_t)i, line_field, line_field ? find_job(g_own[slot].key) : 0);
            pages = (count + PAGE - 1) / PAGE;
            if (pages > MAX_PAGES) pages = MAX_PAGES;
            for (pg = 0; pg < pages; ++pg) {
                const uint64_t it = new_item(c, sub, g_vt_dyn);
                if (it) tag_set(it, TAG_RANK, slot, (uint8_t)i, t->field, (uint64_t)pg | (uint64_t)pages << 8);
            }
        }
        c->x[0] = 10;
        return;
    }
    if (t->kind == TAG_BCAT) {          /* the tiers that have classes of this kind */
        const uint8_t slot = t->slot;
        const uint32_t cat = (uint32_t)t->row;
        if (!(sub = open_sub(c, item))) { c->x[0] = 1; return; }
        for (i = 1; i <= 5; ++i) {
            int count = 0, pages, pg, k;
            for (k = 0; k < g_job_count; ++k) count += offered(g_jobs[k], (uint8_t)i, 0, 0) && class_cat(g_jobs[k]) == cat;
            pages = (count + PAGE - 1) / PAGE;
            if (pages > MAX_PAGES) pages = MAX_PAGES;
            for (pg = 0; pg < pages; ++pg) {
                const uint64_t it = new_item(c, sub, g_vt_dyn);
                if (it) tag_set(it, TAG_RANK, slot, (uint8_t)i, t->field, (uint64_t)pg | (uint64_t)pages << 8 | (uint64_t)(cat + 1) << 16);
            }
        }
        c->x[0] = 10;
        return;
    }
    if (t->kind == TAG_RANK) {
        const uint8_t slot = t->slot, rank = t->rank;
        const int page = (int)(t->row & 0xff);
        const uint32_t bcat = (uint32_t)(t->row >> 16 & 0xff);          /* 0 = every kind (the icon's list) */
        const int line_field = t->field == FIELD_LOWER || t->field == FIELD_UPPER;
        int seen = 0;
        if (!(sub = open_sub(c, item))) { c->x[0] = 1; return; }
        for (i = 0; i < g_job_count; ++i) {
            const uint64_t row = g_jobs[i];
            if (!offered(row, rank, line_field, line_field ? find_job(g_own[slot].key) : 0) || (bcat && class_cat(row) != bcat - 1)) continue;
            if (seen++ / PAGE != page) continue;
            { const uint64_t it = new_item(c, sub, g_vt_cls); if (it) tag_set(it, TAG_CLASS, slot, rank, t->field, row); }
        }
        c->x[0] = 10;
        return;
    }
    if (t->kind == TAG_CLASS && t->slot < g_own_count && t->field == FIELD_ICON) {
        take_icon(t->slot, t->row);
        call(c, g_vt[2 + SLOT_DECIDE_SE], item, 0, 0, 0);
        c->x[0] = pick_done(c, item);
        return;
    }
    if (t->kind == TAG_CLASS && t->slot < g_own_count && (t->field == FIELD_LOWER || t->field == FIELD_UPPER)) {          /* the class line */
        const uint64_t row = find_job(g_own[t->slot].key);
        const int up = t->field == FIELD_UPPER;
        char* dst = up ? g_own[t->slot].upper : g_own[t->slot].lower;
        uint64_t to = t->row;
        if (!row) { c->x[0] = 1; return; }
        if (!to) { char* e2 = put_text(dst, up ? "none" : "JID_\xe7\x84\xa1\xe3\x81\x97"); *e2 = 0; to = up ? 0 : find_job("JID_\xe7\x84\xa1\xe3\x81\x97"); }          /* the game's first classes name its empty class as the one before */
        else if (!row_key(to, dst, JB_KEY_MAX)) { c->x[0] = 1; return; }
        *(volatile uint64_t*)(row + (up ? 0x50 : 0x48)) = to;
        save_own();
        call(c, g_vt[2 + SLOT_DECIDE_SE], item, 0, 0, 0);
        c->x[0] = pick_done(c, item);
        return;
    }
    if (t->kind == TAG_CLASS && t->slot < g_own_count && t->row) {
        if (!find_job(g_own[t->slot].key) || !row_key(t->row, key, sizeof key)) { c->x[0] = 1; return; }
        if (slot_empty(t->slot)) {          /* a class is made: asked first on the game's own prompt (without one it is made at once) */
            g_ask.kind = 1; g_ask.slot = t->slot; g_ask.row = t->row; g_ask.item = item;
            if (prompt_ask(c, item, "MCNC_Create")) { c->x[0] = 12; return; }
            g_ask.kind = 0;
        }
        base_take(c, t->slot, t->row, item);
        c->x[0] = pick_done(c, item);
        return;
    }
    c->x[0] = 1;
}

/* the cursor came to one of our lines: the class change screen, when it is open, shows what the line stands for */
static void dyn_select(Ctx* c, void* user) {
    const uint64_t item = c->x[0];
    const Tag* t;
    (void)user; g_ctx = c;
    call(c, g_vt[2 + SLOT_SELECT], item, c->x[1], c->x[2], c->x[3]);
    t = tag_of(item);
    if (!t) return;
    pend_drop(1);
    if (t->slot >= g_own_count) return;
    if (t->kind == TAG_VALUE && t->field < JB_NFIELD) apply_value(t->slot, t->field, t->row, 1);
    if (t->kind == TAG_AVALUE) apply_acc(t->slot, t->row, 1);
    if (t->kind == TAG_WVALUE) wanim_try(t->slot, g_wsel, t->row);
    if (t->kind == TAG_CSHADE) { uint8_t col[12]; shade_colors(t->slot, t->row, col); apply_colors(t->slot, col, 1); }
    if (t->kind == TAG_CLASS && t->field == FIELD_BASE) preview_row(c, t->row);          /* a class offered as the base: the unit wears that class */
    else if (find_job(g_own[t->slot].key)) preview_row(c, find_job(g_own[t->slot].key));
}
/* left and right on a number line: one down, one up (the menus call the line's "previous value" and "next value") */
static void num_side(Ctx* c, void* user) {
    const uint64_t item = c->x[0], a1 = c->x[1], a2 = c->x[2], a3 = c->x[3];
    const Tag* t = tag_of(item);
    const int up = user != 0;
    g_ctx = c;
    if (t && t->kind == TAG_NUM && t->slot < g_own_count && t->field < JB_NBLOCK) { c->x[0] = (uint64_t)num_step(t->slot, t->field, (uint32_t)t->row, up ? 1 : -1, 0); return; }
    c->x[0] = call(c, g_vt[2 + (up ? SLOT_NEXT : SLOT_PREV)], item, a1, a2, a3);
}
/* B on one of our lines: a value that was only tried is taken back; leaving the list of placeholders ends the preview */
static void dyn_bcall(Ctx* c, void* user) {
    const uint64_t item = c->x[0], a1 = c->x[1], a2 = c->x[2], a3 = c->x[3];
    const Tag* t = tag_of(item);
    (void)user; g_ctx = c;
    if (g_adv.stage) { c->x[0] = 0; return; }          /* the lists are closing by themselves */
    if (g_ask_menu && ++g_ask_b < 4) { c->x[0] = 0; return; }          /* a prompt is up: B belongs to it (a fourth B gets through, should the prompt's end be missed) */
    g_ask_menu = 0;
    pend_drop(1);
    if (t && t->kind == TAG_SLOT) preview_end(c);
    else if (t && t->slot < g_own_count) preview_row(c, find_job(g_own[t->slot].key));
    c->x[0] = call(c, g_vt[2 + SLOT_BCALL], item, a1, a2, a3);
}

/* Create New Class itself: the list of placeholders, as a sub menu of the item it was opened from */
static uint64_t open_creator(Ctx* c, uint64_t item);
#define INPUT_GOT 0x216a4b0ull          /* [[main + this]] + 0x3b8: the buttons pressed this frame, as the menus read them */
static uint64_t open_creator(Ctx* c, uint64_t item) {
    uint64_t sub;
    uint32_t i;
    char line[120];
    char* at;
    collect_jobs();
    at = put_text(line, "Create New Class chosen: "); at = put_int(at, (uint64_t)g_job_count); at = put_text(at, " classes in the game's table, ");
    at = put_int(at, g_own_count); at = put_text(at, " placeholders in job.txt"); *at = 0;
    TRACE(line);
    if (!g_own_count || !(sub = open_sub(c, item))) {
        if (!g_own_count) g_api->log(g_api, 1, "no placeholder classes: this needs mod interface version 3 and this mod's job.txt");
        return 1;
    }
    {   /* the classes made, then one empty placeholder to make another; dismounted classes are reached from their mounted class */
        int spare = -1;
        g_top_count = 0;
        for (i = 0; i < g_own_count; ++i) {
            if (slot_dead(i) || g_own[i].no_mount) continue;
            if (slot_empty(i)) { if (spare < 0 && find_job(g_own[i].key)) spare = (int)i; continue; }
            g_top[g_top_count++] = (uint8_t)i;
        }
        if (spare >= 0) g_top[g_top_count++] = (uint8_t)spare;
    }
    if (g_top_count <= PAGE) {
        for (i = 0; i < g_top_count; ++i) { const uint64_t it = new_item(c, sub, g_vt_dyn); if (it) tag_set(it, TAG_SLOT, g_top[i], 0, 0, 0); }
    } else {
        for (i = 0; i * PAGE < g_top_count && i < 8; ++i) { const uint64_t it = new_item(c, sub, g_vt_dyn); if (it) tag_set(it, TAG_SPAGE, 0, (uint8_t)i, 0, (uint64_t)i * PAGE); }
    }
    return 10;
}

/* stands in slot 25 of an Information entry: the game's ACall, then our entry */

/* Main thread, once per frame, until the game's data is there: checks the build and points the five slots at us. */
/* The class change screen, once per frame. The vtable route (handlers 111 and 114 of facility::ClassChangeJobMenu) was never
 * reached in the user's game although the buttons arrived, so the menu in front is found from the process tree instead: from the
 * scene up to the root, then every process below it whose table has the menus' input function (slot 2 = 0x1713860). The button
 * with bit 4 of the pad word (no menu uses it: its handler is an empty one in the base menu) opens the creator from that menu's
 * current item; the menu is told that a child opened (0x17141e0(menu, 10)) as when one of its own items opens one. */
#define MENU_INPUT 0x1713860ull
#define MENU_ANSWER 0x17141e0ull
static void adv_step(Ctx* c) {
    uint64_t vt = 0;
    const int st = g_adv.stage;
    if (!st) return;
    if (g_frame - g_adv.at > 900) { g_adv.stage = 0; return; }          /* never got its turn: left alone */
    if (g_ask_menu || g_frame - g_adv.at < (st == 4 ? 24u : 10u)) return;          /* the prompt first, then a moment for each list */
    if (!g_api->is_function(g_base + MENU_ANSWER)) { g_adv.stage = 0; return; }
    if (!g_api->is_mapped(g_adv.menu[st - 1], 0x170) || !rd64(g_adv.menu[st - 1], &vt) || vt != g_adv.vt[st - 1]) { g_adv.stage = 0; return; }
    g_ctx = c;
    if (st < 4) {
        call(c, g_base + MENU_ANSWER, g_adv.menu[st - 1], 3, 0, 0);
        g_adv.stage = st + 1; g_adv.at = g_frame;
        return;
    }
    {
        const Tag* t = g_api->is_mapped(g_adv.top, ITEM_BYTES) && rd64(g_adv.top, &vt) && vt == (uint64_t)&g_vt_dyn[2] ? tag_of(g_adv.top) : 0;
        if (t && t->kind == TAG_SLOT && t->slot == g_adv.slot && !slot_empty(g_adv.slot)) {
            uint64_t r;
            g_adv_open = 1;
            r = call(c, g_vt_dyn[2 + SLOT_ACALL], g_adv.top, 0, 0, 0);
            g_adv_open = 0;
            if ((uint32_t)r == 10) call(c, g_base + MENU_ANSWER, g_adv.menu[3], 10, 0, 0);
        }
        g_adv.stage = 0;
    }
}
#define CREATOR_BIT 0x10u
static uint64_t g_walk_menu[24];
static uint8_t g_walk_depth[24];
static uint32_t g_walk_count, g_walk_nodes;
static void walk(uint64_t p, uint32_t depth) {
    uint32_t guard = 0;
    while (p > 0x10000 && g_api->is_mapped(p, 0x170) && guard++ < 256 && g_walk_nodes < 3000) {
        uint64_t vt = *(const uint64_t*)p, fn = 0, child = *(const uint64_t*)(p + 0x28);
        g_walk_nodes++;
        if (rd64(vt + 16, &fn) && (fn == g_base + MENU_INPUT || (g_menu_tick && fn == g_menu_tick)) && g_walk_count < 24) { g_walk_menu[g_walk_count] = p; g_walk_depth[g_walk_count++] = (uint8_t)depth; }
        if (child && depth < 40) walk(child, depth + 1);
        p = *(const uint64_t*)(p + 0x38);          /* the sibling made before this one */
    }
}
static void scene_frame(Ctx* c) {
    const int ctx_set = ((g_ctx = c), 1);
    static uint32_t last, told, opened;
    uint64_t sc = 0, p = 0, in = 0, top, up = 0, best = 0;
    uint32_t w, i, k, best_depth = 0;
    char line[240], name[48];
    char* at;
    (void)ctx_set;
    if (!rd64(g_base + SCENE_VAR, &sc) || sc < 0x10000 || !rd64(g_base + INPUT_GOT, &p) || !rd64(p, &in) || in < 0x10000 || !g_api->is_mapped(in, 0x3c0)) { last = 0; return; }
    {   /* requests under way */
        uint64_t pa = 0;
        if (g_api->is_mapped(sc, 0x200) && rd64(g_base + PARAM_VAR, &pa) && pa > 0x10000 && g_api->is_mapped(pa, 0x48)) {
            if (g_bounce.stage && g_bounce.sc == sc) {
                g_bounce.frames++;
                if ((g_bounce.frames >= 3 && !*(const uint8_t*)(sc + 0xf1)) || g_bounce.frames > 90) {
                    g_bounce.stage = 0;
                    *(volatile uint32_t*)(pa + 0x14) = g_bounce.idx;
                    *(volatile uint32_t*)(pa + 0x10) = g_bounce.rank;
                    scene_kick(c, sc);
                    if (g_diag_frame) scene_note("asked for the class again", sc, pa);
                }
            } else g_bounce.stage = 0;
            if (g_diag_frame) {
                const uint32_t f = g_diag_frame++;
                if (f == 1 || f == 2 || f == 4 || f == 8 || f == 16 || f == 32 || f == 60) scene_note(f == 1 ? "1 frame later" : f == 2 ? "2 frames later" : f == 4 ? "4 frames later" : f == 8 ? "8 frames later" : f == 16 ? "16 frames later" : f == 32 ? "32 frames later" : "60 frames later", sc, pa);
                if (f >= 60) g_diag_frame = 0;
            }
        }
    }
    w = *(const uint32_t*)(in + 0x3b8);
    if (w == last) return;
    last = w;
    if (!w) return;
    if (told < 48) { told++; at = put_text(line, "class change screen: buttons word "); at = put_hex(at, w); *at = 0; TRACE(line); }
    if (!(w & CREATOR_BIT) || !g_api->is_mapped(sc, 0x170)) return;
    for (top = sc, k = 0; k < 64 && rd64(top + 0x20, &up) && up > 0x10000 && g_api->is_mapped(up, 0x170); ++k) top = up;
    g_walk_count = 0; g_walk_nodes = 0;
    walk(top, 0);
    for (i = 0; i < g_walk_count; ++i) {
        const uint64_t m = g_walk_menu[i], item = *(const uint64_t*)(m + 0x150);
        const uint32_t state = *(const uint8_t*)(m + 0x160);
        const int usable = item > 0x10000 && g_api->is_mapped(item, 0x60) && !state && !(*(const uint8_t*)(m + 0x169) & 1);
        if (opened < 4) {
            uint64_t nm = 0;
            name[0] = 0;
            if (rd64(m + 0x18, &nm)) text_at(nm, name, sizeof name);
            at = put_text(line, "class change screen: menu "); at = put_text(at, name[0] ? name : "?"); at = put_text(at, ", table "); at = put_hex(at, *(const uint64_t*)m - g_base);
            at = put_text(at, ", depth "); at = put_int(at, g_walk_depth[i]); at = put_text(at, ", current item "); at = put_hex(at, item);
            if (item > 0x10000 && g_api->is_mapped(item, 8)) { at = put_text(at, " (table "); at = put_hex(at, *(const uint64_t*)item - g_base); *at++ = ')'; }
            at = put_text(at, ", state "); at = put_int(at, state); at = put_text(at, usable ? ", usable" : ", not usable"); *at = 0;
            TRACE(line);
        }
        if (usable && (!best || g_walk_depth[i] > best_depth)) { best = m; best_depth = g_walk_depth[i]; }
    }
    at = put_text(line, "class change screen: the creator's button: "); at = put_int(at, g_walk_nodes); at = put_text(at, " processes, "); at = put_int(at, g_walk_count);
    at = put_text(at, " menus, chosen "); at = put_hex(at, best); *at = 0;
    TRACE(line);
    opened++;
    if (!best) return;
    {
        const uint64_t item = *(const uint64_t*)(best + 0x150);
        uint64_t iv = 0;
        if (rd64(item, &iv) && (iv == (uint64_t)&g_vt_dyn[2] || iv == (uint64_t)&g_vt_cls[2] || iv == (uint64_t)&g_vt[2])) return;      /* already in the creator */
        if (open_creator(c, item) == 10 && g_api->is_function(g_base + MENU_ANSWER)) call(c, g_base + MENU_ANSWER, best, 10, 0, 0);
    }
}

/* ---- the class picture ----
 * The picture of a class (the list of the class change screen, the unit's sheet: game::gameui::control::JobIcon, 0x19defe0) is not
 * found by the row's icon name: the game takes the class key, cuts it at "下馬", adds "男" / "女" (and "_Clr") and looks that name up
 * among the textures of Layout/Textures/Icon_Job (0x13926f0 with kind 4, then 0x59e700). A created class has no texture under its
 * key, so the picture stays grey.
 * The textures of a loaded archive are a list in the layout library's texture container: a head {prev, next} at
 * [main + 0x2281910] + 0x4f0 + kind * 0x178 + 0xa0 and nodes {prev, next, name[0x80] at +0x10, texture info at +0x90, "owns its
 * texture" byte at +0x98}, 0xa0 bytes from the library's allocator (0x579180). When the container is taken down (0x59e100) only
 * the owning nodes finalize their texture; every node's info is destroyed and freed, and the node freed.
 * So a created class gets entries of its own: for every texture whose name starts with the key of the class whose icon it shows
 * (its base, unless another was chosen), a node named with the created class's key instead, not owning, with a copy of that
 * texture's info (so that the game's take-down frees a block of its own). Checked a few times a second: the archive is loaded and
 * dropped by the screens that use it, and the copies are made again and kept equal to their originals. */
#define TEX_MANAGER 0x2281910ull
#define UI_ALLOC 0x579180ull
#define UI_ALLOC_FN 0x216eef0ull
#define UI_ALLOC_ARG 0x21e38a8ull
#define TEXINFO_SMALL 0x2174360ull          /* GOT entries of the two texture info classes' tables: 0x18 and 0xd8 bytes */
#define TEXINFO_BIG 0x2174228ull
#define MAX_TEX 4096
static uint64_t g_tex_node[MAX_TEX];
static uint32_t g_icon_made, g_icon_told;
static uint64_t ui_alloc(Ctx* c, uint64_t size) {
    uint64_t fnp = 0, fn = 0, arg = 0, p;
    if (g_api->is_function(g_base + UI_ALLOC)) p = call(c, g_base + UI_ALLOC, size, 0, 0, 0);
    else {
        if (!rd64(g_base + UI_ALLOC_FN, &fnp) || !rd64(fnp, &fn) || !g_api->is_function(fn) || !rd64(g_base + UI_ALLOC_ARG, &arg)) return 0;
        p = call(c, fn, size, 4, arg, 0);
    }
    return p && g_api->is_mapped(p, size) ? p : 0;
}
static uint32_t texinfo_size(uint64_t info) {
    uint64_t vt = 0, a = 0, b = 0;
    if (!rd64(info, &vt) || !rd64(g_base + TEXINFO_SMALL, &a) || !rd64(g_base + TEXINFO_BIG, &b)) return 0;
    return vt == a + 0x10 ? 0x18 : vt == b + 0x10 ? 0xd8 : 0;
}
static int starts_with(const char* t, const char* pre) { while (*pre && *t == *pre) { t++; pre++; } return !*pre; }
static void cut_dismount(char* key) {          /* "JID_X下馬" -> "JID_X", as the game does */
    char* k;
    for (k = key; *k; ++k)
        if ((uint8_t)k[0] == 0xe4 && (uint8_t)k[1] == 0xb8 && (uint8_t)k[2] == 0x8b && (uint8_t)k[3] == 0xe9 && (uint8_t)k[4] == 0xa6 && (uint8_t)k[5] == 0xac) { *k = 0; return; }
}
static int g_icon_kind = -1;
static void icon_alias(Ctx* c, int only) {
    uint64_t mgr = 0;
    uint32_t kind, s;
    if (!g_own_count || !rd64(g_base + TEX_MANAGER, &mgr) || mgr < 0x10000 || !g_api->is_mapped(mgr + 0x4f0, 14 * 0x178)) return;
    if (!g_job_count) collect_jobs();
    for (kind = 0; kind < 14; ++kind) {
        const uint64_t head = mgr + 0x4f0 + (uint64_t)kind * 0x178 + 0xa0;
        uint64_t n = 0;
        uint32_t count = 0, i, any = 0;
        if (only >= 0 && (int)kind != only) continue;
        if (!rd64(head + 8, &n) || n < 0x10000 || n == head) continue;
        while (n != head && count < MAX_TEX) {
            if (!g_api->is_mapped(n, 0xa0)) { count = 0; break; }
            if (starts_with((const char*)(n + 0x10), "JID_")) any = 1;
            g_tex_node[count++] = n;
            n = m64(n + 8);
        }
        if (!count || !any || n != head) continue;          /* not a whole list, or not the class pictures */
        g_icon_kind = (int)kind;
        for (s = 0; s < g_own_count; ++s) {
            char src[JB_KEY_MAX + 8], own[JB_KEY_MAX + 8];
            const char* from = g_own[s].icon[0] ? g_own[s].icon : g_own[s].base;
            uint32_t own_len = 0, src_len = 0, found = count;
            char* e;
            if (!find_job(g_own[s].key) || !find_job(from) || same_text(from, g_own[s].key)) continue;
            e = put_text(src, from); *e = 0; cut_dismount(src);
            e = put_text(own, g_own[s].key); *e = 0; cut_dismount(own);
            while (own[own_len]) own_len++;
            while (src[src_len]) src_len++;
            if (same_text(src, own)) continue;
            for (i = 0; i < found; ++i) {
                const uint64_t sn = g_tex_node[i];
                const char* name = (const char*)(sn + 0x10);
                char want[0x80];
                uint64_t sinfo, tn = 0;
                uint32_t size, k, j;
                if (!starts_with(name, src)) continue;
                {   /* not from a picture made here (a created class's key may begin like the class it shows) */
                    uint32_t s2, mine = 0;
                    for (s2 = 0; s2 < g_own_count && !mine; ++s2) mine = (uint32_t)starts_with(name, g_own[s2].key);
                    if (mine) continue;
                }
                for (k = 0; name[src_len + k] && k < 0x7f; ++k) {}
                if (own_len + k >= 0x7f) continue;
                for (j = 0; j < own_len; ++j) want[j] = own[j];
                for (j = 0; j <= k; ++j) want[own_len + j] = j < k ? name[src_len + j] : 0;
                sinfo = m64(sn + 0x90);
                size = sinfo > 0x10000 && g_api->is_mapped(sinfo, 0xd8) ? texinfo_size(sinfo) : 0;
                if (!size) {
                    if (!(g_icon_told & 1)) { char line[160]; char* at = put_text(line, "class picture: a texture info of a kind not known here (table "); g_icon_told |= 1; at = put_hex(at, sinfo > 0x10000 && g_api->is_mapped(sinfo, 8) ? m64(sinfo) - g_base : 0); at = put_text(at, "): no picture for created classes"); *at = 0; g_api->log(g_api, 1, line); }
                    continue;
                }
                if (m64(sinfo + 8) == ~(uint64_t)0) continue;          /* not registered for drawing yet: later */
                for (j = 0; j < count && !tn; ++j) if (same_text((const char*)(g_tex_node[j] + 0x10), want)) tn = g_tex_node[j];
                if (tn) {          /* there already: one of ours is kept equal to its original */
                    const uint64_t tinfo = m64(tn + 0x90);
                    if (*(const uint8_t*)(tn + 0x98) == 0 && tinfo > 0x10000 && tinfo != sinfo && g_api->is_mapped(tinfo, size) && texinfo_size(tinfo) == size)
                        for (j = 0; j < size; ++j) if (((const uint8_t*)tinfo)[j] != ((const uint8_t*)sinfo)[j]) ((volatile uint8_t*)tinfo)[j] = ((const uint8_t*)sinfo)[j];
                    continue;
                }
                {
                    const uint64_t node = ui_alloc(c, 0xa0), info = node ? ui_alloc(c, size) : 0;
                    uint64_t tail;
                    if (!node || !info) { if (!(g_icon_told & 2)) { g_icon_told |= 2; g_api->log(g_api, 1, "class picture: the layout library's allocator gave no memory"); } return; }
                    for (j = 0; j < size; ++j) ((volatile uint8_t*)info)[j] = ((const uint8_t*)sinfo)[j];
                    for (j = 0; j < 0xa0; ++j) ((volatile uint8_t*)node)[j] = 0;
                    for (j = 0; want[j]; ++j) ((volatile uint8_t*)node)[0x10 + j] = (uint8_t)want[j];
                    *(volatile uint64_t*)(node + 0x90) = info;
                    tail = m64(head);          /* at the end of the list, as the library's own entries are added */
                    *(volatile uint64_t*)node = tail; *(volatile uint64_t*)(node + 8) = head;
                    *(volatile uint64_t*)(tail + 8) = node; *(volatile uint64_t*)head = node;
                    if (count < MAX_TEX) g_tex_node[count++] = node;
                    if (g_icon_made++ < 12) { char line[240]; char* at = put_text(line, "class picture: "); at = put_text(at, want); at = put_text(at, " shows "); at = put_text(at, name); at = put_text(at, " (texture list "); at = put_int(at, kind); *at++ = ')'; *at = 0; TRACE(line); }
                }
            }
        }
    }
}

/* ---- a character keeps its portrait ----
 * The portrait and face picture of a unit come from the asset rows' appearance name (+0x158, with its looked-up entry at +0x160:
 * "兵士男" -> FID_兵士男 in the appearance sheet = the helmeted generic soldier; a character's own row has its own, "ヨーカイ").
 * A class's main rows and its portrait rows carry the class's generic appearance. In the user's game a named character in a
 * created class showed that generic portrait, so for created classes the generic appearance is moved to the rows that only
 * generic soldiers match (class, sex, AID_モブ): characters never get an appearance from a created class. Done in memory once the
 * table is there, and again after a new base class was copied in. */
static void app_flag(uint64_t r) {          /* the row object's "has scale, appearance and the like" byte, by the game's rule */
    uint64_t d = 0;
    uint32_t any, o;
    if (!rd64(r + 0x18, &d) || d < 0x10000 || !g_api->is_mapped(d, 0x150)) return;
    any = *(const uint8_t*)(r + 0x38) | (uint32_t)str_set(r, 0xf0) | (uint32_t)str_set(r, 0x90) | (m64(r + 0x160) != 0) | (m64(r + 0x158) != 0);
    for (o = 0xf8; o < 0x108; ++o) any |= *(const uint8_t*)(r + o);
    *(volatile uint8_t*)(d + 0x14d) = any ? 1 : 0;
}
/* A row's references to other sheets are two words: the key's text, and the row it names, which the game fills in when it loads
 * the table. For the rows this mod adds the second word was found empty (the mount of a created class: null at +0x70, read
 * without a check at 0x96bbbc). A created class's empty second word is given the one of a class of the game that names the same
 * key. */
/* Class experience follows the rank, as in every class of the game: rank 1: 30 / 15, 2: 60 / 15, 3: 100 / 20, 4: 130 / 25,
 * 5: 160 / 30 (+0x19c what mastering the class takes, shown as "Next"; +0x19e the battle number). A created class gets its
 * rank's pair when its rank is set or changed, unless job.txt already says otherwise for that rank. */
static void exp_by_rank(uint32_t slot, int force) {
    static const uint16_t kExp[6][2] = {{0, 0}, {30, 15}, {60, 15}, {100, 20}, {130, 25}, {160, 30}};
    const uint64_t row = find_job(g_own[slot].key);
    uint32_t rank;
    char line[160];
    char* at;
    if (!row || (g_own[slot].has_cexp && !force)) return;
    rank = *(const uint8_t*)(row + 0x31);
    if (rank < 1 || rank > 5) return;
    *(volatile uint16_t*)(row + 0x19c) = kExp[rank][0]; *(volatile uint16_t*)(row + 0x19e) = kExp[rank][1];
    g_own[slot].cexp[0] = kExp[rank][0]; g_own[slot].cexp[1] = kExp[rank][1]; g_own[slot].has_cexp = 1;
    save_own();
    at = put_text(line, "class "); at = put_text(at, g_own[slot].key); at = put_text(at, ": class experience of rank "); at = put_int(at, rank); at = put_text(at, " ("); at = put_int(at, kExp[rank][0]); at = put_text(at, " / "); at = put_int(at, kExp[rank][1]); at = put_text(at, ")"); *at = 0;
    TRACE(line);
}
static void ref_fix(void) {
    static const uint16_t kRef[5] = {0x38, 0x68, 0x110, 0x1d8, 0x208};      /* support effect, mount, difficulty differences, class change item, weapon shown */
    uint32_t s, f, fixed = 0, open = 0;
    int i;
    char line[160], a[64], b[64];
    char* at;
    collect_jobs();
    if (exp_by_rank_later) { const int w = exp_by_rank_later - 1; exp_by_rank_later = 0; if (w < (int)g_own_count) exp_by_rank((uint32_t)w, 1); }
    for (s = 0; s < g_own_count; ++s) {
        const uint64_t row = find_job(g_own[s].key);
        if (!row) continue;
        exp_by_rank(s, 0);
        for (f = 0; f < 5; ++f) {
            const uint64_t text = *(const uint64_t*)(row + kRef[f]);
            if (!text || *(const uint64_t*)(row + kRef[f] + 8) || !text_at(text, a, sizeof a)) continue;
            open++;
            for (i = 0; i < g_job_count; ++i) {
                const uint64_t r2 = g_jobs[i], t2 = *(const uint64_t*)(r2 + kRef[f]), v2 = *(const uint64_t*)(r2 + kRef[f] + 8);
                if (r2 == row || !t2 || !v2 || !text_at(t2, b, sizeof b) || !same_text(a, b)) continue;
                *(volatile uint64_t*)(row + kRef[f] + 8) = v2;
                fixed++; open--;
                at = put_text(line, "class "); at = put_text(at, g_own[s].key); at = put_text(at, ": reference at +"); at = put_hex(at, kRef[f]); at = put_text(at, " ("); at = put_text(at, a); at = put_text(at, ") filled in, as the game's own classes have it"); *at = 0;
                TRACE(line);
                break;
            }
        }
    }
    { static uint32_t told; if (open && !fixed && !told) { told = 1; TRACE("a created class names a mount, item or effect that no class of the game has filled in: left empty"); } }
}
static void app_startup(void) {
    static uint32_t done_gen = 0xffffffffu;
    uint32_t s, i, moved = 0, cleared = 0;
    if (!find_asset_table() || (g_at_gen == done_gen && !g_app_redo)) return;
    done_gen = g_at_gen; g_app_redo = 0;
    for (s = 0; s < g_own_count; ++s) {
        uint64_t app[2][2] = {{0, 0}, {0, 0}}, ca = 0;
        uint32_t cn = 0;
        int pass;
        for (pass = 0; pass < 2; ++pass)
            for (i = 0; i < g_at_count; ++i) {
                const uint64_t r = at_row(i);
                int sex, mob;
                if (!m16(r + 0x20) || !m_group(r, g_own[s].key, &ca, &cn) || !m_own(r, g_own[s].key)) continue;
                sex = m_sex(ca, cn);
                mob = m_has(ca, cn, "AID_\xe3\x83\xa2\xe3\x83\x96");
                if (!pass && !mob && m64(r + 0x158)) {          /* a row characters match too: its appearance is taken off */
                    int s2;
                    for (s2 = 0; s2 < 2; ++s2) if ((sex == 2 || sex == s2) && !app[s2][0]) { app[s2][0] = m64(r + 0x158); app[s2][1] = m64(r + 0x160); }
                    *(volatile uint64_t*)(r + 0x158) = 0; *(volatile uint64_t*)(r + 0x160) = 0;
                    app_flag(r); cleared++;
                }
                if (pass && mob && !m64(r + 0x158) && sex < 2 && app[sex][0]) {          /* generic soldiers keep the class's */
                    *(volatile uint64_t*)(r + 0x158) = app[sex][0]; *(volatile uint64_t*)(r + 0x160) = app[sex][1];
                    app_flag(r); moved++;
                }
            }
    }
    if (cleared) { char line[160]; char* at = put_text(line, "portraits: the created classes' generic appearance taken off "); at = put_int(at, cleared); at = put_text(at, " rows and given to "); at = put_int(at, moved); at = put_text(at, " generic-soldier rows"); *at = 0; TRACE(line); }
}

static void glyph_patch(void);
static void on_frame(Ctx* c, void* user) {
    uint64_t cur = 0;
    int i;
    (void)user;
    /* the pictures' list every frame once it is known (a screen asks for a picture as soon as its archive is loaded), all lists now and then */
    { static uint32_t tick; if (g_hooked) { g_ctx = c; ++tick; if ((tick & 31) == 0) { icon_alias(c, -1); ref_fix(); app_startup(); wanim_startup(); } else if (g_icon_kind >= 0) icon_alias(c, g_icon_kind); } }
    ++g_frame;
    if (g_hooked) { glyph_patch(); scene_frame(c); prompt_watch(); adv_step(c); }
    if (g_hooked || g_failed) return;
    if (!rd64(g_base + VT_SHAPE, &cur) || !cur) return;       /* not relocated yet */
    {   /* the build check: the game's sub menu table (through its pointer table) has the menus' input function in slot 2 */
        uint64_t got = 0, fn = 0;
        if (!rd64(g_base + 0x216b6e8ull, &got) || !rd64(got + 0x10 + 2 * 8, &fn) || fn != g_base + MENU_INPUT || !g_api->is_function(fn)) goto wrong;
    }
    if (!g_api->is_mapped(g_base + VT_SHAPE - 16, sizeof g_vt) || !g_api->is_function(g_base + ALLOCATE) || !g_api->is_function(g_base + ADD_ITEM)) goto wrong;
    for (i = 0; i < 2 + SLOTS; ++i) g_vt[i] = ((const uint64_t*)(g_base + VT_SHAPE - 16))[i];
    for (i = 0; i < SLOTS; ++i)
        if (!g_api->is_function(g_vt[2 + i])) goto wrong;
    for (i = 0; i < 2 + SLOTS; ++i) g_vt_dyn[i] = g_vt_cls[i] = g_vt[i];
    g_vt_dyn[2 + SLOT_TEXT] = g_api->make_function(dyn_text, 0);
    g_vt_dyn[2 + SLOT_ACALL] = g_vt_cls[2 + SLOT_ACALL] = g_api->make_function(dyn_acall, 0);
    g_vt_cls[2 + SLOT_MID] = g_api->make_function(cls_mid, 0);
    g_vt_dyn[2 + SLOT_SELECT] = g_vt_cls[2 + SLOT_SELECT] = g_api->make_function(dyn_select, 0);
    g_vt_dyn[2 + SLOT_BCALL] = g_vt_cls[2 + SLOT_BCALL] = g_api->make_function(dyn_bcall, 0);
    if (!g_vt_dyn[2 + SLOT_SELECT] || !g_vt_dyn[2 + SLOT_BCALL]) goto nofn;
    { static int one = 1; g_vt_dyn[2 + SLOT_NEXT] = g_api->make_function(num_side, &one); g_vt_dyn[2 + SLOT_PREV] = g_api->make_function(num_side, 0); }
    if (!g_vt_dyn[2 + SLOT_NEXT] || !g_vt_dyn[2 + SLOT_PREV]) goto nofn;
    if (!g_vt_dyn[2 + SLOT_TEXT] || !g_vt_dyn[2 + SLOT_ACALL] || !g_vt_cls[2 + SLOT_MID]) goto nofn;
    if (!g_api->is_function(g_base + MAKE_SUB)) goto wrong;
    prompt_setup();
    g_hooked = 1;
    g_api->log(g_api, 2, g_trace ? "ready: left stick click on the Class Change screen opens the creator (debug trace on)" : "ready: left stick click on the Class Change screen opens the creator");
    return;
wrong:
    g_failed = 1;
    g_api->log(g_api, 1, "this is not the game build CreateNewClass was written for (update 131072): not active");
    return;
nofn:
    g_failed = 1;
    g_api->log(g_api, 1, "the runtime had no room for this mod's functions: not active");
}

/* ---- class rows from text (mod interface version 3) ----
 * When the game's file system is set up, GameData/Job.bin is handed to job_edit as it stands (the game's file, or another mod's).
 * Every installed mod folder may hold a job.txt: each "[JID_Key]" entry becomes a new class row at the end of the table, a copy of
 * the row named by "base = JID_..." (the game's "none" class when absent). A job.ini with "enabled = false" switches a folder's
 * job.txt off. This mod's own job.txt holds the placeholder classes the creator fills in while the game runs. */
#define MAX_ENTRIES 128
static JbEntry g_entries[MAX_ENTRIES];
static char g_text[1 << 16];

static void jb_log(const char* text, const char* arg) {
    char line[200];
    char* at = put_text(line, text);
    if (arg) { int n = 0; while (arg[n] && n < 80) *at++ = arg[n++]; }
    *at = 0;
    g_api->log(g_api, 1, line);
}

static JbEntry g_entries2[MAX_ENTRIES];      /* the entries as the asset rows are made from them (see asset_edit) */
static JbLook g_looks[MAX_ENTRIES];        /* asset.txt of every mod folder, by entry */
static uint32_t g_entry_count;
static int g_gathered;

/* every installed mod folder's job.txt (unless its job.ini switches it off), read once */
static uint32_t gather_entries(void) {
    uint32_t count = 0, mods, i;
    char line[260];
    char* at;
    if (g_gathered) return g_entry_count;
    g_gathered = 1;
    mods = g_api->mod_count();
    for (i = 0; i < mods && count < MAX_ENTRIES; ++i) {
        const char* folder = g_api->mod_folder(i);
        char path[1024];
        int64_t n;
        uint32_t got;
        int len = 0;
        if (!folder) continue;
        while (folder[len]) len++;
        if (len > 1000) continue;
        at = put_text(path, folder); at = put_text(at, "/job.ini"); *at = 0;
        n = g_api->read_file(path, g_text, sizeof g_text);
        if (n > 0 && n <= (int64_t)sizeof g_text && !jb_enabled(g_text, (uint64_t)n)) {
            at = put_text(line, "job.txt switched off by job.ini in "); at = put_text(at, folder); *at = 0;
            g_api->log(g_api, 2, line);
            continue;
        }
        at = put_text(path, folder); at = put_text(at, "/job.txt"); *at = 0;
        n = g_api->read_file(path, g_text, sizeof g_text);
        if (n < 0) continue;
        if (n > (int64_t)sizeof g_text) { jb_log("job.txt is too large (64 KiB at most), not read: ", folder); continue; }
        got = jb_parse(g_text, (uint64_t)n, g_entries + count, MAX_ENTRIES - count);
        at = put_text(line, "job.txt: "); at = put_int(at, got); at = put_text(at, " class entries from ");
        { int k = 0; while (folder[k] && k < 180) *at++ = folder[k++]; }
        *at = 0;
        g_api->log(g_api, 2, line);
        count += got;
    }
    g_entry_count = count;
    for (i = 0; i < mods && count; ++i) {      /* asset.txt beside job.txt: what the created classes take from other classes */
        const char* folder = g_api->mod_folder(i);
        char path[1024];
        int64_t n;
        int len = 0;
        if (!folder) continue;
        while (folder[len]) len++;
        if (len > 1000) continue;
        at = put_text(path, folder); at = put_text(at, "/job.ini"); *at = 0;
        n = g_api->read_file(path, g_text, sizeof g_text);
        if (n > 0 && n <= (int64_t)sizeof g_text && !jb_enabled(g_text, (uint64_t)n)) continue;
        at = put_text(path, folder); at = put_text(at, "/asset.txt"); *at = 0;
        n = g_api->read_file(path, g_text, sizeof g_text);
        if (n > 0 && n <= (int64_t)sizeof g_text) jb_parse_looks(g_text, (uint64_t)n, g_entries, count, g_looks);
    }
    return count;
}

/* The internal names behind each class's look, for the menu: from the rows that apply to a class and one sex and nothing else
 * (the male row when there is one). Read from the table as the game or another mod supplies it, before this mod's changes. */
static void read_vals(const uint8_t* in, uint64_t size) {
    static const char sheet[] = "\xe3\x82\xa2\xe3\x82\xbb\xe3\x83\x83\xe3\x83\x88\xe3\x83\x86\xe3\x83\xbc\xe3\x83\x96\xe3\x83\xab";
    uint64_t dsz, np, nl, lt, st, hdr = 0, rows, rcount, i, k;
    if (size < 0x20 || jb_u32(in) != size) return;
    dsz = jb_u32(in + 4); np = jb_u32(in + 8); nl = jb_u32(in + 12);
    lt = 0x20 + dsz + np * 8; st = lt + nl * 8;
    if (st >= size) return;
    for (i = 0; i < nl; ++i) {
        const uint64_t name = jb_u32(in + lt + i * 8 + 4);
        if (st + name < size && jb_same((const char*)(in + st + name), sheet)) { hdr = jb_u32(in + lt + i * 8); break; }
    }
    if (!hdr || hdr + 0x30 > dsz) return;
    rcount = jb_u32(in + 0x20 + hdr + 0x10) >> 16;
    rows = jb_u64(in + 0x20 + hdr + 0x28);
    if (rows + rcount * 0x180 > dsz) return;
#define RV_STR(v) ((v) >= dsz && (v) < size - 0x20 ? (const char*)(in + 0x20 + (v)) : "")
    for (i = 0; i < rcount; ++i) {
        const uint64_t r = rows + i * 0x180, arr = jb_u64(in + 0x20 + r + 0x28);
        const char *a, *b, *key;
        ClassVals* cv = 0;
        int male;
        uint32_t f;
        if ((jb_u64(in + 0x20 + r + 0x20) & 0xffff) != 1 || arr + 16 > dsz || (jb_u64(in + 0x20 + arr) & 0xffff) != 2) continue;
        k = jb_u64(in + 0x20 + arr + 8);
        if (k + 16 > dsz) continue;
        a = RV_STR(jb_u64(in + 0x20 + k)); b = RV_STR(jb_u64(in + 0x20 + k + 8));
        if (jb_is_class(b)) { key = b; b = a; } else key = a;
        if (!jb_is_class(key)) continue;
        male = same_text(b, "\xe7\x94\xb7");
        if (!male && !same_text(b, "\xe5\xa5\xb3")) continue;
        for (k = 0; k < g_val_count; ++k)
            if (same_text(g_vals[k].key, key)) { cv = &g_vals[k]; break; }
        if (cv && !male) continue;          /* the male row is the one shown */
        if (!cv) {
            int n = 0;
            if (g_val_count >= 192) continue;
            cv = &g_vals[g_val_count++];
            while (key[n] && n < JB_KEY_MAX - 1) { cv->key[n] = key[n]; n++; }
            cv->key[n] = 0;
        }
        for (f = 0; f < JB_NFIELD; ++f) {
            const uint64_t off = r + kJbField[f][0].off;
            const char* v = "";
            int n = 0;
            if (kJbFieldIsString[f]) v = RV_STR(jb_u64(in + 0x20 + off));
            else if (f == JB_ANIM || f == JB_ACCESSORY) {          /* the first of the list */
                const uint64_t p = jb_u64(in + 0x20 + off + 8);
                if ((jb_u64(in + 0x20 + off) & 0xffff) && p + 8 <= dsz) v = RV_STR(jb_u64(in + 0x20 + p));
            }
            while (v[n] && n < 31) { cv->val[f][n] = v[n]; n++; }
            cv->val[f][n] = 0;
        }
    }
#undef RV_STR
}

static void job_edit(const NxModFile* file, void* user) {
    const uint32_t count = gather_entries();
    uint64_t out_size = 0;
    uint8_t* out;
    char line[160];
    char* at;
    (void)user;
    if (!count) return;
    out = jb_add(file->data, file->size, g_entries, count, &out_size, g_api->alloc, jb_log);
    if (!out) { g_api->log(g_api, 1, "no class rows were added to Job.bin"); return; }
    file->replace(file, out, out_size);
    g_api->free_mem(out);
    at = put_text(line, "Job.bin: class rows added from text, "); at = put_int(at, file->size); at = put_text(at, " -> ");
    at = put_int(at, out_size); at = put_text(at, " bytes"); *at = 0;
    g_api->log(g_api, 2, line);
}

/* AssetTable.bin: a class that is a copy of another gets that class's models and animations, in three steps (jobbin.h): its
 * condition set (the extra conditions the base class brings) as a row of the 条件セット sheet; the base's own asset rows pasted as
 * rows of the new class's own, each straight after its original (these are what the creator will edit); and, in the rows of
 * characters and situations that mention the base, a twin condition naming the new class. A class whose base is the empty class has
 * nothing to copy. */
typedef struct SpellRows { JbCast* casts; uint32_t* n; uint32_t max; const char* key; } SpellRows;
static void spell_row(const char* cond, const char* effect, void* user) {
    SpellRows* sr = (SpellRows*)user;
    JbCast* c2;
    uint32_t x;
    if (jb_same(effect, "Rod_Reskia")) return;          /* named by the table, absent from the spell file */
    if (!bp_wanted(effect) || *sr->n >= sr->max || jb_len(cond) >= 48 || jb_len(effect) >= 48) return;
    c2 = &sr->casts[(*sr->n)++];
    for (x = 0; sr->key[x] && x < JB_KEY_MAX - 1; ++x) c2->key[x] = sr->key[x];
    c2->key[x] = 0; c2->hand[0] = 0;
    for (x = 0; effect[x]; ++x) c2->effect[x] = effect[x];
    c2->effect[x] = 0; c2->effect[0] = 'Z';
    for (x = 0; cond[x]; ++x) c2->cond[x] = cond[x];
    c2->cond[x] = 0;
}
static void asset_edit(const NxModFile* file, void* user) {
    const uint32_t count = gather_entries();
    uint64_t size1 = 0, size2 = 0, size3 = 0, cur_size;
    uint32_t groups = 0, pasted = 0, k;
    uint8_t *sets, *own, *out, *cast = 0;
    const uint8_t* cur;
    char line[200];
    char* at;
    (void)user;
    read_vals(file->data, file->size);
    if (!count) return;
    sets = jb_add_rows(file->data, file->size, "\xe6\x9d\xa1\xe4\xbb\xb6\xe3\x82\xbb\xe3\x83\x83\xe3\x83\x88", 0x40, 1, g_entries, count, &size1, g_api->alloc, jb_log);
    cur = sets ? sets : file->data; cur_size = sets ? size1 : file->size;
    /* a class with no base yet still gets rows of its own (the sword fighter's, as a start), so that a base and a look chosen in
     * the menu have rows to change at once */
    for (k = 0; k < count; ++k) {
        g_entries2[k] = g_entries[k];
        if (same_text(g_entries2[k].base, "JID_\xe7\x84\xa1\xe3\x81\x97")) { char* b = put_text(g_entries2[k].base, "JID_\xe5\x89\xa3\xe5\xa3\xab"); *b = 0; }
    }
    own = jb_asset_own(cur, cur_size, g_entries2, g_looks, count, &size2, &pasted, g_api->alloc, jb_log);
    if (own) { cur = own; cur_size = size2; }
    out = jb_asset_groups(cur, cur_size, g_entries, count, 1, &size3, &groups, g_api->alloc, jb_log);
    if (out) { cur = out; cur_size = size3; }
    {   /* rows for the created classes' black magic, at the end of the table */
        static JbCast casts[1024];
        uint32_t nc = 0;
        uint64_t size4 = 0;
        for (k = 0; k < g_own_count; ++k)          /* a row per spell: the class uses the copy that starts at the hand (bprop.h) */
            if (g_cast_spells[k]) { SpellRows sr; sr.casts = casts; sr.n = &nc; sr.max = 1024; sr.key = g_own[k].key; jb_asset_spell_rows(cur, cur_size, spell_row, &sr); }
        for (k = 0; k < g_own_count; ++k)          /* after them, so a chosen effect wins for black magic */
            if ((g_cast[k].hand[0] || g_cast[k].effect[0]) && nc < 1024) { char* e2 = put_text(g_cast[k].key, g_own[k].key); *e2 = 0; g_cast[k].cond[0] = 0; casts[nc++] = g_cast[k]; }
        { char l3[160]; char* a3 = put_text(l3, "asset.txt: casting rows to add: "); a3 = put_int(a3, nc); a3 = put_text(a3, ""); if (nc) { a3 = put_text(a3, ", first "); a3 = put_text(a3, casts[0].key); a3 = put_text(a3, ": hand '"); a3 = put_text(a3, casts[0].hand); a3 = put_text(a3, "', effect '"); a3 = put_text(a3, casts[0].effect); *a3++ = '\''; } *a3 = 0; g_api->log(g_api, 2, l3); }
        if (nc) {
            cast = jb_asset_cast(cur, cur_size, casts, nc, &size4, g_api->alloc, jb_log);
            if (cast) { char l2[120]; char* a2 = put_text(l2, "AssetTable.bin: "); a2 = put_int(a2, nc); a2 = put_text(a2, " rows for the created classes' black magic (held model, spell effect)"); *a2 = 0; g_api->log(g_api, 2, l2); cur = cast; cur_size = size4; }
        }
    }
    at = put_text(line, "AssetTable.bin: ");
    if (cur == file->data) at = put_text(at, "nothing to add (no class has a base with models)");
    else {
        file->replace(file, cur, cur_size);
        at = put_int(at, pasted); at = put_text(at, " rows of their own and "); at = put_int(at, groups);
        at = put_text(at, " shared conditions added for the new classes, ");
        at = put_int(at, file->size); at = put_text(at, " -> "); at = put_int(at, cur_size); at = put_text(at, " bytes");
    }
    *at = 0;
    g_api->log(g_api, 2, line);
    if (cast) g_api->free_mem(cast);
    if (out) g_api->free_mem(out);
    if (own) g_api->free_mem(own);
    if (sets) g_api->free_mem(sets);
}

/* ---- where tome spells start ----
 * GameData/Combat/CombatMagic.bprop has one entry per spell effect with a LaunchBoneName. Tomes and staves launch from
 * "Loc_Reserve1", a spare point that only the casting animations move into place; thrown weapons and arrows from "Loc_R_Weapon"
 * (the right hand's weapon point), the magic swords from "Loc_TrailTip" (on the weapon model). A class that casts with another
 * animation (the sword lift) leaves Loc_Reserve1 where it rests, on the ground under the unit.
 * A file magic_launch.txt beside job.txt naming a point of the same length (12 characters: Loc_R_Weapon, Loc_L_Weapon,
 * Loc_TrailTip, Loc_C_Center) makes every "Loc_Reserve1" in the file that point: for every caster in the game. */
static char g_launch[16];
static void magic_edit(const NxModFile* file, void* user) {
    static const char kOld[] = "Loc_Reserve1";
    uint8_t* out;
    uint64_t i, k, n = 0;
    char line[160];
    char* at;
    (void)user;
    {   /* copies of the tome and staff spells for the classes with cast_spells = hand */
        uint32_t any = 0, made = 0;
        uint64_t os = 0;
        uint8_t* o2;
        for (i = 0; i < g_own_count; ++i) any |= g_cast_spells[i];
        if (any && (o2 = bp_class_copies(file->data, file->size, &os, &made, g_api->alloc, jb_log)) != 0) {
            file->replace(file, o2, os);
            at = put_text(line, "CombatMagic.bprop: "); at = put_int(at, made); at = put_text(at, " spells copied to start at the right hand, for classes with cast_spells = hand"); *at = 0;
            g_api->log(g_api, 2, line);
            g_api->free_mem(o2);
            return;          /* not together with magic_launch.txt (which is for every caster) */
        }
    }
    if (!g_launch[0] || file->size < 64 || !(out = (uint8_t*)g_api->alloc(file->size))) return;
    for (i = 0; i < file->size; ++i) out[i] = file->data[i];
    for (i = 2; i + 13 <= file->size; ++i) {
        if (out[i - 2] != 13 || out[i - 1] != 0 || out[i + 12] != 0) continue;          /* a 12-character name with its length in front */
        for (k = 0; k < 12 && out[i + k] == (uint8_t)kOld[k]; ++k) {}
        if (k < 12) continue;
        for (k = 0; k < 12; ++k) out[i + k] = (uint8_t)g_launch[k];
        n++;
    }
    if (n) file->replace(file, out, file->size);
    at = put_text(line, "CombatMagic.bprop: "); at = put_int(at, n); at = put_text(at, " uses of Loc_Reserve1 now "); at = put_text(at, g_launch); at = put_text(at, " (magic_launch.txt): spells start there for every caster"); *at = 0;
    g_api->log(g_api, n ? 2 : 1, line);
    g_api->free_mem(out);
}
/* The glyph (magic circle) of a cast is not in that file: main has a table of the four glyph effects (main + 0x204a418:
 * Magic_Circle, Magic_CircleWhite, Magic_CircleDark, Magic_CircleConron) followed by the point each is put at (+0x204a438:
 * Loc_Reserve1, Loc_Reserve1, Loc_C_Trans, Loc_Reserve1). With magic_launch.txt the entries that say Loc_Reserve1 are pointed at
 * the chosen name as well (the dark glyph's ground point is left as the game has it). */
#define GLYPH_BONES 0x204a438ull
#define STR_RESERVE1 0x1afa0c2ull
static void glyph_patch(void) {
    static int done;
    uint64_t v = 0;
    uint32_t k, n = 0;
    char line[160];
    char* at;
    if (done || !g_launch[0]) return;
    if (!rd64(g_base + GLYPH_BONES, &v) || !v) return;          /* not relocated yet */
    done = 1;
    if (!g_api->make_writable(g_base + GLYPH_BONES, 32)) { g_api->log(g_api, 1, "glyph: the game's table of glyph points could not be made writable"); return; }
    for (k = 0; k < 4; ++k)
        if (m64(g_base + GLYPH_BONES + k * 8) == g_base + STR_RESERVE1) { *(volatile uint64_t*)(g_base + GLYPH_BONES + k * 8) = (uint64_t)g_launch; n++; }
    at = put_text(line, "glyph: "); at = put_int(at, n); at = put_text(at, " of the 4 casting glyphs moved from Loc_Reserve1 to "); at = put_text(at, g_launch);
    if (!n) { at = put_text(at, " (the table is not as expected: first entry "); at = put_hex(at, v - g_base); *at++ = ')'; }
    *at = 0;
    g_api->log(g_api, n ? 2 : 1, line);
}
static void magic_setup(const NxModApi* api, const char* mod_dir) {
    char path[1100], text[64];
    char* p2 = put_text(path, mod_dir);
    int64_t n;
    uint32_t k = 0, a = 0;
    p2 = put_text(p2, "/magic_launch.txt"); *p2 = 0;
    if (!api->edit_file("GameData/Combat/CombatMagic.bprop", magic_edit, 0)) api->log(api, 1, "CombatMagic.bprop could not be registered for editing");
    n = api->read_file(path, text, sizeof text - 1);
    if (n <= 0) return;
    if (n > (int64_t)sizeof text - 1) n = sizeof text - 1;
    text[n] = 0;
    if ((uint8_t)text[0] == 0xef && (uint8_t)text[1] == 0xbb && (uint8_t)text[2] == 0xbf) a = 3;
    while (text[a] == ' ' || text[a] == '\t' || text[a] == '\r' || text[a] == '\n') a++;
    while (k < 15 && ((text[a + k] >= 'A' && text[a + k] <= 'Z') || (text[a + k] >= 'a' && text[a + k] <= 'z') || (text[a + k] >= '0' && text[a + k] <= '9') || text[a + k] == '_')) { g_launch[k] = text[a + k]; k++; }
    g_launch[k] = 0;
    if (k != 12) { g_launch[0] = 0; api->log(api, 1, "magic_launch.txt: the name must have 12 characters (Loc_R_Weapon, Loc_L_Weapon, Loc_TrailTip, Loc_C_Center): not used"); return; }
}
/* called as soon as the DLL is loaded (version 3 runtimes only): the game is not in memory yet */
/* A class's own name and help text go into the game's class message file as it is set up: "M<key>" and "MJID_H_<key without JID_>",
 * the ids jobbin.h writes into the class's row. The file is zstd-compressed in the game's files. */
static void msg_edit(const NxModFile* file, void* user) {
    static MbNew add[2 * MAX_SLOTS + 2];
    static char label[MAX_SLOTS][2][80];
    const uint32_t count = gather_entries();
    uint32_t k, n = 0, added = 0;
    uint64_t cap = file->size * 24 + 65536, out_size = 0;
    int64_t raw_size, packed;
    uint8_t *raw, *out, *zip;
    char line[200];
    char* at;
    (void)user;
    for (k = 0; k < count && n + 2 <= 2 * MAX_SLOTS; ++k) {
        const JbEntry* e = &g_entries[k];
        char* w;
        uint64_t len = 0;
        if (!e->name[0] || !e->key[0]) continue;
        while (e->key[len]) len++;
        if (len > 60) continue;
        w = label[n / 2][0]; *w++ = 'M'; w = put_text(w, e->key); *w = 0;
        add[n].label = label[n / 2][0]; add[n].text = e->name; add[n].like = "MJID_SwordMaster"; n++;
        w = put_text(label[n / 2][1], "MJID_H_"); w = put_text(w, len > 4 ? e->key + 4 : e->key); *w = 0;
        add[n].label = label[n / 2][1]; add[n].text = e->help[0] ? e->help : e->name; add[n].like = "MJID_H_SwordMaster"; n++;
    }
    /* what the game's prompt asks when a class is made or removed (looked up by these ids) */
    add[n].label = "MCNC_Create"; add[n].like = "MJID_H_SwordMaster";
    add[n].text = "Create this class?\nA saved game with a unit in a created class\nneeds this mod and its class files to load.\nKeep a spare save slot without such units."; n++;
    add[n].label = "MCNC_Remove"; add[n].like = "MJID_H_SwordMaster";
    add[n].text = "Remove this class?\nA saved game with a unit in it will no longer load:\nmove those units to another class and save first.\nThe class is gone after a restart."; n++;
    raw = (uint8_t*)g_api->alloc(cap);
    if (!raw) return;
    raw_size = g_api->zstd_decompress(file->data, file->size, raw, cap);
    if (raw_size <= 0) { g_api->log(g_api, 1, "the class message file could not be unpacked: created classes keep their base's name"); g_api->free_mem(raw); return; }
    out = mb_add(raw, (uint64_t)raw_size, add, n, &out_size, &added, g_api->alloc);
    g_api->free_mem(raw);
    if (!out) { g_api->log(g_api, 1, "the class message file is not as expected, or the names are in it already: nothing added"); return; }
    zip = (uint8_t*)g_api->alloc(out_size + 4096);
    packed = zip ? g_api->zstd_compress(out, out_size, zip, out_size + 4096, 3) : -1;
    if (packed > 0) {
        file->replace(file, zip, (uint64_t)packed);
        at = put_text(line, file->path); at = put_text(at, ": "); at = put_int(at, added); at = put_text(at, " messages added (created classes' names, the two prompts)"); *at = 0;
        g_api->log(g_api, 2, line);
    } else g_api->log(g_api, 1, "the class message file could not be packed again: created classes keep their base's name");
    if (zip) g_api->free_mem(zip);
    g_api->free_mem(out);
}
/* A placeholder that was given a name gets a key from it, once: "name = Sky Lancer" turns [JID_Custom01] into [JID_SkyLancer] (the
 * name's letters and digits). job.txt and asset.txt are rewritten with the new key before anything reads them. Units that are in
 * the class in a saved game are stored under the old key. */
/* The files that hold the created classes are kept safe at the start, before anything reads them. A saved game with a unit in a
 * created class does not load without that class (the unit's class is looked up by key and not found: seen as a crash at
 * 0x6c963c), so job.txt must not be lost to a new copy of this mod:
 *   - job.txt is not part of the mod's package; when there is none, the ten empty placeholders are written;
 *   - while job.txt holds a class that was made, it and asset.txt are copied to job.txt.bak / asset.txt.bak at every start;
 *   - a job.txt with only empty placeholders beside a .bak with made classes is the sign of an overwritten file: the .bak is
 *     put back (delete job.txt.bak to start over on purpose). */
static char g_bak[1 << 16];
static int made_classes(const char* text, uint64_t n) {
    static JbEntry e[MAX_SLOTS];
    const uint32_t c = jb_parse(text, n, e, MAX_SLOTS);
    uint32_t i;
    for (i = 0; i < c; ++i) {
        static const char pre[] = "JID_Custom";
        uint32_t k = 0;
        while (pre[k] && e[i].key[k] == pre[k]) k++;
        if (pre[k] || !same_text(e[i].base, "JID_\xe7\x84\xa1\xe3\x81\x97")) return 1;
    }
    return 0;
}
static void files_guard(const NxModApi* api) {
    static const char kDefault[] =
        "# Placeholder classes for Create New Class. Each entry becomes a class at the end of the game's table.\n"
        "# base = the class a new entry starts as a copy of (JID_\xe7""\x84""\xa1""\xe3""\x81""\x97"" is the game's empty \"none\" class).\n"
        "\n"
        "[JID_Custom01]\n"
        "base = JID_\xe7""\x84""\xa1""\xe3""\x81""\x97""\n"
        "\n"
        "[JID_Custom02]\n"
        "base = JID_\xe7""\x84""\xa1""\xe3""\x81""\x97""\n"
        "\n"
        "[JID_Custom03]\n"
        "base = JID_\xe7""\x84""\xa1""\xe3""\x81""\x97""\n"
        "\n"
        "[JID_Custom04]\n"
        "base = JID_\xe7""\x84""\xa1""\xe3""\x81""\x97""\n"
        "\n"
        "[JID_Custom05]\n"
        "base = JID_\xe7""\x84""\xa1""\xe3""\x81""\x97""\n"
        "\n"
        "[JID_Custom06]\n"
        "base = JID_\xe7""\x84""\xa1""\xe3""\x81""\x97""\n"
        "\n"
        "[JID_Custom07]\n"
        "base = JID_\xe7""\x84""\xa1""\xe3""\x81""\x97""\n"
        "\n"
        "[JID_Custom08]\n"
        "base = JID_\xe7""\x84""\xa1""\xe3""\x81""\x97""\n"
        "\n"
        "[JID_Custom09]\n"
        "base = JID_\xe7""\x84""\xa1""\xe3""\x81""\x97""\n"
        "\n"
        "[JID_Custom10]\n"
        "base = JID_\xe7""\x84""\xa1""\xe3""\x81""\x97""\n"
        "\n";
    static char job[1100], asset[1100], jbak[1100], abak[1100];          /* static: the mod has no stack probe */
    char* p2;
    int64_t n, b;
    p2 = put_text(job, g_mod_dir); p2 = put_text(p2, "/job.txt"); *p2 = 0;
    p2 = put_text(asset, g_mod_dir); p2 = put_text(p2, "/asset.txt"); *p2 = 0;
    p2 = put_text(jbak, g_mod_dir); p2 = put_text(p2, "/job.txt.bak"); *p2 = 0;
    p2 = put_text(abak, g_mod_dir); p2 = put_text(p2, "/asset.txt.bak"); *p2 = 0;
    n = api->read_file(job, g_text, sizeof g_text);
    if (n > (int64_t)sizeof g_text) return;          /* too large to judge: left alone */
    if (n > 0 && made_classes(g_text, (uint64_t)n)) {          /* the usual start: keep a copy */
        if (!api->write_file(jbak, g_text, (uint64_t)n)) api->log(api, 1, "job.txt.bak could not be written: there is no spare copy of the created classes");
        b = api->read_file(asset, g_bak, sizeof g_bak);
        if (b > 0 && b <= (int64_t)sizeof g_bak) api->write_file(abak, g_bak, (uint64_t)b);
        return;
    }
    b = api->read_file(jbak, g_bak, sizeof g_bak);
    if (b > 0 && b <= (int64_t)sizeof g_bak && made_classes(g_bak, (uint64_t)b)) {          /* job.txt is gone or empty again: the copy comes back */
        if (api->write_file(job, g_bak, (uint64_t)b)) api->log(api, 1, n > 0 ? "job.txt held only empty placeholders but job.txt.bak has created classes: job.txt was put back from it (delete job.txt.bak to start over)"
                                                                           : "job.txt was missing: put back from job.txt.bak");
        n = api->read_file(asset, g_text, sizeof g_text);
        if (n <= 0) {
            b = api->read_file(abak, g_bak, sizeof g_bak);
            if (b > 0 && b <= (int64_t)sizeof g_bak && api->write_file(asset, g_bak, (uint64_t)b)) api->log(api, 1, "asset.txt was missing: put back from asset.txt.bak");
        }
        return;
    }
    if (n <= 0) {
        if (api->write_file(job, kDefault, sizeof kDefault - 1)) api->log(api, 2, "job.txt was missing: written with ten empty placeholders");
        else api->log(api, 1, "job.txt is missing and could not be written: no classes can be made");
    }
}
static void rename_named(const NxModApi* api, const char* mod_dir) {
    char path[1100];
    char* p2;
    int64_t n;
    uint32_t i, k, changed = 0;
    for (i = 0; mod_dir && mod_dir[i] && i < 1000; ++i) g_mod_dir[i] = mod_dir[i];
    g_mod_dir[i] = 0;
    if (!g_mod_dir[0]) return;
    files_guard(api);
    p2 = put_text(path, g_mod_dir); p2 = put_text(p2, "/job.txt"); *p2 = 0;
    n = api->read_file(path, g_text, sizeof g_text);
    if (n <= 0 || n > (int64_t)sizeof g_text) return;
    g_own_count = jb_parse(g_text, (uint64_t)n, g_own, MAX_SLOTS);
    p2 = put_text(path, g_mod_dir); p2 = put_text(p2, "/asset.txt"); *p2 = 0;
    n = api->read_file(path, g_text, sizeof g_text);
    if (n > 0 && n <= (int64_t)sizeof g_text) { jb_parse_looks(g_text, (uint64_t)n, g_own, g_own_count, g_own_look); wanim_parse(g_text, (uint64_t)n); }
    /* Keys are no longer made from names (stages 15 to 48 did that at the start after a name was given): a saved game keeps a
     * unit's class by its key, and a class can now be named at any time in the menu, so a key must never change once made.
     * Keys that were renamed before stay as they are. */
    /* Spare placeholders: a class's row has to be in the game's table from the start, so there are always a few empty ones to
     * make a class from (a mounted class takes a second entry for its dismounted class, added when it is generated). */
    {
        uint32_t empty = 0;
        for (i = 0; i < g_own_count; ++i) empty += g_own[i].key[0] && !g_own[i].no_mount && slot_empty(i);
        while (empty < 4 && g_own_count < MAX_SLOTS - 8) {
            JbEntry* e = &g_own[g_own_count];
            uint32_t num;
            char* w;
            for (k = 0; k < sizeof *e; ++k) ((uint8_t*)e)[k] = 0;
            for (num = 1; num < 100; ++num) {
                w = put_text(e->key, "JID_Custom"); *w++ = (char)('0' + num / 10); *w++ = (char)('0' + num % 10); *w = 0;
                for (k = 0; k < g_own_count && !same_text(g_own[k].key, e->key); ++k) {}
                if (k == g_own_count) break;
            }
            if (num == 100) { e->key[0] = 0; break; }
            w = put_text(e->base, "JID_\xe7\x84\xa1\xe3\x81\x97"); *w = 0;
            for (k = 0; k < sizeof g_own_look[0]; ++k) ((uint8_t*)&g_own_look[g_own_count])[k] = 0;
            g_own_count++; empty++; changed++;
        }
    }
    if (changed) { save_own(); api->log(api, 2, "job.txt: empty placeholders added, so that there are four to make classes from"); }
}
__declspec(dllexport) int nxmod_early(const NxModApi* api, const char* mod_dir) {
    const char* id;
    if (!api || api->version < 3 || api->size < API_V3_SIZE) return 0;      /* an older runtime: the menu entry still works */
    id = api->game_id;
    if (!id || id[0] != 'f' || id[1] != 'e' || id[2] != 'f' || id[3] != 'w' || id[4] != 0) return 1;
    g_api = api;
    if (!api->edit_file("GameData/Job.bin", job_edit, 0)) api->log(api, 1, "Job.bin could not be registered for editing: no class rows from text");
    else api->log(api, 2, "registered for GameData/Job.bin: class rows from job.txt are added when the game's files are set up");
    api->edit_file("GameData/Person.bin", person_read, 0);          /* read only: the characters' name ids */
    magic_setup(api, mod_dir);
    api->edit_file("GameData/AnimSet.bin", animset_read, 0);        /* read only: which animation sets there are */
    api->edit_file("GameData/Skill.bin", skill_read, 0);            /* read only: the skills' name ids */
    rename_named(api, mod_dir);
    {   /* the class names, in every language folder the game may use */
        static const char* const lang[] = {"USen", "EUen", "JPja", "USfr", "EUfr", "USes", "EUes", "EUde", "EUit", "KRko", "CNzh", "TWzh"};
        static char path[12][48];
        uint32_t li;
        for (li = 0; li < 12; ++li) { char* w = put_text(path[li], "Msg/"); w = put_text(w, lang[li]); w = put_text(w, "/Job.msbt.zst"); *w = 0; api->edit_file(path[li], msg_edit, 0); }
    }
    if (!api->edit_file("GameData/AssetTable.bin", asset_edit, 0)) api->log(api, 1, "AssetTable.bin could not be registered for editing: new classes get no models");
    return 0;
}

__declspec(dllexport) int nxmod_init(const NxModApi* api, const char* mod_dir) {
    const char* id;
    if (!api || api->version < 1) return 1;
    if (api->version < 2) {
        api->log(api, 1, "CreateNewClass needs mod interface version 2 (an Obelisk Chamber build with make_function): not active");
        return 1;
    }
    id = api->game_id;
    if (!id || id[0] != 'f' || id[1] != 'e' || id[2] != 'f' || id[3] != 'w' || id[4] != 0) return 1;
    g_api = api;
    g_base = api->main_base;
    {
        int n = 0;
        while (mod_dir && mod_dir[n] && n < 1000) { g_mod_dir[n] = mod_dir[n]; n++; }
        g_mod_dir[n] = 0;
    }
    if (g_mod_dir[0]) {          /* job.ini: "debug = true" turns the detailed trace on */
        static char ini[4096], path[1100];
        char* p2 = put_text(path, g_mod_dir);
        int64_t n;
        uint64_t i;
        p2 = put_text(p2, "/job.ini"); *p2 = 0;
        n = api->read_file(path, ini, sizeof ini - 1);
        for (i = 0; n > 0 && i + 5 < (uint64_t)n; ++i)
            if ((i == 0 || ini[i - 1] == '\n') && ini[i] == 'd' && ini[i + 1] == 'e' && ini[i + 2] == 'b' && ini[i + 3] == 'u' && ini[i + 4] == 'g') {
                uint64_t v = i + 5;
                while (v < (uint64_t)n && (ini[v] == ' ' || ini[v] == '\t' || ini[v] == '=')) v++;
                g_trace = v < (uint64_t)n && (ini[v] == 't' || ini[v] == '1' || ini[v] == 'y' || (ini[v] == 'o' && v + 1 < (uint64_t)n && ini[v + 1] == 'n'));
            }
    }
    if (api->version >= 3 && g_mod_dir[0]) {      /* this mod's own placeholders */
        char path[1100];
        char* p2 = put_text(path, g_mod_dir);
        int64_t n;
        p2 = put_text(p2, "/job.txt"); *p2 = 0;
        n = api->read_file(path, g_text, sizeof g_text);
        if (n > 0 && n <= (int64_t)sizeof g_text) g_own_count = jb_parse(g_text, (uint64_t)n, g_own, MAX_SLOTS);
        p2 = put_text(path, g_mod_dir); p2 = put_text(p2, "/asset.txt"); *p2 = 0;
        n = api->read_file(path, g_text, sizeof g_text);
        if (n > 0 && n <= (int64_t)sizeof g_text) { jb_parse_looks(g_text, (uint64_t)n, g_own, g_own_count, g_own_look); wanim_parse(g_text, (uint64_t)n); }
    }
    api->on_frame(on_frame, 0);
    {
        char line[160];
        char* at = put_text(line, "Create New Class 1.0 loaded; the runtime's mod interface is version ");
        at = put_int(at, api->version);
        at = put_text(at, api->version >= 3 ? "" : " (version 3 is needed for class rows from job.txt: this build of the game does not have it)");
        *at = 0;
        api->log(api, api->version >= 3 ? 2 : 1, line);
    }
    return 0;
}
