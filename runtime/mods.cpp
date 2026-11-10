// Mod loader.
//   mods/disc/<PATH>          replaces the disc file <PATH> (e.g. mods/disc/RELOCS/GT.LVB)
//   mods/disc/<PATH>@<HEX>    patches the disc file at byte offset HEX (e.g. mods/disc/PROJFILE.MWD@1A2000)
//   mods/*.dll                native code mods, see mods/c12_mod_api.h
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "psx.h"
#include "c12_mod_api.h"

static std::vector<void (*)(void)> g_frame_cbs;

static void api_log(const char* msg) { logf("[mod] %s\n", msg); }
static int api_hook(uint32_t addr, C12HookFn fn) { return hook_set(addr, (HookFn)fn) ? 1 : 0; }
static void api_call(uint32_t addr) { Ctx* c = &g_ctx; u32 ra = c->r[31]; guest_call(c, addr); c->r[31] = ra; }
static uint32_t api_r32(uint32_t a) { return mem_r32(&g_ctx, a); }
static void api_w32(uint32_t a, uint32_t v) { mem_w32(&g_ctx, a, v); }
static void api_on_frame(void (*cb)(void)) { g_frame_cbs.push_back(cb); }
static int api_override(const char* path, uint32_t off, const void* data, uint32_t len) { return cdrom_add_override(path, off, (const u8*)data, len); }

void mods_frame() {
    for (auto cb : g_frame_cbs) cb();
}

static void scan_disc(const std::string& dir, const std::string& rel) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        std::string full = dir + "\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { scan_disc(full, rel + name + "\\"); continue; }
        std::string disc = rel + name;
        u32 off = 0;
        size_t at = disc.rfind('@');
        if (at != std::string::npos) { off = strtoul(disc.c_str() + at + 1, nullptr, 16); disc.erase(at); }
        FILE* f = fopen(full.c_str(), "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        std::vector<u8> data(ftell(f));
        fseek(f, 0, SEEK_SET);
        fread(data.data(), 1, data.size(), f);
        fclose(f);
        bool ok = !data.empty() && cdrom_add_override(disc.c_str(), off, data.data(), (u32)data.size());
        logf("mod: %s -> %s+%x (%u bytes) %s\n", full.c_str(), disc.c_str(), off, (u32)data.size(), ok ? "ok" : "FAILED (no such disc file?)");
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

void mods_init() {
    const std::string& dir = g_cfg.moddir;
    scan_disc(dir + "\\disc", "");
    static C12ModApi api;
    api.version = C12_MOD_API_VERSION;
    api.ram = g_ram;
    api.regs = g_ctx.r;
    api.log = api_log;
    api.hook = api_hook;
    api.call = api_call;
    api.read32 = api_r32;
    api.write32 = api_w32;
    api.on_frame = api_on_frame;
    api.override_file = api_override;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*.dll").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string full = dir + "\\" + fd.cFileName;
        HMODULE m = LoadLibraryA(full.c_str());
        auto init = m ? (int (*)(const C12ModApi*))GetProcAddress(m, "c12_mod_init") : nullptr;
        if (!init) { logf("mod: %s has no c12_mod_init export\n", full.c_str()); continue; }
        int r = init(&api);
        logf("mod: loaded %s (init=%d)\n", full.c_str(), r);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
