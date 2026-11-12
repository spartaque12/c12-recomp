// mods\vr.dll - placeholder: registers "VR" (on/off) in the settings menu and, on init, logs whether the OpenXR loader
// and an active OpenXR runtime are there (vr::probe: no instance, no session). The real mod replaces this file; it links
// the OpenXR layer (build\mods\vr_xr.lib, see xr.h). Built by mods_src\vr\build_vr.ps1 (C++, /GS-, fixed /BASE).
#include "c12_mod_api.h"
#include "xr.h"

#include <string>

namespace
{
    const C12ModApi *g_api = nullptr;
    int g_enabled = 0;

    void gameLog(const char *line)
    {
        if (g_api && g_api->log)
            g_api->log(line);
    }
}

extern "C" __declspec(dllexport) int c12_mod_init(const C12ModApi *api)
{
    if (!api || api->version < 2)
        return 0;
    g_api = api;
    api->mod_info("VR", "OpenXR headset support (placeholder: only checks for an OpenXR runtime).", &g_enabled);
    vr::setLogSink(gameLog);
    vr::log("vr.dll (placeholder) loaded; VR is %s\n", g_enabled ? "on" : "off");
    std::string info;
    const bool ok = vr::probe(info);
    vr::log("%s: %s (log: %s)\n", ok ? "OpenXR ready" : "OpenXR not available", info.c_str(), vr::logPath().c_str());
    return 1;
}
