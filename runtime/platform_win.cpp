// Win32 platform: window, pacing, XAudio2 output, keyboard + mouse + XInput.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xaudio2.h>
#include <xinput.h>
#include <mmsystem.h>
#include "gpu.h"
#include "ui.h"
#include <deque>

HWND g_hwnd;
static LARGE_INTEGER g_qpf, g_qpc0;
static bool g_turbo;
static u16 g_pad_state[2] = {0xFFFF, 0xFFFF};
static u8 g_pad_axes[2][4] = {{128, 128, 128, 128}, {128, 128, 128, 128}};
extern u8 g_pad_motor[2][2];
bool g_window_resized;

// raw mouse state (accumulated between pad polls)
static long g_mouse_dx, g_mouse_dy;
long g_cam_mx, g_cam_my;   // raw mouse counts waiting for the game's next camera update
long g_aim_mx, g_aim_my;   // same, for the scope view (R2)
static int g_wheel_pulse;
static bool g_captured;
static float g_mx_f, g_my_f;

// scripted input (tests): entries "from to mask"
struct ScriptEnt { int from, to; u32 mask; int mx, my; };   // bits 16..19: right stick left/right/up/down; MX<n>/MY<n>: mouse counts per poll
static std::vector<ScriptEnt> g_script;

static const struct { const char* name; int bit; } g_btn_names[] = {
    {"SELECT", 0}, {"L3", 1}, {"R3", 2}, {"START", 3}, {"UP", 4}, {"RIGHT", 5}, {"DOWN", 6}, {"LEFT", 7},
    {"L2", 8}, {"R2", 9}, {"L1", 10}, {"R1", 11}, {"TRIANGLE", 12}, {"CIRCLE", 13}, {"CROSS", 14}, {"SQUARE", 15}};

void plat_load_script(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        int a, b;
        char names[200] = "";
        if (sscanf(line, "%d %d %199[^\n]", &a, &b, names) < 3) continue;
        u32 mask = 0;
        int mx = 0, my = 0;
        for (char* t = strtok(names, " +,\r"); t; t = strtok(nullptr, " +,\r")) {
            if (!_stricmp(t, "RSLEFT")) mask |= 1u << 16;
            if (!_stricmp(t, "RSRIGHT")) mask |= 1u << 17;
            if (!_stricmp(t, "RSUP")) mask |= 1u << 18;
            if (!_stricmp(t, "RSDOWN")) mask |= 1u << 19;
            if (!_strnicmp(t, "MX", 2)) mx = atoi(t + 2);
            if (!_strnicmp(t, "MY", 2)) my = atoi(t + 2);
            for (auto& bn : g_btn_names)
                if (!_stricmp(t, bn.name)) mask |= 1u << bn.bit;
        }
        g_script.push_back({a, b, mask, mx, my});
    }
    fclose(f);
}

double plat_time() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - g_qpc0.QuadPart) / (double)g_qpf.QuadPart;
}

void plat_sleep_until(double t) {
    for (;;) {
        double d = t - plat_time();
        if (d <= 0) break;
        if (d > 0.002) Sleep((DWORD)((d - 0.0015) * 1000));
        else YieldProcessor();
    }
}

const char* plat_key_name(int vk) {
    static char buf[64];
    switch (vk) {
    case VK_LBUTTON: return "Mouse Left";
    case VK_RBUTTON: return "Mouse Right";
    case VK_MBUTTON: return "Mouse Middle";
    case VK_XBUTTON1: return "Mouse 4";
    case VK_XBUTTON2: return "Mouse 5";
    case VK_LSHIFT: return "Left Shift";
    case VK_RSHIFT: return "Right Shift";
    case VK_LCONTROL: return "Left Ctrl";
    case VK_RCONTROL: return "Right Ctrl";
    case VK_LMENU: return "Left Alt";
    case VK_RMENU: return "Right Alt";
    case VK_UP: return "Up";
    case VK_DOWN: return "Down";
    case VK_LEFT: return "Left";
    case VK_RIGHT: return "Right";
    case VK_SPACE: return "Space";
    case VK_RETURN: return "Enter";
    case VK_BACK: return "Backspace";
    }
    UINT sc = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    if (sc && GetKeyNameTextA((LONG)(sc << 16), buf, sizeof buf) > 0) return buf;
    snprintf(buf, sizeof buf, "Key %d", vk);
    return buf;
}

int plat_scan_key() {
    for (int vk = 1; vk < 255; vk++) {
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_TAB || vk == VK_F1 || vk == VK_LWIN || vk == VK_RWIN) continue;
        if (GetAsyncKeyState(vk) & 0x8000) return vk;
    }
    return 0;
}

static bool game_has_focus() { return g_hwnd && GetForegroundWindow() == g_hwnd && !ui_menu_open(); }

void plat_input_poll() {
    u32 sdown = 0;
    int smx = 0, smy = 0;
    for (auto& s : g_script)
        if (g_frame >= s.from && g_frame <= s.to) { sdown |= s.mask; smx += s.mx; smy += s.my; }
    u16 down = (u16)sdown;
    u8 ax[4] = {128, 128, 128, 128};   // RX, RY, LX, LY
    if (sdown & (1u << 16)) ax[0] = 0;
    if (sdown & (1u << 17)) ax[0] = 255;
    if (sdown & (1u << 18)) ax[1] = 0;
    if (sdown & (1u << 19)) ax[1] = 255;
    bool focus = !g_cfg.headless && game_has_focus();
    if (focus) {
        for (int b = 0; b < 16; b++)
            for (int s = 0; s < 2; s++) {
                int vk = g_cfg.binds[b][s];
                if (!vk) continue;
                bool mouse_btn = vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
                if (mouse_btn && !g_captured) continue;
                if (GetAsyncKeyState(vk) & 0x8000) down |= 1u << b;
            }
        if (g_wheel_pulse > 0) { g_wheel_pulse--; down |= 1u << 10; }   // wheel = cycle weapons (L1)
    }
    if (!g_cfg.headless) {
        XINPUT_STATE xs;
        if (!ui_menu_open() && XInputGetState(0, &xs) == ERROR_SUCCESS) {
            WORD b = xs.Gamepad.wButtons;
            if (b & XINPUT_GAMEPAD_BACK) down |= 1 << 0;
            if (b & XINPUT_GAMEPAD_LEFT_THUMB) down |= 1 << 1;
            if (b & XINPUT_GAMEPAD_RIGHT_THUMB) down |= 1 << 2;
            if (b & XINPUT_GAMEPAD_START) down |= 1 << 3;
            if (b & XINPUT_GAMEPAD_DPAD_UP) down |= 1 << 4;
            if (b & XINPUT_GAMEPAD_DPAD_RIGHT) down |= 1 << 5;
            if (b & XINPUT_GAMEPAD_DPAD_DOWN) down |= 1 << 6;
            if (b & XINPUT_GAMEPAD_DPAD_LEFT) down |= 1 << 7;
            if (xs.Gamepad.bLeftTrigger > 60) down |= 1 << 8;
            if (xs.Gamepad.bRightTrigger > 60) down |= 1 << 9;
            if (b & XINPUT_GAMEPAD_LEFT_SHOULDER) down |= 1 << 10;
            if (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) down |= 1 << 11;
            if (b & XINPUT_GAMEPAD_Y) down |= 1 << 12;
            if (b & XINPUT_GAMEPAD_B) down |= 1 << 13;
            if (b & XINPUT_GAMEPAD_A) down |= 1 << 14;
            if (b & XINPUT_GAMEPAD_X) down |= 1 << 15;
            auto conv = [](SHORT v, bool inv) -> u8 {
                int t = (v + 32768) >> 8;
                if (inv) t = 255 - t;
                return (u8)t;
            };
            ax[0] = conv(xs.Gamepad.sThumbRX, false);
            ax[1] = conv(xs.Gamepad.sThumbRY, true);
            ax[2] = conv(xs.Gamepad.sThumbLX, false);
            ax[3] = conv(xs.Gamepad.sThumbLY, true);
            static u8 last_m[2];
            if (last_m[0] != g_pad_motor[0][0] || last_m[1] != g_pad_motor[0][1]) {
                last_m[0] = g_pad_motor[0][0];
                last_m[1] = g_pad_motor[0][1];
                XINPUT_VIBRATION vib = {(WORD)(last_m[1] * 257), (WORD)(last_m[0] ? 0xFFFF : 0)};
                XInputSetState(0, &vib);
            }
        }
    }
    // mouse look: mouse speed becomes right-stick deflection (the game's camera is rate controlled).
    // While the first-person view button (R2) is held the same motion also drives the left stick.
    if (focus && g_cfg.mouse && g_captured) {
        long dx = g_mouse_dx, dy = g_mouse_dy;
        g_mouse_dx = g_mouse_dy = 0;
        // the game's camera stick is "pull" style on both axes: mouse right must push the stick left
        g_mx_f = g_mx_f * 0.4f - (float)dx * (g_cfg.mouse_invert_x ? -1.0f : 1.0f);
        g_my_f = g_my_f * 0.4f - (float)dy * (g_cfg.mouse_invert_y ? -1.0f : 1.0f);
        auto stick = [](float v, float sens) -> int {
            float d = v * sens * 5.0f;
            if (fabsf(d) < 1.0f) return 0;
            d += d > 0 ? 30.0f : -30.0f;   // jump over the game's dead zone
            return (int)(d > 127 ? 127 : d < -127 ? -127 : d);
        };
        int sx = stick(g_mx_f, g_cfg.mouse_sens), sy = stick(g_my_f, g_cfg.mouse_sens);
        if (down & (1u << 9)) {
            // first-person view (R2 held): the game aims with the left stick, and directly
            // (stick right = look right), unlike the pull-style third-person camera
            int k = g_cfg.mouse_aim_invert ? 1 : -1;
            if (g_cfg.mouse_direct_aim) {
                // True mouse aim: add the raw mouse movement straight to the game's own view
                // angles (s16, 4096 units per turn; pitch at 800f0ca6, yaw at 800f0ca8). No stick,
                // no dead zone, no acceleration: 1:1 and sub-step accurate.
                g_aim_mx += dx;
                g_aim_my += dy * (g_cfg.mouse_aim_invert ? -1 : 1);
                g_mx_f = g_my_f = 0;
            } else {
                if (sx) ax[2] = (u8)(128 - sx);
                if (sy) ax[3] = (u8)(128 + k * sy);
            }
        } else if (g_cfg.mouse_direct_cam) {
            // third person: hand the raw counts to the camera hook (game_c12.cpp), which turns the
            // game's camera by exactly that much on its next update
            g_cam_mx += dx * (g_cfg.mouse_invert_x ? -1 : 1);
            g_cam_my += dy * (g_cfg.mouse_invert_y ? -1 : 1);
            g_mx_f = g_my_f = 0;
        } else {
            if (sx) ax[0] = (u8)(128 + sx);
            if (sy) ax[1] = (u8)(128 + sy);
        }
    } else g_mouse_dx = g_mouse_dy = 0;
    if (g_cfg.mouse_direct_cam && !(down & (1u << 9))) { g_cam_mx += smx; g_cam_my += smy; }   // scripted mouse (tests)
    if (g_cfg.mouse_direct_aim && (down & (1u << 9))) { g_aim_mx += smx; g_aim_my += smy; }
    if (g_cfg.mouse_direct_aim && (down & (1u << 9)) && (g_aim_mx || g_aim_my)) {
        // True mouse aim: add the movement straight to the scope view's angles (s16, 4096 per turn;
        // pitch at 800f0ca6, yaw at 800f0ca8). The level code turns the hero (and so his shots) only
        // when its own input turned the view, so his rotation - a turn about the vertical axis by
        // that yaw, in the hero object's matrix - is set here as well.
        static float fy, fp;
        float scale = 0.55f * g_cfg.mouse_sens;
        fy += (float)g_aim_mx * scale;
        fp += (float)g_aim_my * scale;
        g_aim_mx = g_aim_my = 0;
        int iy = (int)fy, ip = (int)fp;
        fy -= iy; fp -= ip;
        if (iy || ip) {
            s16 yaw, pitch;
            memcpy(&pitch, g_ram + 0xf0ca6, 2);
            memcpy(&yaw, g_ram + 0xf0ca8, 2);
            yaw = (s16)(((yaw + iy) & 0xfff));
            int np = pitch + ip;
            if (np > 780) np = 780;
            if (np < -780) np = -780;
            pitch = (s16)np;
            memcpy(g_ram + 0xf0ca6, &pitch, 2);
            memcpy(g_ram + 0xf0ca8, &yaw, 2);
            u32 hero;
            memcpy(&hero, g_ram + 0xefdb8, 4);
            if ((hero >> 24) == 0x80 && (hero & 0x1FFFFF) + 0x28 < RAM_SIZE) {
                double a = yaw * (6.283185307179586 / 4096.0);
                s16 cs = (s16)lround(cos(a) * 4096.0), sn = (s16)lround(sin(a) * 4096.0), msn = (s16)-sn;
                u8* m = g_ram + ((hero + 0x14) & 0x1FFFFF);
                memcpy(m + 0, &cs, 2);    // m00
                memcpy(m + 4, &sn, 2);    // m02
                memcpy(m + 12, &msn, 2);  // m20
                memcpy(m + 16, &cs, 2);   // m22
            }
        }
    } else if (!(down & (1u << 9))) g_aim_mx = g_aim_my = 0;
    g_pad_state[0] = (u16)~down;
    memcpy(g_pad_axes[0], ax, 4);
}

u16 pad_buttons(int port) { return g_pad_state[port & 1]; }
void pad_analog(int port, u8 out[4]) { memcpy(out, g_pad_axes[port & 1], 4); }

// ---- audio -----------------------------------------------------------------
static IXAudio2* g_xa;
static IXAudio2MasteringVoice* g_master;
static IXAudio2SourceVoice* g_src;
#define ABUF_FRAMES 441
#define ABUF_COUNT 64
static float g_abuf[ABUF_COUNT][ABUF_FRAMES * 2];
static int g_abuf_idx, g_abuf_fill;

static bool audio_init() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    if (FAILED(XAudio2Create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) return false;
    if (FAILED(g_xa->CreateMasteringVoice(&g_master))) return false;
    WAVEFORMATEX wf = {};
    wf.wFormatTag = 3;  // IEEE float
    wf.nChannels = 2;
    wf.nSamplesPerSec = 44100;
    wf.wBitsPerSample = 32;
    wf.nBlockAlign = 8;
    wf.nAvgBytesPerSec = 44100 * 8;
    if (FAILED(g_xa->CreateSourceVoice(&g_src, &wf))) return false;
    g_src->Start();
    return true;
}

int plat_audio_queued_ms() {
    if (!g_src) return 0;
    XAUDIO2_VOICE_STATE st;
    g_src->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    return (int)(st.BuffersQueued * ABUF_FRAMES * 1000 / 44100);
}

void plat_audio_push(const float* lr, int frames) {
    if (!g_src) return;
    while (frames > 0) {
        int n = ABUF_FRAMES - g_abuf_fill;
        if (n > frames) n = frames;
        float* dst = &g_abuf[g_abuf_idx][g_abuf_fill * 2];
        float vol = g_cfg.volume;
        for (int i = 0; i < n * 2; i++) dst[i] = lr[i] * vol;
        g_abuf_fill += n;
        lr += n * 2;
        frames -= n;
        if (g_abuf_fill == ABUF_FRAMES) {
            XAUDIO2_VOICE_STATE st;
            g_src->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            int maxq = (g_cfg.audio_latency_ms * 4 * 44100 / 1000) / ABUF_FRAMES + 2;
            if (maxq > ABUF_COUNT - 2) maxq = ABUF_COUNT - 2;
            if ((int)st.BuffersQueued < maxq && !g_turbo) {
                XAUDIO2_BUFFER b = {};
                b.AudioBytes = ABUF_FRAMES * 8;
                b.pAudioData = (const BYTE*)g_abuf[g_abuf_idx];
                g_src->SubmitSourceBuffer(&b);
                g_abuf_idx = (g_abuf_idx + 1) % ABUF_COUNT;
            }
            g_abuf_fill = 0;
        }
    }
}

// ---- window ----------------------------------------------------------------
void plat_toggle_fullscreen() {
    static WINDOWPLACEMENT prev = {sizeof prev};
    HWND h = g_hwnd;
    if (!h) return;
    DWORD style = GetWindowLong(h, GWL_STYLE);
    if (style & WS_OVERLAPPEDWINDOW) {
        MONITORINFO mi = {sizeof mi};
        GetWindowPlacement(h, &prev);
        GetMonitorInfo(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowLong(h, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
        g_cfg.fullscreen = 1;
    } else {
        SetWindowLong(h, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(h, &prev);
        SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        g_cfg.fullscreen = 0;
    }
}

// hide + confine the cursor while the game (not the menu) has the mouse
static void update_capture() {
    bool want = g_cfg.mouse && game_has_focus();
    if (want == g_captured) {
        if (want) {   // keep the clip rectangle current (window may have moved)
            RECT rc;
            GetClientRect(g_hwnd, &rc);
            MapWindowPoints(g_hwnd, nullptr, (POINT*)&rc, 2);
            ClipCursor(&rc);
        }
        return;
    }
    g_captured = want;
    if (want) {
        while (ShowCursor(FALSE) >= 0) {}
        g_mouse_dx = g_mouse_dy = 0;
    } else {
        ClipCursor(nullptr);
        while (ShowCursor(TRUE) < 0) {}
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (ui_wndproc(h, m, w, l)) return 1;
    switch (m) {
    case WM_CLOSE: g_quit = 1; return 0;
    case WM_SIZE: g_window_resized = true; return 0;
    case WM_INPUT: {
        RAWINPUT ri;
        UINT sz = sizeof ri;
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 && ri.header.dwType == RIM_TYPEMOUSE) {
            if (!(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) { g_mouse_dx += ri.data.mouse.lLastX; g_mouse_dy += ri.data.mouse.lLastY; }
            if ((ri.data.mouse.usButtonFlags & RI_MOUSE_WHEEL) && g_captured) g_wheel_pulse = 3;
        }
        break;
    }
    case WM_KEYDOWN:
        if (l & 0x40000000) return 0;   // auto-repeat
        if (w == VK_ESCAPE || w == VK_F1) ui_toggle_menu();
        else if (w == VK_TAB) settings_toggle_classic();
        else if (w == VK_OEM_3) g_turbo = true;
        else if (w == VK_F11) plat_toggle_fullscreen();
        return 0;
    case WM_KEYUP:
        if (w == VK_OEM_3) g_turbo = false;
        return 0;
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN) { plat_toggle_fullscreen(); return 0; }
        break;
    case WM_KILLFOCUS: g_turbo = false; break;
    }
    return DefWindowProc(h, m, w, l);
}

bool plat_init() {
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_qpc0);
    if (!g_cfg.input_script.empty()) plat_load_script(g_cfg.input_script.c_str());
    if (g_cfg.headless) return true;
    timeBeginPeriod(1);
    WNDCLASSA wc = {};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = "C12Recomp";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);
    RECT r = {0, 0, g_cfg.win_w, g_cfg.win_h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowA("C12Recomp", "C-12: Final Resistance (recompiled)", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) return false;
    RAWINPUTDEVICE rid = {0x01, 0x02, 0, g_hwnd};   // generic desktop / mouse
    RegisterRawInputDevices(&rid, 1, sizeof rid);
    if (g_cfg.fullscreen) { g_cfg.fullscreen = 0; plat_toggle_fullscreen(); }
    if (!audio_init()) logf("audio init failed (continuing silent)\n");
    return true;
}

static double g_next_frame;

void plat_pump() {
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    update_capture();
    // pace to the emulated refresh rate
    double period = 1.0 / 49.76;
    double now = plat_time();
    if (g_turbo && !ui_menu_open()) { g_next_frame = now; return; }
    if (g_next_frame < now - 0.1) g_next_frame = now;
    g_next_frame += period;
    plat_sleep_until(g_next_frame);
}

void plat_shutdown() {}

void plat_present_sw(const u32* bgra, int w, int h) {
    if (!g_hwnd) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int cw = rc.right, ch = rc.bottom;
    double aspect = g_cfg.widescreen ? 16.0 / 9.0 : 4.0 / 3.0;
    int dw = cw, dh = (int)(cw / aspect);
    if (dh > ch) { dh = ch; dw = (int)(ch * aspect); }
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    HDC dc = GetDC(g_hwnd);
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, (cw - dw) / 2, (ch - dh) / 2, dw, dh, 0, 0, w, h, bgra, &bi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(g_hwnd, dc);
}
