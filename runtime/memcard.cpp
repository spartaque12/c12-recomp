// Memory card: a real 128 KB card image (saves/card1.mcd, the raw format every emulator reads).
// The game's card library uses two routes and both end here:
//   - the BIOS file API ("bu00:NAME": open / read / write / firstfile / delete ...), and
//   - raw sector access (_card_read / _card_write) to edit the card's directory itself.
// So the file API below is a small implementation of the real card file system on that image.
#include "psx.h"
#include <direct.h>
#include "savestate.h"

static const char* SAVEDIR = "saves";
static const char* CARDFILE = "saves/card1.mcd";
#define CARD_SIZE 131072
#define MAXFD 16
static u8 g_card[CARD_SIZE];
static bool g_loaded;
struct CardFd { bool used; int first; u32 size, pos; bool nowait; };
static CardFd g_fd[MAXFD];
static std::string g_find_pat;
static u32 g_find_dirent;
static int g_find_i;

static inline u8 rd8(u32 a) { return (u8)mem_r8(&g_ctx, a); }
static inline void wr8(u32 a, u8 v) { mem_w8(&g_ctx, a, v); }

static std::string gstr(u32 a) {
    std::string s;
    for (int i = 0; i < 128; i++) {
        u8 ch = rd8(a + i);
        if (!ch) break;
        s.push_back((char)ch);
    }
    return s;
}

// ---- card image ---------------------------------------------------------------------------------
static void cks(u8* frame) {
    u8 x = 0;
    for (int i = 0; i < 127; i++) x ^= frame[i];
    frame[127] = x;
}
static u8* dirent(int i) { return g_card + (i + 1) * 128; }               // 15 entries, one per data block
static u32 get32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
static void put32(u8* p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static int next_of(int i) { int n = dirent(i)[8] | (dirent(i)[9] << 8); return n < 15 ? n : -1; }

static void free_entry(int i) {
    u8* d = dirent(i);
    memset(d, 0, 128);
    put32(d, 0xA0);
    d[8] = d[9] = 0xFF;
    cks(d);
}

static void card_format() {
    memset(g_card, 0, CARD_SIZE);
    g_card[0] = 'M'; g_card[1] = 'C';
    cks(g_card);
    for (int i = 0; i < 15; i++) free_entry(i);
    for (int f = 16; f < 36; f++) {            // broken-sector list: none
        u8* p = g_card + f * 128;
        put32(p, 0xFFFFFFFFu);
        p[8] = p[9] = 0xFF;
        cks(p);
    }
    memcpy(g_card + 63 * 128, g_card, 128);
}

static void card_flush() {
    _mkdir(SAVEDIR);
    FILE* f = fopen(CARDFILE, "wb");
    if (!f) { logf("memcard: cannot write %s\n", CARDFILE); return; }
    fwrite(g_card, 1, CARD_SIZE, f);
    fclose(f);
}

static void card_load() {
    if (g_loaded) return;
    g_loaded = true;
    FILE* f = fopen(CARDFILE, "rb");
    size_t n = f ? fread(g_card, 1, CARD_SIZE, f) : 0;
    if (f) fclose(f);
    if (n != CARD_SIZE || g_card[0] != 'M' || g_card[1] != 'C') {
        card_format();
        logf("memcard: new empty card (%s is written on the first save)\n", CARDFILE);
    } else logf("memcard: loaded %s\n", CARDFILE);
}

static int find_file(const std::string& name) {
    for (int i = 0; i < 15; i++)
        if (get32(dirent(i)) == 0x51 && !strncmp((const char*)dirent(i) + 0x0A, name.c_str(), 20)) return i;
    return -1;
}

static int create_file(const std::string& name, int blocks) {
    if (blocks < 1) blocks = 1;
    int idx[15], n = 0;
    for (int i = 0; i < 15 && n < blocks; i++)
        if ((get32(dirent(i)) & 0xF0) == 0xA0) idx[n++] = i;
    if (n < blocks) return -1;
    for (int k = 0; k < blocks; k++) {
        u8* d = dirent(idx[k]);
        memset(d, 0, 128);
        put32(d, k == 0 ? 0x51 : k == blocks - 1 ? 0x53 : 0x52);
        put32(d + 4, (u32)blocks * 8192);
        int nx = k < blocks - 1 ? idx[k + 1] : 0xFFFF;
        d[8] = (u8)nx; d[9] = (u8)(nx >> 8);
        if (k == 0) strncpy((char*)d + 0x0A, name.c_str(), 20);
        cks(d);
        memset(g_card + (idx[k] + 1) * 8192, 0, 8192);
    }
    return idx[0];
}

static void delete_file(int first) {
    for (int i = first, guard = 0; i >= 0 && guard < 15; guard++) {
        int nx = next_of(i);
        free_entry(i);
        i = nx;
    }
}

// byte offset in the image of position `pos` of the file starting at directory entry `first`
static int file_offset(int first, u32 pos) {
    int b = first;
    for (u32 k = pos / 8192; k > 0 && b >= 0; k--) b = next_of(b);
    return b < 0 ? -1 : (b + 1) * 8192 + (int)(pos % 8192);
}

// ---- events ---------------------------------------------------------------------------------------
static void card_done(bool ok) {
    bios_deliver_event(0xF0000011u, 0x0004);
    bios_deliver_event(0xF4000001u, ok ? 0x0004 : 0x0100);
}

void mc_card_info(int chan) {
    // slot 1 always holds a card; slot 2 is empty
    card_done((chan & 0x10) == 0);
}

// _card_read / _card_write: one 128-byte sector, completion reported through the HwCARD event
void mc_sector(int chan, u32 sector, u32 buf, bool write) {
    if (chan & 0x10) { bios_deliver_event(0xF0000011u, 0x0100); return; }   // slot 2: nothing there
    card_load();
    if (buf && sector < CARD_SIZE / 128) {
        u8* p = g_card + sector * 128;
        if (write) {
            for (int i = 0; i < 128; i++) p[i] = rd8(buf + i);
            card_flush();
            logf("memcard: directory sector %u written\n", sector);
        } else
            for (int i = 0; i < 128; i++) wr8(buf + i, p[i]);
    }
    bios_deliver_event(0xF0000011u, 0x0004);
}

// "bu00:NAME" -> file name on the card; false for other devices / slot 2
static bool card_name(const std::string& path, std::string& name) {
    if (path.size() < 5 || _strnicmp(path.c_str(), "bu", 2) || path[4] != ':') return false;
    if (path[2] != '0') return false;
    name = path.substr(5);
    return true;
}

static bool wild(const char* p, const char* s) {
    for (; *p; p++, s++) {
        if (*p == '*') return true;
        if (*p == '?') { if (!*s) return false; continue; }
        if (toupper((u8)*p) != toupper((u8)*s)) return false;
    }
    return !*s;
}

static u32 find_next() {
    Ctx* c = &g_ctx;
    for (; g_find_i < 15; g_find_i++) {
        const u8* e = dirent(g_find_i);
        if (get32(e) != 0x51) continue;
        char name[21];
        memcpy(name, e + 0x0A, 20);
        name[20] = 0;
        if (!wild(g_find_pat.c_str(), name)) continue;
        u32 d = g_find_dirent;
        for (int i = 0; i < 40; i++) wr8(d + i, 0);
        for (int i = 0; i < 20 && name[i]; i++) wr8(d + i, (u8)name[i]);
        mem_w32(c, d + 20, 0x50);
        mem_w32(c, d + 24, get32(e + 4));
        g_find_i++;
        return d;
    }
    return 0;
}

bool bios_file_call(Ctx* c, int fn) {
    u32 a0 = c->r[4], a1 = c->r[5], a2 = c->r[6];
    u32& v0 = c->r[2];
    switch (fn & 0xFF) {
    case 0x00: case 0x32: {  // open(name, mode)
        std::string name, path = gstr(a0);
        v0 = (u32)-1;
        if (!card_name(path, name)) { LOGV("open(%s): no such device\n", path.c_str()); return true; }
        card_load();
        int first = find_file(name);
        if (a1 & 0x200) {  // create: size in 8K blocks in the high word
            if (first >= 0) { logf("memcard: create %s: exists\n", name.c_str()); card_done(true); return true; }
            first = create_file(name, (int)(a1 >> 16));
            if (first >= 0) card_flush();
        }
        if (first >= 0)
            for (int i = 3; i < MAXFD; i++)
                if (!g_fd[i].used) { g_fd[i] = CardFd{true, first, get32(dirent(first) + 4), 0, (a1 & 0x8000) != 0}; v0 = i; break; }
        logf("memcard: open %s mode %x -> %s\n", path.c_str(), a1, v0 != (u32)-1 ? "ok" : "fail");
        if (a1 & 0x8000) card_done(v0 != (u32)-1);
        return true;
    }
    case 0x01: case 0x33:  // lseek(fd, off, whence)
        if (a0 < MAXFD && g_fd[a0].used) { g_fd[a0].pos = a2 == 1 ? g_fd[a0].pos + a1 : a1; v0 = g_fd[a0].pos; }
        else v0 = (u32)-1;
        return true;
    case 0x02: case 0x34:    // read(fd, buf, len)
    case 0x03: case 0x35: {  // write(fd, buf, len)
        bool write = (fn & 0xFF) == 0x03 || (fn & 0xFF) == 0x35;
        if (write && a0 == 1) {
            std::string t;
            for (u32 i = 0; i < a2 && i < 1000; i++) t.push_back((char)rd8(a1 + i));
            logf("[tty] %s", t.c_str());
            v0 = a2;
            return true;
        }
        if (a0 >= MAXFD || !g_fd[a0].used) { v0 = (u32)-1; return true; }
        CardFd& f = g_fd[a0];
        u32 n = 0;
        for (; n < a2 && f.pos < f.size; n++, f.pos++) {
            int off = file_offset(f.first, f.pos);
            if (off < 0 || off >= CARD_SIZE) break;
            if (write) g_card[off] = rd8(a1 + n);
            else wr8(a1 + n, g_card[off]);
        }
        if (write) card_flush();
        logf("memcard: %s %u bytes -> %u\n", write ? "write" : "read", a2, n);
        // a file opened with O_NOWAIT returns 0 at once (the game's library loops until it does)
        // and reports completion through the card events
        v0 = f.nowait ? 0 : n;
        card_done(true);
        return true;
    }
    case 0x04: case 0x36:  // close
        if (a0 < MAXFD && g_fd[a0].used) { g_fd[a0].used = false; v0 = a0; }
        else v0 = (u32)-1;
        return true;
    case 0x41:  // format
        card_load();
        card_format();
        card_flush();
        v0 = 1;
        return true;
    case 0x42: {  // firstfile(pattern, dirent)
        std::string name;
        v0 = 0;
        if (!card_name(gstr(a0), name)) return true;
        card_load();
        g_find_pat = name.empty() ? "*" : name;
        g_find_dirent = a1;
        g_find_i = 0;
        v0 = find_next();
        if (!v0) bios_deliver_event(0xF0000011u, 0x0004);  // directory scanned, nothing found
        return true;
    }
    case 0x43:
        g_find_dirent = a0;
        v0 = find_next();
        if (!v0) bios_deliver_event(0xF0000011u, 0x0004);
        return true;
    case 0x44: {  // rename
        std::string n1, n2;
        v0 = 0;
        if (card_name(gstr(a0), n1) && card_name(gstr(a1), n2)) {
            card_load();
            int i = find_file(n1);
            if (i >= 0 && find_file(n2) < 0) {
                memset(dirent(i) + 0x0A, 0, 21);
                strncpy((char*)dirent(i) + 0x0A, n2.c_str(), 20);
                cks(dirent(i));
                card_flush();
                v0 = 1;
            }
        }
        return true;
    }
    case 0x45: {  // delete
        std::string name;
        v0 = 0;
        if (card_name(gstr(a0), name)) {
            card_load();
            int i = find_file(name);
            if (i >= 0) { delete_file(i); card_flush(); v0 = 1; }
        }
        logf("memcard: delete %s -> %d\n", gstr(a0).c_str(), v0);
        if (!v0) bios_deliver_event(0xF0000011u, 0x0004);   // the library waits for the card after a failed delete
        return true;
    }
    }
    v0 = (u32)-1;
    return true;
}

// ---- save states ---------------------------------------------------------------------------
// The card's contents are not part of a state (games saved on it stay); only open files are.
void memcard_state(StateIO& s) {
    s.tag("MCRD");
    s.pod(g_fd);
    s.str(g_find_pat); s.pod(g_find_dirent); s.pod(g_find_i);
}
