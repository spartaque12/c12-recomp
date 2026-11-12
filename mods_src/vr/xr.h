// The OpenXR layer for C-12's VR mod (modelled on praydog/REFramework's src/mods/vr/runtimes/OpenXR.cpp, MIT; ported
// from the VCS-Recomp VR.asi layer): the loader (openxr_loader.dll next to the mod DLL, loaded at run time), an instance
// with XR_KHR_D3D11_enable, the head-mounted display's system, a session on an existing D3D11 device, the reference
// spaces (VIEW, LOCAL and a recentred LOCAL), swapchains and the frame loop (xrWaitFrame / xrBeginFrame / xrEndFrame)
// with a projection layer (mono or stereo) and quad layers. Main thread only.
//
// Stereo: each frame after waitAndBegin(), locateViews() gives the two eyes' poses and fields of view at the predicted
// display time in the recentred space; render eye i with views[i] into panelImage(eyePanel(i), ...) and hand the same
// views back in ProjectionLayer::views with stereo = true. Mono: one image (Panel::Scene) shown to both eyes from
// ProjectionLayer::pose / fov.
#pragma once

#include <cstdint>
#include <string>

struct ID3D11Device;
struct ID3D11Texture2D;

namespace vr
{
    // A line for <exe folder>\vr.log (time-stamped, started afresh each run) and for the sink (setLogSink: e.g. the
    // game's log; given without a trailing newline). What a player sends back when VR does not work.
    void log(const char *format, ...);
    std::string logPath();
    void setLogSink(void (*sink)(const char *line));

    struct Pose
    {
        float q[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // x, y, z, w
        float p[3] = {0.0f, 0.0f, 0.0f};       // metres; OpenXR axes: x right, y up, -z ahead
    };
    struct Fov
    {
        float left = 0.0f, right = 0.0f, up = 0.0f, down = 0.0f; // radians (left, down negative)
    };
    // One eye: where it is and what it sees.
    struct View
    {
        Pose pose;
        Fov fov;
    };

    enum class Panel
    {
        Scene = 0,      // the 3D picture: mono, or the left eye's image in stereo
        SceneRight = 1, // the right eye's image (stereo)
        Hud = 2,        // the HUD (quad, premultiplied alpha)
        Cinema = 3,     // the window's picture and menus (quad)
        Count = 4
    };
    inline Panel eyePanel(int eye) { return eye ? Panel::SceneRight : Panel::Scene; }

    // The projection layer. Mono (stereo == false): Panel::Scene's image shown to both eyes, rendered for `pose` with
    // field of view `fov`. Stereo: views[0] (left) shows Panel::Scene, views[1] (right) Panel::SceneRight, each
    // rendered for that view's pose and fov (normally exactly what locateViews returned this frame).
    struct ProjectionLayer
    {
        bool on = false;
        bool stereo = false;
        Pose pose; // mono
        Fov fov;   // mono
        View views[2]; // stereo
    };
    // A quad in the recentred space: centre `pose`, width x height metres.
    struct QuadLayer
    {
        bool on = false;
        Panel panel = Panel::Hud;
        Pose pose;
        float width = 1.0f, height = 1.0f;
        bool alpha = false; // blended by the texture's (premultiplied) alpha
    };

    // Is there a loader and an active runtime with D3D11 support? Loads the loader (and, through it, the runtime's
    // DLL) and asks for the runtime's extensions; no instance, no session. `info`: a line for the log either way.
    bool probe(std::string &info);

    // Start: false with `why` (a line for the log) when there is no loader, runtime, headset or D3D11 support; the
    // caller then stays inert. Waiting for a headset that is off is a start that succeeds later (pending()).
    bool start(ID3D11Device *device, std::string &why);
    void stop();
    bool started();
    bool pending(); // the instance is there, the headset not (yet): start again later
    bool exited();  // the runtime ended the session (XR_SESSION_STATE_EXITING): not restarted until VR is switched on again
    void clearExited();
    const char *sessionStateName();
    float refreshHz(); // from the predicted display period (0 before the first frame)
    std::string runtimeName();

    // Events (session state); call once per frame before waitAndBegin().
    void poll();
    bool running(); // the session runs: frames go to the headset
    bool focused(); // the application has the headset's input focus

    // xrWaitFrame (paces the caller to the headset) + xrBeginFrame. False when the session does not run.
    bool waitAndBegin();
    bool frameBegun();
    // The headset's recommended image size per eye (from xrEnumerateViewConfigurationViews; false before start).
    bool recommendedSize(uint32_t &width, uint32_t &height);
    // The headset's recommended width per eye and the left eye's field of view (once the session runs).
    bool eyeView(uint32_t &recommendedWidth, Fov &fov);
    bool shouldRender();
    int64_t predictedDisplayTime();
    // The head (the VIEW space) in the recentred space at the predicted display time.
    bool locateHead(Pose &out);
    // Both eyes (xrLocateViews, PRIMARY_STEREO) in the recentred space at the predicted display time; false when the
    // runtime has no valid orientation (then render nothing new, or mono from the last head pose).
    bool locateViews(View out[2]);
    // The recentred space: its origin at the head's position, facing where the head faces (yaw only), from now on.
    void recenter();

    // The image to draw this frame's panel into (acquired; released by endFrame), made or remade at width x height.
    // `srgb`: the image's format is an *_SRGB one (write linear colour). format(): its DXGI format (an RTV needs it when
    // the runtime makes typeless textures).
    ID3D11Texture2D *panelImage(Panel panel, uint32_t width, uint32_t height, bool &srgb);
    int64_t panelFormat(Panel panel);
    // xrEndFrame with the layers (projection first, quads in order); none when shouldRender is false.
    void endFrame(const ProjectionLayer &projection, const QuadLayer *quads, int quadCount);

    // statistics
    uint64_t framesSubmitted();
    std::string status();
}
