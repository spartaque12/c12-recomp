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
    struct Space
    {
        XrReferenceSpaceType type = XR_REFERENCE_SPACE_TYPE_LOCAL;
        XrPosef offset = identity();
    };
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

    // XRTEST_CONTROL=<file>: one command per line, read and deleted when seen (polled every frame / event poll):
    //   sleep | wake | unfocus | focus | loss | exit | instance_loss | recenter | hz <n>
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

    XrPosef spaceInLocal(const Space &s, int64_t t)
    {
        if (s.type == XR_REFERENCE_SPACE_TYPE_VIEW)
            return compose(headAt(t), s.offset);
        return s.offset;
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
        *count = 1;
        if (cap == 0)
            return XR_SUCCESS;
        if (cap < 1)
            return XR_ERROR_SIZE_INSUFFICIENT;
        std::snprintf(props[0].extensionName, XR_MAX_EXTENSION_NAME_SIZE, "%s", XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
        props[0].extensionVersion = XR_KHR_D3D11_enable_SPEC_VERSION;
        return XR_SUCCESS;
    }
    XrResult XRAPI_CALL rtCreateInstance(const XrInstanceCreateInfo *, XrInstance *out)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
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
