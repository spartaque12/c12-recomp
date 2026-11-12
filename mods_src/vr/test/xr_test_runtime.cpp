// A minimal OpenXR runtime for testing C-12's VR layer (mods_src\vr\xr.cpp) without a headset (never installed: a run
// selects it for itself with XR_RUNTIME_JSON=<mods>\vr_xr_test_runtime.json, which the Khronos loader honours per
// process when not elevated; otherwise C12_VR_RUNTIME=<mods>\vr_xr_test_runtime.dll makes xr.cpp load it directly).
// Ported from the VCS-Recomp VR.asi test runtime.
//
// One simulated HMD (two views, 90 Hz), D3D11 only (XR_KHR_D3D11_enable), LOCAL / VIEW / STAGE spaces, swapchains of
// real D3D11 textures on the application's device, the session states up to FOCUSED. The head:
//   XRTEST_HEAD=yaw,pitch,roll[,x,y,z]   degrees (left +, up +, right +) and metres, fixed
//   XRTEST_YAW_SWEEP=deg                 plus a yaw swinging +-deg over 8 s
//   XRTEST_IPD=mm                        the eyes' distance (64): views at -IPD/2 and +IPD/2 on the head's x axis
// The eyes' fields of view: 50 deg up / down, 50 deg outwards, 45 deg inwards (asymmetric, as real headsets).
// Log: stderr, and XRTEST_LOG=<file> (appended) when set.
// Captures (what the headset would show): XRTEST_CAPTURE=<dir>, every XRTEST_CAPTURE_EVERY frames (90), at most
// XRTEST_CAPTURE_MAX (20): per capture eye_<n>_left / eye_<n>_right - each eye's view (110 deg, 900x900) composited on
// the CPU from the submitted layers (a stereo projection layer: each eye from its own view's image) - the layers' own
// images layer_<n>_<i>[_v<view>], and a line per frame in layers.txt (layer kinds, poses, fields of view, sizes).
// XRTEST_CAPTURE_FORMAT=bmp|png (png). On request: a line "capture <name>" in the control file (XRTEST_CONTROL) or a
// file <dir>\capture.txt holding a name captures the next frame as eye_<name>_left / _right (and layer_<name>_*),
// whatever the period; with XRTEST_CAPTURE unset, "capture" writes into the control file's folder.
//
// Controllers: the actions (paths, action sets, actions, suggested bindings validated per profile - khr/simple,
// oculus/touch, valve/index, htc/vive, microsoft/motion, hp/mixed_reality with XR_EXT_hp_mixed_reality_controller,
// which is offered unless XRTEST_NO_HP_EXT is set -, attach, sync, states, action spaces, haptics, which are logged).
// The hardware simulated is an oculus/touch_controller pair: a hand has that profile once the application bound
// something of it there (INTERACTION_PROFILE_CHANGED at the first xrSyncActions); actions are active only while the
// session is FOCUSED. The hands' poses are in the recentred space (the head at the session's start, or at the last
// "recenter", yaw only): left (-0.2, -0.3, -0.35), right (0.2, -0.3, -0.35), pointing ahead; aim = grip. Control file:
//   hand l|r x,y,z[,yaw,pitch,roll]      the grip pose (m, deg)
//   button l|r trigger|squeeze|stick_click|primary|secondary|menu <0..1>   (primary = X / A, secondary = Y / B)
//   stick l|r x,y
#include <windows.h>
#include <unknwn.h>
#include <d3d11.h>
#include <dxgi.h>

#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include <algorithm>
#include <cstdarg>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "dxgi.lib")

namespace
{
    constexpr float kPi = 3.14159265f;

    void rtLog(const char *format, ...)
    {
        char text[1024];
        va_list args;
        va_start(args, format);
        std::vsnprintf(text, sizeof(text), format, args);
        va_end(args);
        std::fprintf(stderr, "[xr_test_runtime] %s", text);
        static const char *path = std::getenv("XRTEST_LOG");
        if (path && *path)
            if (FILE *f = std::fopen(path, "a"))
            {
                std::fprintf(f, "[xr_test_runtime] %s", text);
                std::fclose(f);
            }
    }

    float envFloat(const char *name, float fallback)
    {
        const char *e = std::getenv(name);
        return e && *e ? static_cast<float>(std::atof(e)) : fallback;
    }
    // half the eyes' distance, metres
    float halfIpd()
    {
        static const float h = std::clamp(envFloat("XRTEST_IPD", 64.0f), 0.0f, 100.0f) * 0.0005f;
        return h;
    }

    int64_t nowNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // ---- maths ------------------------------------------------------------------------------------------------------
    struct V3
    {
        float x = 0, y = 0, z = 0;
    };
    V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
    XrQuaternionf qmul(const XrQuaternionf &a, const XrQuaternionf &b)
    {
        return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    }
    XrQuaternionf qconj(const XrQuaternionf &q) { return {-q.x, -q.y, -q.z, q.w}; }
    V3 rot(const XrQuaternionf &q, V3 v)
    {
        const XrQuaternionf p = qmul(qmul(q, {v.x, v.y, v.z, 0.0f}), qconj(q));
        return {p.x, p.y, p.z};
    }
    XrQuaternionf axisAngle(V3 axis, float a) { return {axis.x * std::sin(a / 2), axis.y * std::sin(a / 2), axis.z * std::sin(a / 2), std::cos(a / 2)}; }
    XrPosef compose(const XrPosef &a, const XrPosef &b) // a * b: b in a's frame
    {
        const V3 p = rot(a.orientation, {b.position.x, b.position.y, b.position.z});
        XrPosef r;
        r.orientation = qmul(a.orientation, b.orientation);
        r.position = {a.position.x + p.x, a.position.y + p.y, a.position.z + p.z};
        return r;
    }
    XrPosef inverse(const XrPosef &a)
    {
        XrPosef r;
        r.orientation = qconj(a.orientation);
        const V3 p = rot(r.orientation, {-a.position.x, -a.position.y, -a.position.z});
        r.position = {p.x, p.y, p.z};
        return r;
    }
    XrPosef identity()
    {
        XrPosef p = {};
        p.orientation.w = 1.0f;
        return p;
    }

    // ---- PNG (stored deflate) ---------------------------------------------------------------------------------------
    uint32_t crc32(const uint8_t *d, size_t n, uint32_t c = 0)
    {
        static uint32_t table[256];
        static bool made = false;
        if (!made)
        {
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t k = i;
                for (int j = 0; j < 8; ++j)
                    k = (k & 1) ? 0xEDB88320u ^ (k >> 1) : k >> 1;
                table[i] = k;
            }
            made = true;
        }
        c = ~c;
        for (size_t i = 0; i < n; ++i)
            c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
        return ~c;
    }
    void writePng(const std::string &path, const std::vector<uint8_t> &rgb, int w, int h)
    {
        std::vector<uint8_t> raw;
        raw.reserve(static_cast<size_t>(h) * (w * 3 + 1));
        for (int y = 0; y < h; ++y)
        {
            raw.push_back(0);
            raw.insert(raw.end(), rgb.begin() + static_cast<size_t>(y) * w * 3, rgb.begin() + static_cast<size_t>(y + 1) * w * 3);
        }
        std::vector<uint8_t> z = {0x78, 0x01};
        uint32_t a = 1, b = 0;
        for (uint8_t v : raw)
        {
            a = (a + v) % 65521u;
            b = (b + a) % 65521u;
        }
        for (size_t off = 0; off < raw.size() || off == 0;)
        {
            const size_t n = std::min<size_t>(65535, raw.size() - off);
            const bool last = off + n >= raw.size();
            z.push_back(last ? 1 : 0);
            z.push_back(static_cast<uint8_t>(n & 0xFF)), z.push_back(static_cast<uint8_t>(n >> 8));
            z.push_back(static_cast<uint8_t>(~n & 0xFF)), z.push_back(static_cast<uint8_t>((~n >> 8) & 0xFF));
            z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
            off += n;
            if (last)
                break;
        }
        const uint32_t adler = (b << 16) | a;
        for (int i = 3; i >= 0; --i)
            z.push_back((adler >> (8 * i)) & 0xFF);
        FILE *f = std::fopen(path.c_str(), "wb");
        if (!f)
            return;
        auto be32 = [&](uint32_t v) {
            const uint8_t x[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
            std::fwrite(x, 1, 4, f);
        };
        auto chunk = [&](const char *type, const std::vector<uint8_t> &data) {
            be32(static_cast<uint32_t>(data.size()));
            std::vector<uint8_t> td(type, type + 4);
            td.insert(td.end(), data.begin(), data.end());
            std::fwrite(td.data(), 1, td.size(), f);
            be32(crc32(td.data(), td.size()));
        };
        const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        std::fwrite(sig, 1, 8, f);
        std::vector<uint8_t> ihdr = {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
                                     uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h), 8, 2, 0, 0, 0};
        chunk("IHDR", ihdr);
        chunk("IDAT", z);
        chunk("IEND", {});
        std::fclose(f);
    }

    // ---- objects ----------------------------------------------------------------------------------------------------
    struct Swapchain
    {
        uint32_t width = 0, height = 0;
        int64_t format = 0;
        std::vector<ID3D11Texture2D *> images;
        uint32_t next = 0, acquired = ~0u, last = ~0u; // last: the most recently released image (~0u: none yet)
    };
    struct Action;
    struct Space
    {
        XrReferenceSpaceType type = XR_REFERENCE_SPACE_TYPE_LOCAL;
        XrPosef offset = identity();
        Action *action = nullptr; // an action space (a pose action): follows that hand's controller
        int hand = 0;             // 0 left, 1 right
    };

    // ---- actions ----------------------------------------------------------------------------------------------------
    // what a binding path's component is (see componentKind)
    enum Kind
    {
        KNone,
        KBool,
        KFloat,
        KVec,
        KPose,
        KHaptic
    };
    struct ActionSet
    {
        uintptr_t instance = 0;
        std::string name, localized;
        uint32_t priority = 0;
        bool attached = false; // to any session: immutable from then on
        std::vector<Action *> actions;
    };
    struct Binding
    {
        XrPath profile = XR_NULL_PATH;
        int hand = 0;          // 0 left, 1 right
        std::string component; // under /user/hand/<hand>/, e.g. "input/trigger/value"
        Kind kind = KNone;
    };
    struct Action
    {
        ActionSet *set = nullptr;
        std::string name, localized;
        XrActionType type = XR_ACTION_TYPE_BOOLEAN_INPUT;
        std::vector<XrPath> subactions;
        std::vector<Binding> bindings; // all profiles
        // the synced state: per hand (0 left, 1 right) and both hands combined (2: subactionPath XR_NULL_PATH)
        struct State
        {
            bool active = false;
            float v[2] = {0.0f, 0.0f};
            bool changed = false;
            XrTime lastChange = 0;
        } st[3];
    };
    std::vector<ActionSet *> g_actionSets; // the live ones (handle checks)
    std::vector<Action *> g_actions;

    // the simulated oculus/touch_controller pair: pose in the recentred space (x, y, z m; yaw, pitch, roll deg) and
    // the inputs; the control file's "hand", "button" and "stick" set them
    struct Controller
    {
        float pose[6];
        float trigger = 0, squeeze = 0, stick[2] = {0, 0}, stickClick = 0, primary = 0, secondary = 0, menu = 0;
    };
    Controller g_ctl[2] = {{{-0.2f, -0.3f, -0.35f, 0, 0, 0}}, {{0.2f, -0.3f, -0.35f, 0, 0, 0}}};
    XrPosef g_handRef = identity(); // the recentred space in LOCAL: the head at the session's start (or "recenter"), yaw only
    std::vector<std::string> g_paths; // XrPath n: g_paths[n - 1]
    bool g_hpEnabled = false;         // XR_EXT_hp_mixed_reality_controller enabled by the application

    struct Session
    {
        ID3D11Device *device = nullptr;
        ID3D11DeviceContext *context = nullptr;
        bool running = false;   // between xrBeginSession and xrEndSession
        bool lost = false;      // LOSS_PENDING sent: every call fails with XR_ERROR_SESSION_LOST
        bool exitAfterEnd = false;
        XrSessionState state = XR_SESSION_STATE_UNKNOWN;
        bool waited = false, begun = false; // the frame loop: xrWaitFrame -> xrBeginFrame -> xrEndFrame
        int64_t predicted = 0;
        std::vector<ActionSet *> attached; // xrAttachSessionActionSets (once)
        bool profilesPending = false;      // the interaction profiles get chosen at the next xrSyncActions
        XrPath profile[2] = {XR_NULL_PATH, XR_NULL_PATH};
    };

    std::mutex g_mutex;
    std::deque<XrEventDataBuffer> g_events;
    float g_head[6] = {};  // the head: yaw, pitch, roll (degrees), x, y, z (m) - XRTEST_HEAD / the control file's "head"
    bool g_headSet = false; // set by the control file: the yaw sweep stops
    Session *g_session = nullptr;
    int64_t g_start = 0, g_nextDisplay = 0;
    int64_t g_periodNs = 11111111; // XRTEST_HZ / the control file's "hz N"
    uint64_t g_frames = 0;
    int g_captures = 0;
    std::string g_captureRequest; // the control file's "capture <name>": the next frame
    uintptr_t g_instanceId = 0x1001;
    bool g_instanceLost = false;
    uint64_t g_violations = 0;

    XrInstance instanceHandle() { return reinterpret_cast<XrInstance>(g_instanceId); }

    // a spec violation by the application: counted and logged (the test checks for "VALIDATION")
    XrResult violation(XrResult r, const char *what)
    {
        ++g_violations;
        if (g_violations <= 50)
            rtLog("VALIDATION: %s (XrResult %d)\n", what, static_cast<int>(r));
        return r;
    }

    void pushEvent(const void *ev, size_t size)
    {
        XrEventDataBuffer b = {XR_TYPE_EVENT_DATA_BUFFER};
        std::memcpy(&b, ev, std::min(size, sizeof(b)));
        g_events.push_back(b);
    }

    void pushState(XrSessionState s)
    {
        XrEventDataSessionStateChanged e = {XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED};
        e.session = reinterpret_cast<XrSession>(g_session);
        e.state = s;
        e.time = nowNs();
        if (g_session)
            g_session->state = s;
        pushEvent(&e, sizeof(e));
        rtLog("-> session state %d\n", static_cast<int>(s));
    }

    void resetHandRef(int64_t t);
    int handArg(const std::string &s) // "l" / "left" -> 0, "r" / "right" -> 1, else -1
    {
        if (s == "l" || s == "left")
            return 0;
        if (s == "r" || s == "right")
            return 1;
        return -1;
    }

    // XRTEST_CONTROL=<file>: one command per line, read and deleted when seen (polled every frame / event poll):
    //   sleep | wake | unfocus | focus | loss | exit | instance_loss | recenter | hz <n> | head y,p,r[,x,y,z] |
    //   capture <name> | hand l|r x,y,z[,yaw,pitch,roll] | button l|r trigger|squeeze|stick_click|primary|secondary|menu <0..1> |
    //   stick l|r x,y
    void controlPoll()
    {
        static const char *path = std::getenv("XRTEST_CONTROL");
        if (!path || !*path)
            return;
        // taken by a rename first: a line written while it is read is not lost with the file
        const std::string taken = std::string(path) + ".taken";
        std::remove(taken.c_str());
        if (std::rename(path, taken.c_str()) != 0)
            return;
        FILE *f = std::fopen(taken.c_str(), "r");
        if (!f)
            return;
        std::vector<std::string> cmds;
        char line[128];
        while (std::fgets(line, sizeof(line), f))
        {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
                s.pop_back();
            if (!s.empty())
                cmds.push_back(s);
        }
        std::fclose(f);
        std::remove(taken.c_str());
        for (const std::string &c : cmds)
        {
            rtLog("control: %s\n", c.c_str());
            Session *s = g_session;
            if (c == "sleep" && s && s->running && !s->lost)
            {
                if (s->state == XR_SESSION_STATE_FOCUSED)
                    pushState(XR_SESSION_STATE_VISIBLE);
                pushState(XR_SESSION_STATE_SYNCHRONIZED);
                pushState(XR_SESSION_STATE_STOPPING);
            }
            else if (c == "wake" && s && !s->running && s->state == XR_SESSION_STATE_IDLE)
                pushState(XR_SESSION_STATE_READY);
            else if (c == "unfocus" && s && s->state == XR_SESSION_STATE_FOCUSED)
                pushState(XR_SESSION_STATE_VISIBLE);
            else if (c == "focus" && s && s->state == XR_SESSION_STATE_VISIBLE)
                pushState(XR_SESSION_STATE_FOCUSED);
            else if (c == "loss" && s)
            {
                s->lost = true;
                pushState(XR_SESSION_STATE_LOSS_PENDING);
            }
            else if (c == "exit" && s && s->running)
            {
                s->exitAfterEnd = true;
                if (s->state == XR_SESSION_STATE_FOCUSED)
                    pushState(XR_SESSION_STATE_VISIBLE);
                pushState(XR_SESSION_STATE_SYNCHRONIZED);
                pushState(XR_SESSION_STATE_STOPPING);
            }
            else if (c == "instance_loss")
            {
                XrEventDataInstanceLossPending e = {XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING};
                e.lossTime = nowNs();
                pushEvent(&e, sizeof(e));
                g_instanceLost = true;
                if (s)
                    s->lost = true;
            }
            else if (c == "recenter" && s)
            {
                XrEventDataReferenceSpaceChangePending e = {XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING};
                e.session = reinterpret_cast<XrSession>(s);
                e.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
                e.changeTime = nowNs();
                e.poseValid = XR_TRUE;
                e.poseInPreviousSpace = identity();
                pushEvent(&e, sizeof(e));
                resetHandRef(nowNs()); // the application recentres on the head now: the hands follow
            }
            else if (c.rfind("hand ", 0) == 0 || c.rfind("button ", 0) == 0 || c.rfind("stick ", 0) == 0)
            {
                char which[16] = {}, what[32] = {};
                const char *args = c.c_str() + c.find(' ') + 1;
                int consumed = 0;
                if (std::sscanf(args, "%15s %n", which, &consumed) < 1 || handArg(which) < 0)
                {
                    rtLog("control: bad hand in \"%s\" (l or r)\n", c.c_str());
                    continue;
                }
                Controller &ctl = g_ctl[handArg(which)];
                args += consumed;
                if (c[0] == 'h') // hand l|r x,y,z[,yaw,pitch,roll]
                {
                    float p[6] = {};
                    if (std::sscanf(args, "%f,%f,%f,%f,%f,%f", &p[0], &p[1], &p[2], &p[3], &p[4], &p[5]) < 3)
                        rtLog("control: \"hand\" needs x,y,z[,yaw,pitch,roll]\n");
                    else
                        std::copy(p, p + 6, ctl.pose);
                }
                else if (c[0] == 's') // stick l|r x,y
                {
                    float x = 0, y = 0;
                    if (std::sscanf(args, "%f,%f", &x, &y) < 2)
                        rtLog("control: \"stick\" needs x,y\n");
                    else
                        ctl.stick[0] = std::clamp(x, -1.0f, 1.0f), ctl.stick[1] = std::clamp(y, -1.0f, 1.0f);
                }
                else // button l|r <name> <value>
                {
                    float v = 0;
                    if (std::sscanf(args, "%31s %f", what, &v) < 2)
                    {
                        rtLog("control: \"button\" needs a name and a value\n");
                        continue;
                    }
                    v = std::clamp(v, 0.0f, 1.0f);
                    const std::string n(what);
                    float *slot = n == "trigger" ? &ctl.trigger : n == "squeeze" ? &ctl.squeeze : n == "stick_click" ? &ctl.stickClick
                                  : n == "primary" ? &ctl.primary : n == "secondary" ? &ctl.secondary : n == "menu" ? &ctl.menu : nullptr;
                    if (slot)
                        *slot = v;
                    else
                        rtLog("control: unknown button \"%s\" (trigger squeeze stick_click primary secondary menu)\n", what);
                }
            }
            else if (c.rfind("head ", 0) == 0)
            {
                float h[6] = {};
                std::sscanf(c.c_str() + 5, "%f,%f,%f,%f,%f,%f", &h[0], &h[1], &h[2], &h[3], &h[4], &h[5]);
                std::copy(h, h + 6, g_head);
                g_headSet = true;
            }
            else if (c == "capture" || c.rfind("capture ", 0) == 0)
            {
                std::string name = c.size() > 8 ? c.substr(8) : std::string("request");
                for (char &ch : name)
                    if (ch == '\\' || ch == '/' || ch == ':' || ch == ' ')
                        ch = '_';
                g_captureRequest = name;
            }
            else if (c.rfind("hz ", 0) == 0)
            {
                const double hz = std::atof(c.c_str() + 3);
                if (hz >= 30.0 && hz <= 240.0)
                    g_periodNs = static_cast<int64_t>(1e9 / hz);
            }
        }
    }
    // the simulated head in LOCAL at time t (ns)
    XrPosef headAt(int64_t t)
    {
        static bool parsed = false;
        if (!parsed)
        {
            parsed = true;
            if (const char *e = std::getenv("XRTEST_HEAD")) // yaw,pitch,roll[,x,y,z]
                std::sscanf(e, "%f,%f,%f,%f,%f,%f", &g_head[0], &g_head[1], &g_head[2], &g_head[3], &g_head[4], &g_head[5]);
        }
        const float *fixed = g_head;
        const float sweep = g_headSet ? 0.0f : envFloat("XRTEST_YAW_SWEEP", 0.0f);
        const double secs = (t - g_start) / 1e9;
        const float yaw = (fixed[0] + sweep * static_cast<float>(std::sin(secs * 2.0 * kPi / 8.0))) * kPi / 180.0f;
        const float pitch = fixed[1] * kPi / 180.0f, roll = fixed[2] * kPi / 180.0f;
        XrPosef p = identity();
        // yaw about +y, then pitch about the head's x, then roll about its -z (right ear down = roll right +)
        p.orientation = qmul(qmul(axisAngle({0, 1, 0}, yaw), axisAngle({1, 0, 0}, pitch)), axisAngle({0, 0, -1}, roll));
        p.position = {fixed[3], fixed[4], fixed[5]};
        return p;
    }

    // the recentred space as the application makes it: the head's position, its yaw only
    void resetHandRef(int64_t t)
    {
        const XrPosef h = headAt(t);
        const XrQuaternionf &q = h.orientation;
        const float fx = -(2.0f * (q.x * q.z + q.w * q.y)), fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
        g_handRef.orientation = axisAngle({0, 1, 0}, std::atan2(-fx, -fz));
        g_handRef.position = h.position;
    }

    // a simulated controller's grip (and aim) pose in LOCAL
    XrPosef handInLocal(int hand)
    {
        const float *p = g_ctl[hand].pose;
        XrPosef r = identity();
        r.orientation = qmul(qmul(axisAngle({0, 1, 0}, p[3] * kPi / 180.0f), axisAngle({1, 0, 0}, p[4] * kPi / 180.0f)),
                             axisAngle({0, 0, -1}, p[5] * kPi / 180.0f));
        r.position = {p[0], p[1], p[2]};
        return compose(g_handRef, r);
    }

    XrPosef spaceInLocal(const Space &s, int64_t t)
    {
        if (s.action)
            return compose(handInLocal(s.hand), s.offset);
        if (s.type == XR_REFERENCE_SPACE_TYPE_VIEW)
            return compose(headAt(t), s.offset);
        return s.offset;
    }
    // an action space is tracked while its pose action is active (synced, focused, bound) and the action alive
    bool spaceTracked(const Space &s)
    {
        if (!s.action)
            return true;
        if (std::find(g_actions.begin(), g_actions.end(), s.action) == g_actions.end())
            return false;
        return s.action->st[s.hand].active;
    }

    // ---- captures ---------------------------------------------------------------------------------------------------
    struct Pixels
    {
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
        bool valid() const { return w > 0; }
        void sample(float u, float v, float out[4]) const
        {
            const int x = std::clamp(static_cast<int>(u * w), 0, w - 1), y = std::clamp(static_cast<int>(v * h), 0, h - 1);
            const uint8_t *p = &rgba[(static_cast<size_t>(y) * w + x) * 4];
            for (int i = 0; i < 4; ++i)
                out[i] = p[i] / 255.0f;
        }
    };

    Pixels readImage(Session &s, const Swapchain &sc, uint32_t index)
    {
        Pixels px;
        if (index >= sc.images.size())
            return px;
        D3D11_TEXTURE2D_DESC td = {};
        sc.images[index]->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.MiscFlags = 0;
        ID3D11Texture2D *staging = nullptr;
        if (FAILED(s.device->CreateTexture2D(&td, nullptr, &staging)))
            return px;
        s.context->CopyResource(staging, sc.images[index]);
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (SUCCEEDED(s.context->Map(staging, 0, D3D11_MAP_READ, 0, &m)))
        {
            px.w = static_cast<int>(td.Width), px.h = static_cast<int>(td.Height);
            px.rgba.resize(static_cast<size_t>(px.w) * px.h * 4);
            const bool bgra = sc.format == DXGI_FORMAT_B8G8R8A8_UNORM || sc.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            for (int y = 0; y < px.h; ++y)
            {
                const uint8_t *row = static_cast<const uint8_t *>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
                uint8_t *out = &px.rgba[static_cast<size_t>(y) * px.w * 4];
                std::memcpy(out, row, static_cast<size_t>(px.w) * 4);
                if (bgra)
                    for (int x = 0; x < px.w; ++x)
                        std::swap(out[x * 4], out[x * 4 + 2]);
            }
            s.context->Unmap(staging, 0);
        }
        staging->Release();
        return px;
    }

    // the sub-image's rectangle of a whole image
    Pixels crop(const Pixels &px, const XrRect2Di &r)
    {
        if (!px.valid())
            return px;
        const int x0 = std::clamp(r.offset.x, 0, px.w), y0 = std::clamp(r.offset.y, 0, px.h);
        const int w = std::clamp(r.extent.width, 0, px.w - x0), h = std::clamp(r.extent.height, 0, px.h - y0);
        if (x0 == 0 && y0 == 0 && w == px.w && h == px.h)
            return px;
        Pixels out;
        if (w <= 0 || h <= 0)
            return out;
        out.w = w, out.h = h;
        out.rgba.resize(static_cast<size_t>(w) * h * 4);
        for (int y = 0; y < h; ++y)
            std::memcpy(&out.rgba[static_cast<size_t>(y) * w * 4], &px.rgba[(static_cast<size_t>(y0 + y) * px.w + x0) * 4],
                        static_cast<size_t>(w) * 4);
        return out;
    }

    // 24-bit BMP, bottom-up
    void writeBmp(const std::string &path, const std::vector<uint8_t> &rgb, int w, int h)
    {
        FILE *f = std::fopen(path.c_str(), "wb");
        if (!f)
            return;
        const uint32_t stride = (static_cast<uint32_t>(w) * 3 + 3) & ~3u, size = stride * h;
        uint8_t hdr[54] = {'B', 'M'};
        auto le32 = [&](int at, uint32_t v) {
            for (int i = 0; i < 4; ++i)
                hdr[at + i] = static_cast<uint8_t>(v >> (8 * i));
        };
        le32(2, 54 + size);
        le32(10, 54);
        le32(14, 40);
        le32(18, static_cast<uint32_t>(w));
        le32(22, static_cast<uint32_t>(h));
        hdr[26] = 1;
        hdr[28] = 24;
        le32(34, size);
        std::fwrite(hdr, 1, sizeof(hdr), f);
        std::vector<uint8_t> row(stride, 0);
        for (int y = h - 1; y >= 0; --y)
        {
            for (int x = 0; x < w; ++x)
            {
                const uint8_t *p = &rgb[(static_cast<size_t>(y) * w + x) * 3];
                row[x * 3 + 0] = p[2], row[x * 3 + 1] = p[1], row[x * 3 + 2] = p[0];
            }
            std::fwrite(row.data(), 1, stride, f);
        }
        std::fclose(f);
    }

    // a picture as PNG or BMP (XRTEST_CAPTURE_FORMAT); `name` without extension
    void writeImage(const std::string &name, const std::vector<uint8_t> &rgb, int w, int h)
    {
        static const bool bmp = [] {
            const char *e = std::getenv("XRTEST_CAPTURE_FORMAT");
            return e && (std::strcmp(e, "bmp") == 0 || std::strcmp(e, "BMP") == 0);
        }();
        if (bmp)
            writeBmp(name + ".bmp", rgb, w, h);
        else
            writePng(name + ".png", rgb, w, h);
    }


    std::string captureDir()
    {
        const char *dir = std::getenv("XRTEST_CAPTURE");
        if (dir && *dir)
            return dir;
        if (!g_captureRequest.empty()) // into the control file's folder
            if (const char *c = std::getenv("XRTEST_CONTROL"); c && *c)
            {
                const std::string s(c);
                const size_t slash = s.find_last_of("\\/");
                return slash == std::string::npos ? std::string(".") : s.substr(0, slash);
            }
        return std::string();
    }

    void capture(Session &s, const XrFrameEndInfo *info)
    {
        static const int every = std::max(1, static_cast<int>(envFloat("XRTEST_CAPTURE_EVERY", 90.0f)));
        static const int most = static_cast<int>(envFloat("XRTEST_CAPTURE_MAX", 20.0f));
        const std::string dir = captureDir();
        if (dir.empty())
            return;
        const bool periodic = std::getenv("XRTEST_CAPTURE") && *std::getenv("XRTEST_CAPTURE");
        // a line per frame (layers.txt), every frame
        const std::string base = dir + "\\";
        if (periodic)
            if (FILE *f = std::fopen((base + "layers.txt").c_str(), "a"))
            {
                std::fprintf(f, "frame %llu layers %u:", static_cast<unsigned long long>(g_frames), info->layerCount);
                for (uint32_t i = 0; i < info->layerCount; ++i)
                {
                    const XrCompositionLayerBaseHeader *l = info->layers[i];
                    if (l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
                    {
                        const auto *p = reinterpret_cast<const XrCompositionLayerProjection *>(l);
                        std::fprintf(f, " [projection views %u", p->viewCount);
                        for (uint32_t k = 0; k < p->viewCount; ++k)
                        {
                            const XrCompositionLayerProjectionView &v = p->views[k];
                            std::fprintf(f, " {v%u swapchain %p p %.3f %.3f %.3f q %.3f %.3f %.3f %.3f fov L%.1f R%.1f U%.1f D%.1f %dx%d}", k,
                                         static_cast<void *>(v.subImage.swapchain), v.pose.position.x, v.pose.position.y,
                                         v.pose.position.z, v.pose.orientation.x, v.pose.orientation.y, v.pose.orientation.z,
                                         v.pose.orientation.w, v.fov.angleLeft * 57.3f, v.fov.angleRight * 57.3f, v.fov.angleUp * 57.3f,
                                         v.fov.angleDown * 57.3f, v.subImage.imageRect.extent.width, v.subImage.imageRect.extent.height);
                        }
                        std::fprintf(f, "]");
                    }
                    else if (l->type == XR_TYPE_COMPOSITION_LAYER_QUAD)
                    {
                        const auto *q = reinterpret_cast<const XrCompositionLayerQuad *>(l);
                        std::fprintf(f, " [quad at %.2f %.2f %.2f size %.2fx%.2f alpha %d %dx%d]", q->pose.position.x, q->pose.position.y,
                                     q->pose.position.z, q->size.width, q->size.height,
                                     (q->layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) ? 1 : 0,
                                     q->subImage.imageRect.extent.width, q->subImage.imageRect.extent.height);
                    }
                }
                std::fprintf(f, "\n");
                std::fclose(f);
            }
        // a name: the control file's "capture <name>", or <dir>\capture.txt - this frame captured under that name
        std::string tag;
        tag.swap(g_captureRequest);
        if (tag.empty())
            if (FILE *t = std::fopen((base + "capture.txt").c_str(), "r"))
            {
                char name[64] = {};
                std::fgets(name, sizeof(name), t);
                std::fclose(t);
                std::remove((base + "capture.txt").c_str());
                tag = name;
                while (!tag.empty() && (tag.back() == '\n' || tag.back() == '\r' || tag.back() == ' '))
                    tag.pop_back();
            }
        if (tag.empty() && (!periodic || g_frames % every != 0 || g_captures >= most))
            return;
        const int n = g_captures++;
        const std::string id = tag.empty() ? std::to_string(n) : tag;
        // the layers' images (a projection layer: one per view)
        struct Layer
        {
            const XrCompositionLayerBaseHeader *l;
            std::vector<Pixels> px; // per view (a quad: one)
        };
        auto toRgb = [](const Pixels &p) {
            std::vector<uint8_t> rgb(static_cast<size_t>(p.w) * p.h * 3);
            for (size_t k = 0; k < rgb.size() / 3; ++k)
                for (int c = 0; c < 3; ++c)
                    rgb[k * 3 + c] = p.rgba[k * 4 + c];
            return rgb;
        };
        std::vector<Layer> layers;
        for (uint32_t i = 0; i < info->layerCount; ++i)
        {
            const XrCompositionLayerBaseHeader *l = info->layers[i];
            Layer layer = {l, {}};
            std::vector<const XrSwapchainSubImage *> subs;
            if (l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
            {
                const auto *p = reinterpret_cast<const XrCompositionLayerProjection *>(l);
                for (uint32_t k = 0; k < p->viewCount; ++k)
                    subs.push_back(&p->views[k].subImage);
            }
            else if (l->type == XR_TYPE_COMPOSITION_LAYER_QUAD)
                subs.push_back(&reinterpret_cast<const XrCompositionLayerQuad *>(l)->subImage);
            for (size_t k = 0; k < subs.size(); ++k)
            {
                const auto *sc = reinterpret_cast<const Swapchain *>(subs[k]->swapchain);
                // two views of one image (mono): read once
                bool same = false;
                for (size_t j = 0; j < k && !same; ++j)
                    if (subs[j]->swapchain == subs[k]->swapchain && std::memcmp(&subs[j]->imageRect, &subs[k]->imageRect, sizeof(XrRect2Di)) == 0)
                    {
                        layer.px.push_back(layer.px[j]);
                        same = true;
                    }
                if (same)
                    continue;
                layer.px.push_back(crop(readImage(s, *sc, sc->last), subs[k]->imageRect));
                const Pixels &px = layer.px.back();
                if (px.valid())
                    writeImage(base + "layer_" + id + "_" + std::to_string(i) + (subs.size() > 1 ? "_v" + std::to_string(k) : std::string()),
                               toRgb(px), px.w, px.h);
            }
            layers.push_back(std::move(layer));
        }
        // each eye: rays through a 110 deg view, the layers composited back to front
        constexpr int kSize = 900;
        const float tanHalf = std::tan(55.0f * kPi / 180.0f);
        const XrPosef head = headAt(info->displayTime);
        for (int eye = 0; eye < 2; ++eye)
        {
            XrPosef eyePose = compose(head, {{0, 0, 0, 1}, {eye ? halfIpd() : -halfIpd(), 0.0f, 0.0f}});
            std::vector<uint8_t> rgb(static_cast<size_t>(kSize) * kSize * 3, 0);
            for (int y = 0; y < kSize; ++y)
                for (int x = 0; x < kSize; ++x)
                {
                    const V3 dirEye = {((x + 0.5f) / kSize * 2.0f - 1.0f) * tanHalf, (1.0f - (y + 0.5f) / kSize * 2.0f) * tanHalf, -1.0f};
                    float col[3] = {0.08f, 0.08f, 0.1f}; // the runtime's empty space
                    for (const Layer &L : layers)
                    {
                        float s[4] = {};
                        bool hit = false, alpha = false;
                        if (L.l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
                        {
                            // in the layer's space (LOCAL-based spaces: their offsets); rotation-only reprojection
                            const auto *p = reinterpret_cast<const XrCompositionLayerProjection *>(L.l);
                            const uint32_t vi = std::min<uint32_t>(eye, p->viewCount - 1);
                            if (vi >= L.px.size() || !L.px[vi].valid())
                                continue;
                            const Space &sp = *reinterpret_cast<const Space *>(p->space);
                            const XrPosef eyeInSpace = compose(inverse(spaceInLocal(sp, info->displayTime)), eyePose);
                            const V3 d = rot(eyeInSpace.orientation, dirEye);
                            const XrCompositionLayerProjectionView &v = p->views[vi];
                            const V3 dv = rot(qconj(v.pose.orientation), d);
                            if (dv.z < 0.0f)
                            {
                                const float tx = dv.x / -dv.z, ty = dv.y / -dv.z;
                                const float l = std::tan(v.fov.angleLeft), r = std::tan(v.fov.angleRight), u = std::tan(v.fov.angleUp),
                                            dn = std::tan(v.fov.angleDown);
                                const float uu = (tx - l) / (r - l), vv = (u - ty) / (u - dn);
                                if (uu >= 0 && uu <= 1 && vv >= 0 && vv <= 1)
                                {
                                    L.px[vi].sample(uu, vv, s);
                                    hit = true;
                                    alpha = (p->layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) != 0;
                                }
                            }
                        }
                        else
                        {
                            if (L.px.empty() || !L.px[0].valid())
                                continue;
                            const auto *q = reinterpret_cast<const XrCompositionLayerQuad *>(L.l);
                            if ((q->eyeVisibility == XR_EYE_VISIBILITY_LEFT && eye != 0) || (q->eyeVisibility == XR_EYE_VISIBILITY_RIGHT && eye != 1))
                                continue;
                            const Space &sp = *reinterpret_cast<const Space *>(q->space);
                            const XrPosef quadInLocal = compose(spaceInLocal(sp, info->displayTime), q->pose);
                            const XrPosef eyeInQuad = compose(inverse(quadInLocal), eyePose);
                            const V3 o = {eyeInQuad.position.x, eyeInQuad.position.y, eyeInQuad.position.z};
                            const V3 d = rot(eyeInQuad.orientation, dirEye);
                            if (std::fabs(d.z) > 1e-6f)
                            {
                                const float t = -o.z / d.z;
                                const V3 h = o + d * t;
                                if (t > 0 && std::fabs(h.x) <= q->size.width / 2 && std::fabs(h.y) <= q->size.height / 2)
                                {
                                    L.px[0].sample(h.x / q->size.width + 0.5f, 0.5f - h.y / q->size.height, s);
                                    hit = true;
                                    alpha = (q->layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) != 0;
                                }
                            }
                        }
                        if (!hit)
                            continue;
                        for (int c = 0; c < 3; ++c)
                            col[c] = alpha ? s[c] + col[c] * (1.0f - s[3]) : s[c]; // premultiplied
                    }
                    uint8_t *o = &rgb[(static_cast<size_t>(y) * kSize + x) * 3];
                    for (int c = 0; c < 3; ++c)
                        o[c] = static_cast<uint8_t>(std::clamp(col[c], 0.0f, 1.0f) * 255.0f + 0.5f);
                }
            writeImage(base + "eye_" + id + (eye ? "_right" : "_left"), rgb, kSize, kSize);
        }
        rtLog("captured frame %llu as eye_%s_left / _right in %s\n", static_cast<unsigned long long>(g_frames), id.c_str(), dir.c_str());
    }

    // ---- the functions ----------------------------------------------------------------------------------------------
    XrResult XRAPI_CALL rtEnumerateInstanceExtensionProperties(const char *layer, uint32_t cap, uint32_t *count, XrExtensionProperties *props)
    {
        if (layer)
            return XR_ERROR_API_LAYER_NOT_PRESENT;
        // XR_KHR_D3D11_enable, and XR_EXT_hp_mixed_reality_controller unless XRTEST_NO_HP_EXT is set
        const bool hp = !(std::getenv("XRTEST_NO_HP_EXT") && *std::getenv("XRTEST_NO_HP_EXT"));
        *count = hp ? 2 : 1;
        if (cap == 0)
            return XR_SUCCESS;
        if (cap < *count)
            return XR_ERROR_SIZE_INSUFFICIENT;
        std::snprintf(props[0].extensionName, XR_MAX_EXTENSION_NAME_SIZE, "%s", XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
        props[0].extensionVersion = XR_KHR_D3D11_enable_SPEC_VERSION;
        if (hp)
        {
            std::snprintf(props[1].extensionName, XR_MAX_EXTENSION_NAME_SIZE, "%s", XR_EXT_HP_MIXED_REALITY_CONTROLLER_EXTENSION_NAME);
            props[1].extensionVersion = XR_EXT_hp_mixed_reality_controller_SPEC_VERSION;
        }
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtCreateInstance(const XrInstanceCreateInfo *ci, XrInstance *out)
    {
        const bool hpOffered = !(std::getenv("XRTEST_NO_HP_EXT") && *std::getenv("XRTEST_NO_HP_EXT"));
        bool hp = false;
        for (uint32_t i = 0; ci && i < ci->enabledExtensionCount; ++i)
        {
            const char *e = ci->enabledExtensionNames[i];
            if (std::strcmp(e, XR_EXT_HP_MIXED_REALITY_CONTROLLER_EXTENSION_NAME) == 0 && hpOffered)
                hp = true;
            else if (std::strcmp(e, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) != 0)
                return violation(XR_ERROR_EXTENSION_NOT_PRESENT, "xrCreateInstance with an extension not offered");
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        g_hpEnabled = hp;
        if (g_instanceLost) // the runtime "restarted": a new instance
        {
            g_instanceLost = false;
            ++g_instanceId;
            g_events.clear();
        }
        *out = instanceHandle();
        rtLog("instance created\n");
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtDestroyInstance(XrInstance) { return XR_SUCCESS; }
    XrResult XRAPI_CALL rtGetInstanceProperties(XrInstance, XrInstanceProperties *p)
    {
        p->runtimeVersion = XR_MAKE_VERSION(0, 1, 0);
        std::snprintf(p->runtimeName, XR_MAX_RUNTIME_NAME_SIZE, "C12 xr_test_runtime");
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtGetSystem(XrInstance, const XrSystemGetInfo *gi, XrSystemId *id)
    {
        if (gi->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY)
            return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
        if (g_instanceLost)
            return XR_ERROR_INSTANCE_LOST;
        // XRTEST_NO_HMD=<s>: no headset for the first s seconds (0: never one) - the application's retry
        static const int64_t firstAsk = nowNs();
        if (const char *e = std::getenv("XRTEST_NO_HMD"); e && *e)
            if (std::atof(e) <= 0.0 || nowNs() - firstAsk < static_cast<int64_t>(std::atof(e) * 1e9))
                return XR_ERROR_FORM_FACTOR_UNAVAILABLE;
        *id = 1;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtGetSystemProperties(XrInstance, XrSystemId, XrSystemProperties *p)
    {
        p->systemId = 1;
        std::snprintf(p->systemName, XR_MAX_SYSTEM_NAME_SIZE, "C12 test HMD");
        p->graphicsProperties = {4096, 4096, 16};
        p->trackingProperties = {XR_TRUE, XR_TRUE};
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtEnumerateViewConfigurationViews(XrInstance, XrSystemId, XrViewConfigurationType, uint32_t cap, uint32_t *count,
                                                         XrViewConfigurationView *v)
    {
        *count = 2;
        if (cap == 0)
            return XR_SUCCESS;
        if (cap < 2)
            return XR_ERROR_SIZE_INSUFFICIENT;
        for (int i = 0; i < 2; ++i)
            v[i] = {XR_TYPE_VIEW_CONFIGURATION_VIEW, nullptr, 1440, 2880, 1600, 3200, 1, 1};
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtEnumerateEnvironmentBlendModes(XrInstance, XrSystemId, XrViewConfigurationType, uint32_t cap, uint32_t *count,
                                                        XrEnvironmentBlendMode *m)
    {
        *count = 1;
        if (cap >= 1)
            m[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtGetD3D11GraphicsRequirementsKHR(XrInstance, XrSystemId, XrGraphicsRequirementsD3D11KHR *r)
    {
        IDXGIFactory1 *f = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&f))))
        {
            IDXGIAdapter1 *a = nullptr;
            if (SUCCEEDED(f->EnumAdapters1(0, &a)))
            {
                DXGI_ADAPTER_DESC1 d = {};
                a->GetDesc1(&d);
                r->adapterLuid = d.AdapterLuid;
                a->Release();
            }
            f->Release();
        }
        r->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtCreateSession(XrInstance inst, const XrSessionCreateInfo *ci, XrSession *out)
    {
        const auto *b = reinterpret_cast<const XrGraphicsBindingD3D11KHR *>(ci->next);
        if (!b || b->type != XR_TYPE_GRAPHICS_BINDING_D3D11_KHR || !b->device)
            return violation(XR_ERROR_GRAPHICS_DEVICE_INVALID, "xrCreateSession without a D3D11 binding");
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_instanceLost || inst != instanceHandle())
            return XR_ERROR_INSTANCE_LOST;
        if (g_session)
            return violation(XR_ERROR_LIMIT_REACHED, "xrCreateSession while another session exists");
        g_session = new Session();
        g_session->device = b->device;
        b->device->GetImmediateContext(&g_session->context);
        g_start = nowNs();
        resetHandRef(g_start); // the controllers: relative to the head at the start
        if (const char *hz = std::getenv("XRTEST_HZ"); hz && std::atof(hz) >= 30.0)
            g_periodNs = static_cast<int64_t>(1e9 / std::atof(hz));
        pushState(XR_SESSION_STATE_IDLE);
        pushState(XR_SESSION_STATE_READY);
        *out = reinterpret_cast<XrSession>(g_session);
        rtLog("session created on the application's D3D11 device\n");
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtDestroySession(XrSession s)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto *ss = reinterpret_cast<Session *>(s);
        if (ss->context)
            ss->context->Release();
        delete ss;
        if (g_session == ss)
            g_session = nullptr;
        rtLog("session destroyed\n");
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtBeginSession(XrSession s, const XrSessionBeginInfo *)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto *ss = reinterpret_cast<Session *>(s);
        if (!ss)
            return violation(XR_ERROR_HANDLE_INVALID, "a null session handle");
        if (ss->lost)
            return XR_ERROR_SESSION_LOST;
        if (ss->running)
            return violation(XR_ERROR_SESSION_RUNNING, "xrBeginSession on a running session");
        if (ss->state != XR_SESSION_STATE_READY)
            return violation(XR_ERROR_SESSION_NOT_READY, "xrBeginSession outside READY");
        ss->running = true;
        ss->waited = ss->begun = false;
        pushState(XR_SESSION_STATE_SYNCHRONIZED);
        pushState(XR_SESSION_STATE_VISIBLE);
        pushState(XR_SESSION_STATE_FOCUSED);
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtEndSession(XrSession s)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto *ss = reinterpret_cast<Session *>(s);
        if (!ss)
            return violation(XR_ERROR_HANDLE_INVALID, "a null session handle");
        if (ss->lost)
            return XR_ERROR_SESSION_LOST;
        if (!ss->running)
            return violation(XR_ERROR_SESSION_NOT_RUNNING, "xrEndSession on a session not running");
        if (ss->state != XR_SESSION_STATE_STOPPING)
            return violation(XR_ERROR_SESSION_NOT_STOPPING, "xrEndSession outside STOPPING");
        ss->running = false;
        ss->waited = ss->begun = false;
        pushState(XR_SESSION_STATE_IDLE);
        if (ss->exitAfterEnd)
            pushState(XR_SESSION_STATE_EXITING);
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtPollEvent(XrInstance inst, XrEventDataBuffer *e)
    {
        controlPoll();
        std::lock_guard<std::mutex> lock(g_mutex);
        if (inst != instanceHandle())
            return XR_ERROR_HANDLE_INVALID;
        if (g_events.empty())
            return XR_EVENT_UNAVAILABLE;
        std::memcpy(e, &g_events.front(), sizeof(XrEventDataBuffer));
        g_events.pop_front();
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtCreateReferenceSpace(XrSession s, const XrReferenceSpaceCreateInfo *ci, XrSpace *out)
    {
        if (reinterpret_cast<Session *>(s)->lost)
            return XR_ERROR_SESSION_LOST;
        auto *sp = new Space();
        sp->type = ci->referenceSpaceType;
        sp->offset = ci->poseInReferenceSpace;
        *out = reinterpret_cast<XrSpace>(sp);
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtDestroySpace(XrSpace s)
    {
        delete reinterpret_cast<Space *>(s);
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtLocateSpace(XrSpace space, XrSpace base, XrTime t, XrSpaceLocation *loc)
    {
        if (t <= 0)
            return violation(XR_ERROR_TIME_INVALID, "xrLocateSpace at time 0");
        if (!space || !base)
            return violation(XR_ERROR_HANDLE_INVALID, "xrLocateSpace with a null space");
        if (loc->type != XR_TYPE_SPACE_LOCATION)
            return violation(XR_ERROR_VALIDATION_FAILURE, "xrLocateSpace's location is not XR_TYPE_SPACE_LOCATION");
        if (!spaceTracked(*reinterpret_cast<Space *>(space)) || !spaceTracked(*reinterpret_cast<Space *>(base)))
        {
            loc->pose = identity();
            loc->locationFlags = 0; // a controller not tracked (its action inactive)
            return XR_SUCCESS;
        }
        const XrPosef a = spaceInLocal(*reinterpret_cast<Space *>(space), t), b = spaceInLocal(*reinterpret_cast<Space *>(base), t);
        loc->pose = compose(inverse(b), a);
        loc->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
                             XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
        return XR_SUCCESS;
    }
    // XRTEST_FORMATS=a,b,...: the formats offered (DXGI numbers or rgba_srgb / bgra_srgb / rgba / bgra / rgb10a2 / rgba16f)
    std::vector<int64_t> offeredFormats()
    {
        std::vector<int64_t> out;
        const char *e = std::getenv("XRTEST_FORMATS");
        std::string s = e && *e ? e : "rgba_srgb,rgba";
        size_t pos = 0;
        while (pos <= s.size())
        {
            const size_t comma = s.find(',', pos);
            const std::string n = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            pos = comma == std::string::npos ? s.size() + 1 : comma + 1;
            if (n == "rgba_srgb") out.push_back(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
            else if (n == "bgra_srgb") out.push_back(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
            else if (n == "rgba") out.push_back(DXGI_FORMAT_R8G8B8A8_UNORM);
            else if (n == "bgra") out.push_back(DXGI_FORMAT_B8G8R8A8_UNORM);
            else if (n == "rgb10a2") out.push_back(DXGI_FORMAT_R10G10B10A2_UNORM);
            else if (n == "rgba16f") out.push_back(DXGI_FORMAT_R16G16B16A16_FLOAT);
            else if (!n.empty()) out.push_back(std::atoll(n.c_str()));
        }
        return out;
    }
    XrResult XRAPI_CALL rtLocateViews(XrSession s, const XrViewLocateInfo *li, XrViewState *vs, uint32_t cap, uint32_t *count, XrView *views)
    {
        if (!s)
            return violation(XR_ERROR_HANDLE_INVALID, "xrLocateViews with a null session");
        *count = 2;
        if (cap == 0)
            return XR_SUCCESS;
        if (cap < 2)
            return XR_ERROR_SIZE_INSUFFICIENT;
        if (li->displayTime <= 0)
            return violation(XR_ERROR_TIME_INVALID, "xrLocateViews at time 0");
        const XrPosef head = compose(inverse(spaceInLocal(*reinterpret_cast<Space *>(li->space), li->displayTime)), headAt(li->displayTime));
        for (int i = 0; i < 2; ++i)
        {
            views[i].pose = compose(head, {{0, 0, 0, 1}, {i ? halfIpd() : -halfIpd(), 0.0f, 0.0f}});
            const float r = 50.0f * kPi / 180.0f;
            views[i].fov = {i ? -r * 0.9f : -r, i ? r : r * 0.9f, r, -r};
        }
        vs->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT |
                             XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtEnumerateSwapchainFormats(XrSession, uint32_t cap, uint32_t *count, int64_t *formats)
    {
        const std::vector<int64_t> f = offeredFormats();
        *count = static_cast<uint32_t>(f.size());
        if (cap == 0)
            return XR_SUCCESS;
        if (cap < f.size())
            return XR_ERROR_SIZE_INSUFFICIENT;
        std::copy(f.begin(), f.end(), formats);
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtCreateSwapchain(XrSession s, const XrSwapchainCreateInfo *ci, XrSwapchain *out)
    {
        auto *ss = reinterpret_cast<Session *>(s);
        if (!ss)
            return violation(XR_ERROR_HANDLE_INVALID, "a null session handle");
        if (ss->lost)
            return XR_ERROR_SESSION_LOST;
        const std::vector<int64_t> f = offeredFormats();
        if (std::find(f.begin(), f.end(), ci->format) == f.end())
            return violation(XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED, "xrCreateSwapchain with a format not offered");
        if (!ci->width || !ci->height || ci->width > 4096 || ci->height > 4096)
            return violation(XR_ERROR_VALIDATION_FAILURE, "xrCreateSwapchain size 0 or over the maximum");
        auto *sc = new Swapchain();
        sc->width = ci->width;
        sc->height = ci->height;
        sc->format = ci->format;
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = ci->width;
        td.Height = ci->height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        // typeless, as SteamVR / Oculus make them: the application names the typed format in its views
        const bool bgra = ci->format == DXGI_FORMAT_B8G8R8A8_UNORM || ci->format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        const bool rgba = ci->format == DXGI_FORMAT_R8G8B8A8_UNORM || ci->format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        td.Format = std::getenv("XRTEST_TYPELESS") && (bgra || rgba)
                        ? (bgra ? DXGI_FORMAT_B8G8R8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS)
                        : static_cast<DXGI_FORMAT>(ci->format);
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        for (int i = 0; i < 3; ++i)
        {
            ID3D11Texture2D *t = nullptr;
            if (FAILED(ss->device->CreateTexture2D(&td, nullptr, &t)))
            {
                for (ID3D11Texture2D *x : sc->images)
                    x->Release();
                delete sc;
                return XR_ERROR_RUNTIME_FAILURE;
            }
            sc->images.push_back(t);
        }
        *out = reinterpret_cast<XrSwapchain>(sc);
        rtLog("swapchain %ux%u format %lld\n", ci->width, ci->height, static_cast<long long>(ci->format));
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtDestroySwapchain(XrSwapchain s)
    {
        auto *sc = reinterpret_cast<Swapchain *>(s);
        for (ID3D11Texture2D *t : sc->images)
            t->Release();
        delete sc;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtEnumerateSwapchainImages(XrSwapchain s, uint32_t cap, uint32_t *count, XrSwapchainImageBaseHeader *images)
    {
        auto *sc = reinterpret_cast<Swapchain *>(s);
        *count = static_cast<uint32_t>(sc->images.size());
        if (cap == 0)
            return XR_SUCCESS;
        if (cap < *count)
            return XR_ERROR_SIZE_INSUFFICIENT;
        auto *out = reinterpret_cast<XrSwapchainImageD3D11KHR *>(images);
        for (uint32_t i = 0; i < *count; ++i)
            out[i].texture = sc->images[i];
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtAcquireSwapchainImage(XrSwapchain s, const XrSwapchainImageAcquireInfo *, uint32_t *index)
    {
        auto *sc = reinterpret_cast<Swapchain *>(s);
        if (sc->acquired != ~0u)
            return violation(XR_ERROR_CALL_ORDER_INVALID, "xrAcquireSwapchainImage twice without a release");
        sc->acquired = sc->next;
        sc->next = (sc->next + 1) % sc->images.size();
        *index = sc->acquired;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtWaitSwapchainImage(XrSwapchain, const XrSwapchainImageWaitInfo *) { return XR_SUCCESS; }
    XrResult XRAPI_CALL rtReleaseSwapchainImage(XrSwapchain s, const XrSwapchainImageReleaseInfo *)
    {
        auto *sc = reinterpret_cast<Swapchain *>(s);
        if (sc->acquired == ~0u)
            return violation(XR_ERROR_CALL_ORDER_INVALID, "xrReleaseSwapchainImage without an acquire");
        sc->last = sc->acquired;
        sc->acquired = ~0u;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtWaitFrame(XrSession s, const XrFrameWaitInfo *, XrFrameState *st)
    {
        controlPoll();
        auto *ss = reinterpret_cast<Session *>(s);
        if (!ss)
            return violation(XR_ERROR_HANDLE_INVALID, "a null session handle");
        if (ss->lost)
            return XR_ERROR_SESSION_LOST;
        if (!ss->running)
            return violation(XR_ERROR_SESSION_NOT_RUNNING, "xrWaitFrame on a session not running");
        // the display's refresh: block until the next vsync after the last frame's
        const int64_t now = nowNs();
        if (!g_nextDisplay || g_nextDisplay + g_periodNs < now)
            g_nextDisplay = now; // late (or the first frame): from now on
        else if (g_nextDisplay > now)
            std::this_thread::sleep_for(std::chrono::nanoseconds(g_nextDisplay - now));
        g_nextDisplay += g_periodNs;
        st->predictedDisplayTime = g_nextDisplay;
        st->predictedDisplayPeriod = g_periodNs;
        // as the spec: render only when the session is VISIBLE or FOCUSED
        st->shouldRender = ss->state == XR_SESSION_STATE_VISIBLE || ss->state == XR_SESSION_STATE_FOCUSED ? XR_TRUE : XR_FALSE;
        ss->waited = true;
        ss->predicted = g_nextDisplay;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtBeginFrame(XrSession s, const XrFrameBeginInfo *)
    {
        auto *ss = reinterpret_cast<Session *>(s);
        if (!ss)
            return violation(XR_ERROR_HANDLE_INVALID, "a null session handle");
        if (ss->lost)
            return XR_ERROR_SESSION_LOST;
        if (!ss->running)
            return violation(XR_ERROR_SESSION_NOT_RUNNING, "xrBeginFrame on a session not running");
        if (!ss->waited)
            return violation(XR_ERROR_CALL_ORDER_INVALID, "xrBeginFrame without xrWaitFrame");
        ss->waited = false;
        const bool discarded = ss->begun; // the last begun frame was never ended
        ss->begun = true;
        return discarded ? XR_FRAME_DISCARDED : XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtEndFrame(XrSession s, const XrFrameEndInfo *info)
    {
        auto *ss = reinterpret_cast<Session *>(s);
        if (!ss)
            return violation(XR_ERROR_HANDLE_INVALID, "a null session handle");
        if (ss->lost)
            return XR_ERROR_SESSION_LOST;
        if (!ss->running)
            return violation(XR_ERROR_SESSION_NOT_RUNNING, "xrEndFrame on a session not running");
        if (!ss->begun)
            return violation(XR_ERROR_CALL_ORDER_INVALID, "xrEndFrame without xrBeginFrame");
        ss->begun = false;
        if (info->displayTime != ss->predicted)
            return violation(XR_ERROR_TIME_INVALID, "xrEndFrame's displayTime is not the predicted one");
        if (info->layerCount > 16)
            return violation(XR_ERROR_LAYER_LIMIT_EXCEEDED, "more layers than maxLayerCount");
        for (uint32_t i = 0; i < info->layerCount; ++i)
        {
            const XrCompositionLayerBaseHeader *l = info->layers[i];
            if (!l)
                return violation(XR_ERROR_LAYER_INVALID, "a null layer");
            auto checkSub = [&](const XrSwapchainSubImage &sub) {
                const auto *sc = reinterpret_cast<const Swapchain *>(sub.swapchain);
                if (!sc || sc->acquired != ~0u || sc->last == ~0u)
                    return false; // still acquired, or never released: no image to show
                return sub.imageRect.extent.width > 0 && sub.imageRect.extent.height > 0 &&
                       static_cast<uint32_t>(sub.imageRect.offset.x + sub.imageRect.extent.width) <= sc->width &&
                       static_cast<uint32_t>(sub.imageRect.offset.y + sub.imageRect.extent.height) <= sc->height;
            };
            if (l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
            {
                const auto *p = reinterpret_cast<const XrCompositionLayerProjection *>(l);
                if (p->viewCount != 2)
                    return violation(XR_ERROR_VALIDATION_FAILURE, "a projection layer without 2 views");
                for (uint32_t v = 0; v < p->viewCount; ++v)
                {
                    const XrFovf &fov = p->views[v].fov;
                    if (!checkSub(p->views[v].subImage))
                        return violation(XR_ERROR_LAYER_INVALID, "a projection view's swapchain image not released / rect outside");
                    if (!(fov.angleLeft < fov.angleRight && fov.angleDown < fov.angleUp))
                        return violation(XR_ERROR_VALIDATION_FAILURE, "a projection view's field of view is empty");
                    const XrQuaternionf &q = p->views[v].pose.orientation;
                    if (std::fabs(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w - 1.0f) > 0.01f)
                        return violation(XR_ERROR_POSE_INVALID, "a projection view's orientation is not normalised");
                }
            }
            else if (l->type == XR_TYPE_COMPOSITION_LAYER_QUAD)
            {
                const auto *q = reinterpret_cast<const XrCompositionLayerQuad *>(l);
                if (!checkSub(q->subImage))
                    return violation(XR_ERROR_LAYER_INVALID, "a quad's swapchain image not released / rect outside");
                if (q->size.width <= 0.0f || q->size.height <= 0.0f)
                    return violation(XR_ERROR_VALIDATION_FAILURE, "a quad of size 0");
            }
            else
                return violation(XR_ERROR_LAYER_INVALID, "an unknown layer type");
        }
        ++g_frames;
        capture(*ss, info);
        if (g_frames == 1 || g_frames % 900 == 0)
            rtLog("frame %llu: %u layers (%llu violations so far)\n", static_cast<unsigned long long>(g_frames),
                         info->layerCount, static_cast<unsigned long long>(g_violations));
        return XR_SUCCESS;
    }
    // ---- paths and actions ------------------------------------------------------------------------------------------
    // a well-formed path: "/" + segments of [a-z0-9-_.] (not "." / ".."), no empty segment, no trailing "/"
    bool validPath(const char *s)
    {
        const size_t n = strnlen(s, XR_MAX_PATH_LENGTH);
        if (n == 0 || n >= XR_MAX_PATH_LENGTH || s[0] != '/' || s[n - 1] == '/')
            return false;
        std::string seg;
        for (size_t i = 1; i <= n; ++i)
        {
            const char c = i < n ? s[i] : '/';
            if (c == '/')
            {
                if (seg.empty() || seg == "." || seg == "..")
                    return false;
                seg.clear();
                continue;
            }
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
                return false;
            seg += c;
        }
        return true;
    }
    // an action (set) name: [a-z0-9-_.]+
    bool validName(const char *s, size_t cap)
    {
        const size_t n = strnlen(s, cap);
        if (n == 0 || n >= cap)
            return false;
        for (size_t i = 0; i < n; ++i)
            if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-' || s[i] == '_' || s[i] == '.'))
                return false;
        return true;
    }
    XrPath pathOf(const std::string &s)
    {
        const auto it = std::find(g_paths.begin(), g_paths.end(), s);
        if (it != g_paths.end())
            return static_cast<XrPath>(it - g_paths.begin() + 1);
        g_paths.push_back(s);
        return static_cast<XrPath>(g_paths.size());
    }
    const std::string *pathText(XrPath p) { return p >= 1 && p <= g_paths.size() ? &g_paths[static_cast<size_t>(p - 1)] : nullptr; }
    bool topLevelUserPath(const std::string &s)
    {
        return s == "/user/hand/left" || s == "/user/hand/right" || s == "/user/head" || s == "/user/gamepad" || s == "/user/treadmill";
    }
    ActionSet *liveSet(XrActionSet h)
    {
        auto *s = reinterpret_cast<ActionSet *>(h);
        return std::find(g_actionSets.begin(), g_actionSets.end(), s) != g_actionSets.end() ? s : nullptr;
    }
    Action *liveAction(XrAction h)
    {
        auto *a = reinterpret_cast<Action *>(h);
        return std::find(g_actions.begin(), g_actions.end(), a) != g_actions.end() ? a : nullptr;
    }

    const char *const kProfiles[] = {
        "/interaction_profiles/khr/simple_controller",    "/interaction_profiles/oculus/touch_controller",
        "/interaction_profiles/valve/index_controller",   "/interaction_profiles/htc/vive_controller",
        "/interaction_profiles/microsoft/motion_controller", "/interaction_profiles/hp/mixed_reality_controller",
    };
    enum
    {
        PSimple,
        PTouch,
        PIndex,
        PVive,
        PWmr,
        PHp,
        PCount
    };
    int profileIndex(const std::string &s)
    {
        for (int i = 0; i < PCount; ++i)
            if (s == kProfiles[i])
                return i;
        return -1;
    }
    // what <component> (under /user/hand/<hand>/) is on that profile; KNone: not there. Vector2 sources only as the
    // parent (".../thumbstick"), float / bool ones as full paths (".../trigger/value").
    Kind componentKind(int profile, int hand, const std::string &c)
    {
        if (c == "input/grip/pose" || c == "input/aim/pose")
            return KPose;
        if (c == "output/haptic")
            return KHaptic;
        struct E
        {
            const char *c;
            char hand; // L, R, B
            Kind k;
        };
        static const E simple[] = {{"input/select/click", 'B', KBool}, {"input/menu/click", 'B', KBool}};
        static const E touch[] = {
            {"input/x/click", 'L', KBool},        {"input/x/touch", 'L', KBool},       {"input/y/click", 'L', KBool},
            {"input/y/touch", 'L', KBool},        {"input/menu/click", 'L', KBool},    {"input/a/click", 'R', KBool},
            {"input/a/touch", 'R', KBool},        {"input/b/click", 'R', KBool},       {"input/b/touch", 'R', KBool},
            {"input/system/click", 'R', KBool},   {"input/squeeze/value", 'B', KFloat}, {"input/trigger/value", 'B', KFloat},
            {"input/trigger/touch", 'B', KBool},  {"input/thumbstick", 'B', KVec},     {"input/thumbstick/x", 'B', KFloat},
            {"input/thumbstick/y", 'B', KFloat},  {"input/thumbstick/click", 'B', KBool}, {"input/thumbstick/touch", 'B', KBool},
            {"input/thumbrest/touch", 'B', KBool},
        };
        static const E index[] = {
            {"input/system/click", 'B', KBool},  {"input/system/touch", 'B', KBool},  {"input/a/click", 'B', KBool},
            {"input/a/touch", 'B', KBool},       {"input/b/click", 'B', KBool},       {"input/b/touch", 'B', KBool},
            {"input/squeeze/value", 'B', KFloat}, {"input/squeeze/force", 'B', KFloat}, {"input/trigger/click", 'B', KBool},
            {"input/trigger/value", 'B', KFloat}, {"input/trigger/touch", 'B', KBool}, {"input/thumbstick", 'B', KVec},
            {"input/thumbstick/x", 'B', KFloat}, {"input/thumbstick/y", 'B', KFloat}, {"input/thumbstick/click", 'B', KBool},
            {"input/thumbstick/touch", 'B', KBool}, {"input/trackpad", 'B', KVec},    {"input/trackpad/x", 'B', KFloat},
            {"input/trackpad/y", 'B', KFloat},   {"input/trackpad/force", 'B', KFloat}, {"input/trackpad/touch", 'B', KBool},
        };
        static const E vive[] = {
            {"input/system/click", 'B', KBool},  {"input/squeeze/click", 'B', KBool}, {"input/menu/click", 'B', KBool},
            {"input/trigger/click", 'B', KBool}, {"input/trigger/value", 'B', KFloat}, {"input/trackpad", 'B', KVec},
            {"input/trackpad/x", 'B', KFloat},   {"input/trackpad/y", 'B', KFloat},   {"input/trackpad/click", 'B', KBool},
            {"input/trackpad/touch", 'B', KBool},
        };
        static const E wmr[] = {
            {"input/menu/click", 'B', KBool},     {"input/squeeze/click", 'B', KBool},  {"input/trigger/value", 'B', KFloat},
            {"input/thumbstick", 'B', KVec},      {"input/thumbstick/x", 'B', KFloat},  {"input/thumbstick/y", 'B', KFloat},
            {"input/thumbstick/click", 'B', KBool}, {"input/trackpad", 'B', KVec},      {"input/trackpad/x", 'B', KFloat},
            {"input/trackpad/y", 'B', KFloat},    {"input/trackpad/click", 'B', KBool}, {"input/trackpad/touch", 'B', KBool},
        };
        static const E hp[] = {
            {"input/x/click", 'L', KBool},        {"input/y/click", 'L', KBool},        {"input/a/click", 'R', KBool},
            {"input/b/click", 'R', KBool},        {"input/menu/click", 'B', KBool},     {"input/squeeze/value", 'B', KFloat},
            {"input/trigger/value", 'B', KFloat}, {"input/thumbstick", 'B', KVec},      {"input/thumbstick/x", 'B', KFloat},
            {"input/thumbstick/y", 'B', KFloat},  {"input/thumbstick/click", 'B', KBool},
        };
        const E *t = nullptr;
        size_t n = 0;
        switch (profile)
        {
        case PSimple: t = simple, n = std::size(simple); break;
        case PTouch: t = touch, n = std::size(touch); break;
        case PIndex: t = index, n = std::size(index); break;
        case PVive: t = vive, n = std::size(vive); break;
        case PWmr: t = wmr, n = std::size(wmr); break;
        case PHp: t = hp, n = std::size(hp); break;
        default: return KNone;
        }
        for (size_t i = 0; i < n; ++i)
            if (c == t[i].c && (t[i].hand == 'B' || (t[i].hand == 'L') == (hand == 0)))
                return t[i].k;
        return KNone;
    }
    bool compatible(XrActionType type, Kind k)
    {
        switch (type)
        {
        case XR_ACTION_TYPE_BOOLEAN_INPUT:
        case XR_ACTION_TYPE_FLOAT_INPUT: return k == KBool || k == KFloat;
        case XR_ACTION_TYPE_VECTOR2F_INPUT: return k == KVec;
        case XR_ACTION_TYPE_POSE_INPUT: return k == KPose;
        case XR_ACTION_TYPE_VIBRATION_OUTPUT: return k == KHaptic;
        default: return false;
        }
    }
    // the simulated Touch controller's value for a component (v[1]: a vector's y)
    void touchValue(int hand, const std::string &c, float v[2])
    {
        const Controller &k = g_ctl[hand];
        v[0] = v[1] = 0.0f;
        const bool l = hand == 0;
        if (c == "input/trigger/value") v[0] = k.trigger;
        else if (c == "input/trigger/touch") v[0] = k.trigger > 0.0f ? 1.0f : 0.0f;
        else if (c == "input/squeeze/value") v[0] = k.squeeze;
        else if (c == "input/thumbstick") v[0] = k.stick[0], v[1] = k.stick[1];
        else if (c == "input/thumbstick/x") v[0] = k.stick[0];
        else if (c == "input/thumbstick/y") v[0] = k.stick[1];
        else if (c == "input/thumbstick/click") v[0] = k.stickClick;
        else if (c == "input/thumbstick/touch") v[0] = k.stickClick > 0 || k.stick[0] != 0 || k.stick[1] != 0 ? 1.0f : 0.0f;
        else if (c == (l ? "input/x/click" : "input/a/click")) v[0] = k.primary;
        else if (c == (l ? "input/x/touch" : "input/a/touch")) v[0] = k.primary > 0 ? 1.0f : 0.0f;
        else if (c == (l ? "input/y/click" : "input/b/click")) v[0] = k.secondary;
        else if (c == (l ? "input/y/touch" : "input/b/touch")) v[0] = k.secondary > 0 ? 1.0f : 0.0f;
        else if (c == "input/menu/click") v[0] = k.menu;
    }
    // the action's subaction slot for `path`: 2 for XR_NULL_PATH, 0 / 1 for a hand, -1 for another of its paths,
    // -2 when the path is not one of its subaction paths
    int subactionSlot(const Action &a, XrPath path)
    {
        if (path == XR_NULL_PATH)
            return 2;
        if (std::find(a.subactions.begin(), a.subactions.end(), path) == a.subactions.end())
            return -2;
        const std::string *t = pathText(path);
        return *t == "/user/hand/left" ? 0 : *t == "/user/hand/right" ? 1 : -1;
    }

    XrResult XRAPI_CALL rtStringToPath(XrInstance inst, const char *s, XrPath *out)
    {
        if (inst != instanceHandle())
            return violation(XR_ERROR_HANDLE_INVALID, "xrStringToPath with an unknown instance");
        if (!s || !out)
            return violation(XR_ERROR_VALIDATION_FAILURE, "xrStringToPath with a null pointer");
        if (!validPath(s))
        {
            rtLog("xrStringToPath(\"%s\"): malformed\n", s);
            return violation(XR_ERROR_PATH_FORMAT_INVALID, "xrStringToPath with a malformed path");
        }
        *out = pathOf(s);
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtPathToString(XrInstance inst, XrPath p, uint32_t cap, uint32_t *count, char *buffer)
    {
        if (inst != instanceHandle())
            return violation(XR_ERROR_HANDLE_INVALID, "xrPathToString with an unknown instance");
        const std::string *t = pathText(p);
        if (!t)
            return violation(XR_ERROR_PATH_INVALID, "xrPathToString with an unknown path");
        *count = static_cast<uint32_t>(t->size() + 1);
        if (cap == 0)
            return XR_SUCCESS;
        if (cap < *count)
            return XR_ERROR_SIZE_INSUFFICIENT;
        std::memcpy(buffer, t->c_str(), t->size() + 1);
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtCreateActionSet(XrInstance inst, const XrActionSetCreateInfo *ci, XrActionSet *out)
    {
        if (inst != instanceHandle())
            return g_instanceLost ? XR_ERROR_INSTANCE_LOST : violation(XR_ERROR_HANDLE_INVALID, "xrCreateActionSet with an unknown instance");
        if (!ci || ci->type != XR_TYPE_ACTION_SET_CREATE_INFO)
            return violation(XR_ERROR_VALIDATION_FAILURE, "xrCreateActionSet without XR_TYPE_ACTION_SET_CREATE_INFO");
        if (!validName(ci->actionSetName, XR_MAX_ACTION_SET_NAME_SIZE))
            return violation(XR_ERROR_PATH_FORMAT_INVALID, "xrCreateActionSet: a malformed action set name");
        if (!strnlen(ci->localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE))
            return violation(XR_ERROR_LOCALIZED_NAME_INVALID, "xrCreateActionSet: an empty localized name");
        for (const ActionSet *s : g_actionSets)
        {
            if (s->instance != g_instanceId)
                continue;
            if (s->name == ci->actionSetName)
                return violation(XR_ERROR_NAME_DUPLICATED, "xrCreateActionSet: the name is taken");
            if (s->localized == ci->localizedActionSetName)
                return violation(XR_ERROR_LOCALIZED_NAME_DUPLICATED, "xrCreateActionSet: the localized name is taken");
        }
        auto *s = new ActionSet();
        s->instance = g_instanceId;
        s->name = ci->actionSetName;
        s->localized = ci->localizedActionSetName;
        s->priority = ci->priority;
        g_actionSets.push_back(s);
        *out = reinterpret_cast<XrActionSet>(s);
        rtLog("action set \"%s\" created\n", s->name.c_str());
        return XR_SUCCESS;
    }
    void deleteAction(Action *a)
    {
        g_actions.erase(std::remove(g_actions.begin(), g_actions.end(), a), g_actions.end());
        delete a;
    }
    XrResult XRAPI_CALL rtDestroyActionSet(XrActionSet h)
    {
        ActionSet *s = liveSet(h);
        if (!s)
            return violation(XR_ERROR_HANDLE_INVALID, "xrDestroyActionSet with an unknown handle");
        for (Action *a : s->actions)
            deleteAction(a);
        g_actionSets.erase(std::remove(g_actionSets.begin(), g_actionSets.end(), s), g_actionSets.end());
        rtLog("action set \"%s\" destroyed\n", s->name.c_str());
        delete s;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtCreateAction(XrActionSet h, const XrActionCreateInfo *ci, XrAction *out)
    {
        ActionSet *s = liveSet(h);
        if (!s)
            return violation(XR_ERROR_HANDLE_INVALID, "xrCreateAction with an unknown action set");
        if (!ci || ci->type != XR_TYPE_ACTION_CREATE_INFO)
            return violation(XR_ERROR_VALIDATION_FAILURE, "xrCreateAction without XR_TYPE_ACTION_CREATE_INFO");
        if (s->attached)
            return violation(XR_ERROR_ACTIONSETS_ALREADY_ATTACHED, "xrCreateAction in an action set already attached");
        if (!validName(ci->actionName, XR_MAX_ACTION_NAME_SIZE))
            return violation(XR_ERROR_PATH_FORMAT_INVALID, "xrCreateAction: a malformed action name");
        if (!strnlen(ci->localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE))
            return violation(XR_ERROR_LOCALIZED_NAME_INVALID, "xrCreateAction: an empty localized name");
        for (const Action *a : s->actions)
        {
            if (a->name == ci->actionName)
                return violation(XR_ERROR_NAME_DUPLICATED, "xrCreateAction: the name is taken in its set");
            if (a->localized == ci->localizedActionName)
                return violation(XR_ERROR_LOCALIZED_NAME_DUPLICATED, "xrCreateAction: the localized name is taken in its set");
        }
        switch (ci->actionType)
        {
        case XR_ACTION_TYPE_BOOLEAN_INPUT:
        case XR_ACTION_TYPE_FLOAT_INPUT:
        case XR_ACTION_TYPE_VECTOR2F_INPUT:
        case XR_ACTION_TYPE_POSE_INPUT:
        case XR_ACTION_TYPE_VIBRATION_OUTPUT: break;
        default: return violation(XR_ERROR_VALIDATION_FAILURE, "xrCreateAction: an unknown action type");
        }
        std::vector<XrPath> subs;
        for (uint32_t i = 0; i < ci->countSubactionPaths; ++i)
        {
            const std::string *t = pathText(ci->subactionPaths[i]);
            if (!t)
                return violation(XR_ERROR_PATH_INVALID, "xrCreateAction: an unknown subaction path");
            if (!topLevelUserPath(*t) || std::find(subs.begin(), subs.end(), ci->subactionPaths[i]) != subs.end())
                return violation(XR_ERROR_PATH_UNSUPPORTED, "xrCreateAction: a subaction path not a top level user path, or twice");
            subs.push_back(ci->subactionPaths[i]);
        }
        auto *a = new Action();
        a->set = s;
        a->name = ci->actionName;
        a->localized = ci->localizedActionName;
        a->type = ci->actionType;
        a->subactions = subs;
        s->actions.push_back(a);
        g_actions.push_back(a);
        *out = reinterpret_cast<XrAction>(a);
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtDestroyAction(XrAction h)
    {
        Action *a = liveAction(h);
        if (!a)
            return violation(XR_ERROR_HANDLE_INVALID, "xrDestroyAction with an unknown handle");
        auto &v = a->set->actions;
        v.erase(std::remove(v.begin(), v.end(), a), v.end());
        deleteAction(a);
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtSuggestInteractionProfileBindings(XrInstance inst, const XrInteractionProfileSuggestedBinding *sb)
    {
        if (inst != instanceHandle())
            return violation(XR_ERROR_HANDLE_INVALID, "xrSuggestInteractionProfileBindings with an unknown instance");
        if (!sb || sb->type != XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING)
            return violation(XR_ERROR_VALIDATION_FAILURE, "xrSuggestInteractionProfileBindings without its structure type");
        const std::string *prof = pathText(sb->interactionProfile);
        if (!prof)
            return violation(XR_ERROR_PATH_INVALID, "xrSuggestInteractionProfileBindings: an unknown profile path");
        const int pi = profileIndex(*prof);
        if (pi < 0)
        {
            rtLog("bindings for %s: not a profile this runtime knows (XR_ERROR_PATH_UNSUPPORTED)\n", prof->c_str());
            return XR_ERROR_PATH_UNSUPPORTED;
        }
        if (pi == PHp && !g_hpEnabled)
            return violation(XR_ERROR_PATH_UNSUPPORTED, "the hp/mixed_reality_controller profile without XR_EXT_hp_mixed_reality_controller");
        if (!sb->countSuggestedBindings || !sb->suggestedBindings)
            return violation(XR_ERROR_VALIDATION_FAILURE, "xrSuggestInteractionProfileBindings without bindings");
        std::vector<std::pair<Action *, Binding>> add;
        for (uint32_t i = 0; i < sb->countSuggestedBindings; ++i)
        {
            const XrActionSuggestedBinding &b = sb->suggestedBindings[i];
            Action *a = liveAction(b.action);
            if (!a)
                return violation(XR_ERROR_HANDLE_INVALID, "a suggested binding's action is unknown");
            if (a->set->attached)
                return violation(XR_ERROR_ACTIONSETS_ALREADY_ATTACHED, "bindings suggested for an action set already attached");
            const std::string *bp = pathText(b.binding);
            if (!bp)
                return violation(XR_ERROR_PATH_INVALID, "a suggested binding's path is unknown");
            Binding bd;
            bd.profile = sb->interactionProfile;
            static const std::string left = "/user/hand/left/", right = "/user/hand/right/";
            if (bp->rfind(left, 0) == 0)
                bd.hand = 0, bd.component = bp->substr(left.size());
            else if (bp->rfind(right, 0) == 0)
                bd.hand = 1, bd.component = bp->substr(right.size());
            bd.kind = bd.component.empty() ? KNone : componentKind(pi, bd.hand, bd.component);
            if (bd.kind == KNone)
            {
                rtLog("binding %s is not on %s\n", bp->c_str(), prof->c_str());
                return violation(XR_ERROR_PATH_UNSUPPORTED, "a suggested binding path the profile does not have");
            }
            if (!compatible(a->type, bd.kind))
            {
                rtLog("binding %s does not fit action \"%s\" (type %d)\n", bp->c_str(), a->name.c_str(), static_cast<int>(a->type));
                return violation(XR_ERROR_PATH_UNSUPPORTED, "a suggested binding of the wrong type for its action");
            }
            add.push_back({a, bd});
        }
        // a profile's suggestions replace the earlier ones for it
        for (Action *a : g_actions)
            a->bindings.erase(std::remove_if(a->bindings.begin(), a->bindings.end(),
                                             [&](const Binding &b) { return b.profile == sb->interactionProfile; }),
                              a->bindings.end());
        for (auto &p : add)
            p.first->bindings.push_back(p.second);
        rtLog("bindings for %s: %u\n", prof->c_str(), sb->countSuggestedBindings);
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtAttachSessionActionSets(XrSession s, const XrSessionActionSetsAttachInfo *ai)
    {
        auto *ss = reinterpret_cast<Session *>(s);
        if (!ss)
            return violation(XR_ERROR_HANDLE_INVALID, "a null session handle");
        if (ss->lost)
            return XR_ERROR_SESSION_LOST;
        if (!ai || ai->type != XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO)
            return violation(XR_ERROR_VALIDATION_FAILURE, "xrAttachSessionActionSets without its structure type");
        if (!ss->attached.empty())
            return violation(XR_ERROR_ACTIONSETS_ALREADY_ATTACHED, "xrAttachSessionActionSets twice on a session");
        if (!ai->countActionSets)
            return violation(XR_ERROR_VALIDATION_FAILURE, "xrAttachSessionActionSets with no action set");
        std::vector<ActionSet *> sets;
        for (uint32_t i = 0; i < ai->countActionSets; ++i)
        {
            ActionSet *set = liveSet(ai->actionSets[i]);
            if (!set)
                return violation(XR_ERROR_HANDLE_INVALID, "xrAttachSessionActionSets with an unknown action set");
            sets.push_back(set);
        }
        for (ActionSet *set : sets)
            set->attached = true;
        ss->attached = sets;
        ss->profilesPending = true;
        rtLog("%u action set(s) attached\n", ai->countActionSets);
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtSyncActions(XrSession s, const XrActionsSyncInfo *si)
    {
        auto *ss = reinterpret_cast<Session *>(s);
        if (!ss)
            return violation(XR_ERROR_HANDLE_INVALID, "a null session handle");
        if (ss->lost)
            return XR_ERROR_SESSION_LOST;
        if (!si || si->type != XR_TYPE_ACTIONS_SYNC_INFO)
            return violation(XR_ERROR_VALIDATION_FAILURE, "xrSyncActions without XR_TYPE_ACTIONS_SYNC_INFO");
        if (ss->attached.empty())
            return violation(XR_ERROR_ACTIONSET_NOT_ATTACHED, "xrSyncActions before xrAttachSessionActionSets");
        const XrPath handPath[2] = {pathOf("/user/hand/left"), pathOf("/user/hand/right")};
        // the active sets, each with the hands it is active for
        std::vector<std::pair<ActionSet *, int>> active; // (set, mask: 1 left, 2 right)
        for (uint32_t i = 0; i < si->countActiveActionSets; ++i)
        {
            const XrActiveActionSet &as = si->activeActionSets[i];
            ActionSet *set = liveSet(as.actionSet);
            if (!set)
                return violation(XR_ERROR_HANDLE_INVALID, "xrSyncActions with an unknown action set");
            if (std::find(ss->attached.begin(), ss->attached.end(), set) == ss->attached.end())
                return violation(XR_ERROR_ACTIONSET_NOT_ATTACHED, "xrSyncActions with an action set not attached to the session");
            int mask = 3;
            if (as.subactionPath != XR_NULL_PATH)
            {
                const std::string *t = pathText(as.subactionPath);
                if (!t || !topLevelUserPath(*t))
                    return violation(XR_ERROR_PATH_UNSUPPORTED, "xrSyncActions: an active set's subaction path is not a top level user path");
                mask = as.subactionPath == handPath[0] ? 1 : as.subactionPath == handPath[1] ? 2 : 0;
            }
            active.push_back({set, mask});
        }
        // the interaction profiles: change only here. The simulated hardware is an oculus/touch_controller pair: a
        // hand gets that profile when the application bound something of it there
        const XrPath touch = pathOf(kProfiles[PTouch]);
        if (ss->profilesPending)
        {
            ss->profilesPending = false;
            bool changed = false;
            for (int h = 0; h < 2; ++h)
            {
                bool bound = false;
                for (const ActionSet *set : ss->attached)
                    for (const Action *a : set->actions)
                        for (const Binding &b : a->bindings)
                            bound = bound || (b.profile == touch && b.hand == h);
                const XrPath p = bound ? touch : XR_NULL_PATH;
                if (p != ss->profile[h])
                {
                    ss->profile[h] = p;
                    changed = true;
                    rtLog("%s hand: interaction profile %s\n", h ? "right" : "left", bound ? kProfiles[PTouch] : "none");
                }
            }
            if (changed)
            {
                XrEventDataInteractionProfileChanged e = {XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED};
                e.session = s;
                std::lock_guard<std::mutex> lock(g_mutex);
                pushEvent(&e, sizeof(e));
            }
        }
        const bool focused = ss->state == XR_SESSION_STATE_FOCUSED;
        const XrTime now = nowNs();
        for (ActionSet *set : ss->attached)
        {
            int mask = 0;
            for (const auto &p : active)
                if (p.first == set)
                    mask |= p.second;
            for (Action *a : set->actions)
            {
                for (int h = 0; h < 2; ++h)
                {
                    Action::State &st = a->st[h];
                    float v[2] = {0.0f, 0.0f};
                    bool on = focused && (mask & (1 << h)) && ss->profile[h] == touch &&
                              (a->subactions.empty() ||
                               std::find(a->subactions.begin(), a->subactions.end(), handPath[h]) != a->subactions.end());
                    bool bound = false;
                    for (const Binding &b : a->bindings)
                    {
                        if (b.profile != touch || b.hand != h)
                            continue;
                        bound = true;
                        float x[2];
                        touchValue(h, b.component, x);
                        if (a->type == XR_ACTION_TYPE_BOOLEAN_INPUT)
                            v[0] = std::max(v[0], x[0] >= 0.5f ? 1.0f : 0.0f);
                        else if (a->type == XR_ACTION_TYPE_FLOAT_INPUT)
                        {
                            const float f = b.kind == KBool ? (x[0] >= 0.5f ? 1.0f : 0.0f) : x[0];
                            if (std::fabs(f) > std::fabs(v[0]))
                                v[0] = f;
                        }
                        else if (a->type == XR_ACTION_TYPE_VECTOR2F_INPUT && x[0] * x[0] + x[1] * x[1] > v[0] * v[0] + v[1] * v[1])
                            v[0] = x[0], v[1] = x[1];
                    }
                    on = on && bound;
                    if (!on)
                        v[0] = v[1] = 0.0f;
                    st.changed = on && st.active && (v[0] != st.v[0] || v[1] != st.v[1]);
                    if (st.changed || (on && !st.active))
                        st.lastChange = now;
                    st.active = on;
                    st.v[0] = v[0], st.v[1] = v[1];
                }
                // both hands (XR_NULL_PATH): the larger value of the active ones
                Action::State &both = a->st[2];
                float v[2] = {0.0f, 0.0f};
                bool on = false;
                for (int h = 0; h < 2; ++h)
                {
                    const Action::State &st = a->st[h];
                    if (!st.active)
                        continue;
                    if (!on || st.v[0] * st.v[0] + st.v[1] * st.v[1] > v[0] * v[0] + v[1] * v[1])
                        v[0] = st.v[0], v[1] = st.v[1];
                    on = true;
                }
                both.changed = on && both.active && (v[0] != both.v[0] || v[1] != both.v[1]);
                if (both.changed || (on && !both.active))
                    both.lastChange = now;
                both.active = on;
                both.v[0] = v[0], both.v[1] = v[1];
            }
        }
        return focused ? XR_SUCCESS : XR_SESSION_NOT_FOCUSED;
    }
    XrResult XRAPI_CALL rtGetCurrentInteractionProfile(XrSession s, XrPath top, XrInteractionProfileState *out)
    {
        auto *ss = reinterpret_cast<Session *>(s);
        if (!ss)
            return violation(XR_ERROR_HANDLE_INVALID, "a null session handle");
        if (ss->lost)
            return XR_ERROR_SESSION_LOST;
        if (!out || out->type != XR_TYPE_INTERACTION_PROFILE_STATE)
            return violation(XR_ERROR_VALIDATION_FAILURE, "xrGetCurrentInteractionProfile without XR_TYPE_INTERACTION_PROFILE_STATE");
        if (ss->attached.empty())
            return violation(XR_ERROR_ACTIONSET_NOT_ATTACHED, "xrGetCurrentInteractionProfile before xrAttachSessionActionSets");
        const std::string *t = pathText(top);
        if (!t)
            return violation(XR_ERROR_PATH_INVALID, "xrGetCurrentInteractionProfile with an unknown path");
        if (!topLevelUserPath(*t))
            return violation(XR_ERROR_PATH_UNSUPPORTED, "xrGetCurrentInteractionProfile with a path not a top level user path");
        out->interactionProfile = *t == "/user/hand/left" ? ss->profile[0] : *t == "/user/hand/right" ? ss->profile[1] : XR_NULL_PATH;
        return XR_SUCCESS;
    }
    // the common checks of xrGetActionState*: the synced state for getInfo, or an error
    XrResult actionState(XrSession s, const XrActionStateGetInfo *gi, XrActionType type, XrStructureType stateType,
                         const XrStructureType *outType, const Action::State *&out)
    {
        static const Action::State inactive;
        auto *ss = reinterpret_cast<Session *>(s);
        if (!ss)
            return violation(XR_ERROR_HANDLE_INVALID, "a null session handle");
        if (ss->lost)
            return XR_ERROR_SESSION_LOST;
        if (!gi || gi->type != XR_TYPE_ACTION_STATE_GET_INFO || !outType || *outType != stateType)
            return violation(XR_ERROR_VALIDATION_FAILURE, "xrGetActionState*: a wrong structure type");
        Action *a = liveAction(gi->action);
        if (!a)
            return violation(XR_ERROR_HANDLE_INVALID, "xrGetActionState* with an unknown action");
        if (a->type != type)
            return violation(XR_ERROR_ACTION_TYPE_MISMATCH, "xrGetActionState* of another type than the action's");
        if (std::find(ss->attached.begin(), ss->attached.end(), a->set) == ss->attached.end())
            return violation(XR_ERROR_ACTIONSET_NOT_ATTACHED, "xrGetActionState* for an action whose set is not attached");
        const int slot = subactionSlot(*a, gi->subactionPath);
        if (slot == -2)
            return violation(XR_ERROR_PATH_UNSUPPORTED, "xrGetActionState* with a subaction path the action was not created with");
        out = slot < 0 ? &inactive : &a->st[slot];
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtGetActionStateBoolean(XrSession s, const XrActionStateGetInfo *gi, XrActionStateBoolean *st)
    {
        const Action::State *a = nullptr;
        const XrResult r = actionState(s, gi, XR_ACTION_TYPE_BOOLEAN_INPUT, XR_TYPE_ACTION_STATE_BOOLEAN, st ? &st->type : nullptr, a);
        if (XR_FAILED(r))
            return r;
        st->isActive = a->active, st->currentState = a->v[0] >= 0.5f, st->changedSinceLastSync = a->changed;
        st->lastChangeTime = a->lastChange;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtGetActionStateFloat(XrSession s, const XrActionStateGetInfo *gi, XrActionStateFloat *st)
    {
        const Action::State *a = nullptr;
        const XrResult r = actionState(s, gi, XR_ACTION_TYPE_FLOAT_INPUT, XR_TYPE_ACTION_STATE_FLOAT, st ? &st->type : nullptr, a);
        if (XR_FAILED(r))
            return r;
        st->isActive = a->active, st->currentState = a->v[0], st->changedSinceLastSync = a->changed;
        st->lastChangeTime = a->lastChange;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtGetActionStateVector2f(XrSession s, const XrActionStateGetInfo *gi, XrActionStateVector2f *st)
    {
        const Action::State *a = nullptr;
        const XrResult r = actionState(s, gi, XR_ACTION_TYPE_VECTOR2F_INPUT, XR_TYPE_ACTION_STATE_VECTOR2F, st ? &st->type : nullptr, a);
        if (XR_FAILED(r))
            return r;
        st->isActive = a->active, st->currentState = {a->v[0], a->v[1]}, st->changedSinceLastSync = a->changed;
        st->lastChangeTime = a->lastChange;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtGetActionStatePose(XrSession s, const XrActionStateGetInfo *gi, XrActionStatePose *st)
    {
        const Action::State *a = nullptr;
        const XrResult r = actionState(s, gi, XR_ACTION_TYPE_POSE_INPUT, XR_TYPE_ACTION_STATE_POSE, st ? &st->type : nullptr, a);
        if (XR_FAILED(r))
            return r;
        st->isActive = a->active;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtCreateActionSpace(XrSession s, const XrActionSpaceCreateInfo *ci, XrSpace *out)
    {
        auto *ss = reinterpret_cast<Session *>(s);
        if (!ss)
            return violation(XR_ERROR_HANDLE_INVALID, "a null session handle");
        if (ss->lost)
            return XR_ERROR_SESSION_LOST;
        if (!ci || ci->type != XR_TYPE_ACTION_SPACE_CREATE_INFO)
            return violation(XR_ERROR_VALIDATION_FAILURE, "xrCreateActionSpace without XR_TYPE_ACTION_SPACE_CREATE_INFO");
        Action *a = liveAction(ci->action);
        if (!a)
            return violation(XR_ERROR_HANDLE_INVALID, "xrCreateActionSpace with an unknown action");
        if (a->type != XR_ACTION_TYPE_POSE_INPUT)
            return violation(XR_ERROR_ACTION_TYPE_MISMATCH, "xrCreateActionSpace for an action that is not a pose");
        const int slot = subactionSlot(*a, ci->subactionPath);
        if (slot == -2)
            return violation(XR_ERROR_PATH_UNSUPPORTED, "xrCreateActionSpace with a subaction path the action was not created with");
        const XrQuaternionf &q = ci->poseInActionSpace.orientation;
        if (std::fabs(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w - 1.0f) > 0.01f)
            return violation(XR_ERROR_POSE_INVALID, "xrCreateActionSpace: the pose's orientation is not normalised");
        auto *sp = new Space();
        sp->action = a;
        sp->hand = slot == 1 ? 1 : 0;
        sp->offset = ci->poseInActionSpace;
        *out = reinterpret_cast<XrSpace>(sp);
        rtLog("action space for \"%s\" (%s hand)\n", a->name.c_str(), sp->hand ? "right" : "left");
        return XR_SUCCESS;
    }
    // the common checks of the haptic calls: the action and its hand slot (0 left, 1 right, 2 both, -1 another path)
    XrResult hapticAction(XrSession s, const XrHapticActionInfo *hi, Session *&ss, Action *&a, int &slot)
    {
        ss = reinterpret_cast<Session *>(s);
        if (!ss)
            return violation(XR_ERROR_HANDLE_INVALID, "a null session handle");
        if (ss->lost)
            return XR_ERROR_SESSION_LOST;
        if (!hi || hi->type != XR_TYPE_HAPTIC_ACTION_INFO)
            return violation(XR_ERROR_VALIDATION_FAILURE, "a haptic call without XR_TYPE_HAPTIC_ACTION_INFO");
        a = liveAction(hi->action);
        if (!a)
            return violation(XR_ERROR_HANDLE_INVALID, "a haptic call with an unknown action");
        if (a->type != XR_ACTION_TYPE_VIBRATION_OUTPUT)
            return violation(XR_ERROR_ACTION_TYPE_MISMATCH, "a haptic call for an action that is not a vibration output");
        if (std::find(ss->attached.begin(), ss->attached.end(), a->set) == ss->attached.end())
            return violation(XR_ERROR_ACTIONSET_NOT_ATTACHED, "a haptic call for an action whose set is not attached");
        slot = subactionSlot(*a, hi->subactionPath);
        if (slot == -2)
            return violation(XR_ERROR_PATH_UNSUPPORTED, "a haptic call with a subaction path the action was not created with");
        return XR_SUCCESS;
    }
    const char *slotName(int slot) { return slot == 0 ? "left" : slot == 1 ? "right" : slot == 2 ? "both" : "other"; }
    XrResult XRAPI_CALL rtApplyHapticFeedback(XrSession s, const XrHapticActionInfo *hi, const XrHapticBaseHeader *h)
    {
        Session *ss = nullptr;
        Action *a = nullptr;
        int slot = 0;
        const XrResult r = hapticAction(s, hi, ss, a, slot);
        if (XR_FAILED(r))
            return r;
        if (!h || h->type != XR_TYPE_HAPTIC_VIBRATION)
            return violation(XR_ERROR_VALIDATION_FAILURE, "xrApplyHapticFeedback without XR_TYPE_HAPTIC_VIBRATION");
        const auto *v = reinterpret_cast<const XrHapticVibration *>(h);
        if (!(v->amplitude >= 0.0f && v->amplitude <= 1.0f))
            return violation(XR_ERROR_VALIDATION_FAILURE, "xrApplyHapticFeedback: amplitude outside 0..1");
        if (ss->state != XR_SESSION_STATE_FOCUSED)
        {
            rtLog("haptic %s: ignored (not focused)\n", slotName(slot));
            return XR_SESSION_NOT_FOCUSED;
        }
        char freq[32];
        if (v->frequency == XR_FREQUENCY_UNSPECIFIED)
            std::snprintf(freq, sizeof(freq), "unspecified");
        else
            std::snprintf(freq, sizeof(freq), "%.0f Hz", v->frequency);
        rtLog("haptic %s: amplitude %.2f, duration %s%.1f ms, frequency %s\n", slotName(slot), v->amplitude,
              v->duration == XR_MIN_HAPTIC_DURATION ? "minimum " : "", v->duration == XR_MIN_HAPTIC_DURATION ? 0.0 : v->duration / 1e6, freq);
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtStopHapticFeedback(XrSession s, const XrHapticActionInfo *hi)
    {
        Session *ss = nullptr;
        Action *a = nullptr;
        int slot = 0;
        const XrResult r = hapticAction(s, hi, ss, a, slot);
        if (XR_FAILED(r))
            return r;
        if (ss->state != XR_SESSION_STATE_FOCUSED)
            return XR_SESSION_NOT_FOCUSED;
        rtLog("haptic %s: stopped\n", slotName(slot));
        return XR_SUCCESS;
    }

    XrResult XRAPI_CALL rtResultToString(XrInstance, XrResult r, char buffer[XR_MAX_RESULT_STRING_SIZE])
    {
        std::snprintf(buffer, XR_MAX_RESULT_STRING_SIZE, "XR_RESULT_%d", static_cast<int>(r));
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtRequestExitSession(XrSession) { return XR_SUCCESS; }

    XrResult XRAPI_CALL rtGetInstanceProcAddr(XrInstance, const char *name, PFN_xrVoidFunction *fn)
    {
        struct Entry
        {
            const char *name;
            PFN_xrVoidFunction fn;
        };
#define E(n, f) {n, reinterpret_cast<PFN_xrVoidFunction>(&f)}
        static const Entry table[] = {
            E("xrGetInstanceProcAddr", rtGetInstanceProcAddr),
            E("xrEnumerateInstanceExtensionProperties", rtEnumerateInstanceExtensionProperties),
            E("xrCreateInstance", rtCreateInstance),
            E("xrDestroyInstance", rtDestroyInstance),
            E("xrGetInstanceProperties", rtGetInstanceProperties),
            E("xrGetSystem", rtGetSystem),
            E("xrGetSystemProperties", rtGetSystemProperties),
            E("xrEnumerateViewConfigurationViews", rtEnumerateViewConfigurationViews),
            E("xrEnumerateEnvironmentBlendModes", rtEnumerateEnvironmentBlendModes),
            E("xrGetD3D11GraphicsRequirementsKHR", rtGetD3D11GraphicsRequirementsKHR),
            E("xrCreateSession", rtCreateSession),
            E("xrDestroySession", rtDestroySession),
            E("xrBeginSession", rtBeginSession),
            E("xrEndSession", rtEndSession),
            E("xrRequestExitSession", rtRequestExitSession),
            E("xrPollEvent", rtPollEvent),
            E("xrCreateReferenceSpace", rtCreateReferenceSpace),
            E("xrDestroySpace", rtDestroySpace),
            E("xrLocateSpace", rtLocateSpace),
            E("xrLocateViews", rtLocateViews),
            E("xrEnumerateSwapchainFormats", rtEnumerateSwapchainFormats),
            E("xrCreateSwapchain", rtCreateSwapchain),
            E("xrDestroySwapchain", rtDestroySwapchain),
            E("xrEnumerateSwapchainImages", rtEnumerateSwapchainImages),
            E("xrAcquireSwapchainImage", rtAcquireSwapchainImage),
            E("xrWaitSwapchainImage", rtWaitSwapchainImage),
            E("xrReleaseSwapchainImage", rtReleaseSwapchainImage),
            E("xrWaitFrame", rtWaitFrame),
            E("xrBeginFrame", rtBeginFrame),
            E("xrEndFrame", rtEndFrame),
            E("xrResultToString", rtResultToString),
            E("xrStringToPath", rtStringToPath),
            E("xrPathToString", rtPathToString),
            E("xrCreateActionSet", rtCreateActionSet),
            E("xrDestroyActionSet", rtDestroyActionSet),
            E("xrCreateAction", rtCreateAction),
            E("xrDestroyAction", rtDestroyAction),
            E("xrSuggestInteractionProfileBindings", rtSuggestInteractionProfileBindings),
            E("xrAttachSessionActionSets", rtAttachSessionActionSets),
            E("xrSyncActions", rtSyncActions),
            E("xrGetCurrentInteractionProfile", rtGetCurrentInteractionProfile),
            E("xrGetActionStateBoolean", rtGetActionStateBoolean),
            E("xrGetActionStateFloat", rtGetActionStateFloat),
            E("xrGetActionStateVector2f", rtGetActionStateVector2f),
            E("xrGetActionStatePose", rtGetActionStatePose),
            E("xrCreateActionSpace", rtCreateActionSpace),
            E("xrApplyHapticFeedback", rtApplyHapticFeedback),
            E("xrStopHapticFeedback", rtStopHapticFeedback),
        };
#undef E
        for (const Entry &e : table)
            if (std::strcmp(e.name, name) == 0)
            {
                *fn = e.fn;
                return XR_SUCCESS;
            }
        *fn = nullptr;
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
}

extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo *info,
                                                                                       XrNegotiateRuntimeRequest *req)
{
    if (!info || !req || info->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        req->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST)
        return XR_ERROR_INITIALIZATION_FAILED;
    req->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    req->runtimeApiVersion = XR_MAKE_VERSION(1, 0, 0);
    req->getInstanceProcAddr = &rtGetInstanceProcAddr;
    return XR_SUCCESS;
}
