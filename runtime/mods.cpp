// Mod loader.
//   mods/disc/<PATH>          replaces the disc file <PATH> (e.g. mods/disc/RELOCS/GT.LVB)
//   mods/disc/<PATH>@<HEX>    patches the disc file at byte offset HEX (e.g. mods/disc/PROJFILE.MWD@1A2000)
//   mods/*.dll                native code mods, see mods/c12_mod_api.h
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "psx.h"
#include "c12_mod_api.h"
#include "../third_party/imgui/imgui.h"
#include <map>

static std::vector<void (*)(void)> g_frame_cbs;

static void api_log(const char* msg) { logf("[mod] %s\n", msg); }
static int api_hook(uint32_t addr, C12HookFn fn) { return hook_set(addr, (HookFn)fn) ? 1 : 0; }
static void api_call(uint32_t addr) { Ctx* c = &g_ctx; u32 ra = c->r[31]; guest_call(c, addr); c->r[31] = ra; }
static uint32_t api_r32(uint32_t a) { return mem_r32(&g_ctx, a); }
static void api_w32(uint32_t a, uint32_t v) { mem_w32(&g_ctx, a, v); }
static void api_on_frame(void (*cb)(void)) { g_frame_cbs.push_back(cb); }
static int api_override(const char* path, uint32_t off, const void* data, uint32_t len) { return cdrom_add_override(path, off, (const u8*)data, len); }

// ---- mods in the settings menu ---------------------------------------------------------------------
struct ModSetting { std::string key, label; int* b; float* f; float lo, hi; };
struct ModEntry { std::string name, desc; int* enabled; std::vector<ModSetting> settings; };
static std::vector<ModEntry> g_mods;
static std::map<std::string, std::string> g_mod_ini;   // mod.<name>.<key> values read from c12.ini
extern float g_fov_mult;

void mods_ini_value(const std::string& k, const std::string& v) { g_mod_ini[k] = v; }
static std::string ini_key(const ModEntry& m, const std::string& key) {
    std::string k = "mod." + m.name + "." + key;
    for (char& ch : k) if (ch == ' ' || ch == '=') ch = '_';
    return k;
}
static void api_mod_info(const char* name, const char* desc, int* enabled) {
    g_mods.push_back(ModEntry{name ? name : "mod", desc ? desc : "", enabled, {}});
    auto it = g_mod_ini.find(ini_key(g_mods.back(), "enabled"));
    if (enabled && it != g_mod_ini.end()) *enabled = atoi(it->second.c_str());
}
static void api_setting_bool(const char* key, const char* label, int* value) {
    if (g_mods.empty() || !value) return;
    g_mods.back().settings.push_back(ModSetting{key, label, value, nullptr, 0, 0});
    auto it = g_mod_ini.find(ini_key(g_mods.back(), key));
    if (it != g_mod_ini.end()) *value = atoi(it->second.c_str());
}
static void api_setting_float(const char* key, const char* label, float* value, float lo, float hi) {
    if (g_mods.empty() || !value) return;
    g_mods.back().settings.push_back(ModSetting{key, label, nullptr, value, lo, hi});
    auto it = g_mod_ini.find(ini_key(g_mods.back(), key));
    if (it != g_mod_ini.end()) *value = (float)atof(it->second.c_str());
}
static void api_set_fov(float m) { g_fov_mult = m < 0.3f ? 0.3f : m > 3.0f ? 3.0f : m; }
static int api_game_option(const char* key, int value) {
    int* p = !key ? nullptr : !strcmp(key, "cam_autocenter") ? &g_cfg.cam_autocenter : !strcmp(key, "mouse_direct_cam") ? &g_cfg.mouse_direct_cam :
             !strcmp(key, "interp") ? &g_cfg.interp : nullptr;
    if (!p) return -1;
    int prev = *p;
    *p = value;
    return prev;
}

int mods_ui_count() { return (int)g_mods.size(); }
void mods_ui() {
    for (ModEntry& m : g_mods) {
        ImGui::PushID(m.name.c_str());
        bool on = m.enabled ? *m.enabled != 0 : true;
        if (m.enabled) { if (ImGui::Checkbox(m.name.c_str(), &on)) *m.enabled = on; }
        else ImGui::TextUnformatted(m.name.c_str());
        if (!m.desc.empty()) ImGui::TextDisabled("%s", m.desc.c_str());
        ImGui::Indent();
        ImGui::BeginDisabled(!on);
        for (ModSetting& s : m.settings) {
            if (s.b) { bool v = *s.b != 0; if (ImGui::Checkbox(s.label.c_str(), &v)) *s.b = v; }
            else ImGui::SliderFloat(s.label.c_str(), s.f, s.lo, s.hi, "%.2f");
        }
        ImGui::EndDisabled();
        ImGui::Unindent();
        ImGui::Separator();
        ImGui::PopID();
    }
}
void mods_save(FILE* f) {
    if (g_mods.empty()) return;
    fprintf(f, "\n# --- mods ---\n");
    for (const ModEntry& m : g_mods) {
        if (m.enabled) fprintf(f, "%s=%d\n", ini_key(m, "enabled").c_str(), *m.enabled);
        for (const ModSetting& s : m.settings) {
            if (s.b) fprintf(f, "%s=%d\n", ini_key(m, s.key).c_str(), *s.b);
            else fprintf(f, "%s=%.3f\n", ini_key(m, s.key).c_str(), *s.f);
        }
    }
}

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
    api.mod_info = api_mod_info;
    api.setting_bool = api_setting_bool;
    api.setting_float = api_setting_float;
    api.set_fov = api_set_fov;
    api.game_option = api_game_option;
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
