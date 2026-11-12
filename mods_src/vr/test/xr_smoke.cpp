// A headless smoke test of the OpenXR layer (mods_src\vr\xr.cpp): a D3D11 device of its own, vr::start, ~100 frames of a
// stereo projection layer (left eye red, right eye blue, a white square in the middle of each) plus a HUD quad
// (premultiplied, half-transparent green), then vr::stop. Run with the test runtime:
//   set XR_RUNTIME_JSON=<mods>\vr_xr_test_runtime.json        (or C12_VR_RUNTIME=<mods>\vr_xr_test_runtime.dll)
//   set XRTEST_CONTROL=<dir>\control.txt                        (optional: frame 50 is captured as eye_stereo_*)
//   vr_xr_smoke.exe [frames] [mono]
// Exit code 0 when every frame was submitted.
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
    while (submitted < frames && idle < 500)
    {
        vr::poll();
        if (!vr::waitAndBegin())
        {
            ++idle;
            Sleep(2);
            continue;
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
    vr::stop();
    ctx->Release();
    ctx0->Release();
    const ULONG refs = dev->Release();
    vr::log("smoke: stopped cleanly (device refs left %lu)\n", refs);
    return submitted >= frames ? 0 : 1;
}
