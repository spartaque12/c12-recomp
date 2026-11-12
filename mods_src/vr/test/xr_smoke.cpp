// A headless smoke test of the OpenXR layer (mods_src\vr\xr.cpp): a D3D11 device of its own, vr::start, ~100 frames of a
// stereo projection layer (left eye red, right eye blue, a white square in the middle of each) plus a HUD quad
// (premultiplied, half-transparent green), then vr::stop. Run with the test runtime:
//   set XR_RUNTIME_JSON=<mods>\vr_xr_test_runtime.json        (or C12_VR_RUNTIME=<mods>\vr_xr_test_runtime.dll)
//   set XRTEST_CONTROL=<dir>\control.txt                        (optional: frame 50 is captured as eye_stereo_*)
//   vr_xr_smoke.exe [frames] [mono]
// Controllers: vr::syncInput every frame (the hands printed every 30 frames); with a control file and the test
// runtime it also checks the default hand poses, drives a stick / trigger / button / hand pose change at frame 20 and
// checks it is seen, pulses the right hand's haptic, and unfocuses (60) / refocuses (70) the session to check the hands
// go inactive and come back.
// Exit code 0 when every frame was submitted (and, with a control file, the input checks passed; 4 when not).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_1.h>

#include "../xr.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace
{
    DXGI_FORMAT typedFormat(ID3D11Texture2D *t, vr::Panel panel, bool srgb)
    {
        D3D11_TEXTURE2D_DESC d = {};
        t->GetDesc(&d);
        switch (d.Format)
        {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
        default: return d.Format != DXGI_FORMAT_UNKNOWN ? d.Format : static_cast<DXGI_FORMAT>(vr::panelFormat(panel));
        }
    }

    // fill the panel's image with `colour`, plus a white square of `square` pixels in the middle (0: none)
    bool fill(ID3D11Device *dev, ID3D11DeviceContext1 *ctx, vr::Panel panel, uint32_t w, uint32_t h, const float colour[4], int square)
    {
        bool srgb = false;
        ID3D11Texture2D *t = vr::panelImage(panel, w, h, srgb);
        if (!t)
            return false;
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = typedFormat(t, panel, srgb);
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ID3D11RenderTargetView *rtv = nullptr;
        if (FAILED(dev->CreateRenderTargetView(t, &rd, &rtv)))
            return false;
        ctx->ClearRenderTargetView(rtv, colour);
        if (square > 0)
        {
            const float white[4] = {1, 1, 1, 1};
            const D3D11_RECT r = {static_cast<LONG>(w / 2 - square / 2), static_cast<LONG>(h / 2 - square / 2),
                                  static_cast<LONG>(w / 2 + square / 2), static_cast<LONG>(h / 2 + square / 2)};
            ctx->ClearView(rtv, white, &r, 1);
        }
        rtv->Release();
        return true;
    }

    void printHand(int frame, int i, const vr::Hand &h)
    {
        vr::log("smoke: frame %d %s hand: active %d grip (%.2f %.2f %.2f) q(%.2f %.2f %.2f %.2f) aim (%.2f %.2f %.2f) trigger %.2f "
                "squeeze %.2f stick (%.2f %.2f) click %d primary %d secondary %d menu %d\n",
                frame, i ? "right" : "left", h.active ? 1 : 0, h.grip.p[0], h.grip.p[1], h.grip.p[2], h.grip.q[0], h.grip.q[1],
                h.grip.q[2], h.grip.q[3], h.aim.p[0], h.aim.p[1], h.aim.p[2], h.trigger, h.squeeze, h.stick[0], h.stick[1],
                h.stickClick ? 1 : 0, h.primary ? 1 : 0, h.secondary ? 1 : 0, h.menu ? 1 : 0);
    }
    bool approx(float a, float b) { return a > b - 0.01f && a < b + 0.01f; }
    bool at(const vr::Pose &p, float x, float y, float z) { return approx(p.p[0], x) && approx(p.p[1], y) && approx(p.p[2], z); }

    void send(const char *path, const char *lines) // lines for the test runtime's control file
    {
        if (FILE *f = std::fopen(path, "a"))
        {
            std::fputs(lines, f);
            std::fclose(f);
        }
    }
}

int main(int argc, char **argv)
{
    const int frames = argc > 1 && std::atoi(argv[1]) > 0 ? std::atoi(argv[1]) : 100;
    const bool mono = argc > 2 && std::strcmp(argv[2], "mono") == 0;
    vr::setLogSink([](const char *line) { std::printf("%s\n", line); });

    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx0 = nullptr;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                                   D3D11_SDK_VERSION, &dev, nullptr, &ctx0);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
                               &dev, nullptr, &ctx0);
    if (FAILED(hr))
    {
        std::printf("smoke: no D3D11 device (0x%08lx)\n", static_cast<unsigned long>(hr));
        return 2;
    }
    ID3D11DeviceContext1 *ctx = nullptr;
    ctx0->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void **>(&ctx));
    if (!ctx)
    {
        std::printf("smoke: no ID3D11DeviceContext1\n");
        return 2;
    }

    std::string why;
    std::string probe;
    const bool probed = vr::probe(probe);
    vr::log("smoke: probe -> %d: %s\n", probed ? 1 : 0, probe.c_str());
    bool ok = vr::start(dev, why);
    for (int i = 0; !ok && vr::pending() && i < 50; ++i)
    {
        Sleep(100);
        ok = vr::start(dev, why);
    }
    if (!ok)
    {
        vr::log("smoke: start failed: %s\n", why.c_str());
        return 3;
    }
    vr::log("smoke: started, runtime \"%s\", %s\n", vr::runtimeName().c_str(), mono ? "mono" : "stereo");

    const char *control = std::getenv("XRTEST_CONTROL");
    uint32_t w = 0, h = 0;
    int submitted = 0, rendered = 0, idle = 0;
    // the input checks (with a control file): default poses seen, the change at frame 20 seen, inactive while
    // unfocused, active again after
    bool sawDefault = false, sawChange = false, sawUnfocused = false, sawRefocused = false;
    while (submitted < frames && idle < 500)
    {
        vr::poll();
        if (!vr::waitAndBegin())
        {
            ++idle;
            Sleep(2);
            continue;
        }
        vr::Hand hands[2];
        const bool input = vr::syncInput(hands);
        if (submitted % 30 == 0)
        {
            vr::log("smoke: frame %d input %d (focused %d), profiles left \"%s\" right \"%s\"\n", submitted, input ? 1 : 0,
                    vr::focused() ? 1 : 0, vr::interactionProfile(0).c_str(), vr::interactionProfile(1).c_str());
            printHand(submitted, 0, hands[0]);
            printHand(submitted, 1, hands[1]);
        }
        if (control && *control)
        {
            if (submitted < 20 && hands[0].active && hands[1].active && at(hands[0].grip, -0.2f, -0.3f, -0.35f) &&
                at(hands[1].grip, 0.2f, -0.3f, -0.35f) && approx(hands[0].grip.q[3], 1.0f) && hands[1].trigger == 0.0f)
                sawDefault = true;
            if (submitted == 20)
                send(control, "stick r 0.5,-0.25\nbutton r trigger 0.8\nbutton l primary 1\nhand l -0.1,-0.2,-0.4,10,0,0\n");
            if (submitted > 20 && submitted < 60 && hands[1].active && approx(hands[1].stick[0], 0.5f) && approx(hands[1].stick[1], -0.25f) &&
                approx(hands[1].trigger, 0.8f) && hands[0].primary && !hands[1].primary && at(hands[0].grip, -0.1f, -0.2f, -0.4f) &&
                at(hands[0].aim, -0.1f, -0.2f, -0.4f))
            {
                if (!sawChange)
                    vr::log("smoke: the frame-20 input change seen at frame %d\n", submitted);
                sawChange = true;
            }
            if (submitted == 25)
                vr::haptic(1, 0.5f, 0.1f);
            if (submitted == 60)
                send(control, "unfocus\n");
            if (submitted > 62 && submitted < 70 && !input && !hands[0].active && !hands[1].active && hands[1].trigger == 0.0f)
                sawUnfocused = true;
            if (submitted == 70)
                send(control, "focus\n");
            if (submitted > 73 && input && hands[0].active && hands[1].active && approx(hands[1].trigger, 0.8f))
                sawRefocused = true;
        }
        if (!w && vr::recommendedSize(w, h))
        {
            w /= 2, h /= 2; // half the recommended size: enough for a test
            vr::log("smoke: eye images %ux%u\n", w, h);
        }
        vr::ProjectionLayer proj;
        vr::QuadLayer hud;
        if (vr::shouldRender())
        {
            vr::View views[2];
            vr::Pose head;
            if (vr::locateViews(views) && vr::locateHead(head))
            {
                const float red[4] = {1, 0, 0, 1}, blue[4] = {0, 0, 1, 1}, grey[4] = {0.5f, 0.5f, 0.5f, 1};
                proj.on = true;
                if (mono)
                {
                    proj.pose = head;
                    proj.fov = {-0.9f, 0.9f, 0.9f, -0.9f};
                    proj.on = fill(dev, ctx, vr::Panel::Scene, w, h, grey, 64);
                }
                else
                {
                    proj.stereo = true;
                    proj.views[0] = views[0];
                    proj.views[1] = views[1];
                    proj.on = fill(dev, ctx, vr::eyePanel(0), w, h, red, 64) && fill(dev, ctx, vr::eyePanel(1), w, h, blue, 64);
                }
                const float green[4] = {0.0f, 0.5f, 0.0f, 0.5f}; // premultiplied
                hud.on = fill(dev, ctx, vr::Panel::Hud, 512, 256, green, 0);
                hud.panel = vr::Panel::Hud;
                hud.pose.p[1] = -0.2f;
                hud.pose.p[2] = -1.5f;
                hud.width = 1.0f, hud.height = 0.5f;
                hud.alpha = true;
                ++rendered;
            }
        }
        if (control && *control && submitted == 50)
            if (FILE *f = std::fopen(control, "a"))
            {
                std::fprintf(f, "capture %s\n", mono ? "mono" : "stereo");
                std::fclose(f);
            }
        const uint64_t before = vr::framesSubmitted();
        vr::endFrame(proj, &hud, 1);
        if (vr::framesSubmitted() > before)
            ++submitted;
        idle = 0;
    }
    vr::log("smoke: %d frames submitted (%d rendered), status: %s\n", submitted, rendered, vr::status().c_str());
    bool inputOk = true;
    if (control && *control)
    {
        const bool touch = vr::interactionProfile(0) == "/interaction_profiles/oculus/touch_controller" &&
                           vr::interactionProfile(1) == "/interaction_profiles/oculus/touch_controller";
        inputOk = sawDefault && sawChange && sawUnfocused && sawRefocused && touch;
        vr::log("smoke: input checks %s (default poses %d, frame-20 change %d, inactive unfocused %d, active refocused %d, "
                "touch profiles %d)\n",
                inputOk ? "PASSED" : "FAILED", sawDefault ? 1 : 0, sawChange ? 1 : 0, sawUnfocused ? 1 : 0, sawRefocused ? 1 : 0,
                touch ? 1 : 0);
    }
    vr::stop();
    ctx->Release();
    ctx0->Release();
    const ULONG refs = dev->Release();
    vr::log("smoke: stopped cleanly (device refs left %lu)\n", refs);
    return submitted < frames ? 1 : inputOk ? 0 : 4;
}
