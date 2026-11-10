// Memory card through the BIOS file API ("bu00:NAME"), stored as plain host files in saves/.
// Each save is one file (size = blocks * 8192), so saves can be copied, backed up or modded freely.
#include "psx.h"
#include <direct.h>
#include <io.h>

static std::string g_savedir = "saves";
#define MAXFD 16
static FILE* g_fd[MAXFD];
static struct _finddata_t g_find;
static intptr_t g_find_h = -1;
static std::string g_find_pat;
static u32 g_find_dirent;

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

static void card_done(bool ok) {
    bios_deliver_event(0xF0000011u, 0x0004);
    bios_deliver_event(0xF4000001u, ok ? 0x0004 : 0x0100);
}

void mc_card_info(int chan) {
    // slot 1 always holds a card; slot 2 is empty
    card_done((chan & 0x10) == 0);
}

// "bu00:NAME" -> host path; returns false for other devices / slot 2
static bool card_path(const std::string& name, std::string& out, std::string* leaf = nullptr) {
    if (name.size() < 5 || _strnicmp(name.c_str(), "bu", 2) || name[4] != ':') return false;
    if (name[2] != '0') return false;
    std::string l = name.substr(5);
    for (char& ch : l)
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '"' || ch == '<' || ch == '>' || ch == '|') ch = '_';
    if (leaf) *leaf = l;
    out = g_savedir + "/" + l;
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

static u32 fill_dirent() {
    Ctx* c = &g_ctx;
    u32 d = g_find_dirent;
    for (int i = 0; i < 40; i++) wr8(d + i, 0);
    for (int i = 0; i < 19 && g_find.name[i]; i++) wr8(d + i, (u8)g_find.name[i]);
    mem_w32(c, d + 20, 0x50);
    mem_w32(c, d + 24, (u32)g_find.size);
    return d;
}

static u32 find_next(bool first) {
    for (;;) {
        if (first) {
            if (g_find_h != -1) _findclose(g_find_h);
            g_find_h = _findfirst((g_savedir + "/*").c_str(), &g_find);
            if (g_find_h == -1) return 0;
            first = false;
        } else if (_findnext(g_find_h, &g_find) != 0) {
            _findclose(g_find_h);
            g_find_h = -1;
            return 0;
        }
        if (g_find.attrib & _A_SUBDIR) continue;
        if (wild(g_find_pat.c_str(), g_find.name)) return fill_dirent();
    }
}

bool bios_file_call(Ctx* c, int fn) {
    u32 a0 = c->r[4], a1 = c->r[5], a2 = c->r[6];
    u32& v0 = c->r[2];
    switch (fn & 0xFF) {
    case 0x00: case 0x32: {  // open(name, mode)
        std::string path, name = gstr(a0);
        v0 = (u32)-1;
        if (!card_path(name, path)) { LOGV("open(%s): no such device\n", name.c_str()); return true; }
        _mkdir(g_savedir.c_str());
        FILE* f = nullptr;
        if (a1 & 0x200) {  // create: size in 8K blocks in the high word
            if ((f = fopen(path.c_str(), "rb"))) { fclose(f); card_done(true); return true; }  // exists -> error
            f = fopen(path.c_str(), "w+b");
            if (f) {
                std::vector<u8> zero(8192, 0);
                for (u32 i = 0; i < (a1 >> 16); i++) fwrite(zero.data(), 1, 8192, f);
                fflush(f);
                fseek(f, 0, SEEK_SET);
            }
        } else f = fopen(path.c_str(), "r+b");
        logf("memcard: open %s mode %x -> %s\n", name.c_str(), a1, f ? "ok" : "fail");
        if (f) {
            for (int i = 3; i < MAXFD; i++)
                if (!g_fd[i]) { g_fd[i] = f; v0 = i; f = nullptr; break; }
            if (f) fclose(f);
        }
        if (a1 & 0x8000) card_done(v0 != (u32)-1);
        return true;
    }
    case 0x01: case 0x33:  // lseek(fd, off, whence)
        if (a0 < MAXFD && g_fd[a0]) { fseek(g_fd[a0], (long)a1, a2 == 1 ? SEEK_CUR : SEEK_SET); v0 = (u32)ftell(g_fd[a0]); }
        else v0 = (u32)-1;
        return true;
    case 0x02: case 0x34: {  // read(fd, buf, len)
        if (a0 >= MAXFD || !g_fd[a0]) { v0 = (u32)-1; return true; }
        std::vector<u8> buf(a2);
        size_t n = fread(buf.data(), 1, a2, g_fd[a0]);
        for (size_t i = 0; i < n; i++) wr8(a1 + (u32)i, buf[i]);
        v0 = (u32)n;
        card_done(true);
        return true;
    }
    case 0x03: case 0x35: {  // write(fd, buf, len)
        if (a0 == 1) {
            std::string t;
            for (u32 i = 0; i < a2 && i < 1000; i++) t.push_back((char)rd8(a1 + i));
            logf("[tty] %s", t.c_str());
            v0 = a2;
            return true;
        }
        if (a0 >= MAXFD || !g_fd[a0]) { v0 = (u32)-1; return true; }
        std::vector<u8> buf(a2);
        for (u32 i = 0; i < a2; i++) buf[i] = rd8(a1 + i);
        v0 = (u32)fwrite(buf.data(), 1, a2, g_fd[a0]);
        fflush(g_fd[a0]);
        card_done(true);
        return true;
    }
    case 0x04: case 0x36:  // close
        if (a0 < MAXFD && g_fd[a0]) { fclose(g_fd[a0]); g_fd[a0] = nullptr; v0 = a0; }
        else v0 = (u32)-1;
        return true;
    case 0x41: v0 = 1; return true;  // format
    case 0x42: {  // firstfile(pattern, dirent)
        std::string path, leaf;
        v0 = 0;
        if (!card_path(gstr(a0), path, &leaf)) return true;
        g_find_pat = leaf.empty() ? "*" : leaf;
        g_find_dirent = a1;
        v0 = find_next(true);
        if (!v0) bios_deliver_event(0xF0000011u, 0x0004);  // directory scanned, nothing found
        return true;
    }
    case 0x43:
        g_find_dirent = a0;
        v0 = find_next(false);
        if (!v0) bios_deliver_event(0xF0000011u, 0x0004);
        return true;
    case 0x44: {  // rename
        std::string p1, p2;
        v0 = card_path(gstr(a0), p1) && card_path(gstr(a1), p2) && rename(p1.c_str(), p2.c_str()) == 0;
        return true;
    }
    case 0x45: {  // delete
        std::string p;
        v0 = card_path(gstr(a0), p) && remove(p.c_str()) == 0;
        logf("memcard: delete %s -> %d\n", gstr(a0).c_str(), v0);
        return true;
    }
    }
    v0 = (u32)-1;
    return true;
}
