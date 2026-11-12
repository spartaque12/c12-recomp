// Save states.
//
// The game runs on a coroutine stack (slot 0, see costack.cpp) instead of the program's own thread
// stack, and every one of its fibres has its own. Between frames (state_point, called from the game's
// VSync wait) the game switches back to the program's thread, which can then copy or replace
// everything: guest RAM, CPU/GTE registers, the hardware (GPU incl. VRAM, SPU, CD-ROM, timers, DMA,
// pad), the high-level BIOS, and the native stacks of all fibres and of the game's main thread. Loading
// puts all of that back and switches into the restored main stack, which continues inside the
// state_point() of the moment the state was saved.
//
// Native stacks hold return addresses into c12.exe (linked without ASLR, so at a fixed address) and
// possibly into mod DLLs: a state loads only into the same c12.exe build with the mods at the same
// addresses. The memory card is not part of a state (saves on it are kept).
//
// Code whose frames can be on a saved stack must be built without stack protection (/GS-): a /GS frame
// checks a per-run random cookie when it returns. c12.exe's own code and the bundled mods are.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <direct.h>
#include "savestate.h"
#include "costack.h"

void ui_notify(const char* text);
void plat_audio_reset();
void gte_state_loaded();
void mods_state_loaded();
void level3d_reset();
int mods_module_list(void** bases, int max);
CoStack* game_main_stack();

static const u32 MAGIC = 0x53323143;   // "C12S"
static const u32 VERSION = 2;
enum { REQ_NONE, REQ_SAVE, REQ_LOAD };
static int g_req;
static std::string g_req_path;
int g_state_slot = 1;
int g_state_save_frame = -1;           // tests: --save_state_at FRAME (to --state_file)
int g_state_load_frame = -1;           // tests: --load_state_at FRAME (from --state_file)
std::string g_state_file;              // tests: --load_state PATH / --state_file PATH

const char* state_quick_path() {
    static char buf[64];
    snprintf(buf, sizeof buf, "states\\slot%d.c12s", g_state_slot);
    return buf;
}
void state_request_save(const char* path) { g_req = REQ_SAVE; g_req_path = path; }
void state_request_load(const char* path) { g_req = REQ_LOAD; g_req_path = path; }

// ---- identity of the running program ---------------------------------------------------------
struct Ident {
    u32 magic = MAGIC, version = VERSION;
    u32 exe_stamp = 0, exe_size = 0;     // PE TimeDateStamp / SizeOfImage of c12.exe
    uint64_t exe_base = 0;               // where c12.exe is loaded
    uint64_t stacks = 0;                 // where the coroutine stacks are
    u32 nmods = 0;
    uint64_t mods[16] = {};
    u32 mod_stamp[16] = {}, mod_size[16] = {};   // which build of each mod (stacks can return into them)
};
static Ident ident_now() {
    Ident id;
    HMODULE m = GetModuleHandleA(nullptr);
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)m;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((u8*)m + dos->e_lfanew);
    id.exe_stamp = nt->FileHeader.TimeDateStamp;
    id.exe_size = nt->OptionalHeader.SizeOfImage;
    id.exe_base = (uint64_t)m;
    id.stacks = (uint64_t)co_slot_lo(0);
    void* b[16];
    id.nmods = (u32)mods_module_list(b, 16);
    for (u32 i = 0; i < id.nmods; i++) {
        id.mods[i] = (uint64_t)b[i];
        IMAGE_NT_HEADERS* mn = (IMAGE_NT_HEADERS*)((u8*)b[i] + ((IMAGE_DOS_HEADER*)b[i])->e_lfanew);
        id.mod_stamp[i] = mn->FileHeader.TimeDateStamp;
        id.mod_size[i] = mn->OptionalHeader.SizeOfImage;
    }
    return id;
}

static void all_state(StateIO& s) {
    core_state(s);
    bios_state(s);
    cdrom_state(s);
    hw_state(s);
    spu_state(s);
    memcard_state(s);
    gpu_state(s);
    hdtex_state(s);
    fibres_state(s);
    s.tag("END!");
}

static bool write_state(const std::string& path) {
    _mkdir("states");
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    StateIO s;
    s.f = f;
    Ident id = ident_now();
    s.pod(id);
    all_state(s);
    bool ok = s.ok && fclose(f) == 0;
    if (!ok) { remove(tmp.c_str()); return false; }
    return MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

// 0 = loaded, 1 = refused before anything changed (message in why), 2 = failed half-way
static int read_state(const std::string& path, std::string& why) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { why = "no save state in this slot"; return 1; }
    StateIO s;
    s.f = f;
    s.load = true;
    Ident id, now = ident_now();
    s.pod(id);
    if (!s.ok || id.magic != MAGIC) { fclose(f); why = "not a save state"; return 1; }
    if (id.version != VERSION) { fclose(f); why = "save state from another version"; return 1; }
    if (id.exe_stamp != now.exe_stamp || id.exe_size != now.exe_size) { fclose(f); why = "save state from another build of c12.exe"; return 1; }
    if (id.exe_base != now.exe_base || id.stacks != now.stacks) { fclose(f); why = "save state from a run with a different memory layout"; return 1; }
    if (id.nmods != now.nmods || memcmp(id.mods, now.mods, sizeof id.mods)) { fclose(f); why = "save state made with other mods loaded (or at other addresses)"; return 1; }
    if (memcmp(id.mod_stamp, now.mod_stamp, sizeof id.mod_stamp) || memcmp(id.mod_size, now.mod_size, sizeof id.mod_size)) { fclose(f); why = "save state made with another build of a mod"; return 1; }
    all_state(s);
    fclose(f);
    return s.ok ? 0 : 2;
}

static void do_save(const std::string& path) {
    bool ok = write_state(path);
    char msg[160];
    snprintf(msg, sizeof msg, ok ? "state saved (slot %d)" : "could not save the state (slot %d)", g_state_slot);
    if (path == state_quick_path()) ui_notify(msg);
    else ui_notify(ok ? "state saved" : "could not save the state");
    logf("save state %s: %s\n", path.c_str(), ok ? "ok" : "FAILED");
}

static void do_load(const std::string& path) {
    // keep the current game to fall back on if the file turns out to be damaged
    std::string undo = "states\\undo.c12s";
    if (!write_state(undo)) { ui_notify("could not load: no room for the undo state"); return; }
    std::string why;
    int r = read_state(path, why);
    if (r == 2) {
        logf("load state %s: damaged, going back\n", path.c_str());
        std::string why2;
        if (read_state(undo, why2) != 0) psx_fatal("save state %s was damaged and the game could not be restored", path.c_str());
        ui_notify("save state is damaged");
        return;
    }
    if (r == 1) { ui_notify(why.c_str()); logf("load state %s: %s\n", path.c_str(), why.c_str()); return; }
    plat_audio_reset();
    gte_state_loaded();
    level3d_reset();
    mods_state_loaded();
    char msg[64];
    snprintf(msg, sizeof msg, "state loaded (slot %d)", g_state_slot);
    ui_notify(path == state_quick_path() ? msg : "state loaded");
    logf("load state %s: ok\n", path.c_str());
}

void state_point() {
    if (g_state_save_frame >= 0 && g_frame >= g_state_save_frame && !g_state_file.empty()) {
        g_state_save_frame = -1;
        state_request_save(g_state_file.c_str());
    }
    if (g_state_load_frame >= 0 && g_frame >= g_state_load_frame && !g_state_file.empty()) {
        g_state_load_frame = -1;
        state_request_load(g_state_file.c_str());
    }
    if (g_req) co_switch(game_main_stack(), co_host());
    // continues here after a save, or - after a load - in the state_point() of the saved game
}

void state_run_game(void (*entry)(void*)) {
    if (!co_init()) psx_fatal("could not reserve the coroutine stacks");
    co_create(game_main_stack(), CO_MAIN_SLOT, entry, nullptr);
    if (g_state_save_frame < 0 && g_state_load_frame < 0 && !g_state_file.empty()) state_request_load(g_state_file.c_str());
    for (;;) {
        co_switch(co_host(), game_main_stack());
        int req = g_req;
        std::string path = g_req_path;
        g_req = REQ_NONE;
        if (req == REQ_SAVE) do_save(path);
        else if (req == REQ_LOAD) do_load(path);
    }
}
