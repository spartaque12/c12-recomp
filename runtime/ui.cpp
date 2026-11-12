#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include "ui.h"
#include "savestate.h"
#include "../third_party/imgui/imgui.h"
int mods_ui_count();
void mods_ui();
void mods_save(FILE* f);
#include "../third_party/imgui/imgui_impl_win32.h"
#include "../third_party/imgui/imgui_impl_dx11.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
extern float g_ws_scale;
extern int g_cd_fast, g_hud_fix, g_overlays_native;
extern bool g_hd_on;

const char* const g_btn_label[16] = {"Select", "L3", "R3", "Start", "D-pad Up", "D-pad Right", "D-pad Down", "D-pad Left",
                                     "L2", "R2 (first-person view)", "L1 (cycle weapons)", "R1 (lock on / strafe)",
                                     "Triangle", "Circle", "Cross (attack / accept)", "Square"};
const char* const g_btn_key[16] = {"select", "l3", "r3", "start", "up", "right", "down", "left", "l2", "r2", "l1", "r1",
                                   "triangle", "circle", "cross", "square"};

static bool g_ready, g_open, g_classic;
static Config g_saved;                 // remastered settings while classic is shown
static char g_note[96];
static double g_note_until;
static int g_rebind = -1, g_rebind_slot;
static float g_ui_scale = 1.0f;

bool ui_menu_open() { return g_open; }
void ui_toggle_menu() { g_open = !g_open; g_rebind = -1; }
bool settings_is_classic() { return g_classic; }

void ui_notify(const char* text) {
    snprintf(g_note, sizeof g_note, "%s", text);
    g_note_until = plat_time() + 1.6;
}

void settings_defaults() {
    static const int def[16][2] = {
        {'G', VK_RSHIFT}, {0, 0}, {0, 0}, {VK_RETURN, 0}, {'W', VK_UP}, {'D', VK_RIGHT}, {'S', VK_DOWN}, {'A', VK_LEFT},
        {'C', 0}, {VK_RBUTTON, 0}, {'Q', 0}, {VK_LSHIFT, VK_MBUTTON}, {'R', VK_BACK}, {'F', 0}, {VK_LBUTTON, VK_SPACE}, {'E', 0}};
    memcpy(g_cfg.binds, def, sizeof def);
}

void settings_apply() {
    g_ws_scale = g_cfg.widescreen ? 0.75f : 1.0f;
    core_set_bias(g_cfg.cpu_bias);
    hw_set_scale(g_cfg.scale);
}

// The presentation-only part of the configuration: what the classic/remastered key swaps.
static void copy_look(Config& d, const Config& s) {
    d.scale = s.scale; d.widescreen = s.widescreen; d.filter = s.filter;
    d.hd_enabled = s.hd_enabled; d.present_linear = s.present_linear; d.audio_interp = s.audio_interp; d.interp = s.interp;
}

void settings_toggle_classic() {
    if (!g_classic) {
        g_saved = g_cfg;
        Config c = g_cfg;
        c.scale = 1; c.widescreen = 0; c.filter = 0; c.hd_enabled = 0; c.present_linear = 0; c.audio_interp = 2; c.interp = 0;
        copy_look(g_cfg, c);
    } else copy_look(g_cfg, g_saved);
    g_classic = !g_classic;
    settings_apply();
    ui_notify(g_classic ? "CLASSIC" : "REMASTERED");
}

void settings_save(const char* path) {
    Config c = g_cfg;
    if (g_classic) copy_look(c, g_saved);   // never persist the classic preset
    FILE* f = fopen(path, "w");
    if (!f) { ui_notify("could not write c12.ini"); return; }
    fprintf(f, "# C-12: Final Resistance - recompiled. F1 or Esc in game opens the settings menu.\n");
    fprintf(f, "cue=%s\n\n# --- video ---\n", c.cue.c_str());
    fprintf(f, "scale=%d            # internal resolution multiplier\n", c.scale);
    fprintf(f, "widescreen=%d       # 16:9 field of view\n", c.widescreen);
    fprintf(f, "hud_fix=%d          # keep HUD/menus in proportion in widescreen\n", g_hud_fix);
    { extern int g_level3d; fprintf(f, "level3d=%d          # native 3D renderer for the level (depth buffer)\n", g_level3d); }
    fprintf(f, "filter=%d           # bilinear filtering of original textures\n", c.filter);
    fprintf(f, "hd_enabled=%d       # use the HD texture pack\n", c.hd_enabled);
    fprintf(f, "hd_textures=%s\n", c.hd_textures.c_str());
    fprintf(f, "present_linear=%d   # smooth (1) or sharp-pixel (0) scaling to the window\n", c.present_linear);
    fprintf(f, "vsync=%d\nfullscreen=%d\nwidth=%d\nheight=%d\n\n# --- speed ---\n", c.vsync, c.fullscreen, c.win_w, c.win_h);
    fprintf(f, "cpu_bias=%.2f       # 2.0 = original console CPU speed, lower = faster\n", c.cpu_bias);
    fprintf(f, "cd_fast=%d          # loading speed multiplier\n", g_cd_fast);
    fprintf(f, "interp=%d           # frame interpolation: smooth 50 fps at correct game speed\n", c.interp);
    fprintf(f, "\n# --- audio ---\n");
    fprintf(f, "volume=%.2f\naudio_interp=%d     # 0 nearest, 1 linear, 2 console-like, 3 cubic\nreverb=%d\naudio_latency_ms=%d\n\n# --- input ---\n",
            c.volume, c.audio_interp, c.reverb, c.audio_latency_ms);
    fprintf(f, "mouse=%d\nmouse_sens=%.2f\nmouse_invert_x=%d\nmouse_invert_y=%d\nmouse_aim_invert=%d\nmouse_direct_aim=%d\nmouse_direct_cam=%d\ncam_autocenter=%d\npause_in_menu=%d\n", c.mouse, c.mouse_sens, c.mouse_invert_x, c.mouse_invert_y, c.mouse_aim_invert, c.mouse_direct_aim, c.mouse_direct_cam, c.cam_autocenter, c.pause_in_menu);
    fprintf(f, "# key bindings: Windows virtual-key codes, two per button (0 = none)\n");
    for (int i = 0; i < 16; i++) fprintf(f, "bind_%s=%d,%d\n", g_btn_key[i], c.binds[i][0], c.binds[i][1]);
    fprintf(f, "\n# --- misc ---\nmods=%s\nlog=%d\n", c.moddir.c_str(), g_log_level);
    mods_save(f);
    fclose(f);
    ui_notify("settings saved");
}

void ui_init(void* hwnd, ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 6;
    st.FrameRounding = 3;
    st.Colors[ImGuiCol_WindowBg].w = 0.92f;
    g_ui_scale = (float)GetDpiForWindow((HWND)hwnd) / 96.0f;
    if (g_ui_scale < 1.0f) g_ui_scale = 1.0f;
    g_ui_scale *= 1.25f;   // comfortable size from the couch
    st.ScaleAllSizes(g_ui_scale);
    io.FontGlobalScale = g_ui_scale;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(dev, ctx);
    g_ready = true;
}

bool ui_wndproc(void* hwnd, unsigned msg, unsigned long long wp, long long lp) {
    if (!g_ready) return false;
    LRESULT r = ImGui_ImplWin32_WndProcHandler((HWND)hwnd, msg, (WPARAM)wp, (LPARAM)lp);
    return g_open && r;
}

static void bind_row(int b) {
    ImGui::PushID(b);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(g_btn_label[b]);
    for (int s = 0; s < 2; s++) {
        ImGui::TableNextColumn();
        ImGui::PushID(s);
        bool waiting = g_rebind == b && g_rebind_slot == s;
        const char* name = waiting ? "press a key..." : g_cfg.binds[b][s] ? plat_key_name(g_cfg.binds[b][s]) : "-";
        if (ImGui::Button(name, ImVec2(130 * g_ui_scale, 0))) { g_rebind = b; g_rebind_slot = s; }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) g_cfg.binds[b][s] = 0;
        ImGui::PopID();
    }
    ImGui::PopID();
}

static void draw_menu() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(620 * g_ui_scale, 560 * g_ui_scale), ImGuiCond_Appearing);
    if (!ImGui::Begin("C-12: Final Resistance - settings", &g_open, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    bool changed = false;
    if (ImGui::Button(g_classic ? "Showing: CLASSIC  (Tab)" : "Showing: REMASTERED  (Tab)", ImVec2(250 * g_ui_scale, 0))) settings_toggle_classic();
    ImGui::SameLine();
    ImGui::TextDisabled("Tab switches at any time");
    {   // save states (same as F5 / F9, slot = F6 / F7)
        extern int g_state_slot;
        ImGui::SetNextItemWidth(110 * g_ui_scale);
        ImGui::SliderInt("##slot", &g_state_slot, 1, 9, "Slot %d");
        ImGui::SameLine();
        if (ImGui::Button("Save state (F5)")) state_request_save(state_quick_path());
        ImGui::SameLine();
        WIN32_FILE_ATTRIBUTE_DATA fa;
        bool have = GetFileAttributesExA(state_quick_path(), GetFileExInfoStandard, &fa) != 0;
        ImGui::BeginDisabled(!have);
        if (ImGui::Button("Load state (F9)")) { state_request_load(state_quick_path()); g_open = false; }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (have) {
            FILETIME lt;
            SYSTEMTIME st;
            FileTimeToLocalFileTime(&fa.ftLastWriteTime, &lt);
            FileTimeToSystemTime(&lt, &st);
            ImGui::TextDisabled("saved %02d.%02d. %02d:%02d", st.wDay, st.wMonth, st.wHour, st.wMinute);
        } else ImGui::TextDisabled("empty slot");
    }
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Video")) {
            if (g_classic) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Classic view is active: these show the classic preset.");
            changed |= ImGui::SliderInt("Internal resolution", &g_cfg.scale, 1, 12, "%dx");
            ImGui::SameLine();
            ImGui::TextDisabled("(%dx%d)", 512 * g_cfg.scale, 256 * g_cfg.scale);
            bool b;
            b = g_cfg.widescreen; if (ImGui::Checkbox("Widescreen 16:9 (true field of view)", &b)) { g_cfg.widescreen = b; changed = true; }
            b = g_hud_fix; if (ImGui::Checkbox("Keep HUD and menus in proportion", &b)) g_hud_fix = b;
            { extern int g_level3d; b = g_level3d; if (ImGui::Checkbox("Native 3D renderer (real depth buffer, no wobble)", &b)) g_level3d = b; }
            ImGui::SameLine(); ImGui::TextDisabled("(level, characters, objects, effects; remastered only)");
            b = g_cfg.hd_enabled; if (ImGui::Checkbox("HD texture pack", &b)) g_cfg.hd_enabled = b;
            if (!g_hd_on) { ImGui::SameLine(); ImGui::TextDisabled("(no pack found in '%s')", g_cfg.hd_textures.c_str()); }
            b = g_cfg.filter; if (ImGui::Checkbox("Bilinear filtering of original textures", &b)) g_cfg.filter = b;
            b = g_cfg.present_linear; if (ImGui::Checkbox("Smooth scaling to the window", &b)) g_cfg.present_linear = b;
            b = g_cfg.vsync; if (ImGui::Checkbox("VSync (off = lowest latency)", &b)) g_cfg.vsync = b;
            if (ImGui::Button("Toggle fullscreen (Alt+Enter)")) plat_toggle_fullscreen();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Speed")) {
            float oc = (float)(2.0 / g_cfg.cpu_bias);
            if (ImGui::SliderFloat("CPU overclock", &oc, 1.0f, 8.0f, "%.1fx")) { g_cfg.cpu_bias = 2.0 / oc; changed = true; }
            ImGui::TextDisabled("1x = original console; higher removes slowdown");
            { bool bi = g_cfg.interp; if (ImGui::Checkbox("Frame interpolation (smooth 50 fps, correct game speed)", &bi)) g_cfg.interp = bi; }
            ImGui::TextDisabled("Draws an in-between frame from tracked vertex motion; adds ~20 ms of display delay");
            ImGui::SliderInt("Loading speed", &g_cd_fast, 1, 8, "%dx");
            ImGui::TextDisabled("Hold ` (backquote) to fast-forward");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Audio")) {
            ImGui::SliderFloat("Volume", &g_cfg.volume, 0.0f, 1.5f, "%.2f");
            const char* interp[] = {"Nearest", "Linear", "Console-like (soft)", "Cubic (sharp)"};
            ImGui::Combo("Interpolation", &g_cfg.audio_interp, interp, 4);
            bool b = g_cfg.reverb;
            if (ImGui::Checkbox("Reverb", &b)) g_cfg.reverb = b;
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Controls")) {
            bool b = g_cfg.mouse;
            if (ImGui::Checkbox("Mouse look (camera on the right stick)", &b)) g_cfg.mouse = b;
            ImGui::SliderFloat("Mouse sensitivity", &g_cfg.mouse_sens, 0.2f, 4.0f, "%.2f");
            b = g_cfg.mouse_invert_x; if (ImGui::Checkbox("Invert mouse X", &b)) g_cfg.mouse_invert_x = b;
            ImGui::SameLine();
            b = g_cfg.mouse_invert_y; if (ImGui::Checkbox("Invert mouse Y", &b)) g_cfg.mouse_invert_y = b;
            b = g_cfg.mouse_direct_cam; if (ImGui::Checkbox("Raw mouse camera in third-person view (no stick emulation)", &b)) g_cfg.mouse_direct_cam = b;
            b = g_cfg.cam_autocenter; if (ImGui::Checkbox("Camera auto-centre behind the hero (off = it keeps your direction)", &b)) g_cfg.cam_autocenter = b;
            b = g_cfg.mouse_direct_aim; if (ImGui::Checkbox("True mouse aim in first-person view (1:1, no stick emulation)", &b)) g_cfg.mouse_direct_aim = b;
            b = g_cfg.mouse_aim_invert; if (ImGui::Checkbox("Invert vertical aim in first-person view", &b)) g_cfg.mouse_aim_invert = b;
            ImGui::TextDisabled("Mouse wheel = cycle weapons. Controllers (XInput) always work.");
            ImGui::Separator();
            ImGui::TextDisabled("Click to rebind, right-click to clear.");
            if (ImGui::BeginTable("binds", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
                static const int order[16] = {4, 6, 7, 5, 14, 15, 13, 12, 10, 11, 8, 9, 3, 0, 1, 2};
                for (int i : order) bind_row(i);
                ImGui::EndTable();
            }
            if (ImGui::Button("Reset bindings")) settings_defaults();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("System")) {
            bool b = g_cfg.pause_in_menu;
            if (ImGui::Checkbox("Pause the game while this menu is open", &b)) g_cfg.pause_in_menu = b;
            ImGui::Text("Frame %d   %.1f fps (window)", g_frame, io.Framerate);
            ImGui::TextDisabled("Saves: saves\\    Mods: %s\\", g_cfg.moddir.c_str());
            ImGui::EndTabItem();
        }
        if (mods_ui_count() && ImGui::BeginTabItem("Mods")) {
            mods_ui();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::Separator();
    if (ImGui::Button("Save settings")) settings_save("c12.ini");
    ImGui::SameLine();
    if (ImGui::Button("Resume (Esc)")) g_open = false;
    ImGui::SameLine(ImGui::GetWindowWidth() - 110 * g_ui_scale);
    if (ImGui::Button("Quit game", ImVec2(95 * g_ui_scale, 0))) g_quit = 1;
    ImGui::End();
    if (changed) settings_apply();
}

void ui_render() {
    if (!g_ready) return;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (g_rebind >= 0) {
        static int armed;
        int k = plat_scan_key();
        if (!armed) { if (!k) armed = 1; }           // wait for the click that started it to be released
        else if (k) {
            if (k != VK_ESCAPE) g_cfg.binds[g_rebind][g_rebind_slot] = k;
            g_rebind = -1;
            armed = 0;
        }
    }
    ImGui::GetIO().MouseDrawCursor = false;
    if (g_open) draw_menu();
    if (plat_time() < g_note_until) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        ImVec2 sz = ImGui::CalcTextSize(g_note);
        float scale = 2.0f;   // on top of the DPI-scaled font
        ImVec2 p(ImGui::GetIO().DisplaySize.x - sz.x * scale - 28, 22);
        dl->AddRectFilled(ImVec2(p.x - 12, p.y - 6), ImVec2(p.x + sz.x * scale + 12, p.y + sz.y * scale + 6), IM_COL32(0, 0, 0, 170), 6);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * scale, p, IM_COL32(120, 255, 170, 255), g_note);
    }
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}
