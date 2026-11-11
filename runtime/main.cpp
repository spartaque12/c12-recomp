// Entry point: config, boot, run.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include "gpu.h"
#include "ui.h"
#include <direct.h>

static void parse_kv(const std::string& k, const std::string& v) {
    auto I = [&](int& d) { d = atoi(v.c_str()); };
    if (k == "cue") g_cfg.cue = v;
    else if (k == "mods") g_cfg.moddir = v;
    else if (k == "memcard") g_cfg.memcard = v;
    else if (k == "hd_textures") g_cfg.hd_textures = v;
    else if (k == "hd_enabled") I(g_cfg.hd_enabled);
    else if (k == "interp") I(g_cfg.interp);
    else if (k == "present_linear") I(g_cfg.present_linear);
    else if (k == "volume") g_cfg.volume = (float)atof(v.c_str());
    else if (k == "mouse") I(g_cfg.mouse);
    else if (k == "mouse_sens") g_cfg.mouse_sens = (float)atof(v.c_str());
    else if (k == "mouse_invert_y") I(g_cfg.mouse_invert_y);
    else if (k == "mouse_invert_x") I(g_cfg.mouse_invert_x);
    else if (k == "mouse_aim_invert") I(g_cfg.mouse_aim_invert);
    else if (k == "mouse_direct_aim") I(g_cfg.mouse_direct_aim);
    else if (k == "mouse_direct_cam") I(g_cfg.mouse_direct_cam);
    else if (k == "pause_in_menu") I(g_cfg.pause_in_menu);
    else if (k.rfind("bind_", 0) == 0) {
        for (int b = 0; b < 16; b++)
            if (k.substr(5) == g_btn_key[b]) sscanf(v.c_str(), "%d,%d", &g_cfg.binds[b][0], &g_cfg.binds[b][1]);
    }
    else if (k == "overlays_native") { extern int g_overlays_native; g_overlays_native = atoi(v.c_str()); }
    else if (k == "pgxp_cull") { extern int g_pgxp_cull; g_pgxp_cull = atoi(v.c_str()); }
    else if (k == "dbg_near") { extern int g_dbg_near; g_dbg_near = atoi(v.c_str()); }
    else if (k == "big_polys") { extern int g_big_polys; g_big_polys = atoi(v.c_str()); }
    else if (k == "ws_frustum") { extern int g_ws_frustum; g_ws_frustum = atoi(v.c_str()); }
    else if (k == "shot_from") { extern int g_shot_from; g_shot_from = atoi(v.c_str()); }
    else if (k == "ws_flags") { extern int g_ws_flags; g_ws_flags = atoi(v.c_str()); }
    else if (k == "cam_log") { extern int g_cam_log; g_cam_log = atoi(v.c_str()); }
    else if (k == "near_plane_cull") { extern int g_near_plane_cull; g_near_plane_cull = atoi(v.c_str()); }
    else if (k == "near_cull") { extern float g_near_cull; g_near_cull = (float)atof(v.c_str()); }
    else if (k == "ramdump") { extern int g_ramdump_every; g_ramdump_every = atoi(v.c_str()); }
    else if (k == "ws_view") { extern int g_ws_view; g_ws_view = atoi(v.c_str()); }
    else if (k == "hud_fix") { extern int g_hud_fix; g_hud_fix = atoi(v.c_str()); }
    else if (k == "test_classic") { extern int g_test_classic[4]; sscanf(v.c_str(), "%d,%d,%d,%d", &g_test_classic[0], &g_test_classic[1], &g_test_classic[2], &g_test_classic[3]); }
    else if (k == "test_menu") { extern int g_test_menu; g_test_menu = atoi(v.c_str()); }
    else if (k == "dbg_gpu") { extern int g_dbg_gpu; g_dbg_gpu = atoi(v.c_str()); }
    else if (k == "dbg_frame") { extern int g_dbg_frame; g_dbg_frame = atoi(v.c_str()); }
    else if (k == "scale") I(g_cfg.scale);
    else if (k == "widescreen") I(g_cfg.widescreen);
    else if (k == "pgxp") I(g_cfg.pgxp);
    else if (k == "perspective") I(g_cfg.persp);
    else if (k == "filter") I(g_cfg.filter);
    else if (k == "vsync") I(g_cfg.vsync);
    else if (k == "fullscreen") I(g_cfg.fullscreen);
    else if (k == "width") I(g_cfg.win_w);
    else if (k == "height") I(g_cfg.win_h);
    else if (k == "cpu_bias") g_cfg.cpu_bias = atof(v.c_str());
    else if (k == "fps60") I(g_cfg.fps60);
    else if (k == "audio_interp") I(g_cfg.audio_interp);
    else if (k == "audio_latency_ms") I(g_cfg.audio_latency_ms);
    else if (k == "reverb") I(g_cfg.reverb);
    else if (k == "log") I(g_log_level);
    else if (k == "dump_textures") I(g_cfg.dump_textures);
    else if (k == "cd_fast") { extern int g_cd_fast; g_cd_fast = atoi(v.c_str()); }
}

static void load_ini(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || *p == '[' || !*p) continue;
        char* eq = strchr(p, '=');
        if (!eq) continue;
        std::string k(p, eq), v(eq + 1);
        for (size_t i = 1; i < v.size(); i++)   // inline comment: whitespace followed by '#'
            if (v[i] == '#' && (v[i - 1] == ' ' || v[i - 1] == '\t')) { v.erase(i); break; }
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
        while (!v.empty() && (v[0] == ' ' || v[0] == '"')) v.erase(0, 1);
        while (!v.empty() && v.back() == '"') v.pop_back();
        parse_kv(k, v);
    }
    fclose(f);
}

void gpu_set_backend(GpuBackend* b);
extern float g_ws_scale;
u32 g_peek_ptr, g_peek_off;

// Double-click friendly start-up: run from wherever c12.ini lives (next to the exe or one level up),
// so the relative paths (hd_textures, mods, saves) resolve no matter how the program was launched.
static void find_home() {
    if (GetFileAttributesA("c12.ini") != INVALID_FILE_ATTRIBUTES) return;   // started from the right folder
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    dir.erase(dir.find_last_of("\\/"));
    std::string up = dir.substr(0, dir.find_last_of("\\/"));
    if (GetFileAttributesA((dir + "\\c12.ini").c_str()) != INVALID_FILE_ATTRIBUTES) SetCurrentDirectoryA(dir.c_str());
    else if (GetFileAttributesA((up + "\\c12.ini").c_str()) != INVALID_FILE_ATTRIBUTES) SetCurrentDirectoryA(up.c_str());
    else SetCurrentDirectoryA(dir.c_str());
}

static void fail(const char* msg) {
    logf("%s\n", msg);
    if (!g_cfg.headless) MessageBoxA(nullptr, msg, "C-12: Final Resistance", MB_OK | MB_ICONERROR);
}

// Ask for the disc image when none is configured (or the configured one is gone).
static bool pick_disc() {
    char file[MAX_PATH] = "";
    char cwd[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, cwd);
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = "PlayStation disc image (*.cue)\0*.cue\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = "Select your C-12: Final Resistance (Europe) disc image";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    bool ok = GetOpenFileNameA(&ofn) != 0;
    SetCurrentDirectoryA(cwd);
    if (ok) g_cfg.cue = file;
    return ok;
}

int main(int argc, char** argv) {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) freopen("CONOUT$", "w", stdout);   // keep logging when run from a terminal
    find_home();
    setvbuf(stdout, nullptr, _IONBF, 0);
    settings_defaults();
    load_ini("c12.ini");
    bool sw = false, hw = false, save_cfg = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--headless") g_cfg.headless = 1;
        else if (a == "--sw") sw = true;
        else if (a == "--hw") hw = true;
        else if (a == "--save-config") save_cfg = true;
        else if (a == "--peek" && i + 2 < argc) { g_peek_ptr = strtoul(argv[i + 1], nullptr, 16); g_peek_off = strtoul(argv[i + 2], nullptr, 16); i += 2; }
        else if (a == "--bioslog" && i + 1 < argc) { extern int g_bios_log_from; g_bios_log_from = atoi(argv[++i]); }
        else if (a == "--pcm" && i + 1 < argc) { extern FILE* g_wav; g_wav = fopen(argv[++i], "wb"); }
        else if (a == "--frames" && i + 1 < argc) g_cfg.max_frames = atoi(argv[++i]);
        else if (a == "--shots" && i + 1 < argc) g_cfg.shot_dir = argv[++i];
        else if (a == "--every" && i + 1 < argc) g_cfg.shot_every = atoi(argv[++i]);
        else if (a == "--input" && i + 1 < argc) g_cfg.input_script = argv[++i];
        else if (a == "--watch" && i + 1 < argc) { extern u32 g_watch_addr; g_watch_addr = strtoul(argv[++i], nullptr, 16); }
        else if (a.rfind("--", 0) == 0 && i + 1 < argc) parse_kv(a.substr(2), argv[i + 1]), i++;
    }
    if (save_cfg) { settings_save("c12.ini"); return 0; }
    bool picked = false;
    if (g_cfg.cue.empty() || GetFileAttributesA(g_cfg.cue.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (g_cfg.headless || !pick_disc()) { fail("No disc image selected. Set cue=<path to .cue> in c12.ini."); return 1; }
        picked = true;
    }
    if (!g_cfg.shot_dir.empty()) _mkdir(g_cfg.shot_dir.c_str());
    core_init();
    if (!cdrom_init(g_cfg.cue)) { fail(("Cannot open the disc image:\n" + g_cfg.cue + "\n(the .bin files must be next to the .cue)").c_str()); return 1; }
    std::vector<u8> exe;
    if (!cdrom_read_file("SCES_033.64", exe) || !load_exe_mem(exe.data(), exe.size())) {
        fail("This disc is not C-12: Final Resistance (Europe, SCES-03364).");
        return 1;
    }
    g_ws_scale = g_cfg.widescreen ? 0.75f : 1.0f;
    if (picked) settings_save("c12.ini");
    if (!plat_init()) { fail("Could not create the game window."); return 1; }
    bios_init();
    timers_init();
    gpu_init();
    spu_init();
    sio_init();
    { extern void game_init(); game_init(); }
    mods_init();   // after the built-in hooks so mods can replace them
    if (!sw && (!g_cfg.headless || hw)) {
        if (hw_backend_init()) gpu_set_backend(&g_hw_backend);
        else logf("hardware renderer unavailable, using software\n");
    }
    logf("boot: pc=%08x sp=%08x\n", g_ctx.pc, g_ctx.r[29]);
    guest_call(&g_ctx, g_ctx.pc);
    logf("guest main returned\n");
    return 0;
}
