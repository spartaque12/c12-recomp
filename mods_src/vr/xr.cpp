// See xr.h. (Modelled on praydog/REFramework's src/mods/vr/runtimes/OpenXR.cpp, MIT.)
#include "xr.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
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
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <vector>

namespace vr
{
    namespace
    {
        FILE *g_logFile = nullptr;
        std::string g_logPath;
        bool g_logTried = false;
        void (*g_logSink)(const char *) = nullptr;
    }

    std::string logPath() { return g_logPath; }
    void setLogSink(void (*sink)(const char *line)) { g_logSink = sink; }

    void log(const char *format, ...)
    {
        char text[1024];
        va_list args;
        va_start(args, format);
        std::vsnprintf(text, sizeof(text), format, args);
        va_end(args);
        if (g_logSink)
        {
            char line[1040];
            std::snprintf(line, sizeof(line), "[VR] %s", text);
            size_t n = std::strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
                line[--n] = 0;
            g_logSink(line);
        }
        if (!g_logTried)
        {
            // <the exe's folder>\vr.log
            g_logTried = true;
            char exe[MAX_PATH] = {};
            GetModuleFileNameA(nullptr, exe, MAX_PATH);
            std::string base(exe);
            const size_t slash = base.find_last_of("\\/");
            base = slash == std::string::npos ? std::string(".\\") : base.substr(0, slash + 1);
            g_logPath = base + "vr.log";
            g_logFile = std::fopen(g_logPath.c_str(), "w");
        }
        if (g_logFile)
        {
            SYSTEMTIME t;
            GetLocalTime(&t);
            std::fprintf(g_logFile, "%02d:%02d:%02d.%03d %s", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, text);
            if (!*text || text[std::strlen(text) - 1] != '\n')
                std::fputc('\n', g_logFile);
            std::fflush(g_logFile);
        }
    }

    namespace
    {
        // ---- the loader's entry points ------------------------------------------------------------------------------
        HMODULE g_loader = nullptr;
        PFN_xrGetInstanceProcAddr pGetInstanceProcAddr = nullptr;
#define VR_XR_FUNCTIONS(X)                                                                                                  \
    X(xrDestroyInstance)                                                                                                    \
    X(xrGetInstanceProperties)                                                                                              \
    X(xrGetSystem)                                                                                                          \
    X(xrGetSystemProperties)                                                                                                \
    X(xrEnumerateViewConfigurationViews)                                                                                    \
    X(xrEnumerateEnvironmentBlendModes)                                                                                     \
    X(xrGetD3D11GraphicsRequirementsKHR)                                                                                    \
    X(xrCreateSession)                                                                                                      \
    X(xrDestroySession)                                                                                                     \
    X(xrBeginSession)                                                                                                       \
    X(xrEndSession)                                                                                                         \
    X(xrPollEvent)                                                                                                          \
    X(xrCreateReferenceSpace)                                                                                               \
    X(xrDestroySpace)                                                                                                       \
    X(xrLocateSpace)                                                                                                        \
    X(xrEnumerateSwapchainFormats)                                                                                          \
    X(xrCreateSwapchain)                                                                                                    \
    X(xrDestroySwapchain)                                                                                                   \
    X(xrEnumerateSwapchainImages)                                                                                           \
    X(xrAcquireSwapchainImage)                                                                                              \
    X(xrWaitSwapchainImage)                                                                                                 \
    X(xrReleaseSwapchainImage)                                                                                              \
    X(xrWaitFrame)                                                                                                          \
    X(xrBeginFrame)                                                                                                         \
    X(xrEndFrame)                                                                                                           \
    X(xrResultToString)                                                                                                     \
    X(xrLocateViews)
// the actions (core 1.0 too, but optional here: without them no controller input, the rest works)
#define VR_XR_INPUT_FUNCTIONS(X)                                                                                            \
    X(xrStringToPath)                                                                                                       \
    X(xrPathToString)                                                                                                       \
    X(xrCreateActionSet)                                                                                                    \
    X(xrDestroyActionSet)                                                                                                   \
    X(xrCreateAction)                                                                                                       \
    X(xrDestroyAction)                                                                                                      \
    X(xrSuggestInteractionProfileBindings)                                                                                  \
    X(xrAttachSessionActionSets)                                                                                            \
    X(xrGetCurrentInteractionProfile)                                                                                       \
    X(xrSyncActions)                                                                                                        \
    X(xrGetActionStateBoolean)                                                                                              \
    X(xrGetActionStateFloat)                                                                                                \
    X(xrGetActionStateVector2f)                                                                                             \
    X(xrGetActionStatePose)                                                                                                 \
    X(xrCreateActionSpace)                                                                                                  \
    X(xrApplyHapticFeedback)                                                                                                \
    X(xrStopHapticFeedback)
#define VR_XR_DECLARE(name) PFN_##name p_##name = nullptr;
        VR_XR_FUNCTIONS(VR_XR_DECLARE)
        VR_XR_INPUT_FUNCTIONS(VR_XR_DECLARE)
#undef VR_XR_DECLARE
        PFN_xrEnumerateInstanceExtensionProperties p_xrEnumerateInstanceExtensionProperties = nullptr;
        PFN_xrCreateInstance p_xrCreateInstance = nullptr;

        // ---- the session --------------------------------------------------------------------------------------------
        ID3D11Device *g_device = nullptr;
        XrInstance g_instance = XR_NULL_HANDLE;
        XrSystemId g_system = XR_NULL_SYSTEM_ID;
        XrSession g_session = XR_NULL_HANDLE;
        XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
        bool g_running = false;
        XrSpace g_viewSpace = XR_NULL_HANDLE, g_localSpace = XR_NULL_HANDLE, g_space = XR_NULL_HANDLE; // g_space: recentred
        bool g_wantRecenter = true;
        XrEnvironmentBlendMode g_blend = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        XrFrameState g_frame = {XR_TYPE_FRAME_STATE};
        bool g_frameBegun = false;
        uint32_t g_viewCount = 2;
        std::string g_runtime;
        uint64_t g_submitted = 0;
        int64_t g_formats[4] = {}; // preference order, filled from the runtime's list
        int g_formatCount = 0;
        std::string g_lastError;
        bool g_exited = false;           // XR_SESSION_STATE_EXITING: the runtime / user ended VR
        bool g_retryable = false;        // the last start failed for a reason that passes (the runtime restarting)
        int64_t g_period = 0;            // the predicted display period last logged
        uint32_t g_recWidth = 0, g_recHeight = 0; // the headset's recommended size per eye
        Fov g_eyeFov;                    // the left eye's field of view (xrLocateViews), for the render scale
        bool g_viewsLogged = false;      // the first stereo locate logged
        bool g_eyeFovValid = false;
        uint64_t g_errors = 0;

        // ---- the controllers' actions -------------------------------------------------------------------------------
        enum Act
        {
            ActGrip,
            ActAim,
            ActTrigger,
            ActSqueeze,
            ActStick,
            ActStickClick,
            ActPrimary,
            ActSecondary,
            ActMenu,
            ActHaptic,
            ActCount
        };
        bool g_inputFunctions = false; // the runtime has the action functions
        bool g_hpExtension = false;    // XR_EXT_hp_mixed_reality_controller enabled
        bool g_inputMade = false;      // the action set made and bound (per instance; tried once)
        XrActionSet g_actionSet = XR_NULL_HANDLE;
        XrAction g_actions[ActCount] = {};
        XrPath g_handPath[2] = {XR_NULL_PATH, XR_NULL_PATH};
        XrSpace g_gripSpace[2] = {}, g_aimSpace[2] = {};
        bool g_inputAttached = false; // attached to the current session
        std::string g_profile[2];     // the hands' interaction profiles (from INTERACTION_PROFILE_CHANGED)

        const char *stateName(XrSessionState s)
        {
            switch (s)
            {
            case XR_SESSION_STATE_IDLE: return "IDLE";
            case XR_SESSION_STATE_READY: return "READY";
            case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
            case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
            case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
            case XR_SESSION_STATE_STOPPING: return "STOPPING";
            case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
            case XR_SESSION_STATE_EXITING: return "EXITING";
            default: return "UNKNOWN";
            }
        }

        const char *formatName(int64_t f)
        {
            switch (f)
            {
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
            case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
            case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
            case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
            case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
            case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
            case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
            case DXGI_FORMAT_D16_UNORM: return "D16_UNORM";
            default: return nullptr;
            }
        }

        struct Swapchain
        {
            XrSwapchain handle = XR_NULL_HANDLE;
            uint32_t width = 0, height = 0;
            int64_t format = 0;
            std::vector<XrSwapchainImageD3D11KHR> images;
            bool acquired = false;
            uint32_t index = 0;
        };
        Swapchain g_chains[static_cast<int>(Panel::Count)];

        const char *resultText(XrResult r)
        {
            static char text[XR_MAX_RESULT_STRING_SIZE];
            if (p_xrResultToString && g_instance && XR_SUCCEEDED(p_xrResultToString(g_instance, r, text)))
                return text;
            std::snprintf(text, sizeof(text), "XrResult %d", static_cast<int>(r));
            return text;
        }

        bool check(XrResult r, const char *what)
        {
            if (XR_SUCCEEDED(r))
                return true;
            g_lastError = std::string(what) + ": " + resultText(r);
            if (++g_errors <= 200) // a runtime failing every frame would fill the log
                vr::log("error: %s\n", g_lastError.c_str());
            return false;
        }

        std::wstring moduleDir()
        {
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&moduleDir), &self);
            wchar_t path[MAX_PATH] = {};
            GetModuleFileNameW(self, path, MAX_PATH);
            std::wstring s(path);
            const size_t slash = s.find_last_of(L"\\/");
            return slash == std::wstring::npos ? std::wstring() : s.substr(0, slash + 1);
        }

        bool loadLoader(std::string &why)
        {
            if (g_loader)
                return true;
            // C12_VR_RUNTIME=<runtime.dll>: that runtime directly, without the loader (tests: the loader ignores
            // XR_RUNTIME_JSON in an elevated process; no API layers then)
            if (const char *rt = std::getenv("C12_VR_RUNTIME"); rt && *rt)
            {
                g_loader = LoadLibraryA(rt);
                auto negotiate = g_loader ? reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
                                                GetProcAddress(g_loader, "xrNegotiateLoaderRuntimeInterface"))
                                          : nullptr;
                XrNegotiateLoaderInfo li = {XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION,
                                            sizeof(XrNegotiateLoaderInfo), 1, XR_CURRENT_LOADER_RUNTIME_VERSION,
                                            XR_MAKE_VERSION(1, 0, 0), XR_CURRENT_API_VERSION};
                XrNegotiateRuntimeRequest rq = {XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST, XR_RUNTIME_INFO_STRUCT_VERSION,
                                                sizeof(XrNegotiateRuntimeRequest)};
                if (!negotiate || XR_FAILED(negotiate(&li, &rq)) || !rq.getInstanceProcAddr)
                {
                    why = std::string("C12_VR_RUNTIME=") + rt + ": not an OpenXR runtime";
                    if (g_loader)
                        FreeLibrary(g_loader);
                    g_loader = nullptr;
                    return false;
                }
                pGetInstanceProcAddr = rq.getInstanceProcAddr;
                vr::log("runtime %s loaded directly (C12_VR_RUNTIME)\n", rt);
            }
            if (pGetInstanceProcAddr)
            {
                pGetInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
                                     reinterpret_cast<PFN_xrVoidFunction *>(&p_xrEnumerateInstanceExtensionProperties));
                pGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction *>(&p_xrCreateInstance));
                return p_xrEnumerateInstanceExtensionProperties && p_xrCreateInstance;
            }
            const std::wstring here = moduleDir() + L"openxr_loader.dll";
            g_loader = LoadLibraryW(here.c_str());
            if (!g_loader)
                g_loader = LoadLibraryW(L"openxr_loader.dll");
            if (!g_loader)
            {
                why = "openxr_loader.dll not found (put it next to vr.dll in mods\\)";
                return false;
            }
            pGetInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(g_loader, "xrGetInstanceProcAddr"));
            if (!pGetInstanceProcAddr)
            {
                why = "openxr_loader.dll has no xrGetInstanceProcAddr";
                return false;
            }
            pGetInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
                                 reinterpret_cast<PFN_xrVoidFunction *>(&p_xrEnumerateInstanceExtensionProperties));
            pGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction *>(&p_xrCreateInstance));
            if (!p_xrEnumerateInstanceExtensionProperties || !p_xrCreateInstance)
            {
                why = "openxr_loader.dll: no xrCreateInstance";
                return false;
            }
            wchar_t path[MAX_PATH] = {};
            GetModuleFileNameW(g_loader, path, MAX_PATH);
            char narrow[MAX_PATH * 2] = {};
            WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, sizeof(narrow), nullptr, nullptr);
            vr::log("OpenXR loader: %s\n", narrow);
            // the active runtime as the loader will find it (read-only; per machine, or per user on some setups)
            for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER})
            {
                char value[1024] = {};
                DWORD size = sizeof(value);
                if (RegGetValueA(root, "SOFTWARE\\Khronos\\OpenXR\\1", "ActiveRuntime", RRF_RT_REG_SZ, nullptr, value, &size) ==
                    ERROR_SUCCESS)
                    vr::log("active OpenXR runtime (%s): %s\n", root == HKEY_LOCAL_MACHINE ? "HKLM" : "HKCU", value);
            }
            if (const char *e = std::getenv("XR_RUNTIME_JSON"); e && *e)
                vr::log("XR_RUNTIME_JSON=%s (the loader ignores it when the game runs as administrator)\n", e);
            return true;
        }

        bool createInstance(std::string &why)
        {
            uint32_t count = 0;
            XrResult r = p_xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
            if (XR_FAILED(r))
            {
                why = r == XR_ERROR_RUNTIME_UNAVAILABLE || r == XR_ERROR_RUNTIME_FAILURE
                          ? "no OpenXR runtime (none installed or none active: SteamVR, Meta Quest Link, Virtual Desktop, WMR ...)"
                          : std::string("the OpenXR runtime's extensions: XrResult ") + std::to_string(r);
                return false;
            }
            std::vector<XrExtensionProperties> ext(count, {XR_TYPE_EXTENSION_PROPERTIES});
            p_xrEnumerateInstanceExtensionProperties(nullptr, count, &count, ext.data());
            bool d3d11 = false, hp = false;
            std::string names;
            for (const XrExtensionProperties &e : ext)
            {
                d3d11 = d3d11 || std::strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0;
                hp = hp || std::strcmp(e.extensionName, XR_EXT_HP_MIXED_REALITY_CONTROLLER_EXTENSION_NAME) == 0;
                names += std::string(names.empty() ? "" : " ") + e.extensionName;
            }
            vr::log("runtime extensions (%u): %s\n", count, names.c_str());
            if (!d3d11)
            {
                why = "the OpenXR runtime has no D3D11 support (XR_KHR_D3D11_enable)";
                return false;
            }
            // the HP Reverb G2 controller's profile needs its extension (OpenXR 1.0)
            const char *enabled[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME, XR_EXT_HP_MIXED_REALITY_CONTROLLER_EXTENSION_NAME};
            XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
            std::snprintf(ci.applicationInfo.applicationName, sizeof(ci.applicationInfo.applicationName), "C-12 Final Resistance (recompiled)");
            ci.applicationInfo.applicationVersion = 1;
            std::snprintf(ci.applicationInfo.engineName, sizeof(ci.applicationInfo.engineName), "C12Recomp vr");
            ci.applicationInfo.engineVersion = 1;
            ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0); // 1.0: every runtime accepts it
            ci.enabledExtensionCount = hp ? 2 : 1;
            ci.enabledExtensionNames = enabled;
            r = p_xrCreateInstance(&ci, &g_instance);
            if (XR_FAILED(r) && hp) // a runtime that lists it but will not enable it: without
            {
                vr::log("xrCreateInstance with %s: XrResult %d; again without it\n", XR_EXT_HP_MIXED_REALITY_CONTROLLER_EXTENSION_NAME, r);
                hp = false;
                ci.enabledExtensionCount = 1;
                r = p_xrCreateInstance(&ci, &g_instance);
            }
            if (XR_FAILED(r))
            {
                why = std::string("xrCreateInstance: XrResult ") + std::to_string(r);
                g_instance = XR_NULL_HANDLE;
                return false;
            }
            g_hpExtension = hp;
            g_inputMade = false;
#define VR_XR_LOAD(name) pGetInstanceProcAddr(g_instance, #name, reinterpret_cast<PFN_xrVoidFunction *>(&p_##name));
            VR_XR_FUNCTIONS(VR_XR_LOAD)
#undef VR_XR_LOAD
#define VR_XR_CHECK(name) || !p_##name
            if (false VR_XR_FUNCTIONS(VR_XR_CHECK))
            {
                why = "the OpenXR runtime lacks a core function";
                return false;
            }
            g_inputFunctions = true;
#define VR_XR_INPUT_LOAD(name)                                                                                              \
    p_##name = nullptr;                                                                                                     \
    pGetInstanceProcAddr(g_instance, #name, reinterpret_cast<PFN_xrVoidFunction *>(&p_##name));                             \
    g_inputFunctions = g_inputFunctions && p_##name;
            VR_XR_INPUT_FUNCTIONS(VR_XR_INPUT_LOAD)
#undef VR_XR_INPUT_LOAD
            if (!g_inputFunctions)
                vr::log("warning: the OpenXR runtime lacks the action functions: no controller input\n");
#undef VR_XR_CHECK
            XrInstanceProperties props = {XR_TYPE_INSTANCE_PROPERTIES};
            if (XR_SUCCEEDED(p_xrGetInstanceProperties(g_instance, &props)))
            {
                char v[64];
                std::snprintf(v, sizeof(v), " %u.%u.%u", XR_VERSION_MAJOR(props.runtimeVersion), XR_VERSION_MINOR(props.runtimeVersion),
                              XR_VERSION_PATCH(props.runtimeVersion));
                g_runtime = std::string(props.runtimeName) + v;
            }
            vr::log("OpenXR instance: runtime %s\n", g_runtime.c_str());
            return true;
        }

        // the headset; XR_ERROR_FORM_FACTOR_UNAVAILABLE (off, not connected) is "later"
        enum class SystemResult
        {
            Ok,
            Later,
            Failed
        };
        SystemResult getSystem(std::string &why)
        {
            XrSystemGetInfo gi = {XR_TYPE_SYSTEM_GET_INFO};
            gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
            const XrResult r = p_xrGetSystem(g_instance, &gi, &g_system);
            if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE)
            {
                why = "no headset connected (the OpenXR runtime " + g_runtime + " is there): retrying";
                return SystemResult::Later;
            }
            if (r == XR_ERROR_INSTANCE_LOST)
            {
                why = "the OpenXR runtime restarted: reconnecting";
                vr::stop(); // a new instance on the next try
                return SystemResult::Later;
            }
            if (XR_FAILED(r))
            {
                why = std::string("xrGetSystem: ") + resultText(r);
                return SystemResult::Failed;
            }
            XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
            if (XR_SUCCEEDED(p_xrGetSystemProperties(g_instance, g_system, &sp)))
                vr::log("headset: \"%s\" (vendor 0x%x; max swapchain %ux%u, max layers %u; orientation tracking %d, position "
                        "tracking %d)\n",
                        sp.systemName, sp.vendorId, sp.graphicsProperties.maxSwapchainImageWidth,
                        sp.graphicsProperties.maxSwapchainImageHeight, sp.graphicsProperties.maxLayerCount,
                        sp.trackingProperties.orientationTracking ? 1 : 0, sp.trackingProperties.positionTracking ? 1 : 0);
            uint32_t n = 0;
            p_xrEnumerateViewConfigurationViews(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr);
            std::vector<XrViewConfigurationView> views(n, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
            if (n && XR_SUCCEEDED(p_xrEnumerateViewConfigurationViews(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n,
                                                                          &n, views.data())))
                for (uint32_t i = 0; i < n; ++i)
                    g_recWidth = views[0].recommendedImageRectWidth, g_recHeight = views[0].recommendedImageRectHeight,
                    vr::log("view %u (primary stereo): recommended %ux%u, max %ux%u, %u samples\n", i,
                            views[i].recommendedImageRectWidth, views[i].recommendedImageRectHeight, views[i].maxImageRectWidth,
                            views[i].maxImageRectHeight, views[i].recommendedSwapchainSampleCount);
            else
                vr::log("no primary stereo view configuration (%u views)\n", n);
            g_viewCount = std::max(1u, n);
            uint32_t bn = 0;
            p_xrEnumerateEnvironmentBlendModes(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &bn, nullptr);
            std::vector<XrEnvironmentBlendMode> blends(bn);
            if (bn && XR_SUCCEEDED(p_xrEnumerateEnvironmentBlendModes(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, bn,
                                                                          &bn, blends.data())))
                g_blend = blends[0];
            vr::log("environment blend mode %d (of %u)\n", static_cast<int>(g_blend), bn);
            return SystemResult::Ok;
        }

        LUID adapterLuid(ID3D11Device *d)
        {
            LUID luid = {};
            IDXGIDevice *dx = nullptr;
            if (SUCCEEDED(d->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dx))) && dx)
            {
                IDXGIAdapter *a = nullptr;
                if (SUCCEEDED(dx->GetAdapter(&a)) && a)
                {
                    DXGI_ADAPTER_DESC desc = {};
                    if (SUCCEEDED(a->GetDesc(&desc)))
                        luid = desc.AdapterLuid;
                    a->Release();
                }
                dx->Release();
            }
            return luid;
        }

        XrPath toPath(const char *s)
        {
            XrPath p = XR_NULL_PATH;
            if (!check(p_xrStringToPath(g_instance, s, &p), s))
                return XR_NULL_PATH;
            return p;
        }

        // the suggested bindings: per profile, (action, hand L / R / B(oth), the component under /user/hand/<hand>/)
        struct Binding
        {
            Act act;
            char hand;
            const char *component;
        };
        const Binding kTouch[] = {
            {ActGrip, 'B', "input/grip/pose"},         {ActAim, 'B', "input/aim/pose"},
            {ActTrigger, 'B', "input/trigger/value"},  {ActSqueeze, 'B', "input/squeeze/value"},
            {ActStick, 'B', "input/thumbstick"},       {ActStickClick, 'B', "input/thumbstick/click"},
            {ActPrimary, 'L', "input/x/click"},        {ActPrimary, 'R', "input/a/click"},
            {ActSecondary, 'L', "input/y/click"},      {ActSecondary, 'R', "input/b/click"},
            {ActMenu, 'L', "input/menu/click"},        {ActHaptic, 'B', "output/haptic"},
        };
        const Binding kIndex[] = {
            {ActGrip, 'B', "input/grip/pose"},        {ActAim, 'B', "input/aim/pose"},
            {ActTrigger, 'B', "input/trigger/value"}, {ActSqueeze, 'B', "input/squeeze/value"},
            {ActStick, 'B', "input/thumbstick"},      {ActStickClick, 'B', "input/thumbstick/click"},
            {ActPrimary, 'B', "input/a/click"},       {ActSecondary, 'B', "input/b/click"},
            {ActHaptic, 'B', "output/haptic"},
        };
        const Binding kVive[] = {
            {ActGrip, 'B', "input/grip/pose"},        {ActAim, 'B', "input/aim/pose"},
            {ActTrigger, 'B', "input/trigger/value"}, {ActSqueeze, 'B', "input/squeeze/click"},
            {ActStick, 'B', "input/trackpad"},        {ActStickClick, 'B', "input/trackpad/click"},
            {ActPrimary, 'B', "input/trackpad/click"}, {ActSecondary, 'B', "input/menu/click"},
            {ActMenu, 'B', "input/menu/click"},       {ActHaptic, 'B', "output/haptic"},
        };
        const Binding kWmr[] = {
            {ActGrip, 'B', "input/grip/pose"},         {ActAim, 'B', "input/aim/pose"},
            {ActTrigger, 'B', "input/trigger/value"},  {ActSqueeze, 'B', "input/squeeze/click"},
            {ActStick, 'B', "input/thumbstick"},       {ActStickClick, 'B', "input/thumbstick/click"},
            {ActPrimary, 'B', "input/trackpad/click"}, {ActSecondary, 'B', "input/menu/click"},
            {ActMenu, 'B', "input/menu/click"},        {ActHaptic, 'B', "output/haptic"},
        };
        const Binding kSimple[] = {
            {ActGrip, 'B', "input/grip/pose"},       {ActAim, 'B', "input/aim/pose"}, {ActTrigger, 'B', "input/select/click"},
            {ActMenu, 'B', "input/menu/click"},      {ActHaptic, 'B', "output/haptic"},
        };
        const Binding kHp[] = {
            {ActGrip, 'B', "input/grip/pose"},        {ActAim, 'B', "input/aim/pose"},
            {ActTrigger, 'B', "input/trigger/value"}, {ActSqueeze, 'B', "input/squeeze/value"},
            {ActStick, 'B', "input/thumbstick"},      {ActStickClick, 'B', "input/thumbstick/click"},
            {ActPrimary, 'L', "input/x/click"},       {ActPrimary, 'R', "input/a/click"},
            {ActSecondary, 'L', "input/y/click"},     {ActSecondary, 'R', "input/b/click"},
            {ActMenu, 'B', "input/menu/click"},       {ActHaptic, 'B', "output/haptic"},
        };

        void destroyInput()
        {
            if (g_actionSet && p_xrDestroyActionSet)
                p_xrDestroyActionSet(g_actionSet); // its actions with it
            g_actionSet = XR_NULL_HANDLE;
            for (XrAction &a : g_actions)
                a = XR_NULL_HANDLE;
            g_inputMade = false;
        }

        // the action set, its actions and the suggested bindings: once per instance, before any attach
        void createInput()
        {
            if (g_inputMade)
                return;
            g_inputMade = true;
            if (!g_inputFunctions)
                return;
            g_handPath[0] = toPath("/user/hand/left");
            g_handPath[1] = toPath("/user/hand/right");
            XrActionSetCreateInfo si = {XR_TYPE_ACTION_SET_CREATE_INFO};
            std::snprintf(si.actionSetName, sizeof(si.actionSetName), "c12");
            std::snprintf(si.localizedActionSetName, sizeof(si.localizedActionSetName), "C-12");
            if (!g_handPath[0] || !g_handPath[1] || !check(p_xrCreateActionSet(g_instance, &si, &g_actionSet), "xrCreateActionSet"))
            {
                g_actionSet = XR_NULL_HANDLE;
                return;
            }
            struct Def
            {
                const char *name, *localized;
                XrActionType type;
            };
            static const Def defs[ActCount] = {
                {"grip_pose", "Hand pose", XR_ACTION_TYPE_POSE_INPUT},
                {"aim_pose", "Pointer pose", XR_ACTION_TYPE_POSE_INPUT},
                {"trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT},
                {"squeeze", "Grip", XR_ACTION_TYPE_FLOAT_INPUT},
                {"thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT},
                {"thumbstick_click", "Thumbstick click", XR_ACTION_TYPE_BOOLEAN_INPUT},
                {"primary", "Primary button (A / X)", XR_ACTION_TYPE_BOOLEAN_INPUT},
                {"secondary", "Secondary button (B / Y)", XR_ACTION_TYPE_BOOLEAN_INPUT},
                {"menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT},
                {"haptic", "Vibration", XR_ACTION_TYPE_VIBRATION_OUTPUT},
            };
            for (int i = 0; i < ActCount; ++i)
            {
                XrActionCreateInfo ai = {XR_TYPE_ACTION_CREATE_INFO};
                std::snprintf(ai.actionName, sizeof(ai.actionName), "%s", defs[i].name);
                std::snprintf(ai.localizedActionName, sizeof(ai.localizedActionName), "%s", defs[i].localized);
                ai.actionType = defs[i].type;
                ai.countSubactionPaths = 2;
                ai.subactionPaths = g_handPath;
                if (!check(p_xrCreateAction(g_actionSet, &ai, &g_actions[i]), "xrCreateAction"))
                {
                    destroyInput();
                    g_inputMade = true; // not again for this instance
                    return;
                }
            }
            struct Profile
            {
                const char *path;
                const Binding *b;
                size_t n;
            };
            std::vector<Profile> profiles = {
                {"/interaction_profiles/oculus/touch_controller", kTouch, std::size(kTouch)},
                {"/interaction_profiles/valve/index_controller", kIndex, std::size(kIndex)},
                {"/interaction_profiles/htc/vive_controller", kVive, std::size(kVive)},
                {"/interaction_profiles/microsoft/motion_controller", kWmr, std::size(kWmr)},
                {"/interaction_profiles/khr/simple_controller", kSimple, std::size(kSimple)},
            };
            if (g_hpExtension)
                profiles.push_back({"/interaction_profiles/hp/mixed_reality_controller", kHp, std::size(kHp)});
            std::string bound;
            for (const Profile &p : profiles)
            {
                std::vector<XrActionSuggestedBinding> sb;
                for (size_t i = 0; i < p.n; ++i)
                    for (int h = 0; h < 2; ++h)
                    {
                        if ((p.b[i].hand == 'L' && h != 0) || (p.b[i].hand == 'R' && h != 1))
                            continue;
                        const std::string path = std::string(h ? "/user/hand/right/" : "/user/hand/left/") + p.b[i].component;
                        const XrPath xp = toPath(path.c_str());
                        if (xp)
                            sb.push_back({g_actions[p.b[i].act], xp});
                    }
                XrInteractionProfileSuggestedBinding s = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
                s.interactionProfile = toPath(p.path);
                s.countSuggestedBindings = static_cast<uint32_t>(sb.size());
                s.suggestedBindings = sb.data();
                const XrResult r = s.interactionProfile ? p_xrSuggestInteractionProfileBindings(g_instance, &s) : XR_ERROR_PATH_INVALID;
                if (XR_SUCCEEDED(r))
                    bound += std::string(bound.empty() ? "" : ", ") + (p.path + std::strlen("/interaction_profiles/"));
                else
                    vr::log("controller bindings for %s not accepted: %s\n", p.path, resultText(r));
            }
            vr::log("controller bindings suggested for: %s\n", bound.empty() ? "none" : bound.c_str());
        }

        // the action set onto the session just created, and the hands' pose spaces
        void attachInput()
        {
            g_inputAttached = false;
            g_profile[0].clear(), g_profile[1].clear();
            if (!g_actionSet || !g_session)
                return;
            XrSessionActionSetsAttachInfo ai = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
            ai.countActionSets = 1;
            ai.actionSets = &g_actionSet;
            if (!check(p_xrAttachSessionActionSets(g_session, &ai), "xrAttachSessionActionSets"))
                return;
            for (int h = 0; h < 2; ++h)
            {
                XrActionSpaceCreateInfo si = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
                si.subactionPath = g_handPath[h];
                si.poseInActionSpace.orientation.w = 1.0f;
                si.action = g_actions[ActGrip];
                check(p_xrCreateActionSpace(g_session, &si, &g_gripSpace[h]), "xrCreateActionSpace (grip)");
                si.action = g_actions[ActAim];
                check(p_xrCreateActionSpace(g_session, &si, &g_aimSpace[h]), "xrCreateActionSpace (aim)");
            }
            g_inputAttached = true;
            vr::log("controller actions attached to the session\n");
        }

        // the hands' interaction profiles (after XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED)
        void updateProfiles()
        {
            if (!g_inputAttached || !g_session)
                return;
            for (int h = 0; h < 2; ++h)
            {
                XrInteractionProfileState st = {XR_TYPE_INTERACTION_PROFILE_STATE};
                if (!check(p_xrGetCurrentInteractionProfile(g_session, g_handPath[h], &st), "xrGetCurrentInteractionProfile"))
                    continue;
                std::string name;
                if (st.interactionProfile != XR_NULL_PATH)
                {
                    char text[XR_MAX_PATH_LENGTH] = {};
                    uint32_t n = 0;
                    if (XR_SUCCEEDED(p_xrPathToString(g_instance, st.interactionProfile, sizeof(text), &n, text)))
                        name = text;
                }
                if (name != g_profile[h])
                {
                    g_profile[h] = name;
                    vr::log("%s hand: interaction profile %s\n", h ? "right" : "left", name.empty() ? "none" : name.c_str());
                }
            }
        }

        bool locatePose(XrSpace space, Pose &out)
        {
            XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
            if (!space || XR_FAILED(p_xrLocateSpace(space, g_space, g_frame.predictedDisplayTime, &loc)) ||
                !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
                return false;
            out.q[0] = loc.pose.orientation.x, out.q[1] = loc.pose.orientation.y, out.q[2] = loc.pose.orientation.z,
            out.q[3] = loc.pose.orientation.w;
            const bool pos = (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
            out.p[0] = pos ? loc.pose.position.x : 0.0f, out.p[1] = pos ? loc.pose.position.y : 0.0f,
            out.p[2] = pos ? loc.pose.position.z : 0.0f;
            return true;
        }

        bool createSession(std::string &why)
        {
            XrGraphicsRequirementsD3D11KHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
            if (XR_FAILED(p_xrGetD3D11GraphicsRequirementsKHR(g_instance, g_system, &req)))
            {
                why = "xrGetD3D11GraphicsRequirementsKHR failed";
                return false;
            }
            const LUID mine = adapterLuid(g_device);
            vr::log("D3D11: the game's GPU LUID %08lx:%08lx, the runtime's %08lx:%08lx, feature level 0x%x (runtime minimum 0x%x)\n",
                    static_cast<unsigned long>(mine.HighPart), mine.LowPart, static_cast<unsigned long>(req.adapterLuid.HighPart),
                    req.adapterLuid.LowPart, static_cast<unsigned>(g_device->GetFeatureLevel()),
                    static_cast<unsigned>(req.minFeatureLevel));
            if (mine.LowPart != req.adapterLuid.LowPart || mine.HighPart != req.adapterLuid.HighPart)
                vr::log("warning: the headset wants another GPU than the game's: the session may fail (laptops: run the game "
                        "on the GPU the headset is connected to)\n");
            {
                // a debug aid: the D3D11 device's adapter name
                IDXGIDevice *dx = nullptr;
                if (SUCCEEDED(g_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dx))) && dx)
                {
                    IDXGIAdapter *a = nullptr;
                    DXGI_ADAPTER_DESC desc = {};
                    if (SUCCEEDED(dx->GetAdapter(&a)) && a && SUCCEEDED(a->GetDesc(&desc)))
                    {
                        char name[256] = {};
                        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
                        vr::log("D3D11 adapter: %s\n", name);
                    }
                    if (a)
                        a->Release();
                    dx->Release();
                }
            }
            if (g_device->GetFeatureLevel() < req.minFeatureLevel)
            {
                why = "the game's D3D11 device is below the runtime's minimum feature level";
                return false;
            }
            XrGraphicsBindingD3D11KHR binding = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
            binding.device = g_device;
            XrSessionCreateInfo si = {XR_TYPE_SESSION_CREATE_INFO};
            si.next = &binding;
            si.systemId = g_system;
            XrResult r = p_xrCreateSession(g_instance, &si, &g_session);
            if (XR_FAILED(r))
            {
                why = std::string("xrCreateSession: ") + resultText(r);
                g_session = XR_NULL_HANDLE;
                g_retryable = r == XR_ERROR_INSTANCE_LOST || r == XR_ERROR_SYSTEM_INVALID;
                return false;
            }
            XrReferenceSpaceCreateInfo rs = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
            rs.poseInReferenceSpace.orientation.w = 1.0f;
            rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
            if (!check(p_xrCreateReferenceSpace(g_session, &rs, &g_viewSpace), "VIEW space"))
                return why = g_lastError, false;
            rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
            if (!check(p_xrCreateReferenceSpace(g_session, &rs, &g_localSpace), "LOCAL space") ||
                !check(p_xrCreateReferenceSpace(g_session, &rs, &g_space), "LOCAL space"))
                return why = g_lastError, false;
            // swapchain formats: sRGB first (the picture is sRGB-encoded: written linear into an sRGB image it shows as
            // on the monitor), RGBA before BGRA
            uint32_t n = 0;
            p_xrEnumerateSwapchainFormats(g_session, 0, &n, nullptr);
            std::vector<int64_t> formats(n);
            if (n)
                p_xrEnumerateSwapchainFormats(g_session, n, &n, formats.data());
            const int64_t wanted[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM,
                                      DXGI_FORMAT_B8G8R8A8_UNORM};
            std::string offered;
            for (int64_t f : formats)
            {
                const char *name = formatName(f);
                offered += (offered.empty() ? "" : ", ") + (name ? std::string(name) : std::to_string(f));
            }
            vr::log("swapchain formats offered: %s\n", offered.c_str());
            g_formatCount = 0;
            for (int64_t w : wanted)
                if (std::find(formats.begin(), formats.end(), w) != formats.end())
                    g_formats[g_formatCount++] = w;
            if (!g_formatCount)
            {
                why = "the OpenXR runtime offers no 8-bit RGBA swapchain format";
                return false;
            }
            vr::log("session created; swapchain format %s%s\n", formatName(g_formats[0]),
                    g_formats[0] == DXGI_FORMAT_R8G8B8A8_UNORM || g_formats[0] == DXGI_FORMAT_B8G8R8A8_UNORM
                        ? " (no sRGB format: the picture may look too bright)"
                        : "");
            // the controllers: bound once per instance, attached to every session before its first xrSyncActions
            createInput();
            attachInput();
            return true;
        }

        void destroySwapchain(Swapchain &s)
        {
            if (s.handle)
                p_xrDestroySwapchain(s.handle);
            s = Swapchain();
        }

        bool g_pending = false; // instance up, headset not there yet

        void destroySession()
        {
            for (Swapchain &s : g_chains)
                destroySwapchain(s);
            g_inputAttached = false;
            g_profile[0].clear(), g_profile[1].clear();
            for (XrSpace *sp : {&g_viewSpace, &g_localSpace, &g_space, &g_gripSpace[0], &g_gripSpace[1], &g_aimSpace[0], &g_aimSpace[1]})
                if (*sp)
                {
                    p_xrDestroySpace(*sp);
                    *sp = XR_NULL_HANDLE;
                }
            if (g_session)
                p_xrDestroySession(g_session);
            g_session = XR_NULL_HANDLE;
            g_running = false;
            g_frameBegun = false;
            g_state = XR_SESSION_STATE_UNKNOWN;
        }

        // a frame call's error that means the session is gone or not running (the events say so too, maybe later)
        void lostOrStopped(XrResult r)
        {
            if (r == XR_ERROR_INSTANCE_LOST)
            {
                vr::log("the OpenXR instance is lost (the runtime quit?): retrying later\n");
                g_running = false; // nothing more to end
                vr::stop();
            }
            else if (r == XR_ERROR_SESSION_LOST)
            {
                vr::log("the session is lost: retrying\n");
                destroySession();
                g_system = XR_NULL_SYSTEM_ID;
            }
            else if (r == XR_ERROR_SESSION_NOT_RUNNING)
            {
                g_running = false;
                g_frameBegun = false;
            }
        }
    }

    bool probe(std::string &info)
    {
        if (g_instance)
        {
            info = "OpenXR instance up: runtime " + g_runtime;
            return true;
        }
        if (!loadLoader(info))
            return false;
        uint32_t count = 0;
        const XrResult r = p_xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
        if (XR_FAILED(r))
        {
            info = r == XR_ERROR_RUNTIME_UNAVAILABLE || r == XR_ERROR_RUNTIME_FAILURE
                       ? "OpenXR loader found, but no OpenXR runtime (none installed or none active)"
                       : std::string("OpenXR loader found; the runtime's extensions: XrResult ") + std::to_string(r);
            return false;
        }
        std::vector<XrExtensionProperties> ext(count, {XR_TYPE_EXTENSION_PROPERTIES});
        p_xrEnumerateInstanceExtensionProperties(nullptr, count, &count, ext.data());
        bool d3d11 = false;
        for (const XrExtensionProperties &e : ext)
            d3d11 = d3d11 || std::strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0;
        info = std::string("OpenXR loader and runtime found (") + std::to_string(count) + " extensions, D3D11 " +
               (d3d11 ? "supported)" : "NOT supported)");
        return d3d11;
    }

    bool start(ID3D11Device *device, std::string &why)
    {
        if (g_session)
            return true;
        if (!device)
        {
            why = "no D3D11 device";
            return false;
        }
        g_device = device;
        g_eyeFovValid = false;
        g_viewsLogged = false;
        if (!g_instance)
        {
            if (!loadLoader(why) || !createInstance(why))
            {
                stop();
                return false;
            }
        }
        if (g_system == XR_NULL_SYSTEM_ID)
        {
            switch (getSystem(why))
            {
            case SystemResult::Later: g_pending = true; return false;
            case SystemResult::Failed: stop(); return false;
            case SystemResult::Ok: break;
            }
        }
        g_pending = false;
        g_retryable = false;
        if (!createSession(why))
        {
            destroySession();
            if (g_retryable) // the runtime restarted under us: a new instance on the next try
            {
                stop();
                g_pending = true;
            }
            return false;
        }
        g_wantRecenter = true;
        return true;
    }

    void stop()
    {
        if (g_instance)
        {
            // a session in STOPPING is ended; otherwise destroyed as it is (a frame in flight is dropped; switching VR
            // off, a lost instance)
            if (g_session && g_running && g_state == XR_SESSION_STATE_STOPPING)
                p_xrEndSession(g_session);
            g_frameBegun = false;
            destroySession();
            destroyInput();
            p_xrDestroyInstance(g_instance);
        }
        g_inputMade = false;
        g_actionSet = XR_NULL_HANDLE;
        g_instance = XR_NULL_HANDLE;
        g_system = XR_NULL_SYSTEM_ID;
        g_pending = false;
        g_running = false;
        g_frameBegun = false;
    }

    bool started() { return g_session != XR_NULL_HANDLE; }
    bool pending() { return g_pending; }
    std::string runtimeName() { return g_runtime; }

    void poll()
    {
        if (!g_instance)
            return;
        XrEventDataBuffer e = {XR_TYPE_EVENT_DATA_BUFFER};
        for (;;)
        {
            const XrResult r = p_xrPollEvent(g_instance, &e);
            if (r == XR_EVENT_UNAVAILABLE)
                break;
            if (r != XR_SUCCESS)
            {
                check(r, "xrPollEvent");
                break;
            }
            if (e.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
            {
                const auto &ev = reinterpret_cast<const XrEventDataSessionStateChanged &>(e);
                vr::log("session state %s -> %s\n", stateName(g_state), stateName(ev.state));
                g_state = ev.state;
                if (ev.state == XR_SESSION_STATE_READY && g_session && !g_running) // (once: a late READY is not a second begin)
                {
                    XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    g_running = check(p_xrBeginSession(g_session, &bi), "xrBeginSession");
                    g_frameBegun = false;
                    if (g_running)
                        vr::log("running: frames go to the headset\n");
                }
                else if (ev.state == XR_SESSION_STATE_STOPPING && g_session)
                {
                    // the headset asleep / taken off, the runtime's dashboard, another VR app: frames stop until READY
                    check(p_xrEndSession(g_session), "xrEndSession");
                    g_running = false;
                    g_frameBegun = false;
                    vr::log("stopped by the runtime (headset asleep or taken off?): the game shows on the monitor only "
                            "until the headset is back\n");
                }
                else if (ev.state == XR_SESSION_STATE_EXITING)
                {
                    vr::log("the runtime ended VR for this game (exiting): switch VR off and on to reconnect\n");
                    destroySession();
                    g_system = XR_NULL_SYSTEM_ID;
                    g_exited = true;
                    break;
                }
                else if (ev.state == XR_SESSION_STATE_LOSS_PENDING)
                {
                    vr::log("the session was lost (the headset disconnected or the runtime restarted): retrying\n");
                    destroySession();
                    g_system = XR_NULL_SYSTEM_ID; // find the headset again on the next start
                    break;
                }
            }
            else if (e.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
            {
                vr::log("the OpenXR runtime is going away (instance loss pending): retrying later\n");
                stop();
                return;
            }
            else if (e.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING)
            {
                vr::log("the runtime moved the reference space (its own recentre): recentring\n");
                g_wantRecenter = true;
            }
            else if (e.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED)
                updateProfiles();
            else if (e.type == XR_TYPE_EVENT_DATA_EVENTS_LOST)
                vr::log("warning: %u OpenXR events lost\n", reinterpret_cast<const XrEventDataEventsLost &>(e).lostEventCount);
            e = {XR_TYPE_EVENT_DATA_BUFFER};
        }
    }

    bool exited() { return g_exited; }
    void clearExited() { g_exited = false; }
    const char *sessionStateName() { return g_session ? stateName(g_state) : "none"; }
    float refreshHz() { return g_period > 0 ? static_cast<float>(1e9 / static_cast<double>(g_period)) : 0.0f; }

    bool running() { return g_running && g_session; }
    bool focused() { return g_state == XR_SESSION_STATE_FOCUSED; }

    bool waitAndBegin()
    {
        if (!running() || g_frameBegun)
            return g_frameBegun;
        XrFrameWaitInfo wi = {XR_TYPE_FRAME_WAIT_INFO};
        g_frame = {XR_TYPE_FRAME_STATE};
        const XrResult wr = p_xrWaitFrame(g_session, &wi, &g_frame);
        if (!check(wr, "xrWaitFrame"))
        {
            lostOrStopped(wr);
            return false;
        }
        // the display's own period: the shortest seen (SteamVR reports twice it while it reprojects - pacing the game
        // down to that would keep it reprojecting)
        if (g_frame.predictedDisplayPeriod > 0 &&
            (g_period == 0 || g_frame.predictedDisplayPeriod < g_period - g_period / 50))
        {
            g_period = g_frame.predictedDisplayPeriod;
            vr::log("headset refresh: %.1f Hz (display period %.2f ms)\n", 1e9 / static_cast<double>(g_period), g_period / 1e6);
        }
        XrFrameBeginInfo bi = {XR_TYPE_FRAME_BEGIN_INFO};
        const XrResult r = p_xrBeginFrame(g_session, &bi);
        g_frameBegun = r == XR_SUCCESS || r == XR_FRAME_DISCARDED;
        if (!g_frameBegun)
        {
            check(r, "xrBeginFrame");
            lostOrStopped(r);
        }
        if (g_frameBegun && g_wantRecenter)
        {
            g_wantRecenter = false;
            recenter();
        }
        return g_frameBegun;
    }

    bool eyeView(uint32_t &recommendedWidth, Fov &fov)
    {
        if (!g_session || !g_frame.predictedDisplayTime || !g_recWidth)
            return false;
        if (!g_eyeFovValid)
        {
            XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO};
            li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            li.displayTime = g_frame.predictedDisplayTime;
            li.space = g_viewSpace;
            XrViewState vs = {XR_TYPE_VIEW_STATE};
            XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
            uint32_t n = 0;
            if (XR_SUCCEEDED(p_xrLocateViews(g_session, &li, &vs, 2, &n, views)) && n >= 1 &&
                views[0].fov.angleRight > views[0].fov.angleLeft)
            {
                g_eyeFov = {views[0].fov.angleLeft, views[0].fov.angleRight, views[0].fov.angleUp, views[0].fov.angleDown};
                g_eyeFovValid = true;
                vr::log("eye field of view: L%.1f R%.1f U%.1f D%.1f deg\n", g_eyeFov.left * 57.2958f, g_eyeFov.right * 57.2958f,
                        g_eyeFov.up * 57.2958f, g_eyeFov.down * 57.2958f);
            }
        }
        recommendedWidth = g_recWidth;
        fov = g_eyeFov;
        return g_eyeFovValid;
    }

    bool frameBegun() { return g_frameBegun; }
    bool shouldRender() { return g_frameBegun && g_frame.shouldRender; }
    int64_t predictedDisplayTime() { return g_frame.predictedDisplayTime; }

    bool locateHead(Pose &out)
    {
        if (!g_session || !g_frame.predictedDisplayTime)
            return false;
        XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(p_xrLocateSpace(g_viewSpace, g_space, g_frame.predictedDisplayTime, &loc)) ||
            !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
            return false;
        out.q[0] = loc.pose.orientation.x, out.q[1] = loc.pose.orientation.y, out.q[2] = loc.pose.orientation.z,
        out.q[3] = loc.pose.orientation.w;
        const bool pos = (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
        out.p[0] = pos ? loc.pose.position.x : 0.0f, out.p[1] = pos ? loc.pose.position.y : 0.0f, out.p[2] = pos ? loc.pose.position.z : 0.0f;
        return true;
    }

    bool recommendedSize(uint32_t &width, uint32_t &height)
    {
        if (!g_recWidth || !g_recHeight)
            return false;
        width = g_recWidth;
        height = g_recHeight;
        return true;
    }

    bool locateViews(View out[2])
    {
        if (!g_session || !g_frame.predictedDisplayTime || !g_space)
            return false;
        XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO};
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime = g_frame.predictedDisplayTime;
        li.space = g_space;
        XrViewState vs = {XR_TYPE_VIEW_STATE};
        XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t n = 0;
        const XrResult r = p_xrLocateViews(g_session, &li, &vs, 2, &n, views);
        if (!check(r, "xrLocateViews") || n < 2 || !(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
            return false;
        for (int i = 0; i < 2; ++i)
        {
            const XrPosef &p = views[i].pose;
            out[i].pose.q[0] = p.orientation.x, out[i].pose.q[1] = p.orientation.y, out[i].pose.q[2] = p.orientation.z,
            out[i].pose.q[3] = p.orientation.w;
            out[i].pose.p[0] = p.position.x, out[i].pose.p[1] = p.position.y, out[i].pose.p[2] = p.position.z;
            out[i].fov = {views[i].fov.angleLeft, views[i].fov.angleRight, views[i].fov.angleUp, views[i].fov.angleDown};
        }
        if (!g_viewsLogged)
        {
            g_viewsLogged = true;
            const float dx = out[1].pose.p[0] - out[0].pose.p[0], dy = out[1].pose.p[1] - out[0].pose.p[1],
                        dz = out[1].pose.p[2] - out[0].pose.p[2];
            vr::log("stereo views: IPD %.1f mm (position %s); left fov L%.1f R%.1f U%.1f D%.1f, right fov L%.1f R%.1f U%.1f "
                    "D%.1f deg\n",
                    std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0f,
                    (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) ? "valid" : "not valid", out[0].fov.left * 57.2958f,
                    out[0].fov.right * 57.2958f, out[0].fov.up * 57.2958f, out[0].fov.down * 57.2958f, out[1].fov.left * 57.2958f,
                    out[1].fov.right * 57.2958f, out[1].fov.up * 57.2958f, out[1].fov.down * 57.2958f);
        }
        return true;
    }

    void recenter()
    {
        if (!g_session || !g_frame.predictedDisplayTime)
        {
            g_wantRecenter = true;
            return;
        }
        XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(p_xrLocateSpace(g_viewSpace, g_localSpace, g_frame.predictedDisplayTime, &loc)) ||
            !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
        {
            g_wantRecenter = true;
            return;
        }
        // yaw only: the head's ahead (-z) flattened
        const XrQuaternionf &q = loc.pose.orientation;
        const float fx = -(2.0f * (q.x * q.z + q.w * q.y)), fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
        const float yaw = std::atan2(-fx, -fz); // about +y, left +
        XrReferenceSpaceCreateInfo rs = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        rs.poseInReferenceSpace.orientation = {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
        if (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
            rs.poseInReferenceSpace.position = loc.pose.position;
        XrSpace space = XR_NULL_HANDLE;
        if (XR_SUCCEEDED(p_xrCreateReferenceSpace(g_session, &rs, &space)))
        {
            if (g_space)
                p_xrDestroySpace(g_space);
            g_space = space;
            vr::log("recentred (yaw %.0f deg)\n", yaw * 57.29578f);
        }
    }

    ID3D11Texture2D *panelImage(Panel panel, uint32_t width, uint32_t height, bool &srgb)
    {
        Swapchain &s = g_chains[static_cast<int>(panel)];
        if (!g_session || !g_frameBegun || !width || !height)
            return nullptr;
        if (s.acquired)
        {
            srgb = s.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || s.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            return s.images[s.index].texture;
        }
        if (s.handle && (s.width != width || s.height != height))
            destroySwapchain(s);
        if (!s.handle)
        {
            XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
            ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            ci.format = g_formats[0];
            ci.sampleCount = 1;
            ci.width = width;
            ci.height = height;
            ci.faceCount = 1;
            ci.arraySize = 1;
            ci.mipCount = 1;
            if (!check(p_xrCreateSwapchain(g_session, &ci, &s.handle), "xrCreateSwapchain"))
            {
                s = Swapchain();
                return nullptr;
            }
            s.width = width;
            s.height = height;
            s.format = g_formats[0];
            uint32_t n = 0;
            p_xrEnumerateSwapchainImages(s.handle, 0, &n, nullptr);
            s.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
            if (!n || XR_FAILED(p_xrEnumerateSwapchainImages(s.handle, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader *>(s.images.data()))))
            {
                destroySwapchain(s);
                return nullptr;
            }
            vr::log("swapchain %d: %ux%u, %u images\n", static_cast<int>(panel), width, height, n);
        }
        XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (!check(p_xrAcquireSwapchainImage(s.handle, &ai, &s.index), "xrAcquireSwapchainImage"))
            return nullptr;
        XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = 100000000; // 100 ms
        if (!check(p_xrWaitSwapchainImage(s.handle, &wi), "xrWaitSwapchainImage"))
        {
            XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            p_xrReleaseSwapchainImage(s.handle, &ri);
            return nullptr;
        }
        s.acquired = true;
        srgb = s.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || s.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        return s.images[s.index].texture;
    }

    int64_t panelFormat(Panel panel)
    {
        const Swapchain &s = g_chains[static_cast<int>(panel)];
        return s.handle ? s.format : (g_formatCount ? g_formats[0] : 0);
    }

    void endFrame(const ProjectionLayer &projection, const QuadLayer *quads, int quadCount)
    {
        if (!g_frameBegun || !g_session)
            return;
        bool used[static_cast<int>(Panel::Count)] = {};
        for (Swapchain &s : g_chains)
            if (s.acquired)
            {
                XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                p_xrReleaseSwapchainImage(s.handle, &ri);
                s.acquired = false;
                used[&s - g_chains] = true;
            }
        std::vector<const XrCompositionLayerBaseHeader *> layers;
        std::vector<XrCompositionLayerProjectionView> views;
        XrCompositionLayerProjection proj = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        std::vector<XrCompositionLayerQuad> quadLayers;
        if (g_frame.shouldRender)
        {
            const bool stereo = projection.stereo && g_viewCount >= 2;
            const bool haveImages = used[static_cast<int>(Panel::Scene)] && (!stereo || used[static_cast<int>(Panel::SceneRight)]);
            if (projection.on && projection.stereo && !stereo)
                vr::log("warning: a stereo projection layer, but the runtime has %u view(s)\n", g_viewCount);
            if (projection.on && !haveImages && g_submitted < 3)
                vr::log("warning: the projection layer is on but its eye image(s) were not drawn this frame: left out\n");
            if (projection.on && haveImages)
            {
                views.assign(g_viewCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
                for (uint32_t i = 0; i < g_viewCount; ++i)
                {
                    XrCompositionLayerProjectionView &v = views[i];
                    // mono: the same image and the same (head) pose for each eye; stereo: each eye its own
                    const int eye = stereo ? static_cast<int>(std::min(i, 1u)) : 0;
                    const Pose &pose = stereo ? projection.views[eye].pose : projection.pose;
                    const Fov &fov = stereo ? projection.views[eye].fov : projection.fov;
                    const Swapchain &sc = g_chains[static_cast<int>(stereo ? eyePanel(eye) : Panel::Scene)];
                    v.pose.orientation = {pose.q[0], pose.q[1], pose.q[2], pose.q[3]};
                    v.pose.position = {pose.p[0], pose.p[1], pose.p[2]};
                    v.fov = {fov.left, fov.right, fov.up, fov.down};
                    v.subImage.swapchain = sc.handle;
                    v.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(sc.width), static_cast<int32_t>(sc.height)}};
                    v.subImage.imageArrayIndex = 0;
                }
                proj.space = g_space;
                proj.viewCount = static_cast<uint32_t>(views.size());
                proj.views = views.data();
                layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&proj));
            }
            quadLayers.reserve(static_cast<size_t>(std::max(quadCount, 0)));
            for (int i = 0; i < quadCount; ++i)
            {
                const QuadLayer &q = quads[i];
                const Swapchain &s = g_chains[static_cast<int>(q.panel)];
                if (!q.on || !used[static_cast<int>(q.panel)])
                    continue;
                XrCompositionLayerQuad l = {XR_TYPE_COMPOSITION_LAYER_QUAD};
                l.layerFlags = q.alpha ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
                l.space = g_space;
                l.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                l.subImage.swapchain = s.handle;
                l.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(s.width), static_cast<int32_t>(s.height)}};
                l.pose.orientation = {q.pose.q[0], q.pose.q[1], q.pose.q[2], q.pose.q[3]};
                l.pose.position = {q.pose.p[0], q.pose.p[1], q.pose.p[2]};
                l.size = {q.width, q.height};
                quadLayers.push_back(l);
                layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&quadLayers.back()));
            }
        }
        XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
        ei.displayTime = g_frame.predictedDisplayTime;
        ei.environmentBlendMode = g_blend;
        ei.layerCount = static_cast<uint32_t>(layers.size());
        ei.layers = layers.data();
        g_frameBegun = false;
        const XrResult r = p_xrEndFrame(g_session, &ei);
        if (check(r, "xrEndFrame"))
            ++g_submitted;
        else
            lostOrStopped(r);
    }

    bool syncInput(Hand out[2])
    {
        out[0] = Hand();
        out[1] = Hand();
        if (!g_inputAttached || !running() || !focused() || !g_frame.predictedDisplayTime)
            return false;
        XrActiveActionSet active = {g_actionSet, XR_NULL_PATH};
        XrActionsSyncInfo si = {XR_TYPE_ACTIONS_SYNC_INFO};
        si.countActiveActionSets = 1;
        si.activeActionSets = &active;
        const XrResult r = p_xrSyncActions(g_session, &si);
        if (r == XR_SESSION_NOT_FOCUSED)
            return false;
        if (!check(r, "xrSyncActions"))
        {
            lostOrStopped(r);
            return false;
        }
        bool any = false;
        for (int h = 0; h < 2; ++h)
        {
            Hand &o = out[h];
            XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
            gi.subactionPath = g_handPath[h];
            gi.action = g_actions[ActGrip];
            XrActionStatePose pose = {XR_TYPE_ACTION_STATE_POSE};
            if (XR_SUCCEEDED(p_xrGetActionStatePose(g_session, &gi, &pose)) && pose.isActive)
            {
                o.active = locatePose(g_gripSpace[h], o.grip);
                if (!locatePose(g_aimSpace[h], o.aim))
                    o.aim = o.grip;
            }
            auto value = [&](Act a) {
                gi.action = g_actions[a];
                XrActionStateFloat s = {XR_TYPE_ACTION_STATE_FLOAT};
                return XR_SUCCEEDED(p_xrGetActionStateFloat(g_session, &gi, &s)) && s.isActive ? s.currentState : 0.0f;
            };
            auto button = [&](Act a) {
                gi.action = g_actions[a];
                XrActionStateBoolean s = {XR_TYPE_ACTION_STATE_BOOLEAN};
                return XR_SUCCEEDED(p_xrGetActionStateBoolean(g_session, &gi, &s)) && s.isActive && s.currentState;
            };
            o.trigger = value(ActTrigger);
            o.squeeze = value(ActSqueeze);
            gi.action = g_actions[ActStick];
            XrActionStateVector2f stick = {XR_TYPE_ACTION_STATE_VECTOR2F};
            if (XR_SUCCEEDED(p_xrGetActionStateVector2f(g_session, &gi, &stick)) && stick.isActive)
                o.stick[0] = stick.currentState.x, o.stick[1] = stick.currentState.y;
            o.stickClick = button(ActStickClick);
            o.primary = button(ActPrimary);
            o.secondary = button(ActSecondary);
            o.menu = button(ActMenu);
            any = any || o.active;
        }
        return any;
    }

    void haptic(int hand, float amplitude, float seconds)
    {
        if (!g_inputAttached || !running() || !focused() || hand < 0 || hand > 1)
            return;
        XrHapticActionInfo hi = {XR_TYPE_HAPTIC_ACTION_INFO};
        hi.action = g_actions[ActHaptic];
        hi.subactionPath = g_handPath[hand];
        if (amplitude <= 0.0f)
        {
            check(p_xrStopHapticFeedback(g_session, &hi), "xrStopHapticFeedback");
            return;
        }
        XrHapticVibration v = {XR_TYPE_HAPTIC_VIBRATION};
        v.amplitude = std::min(amplitude, 1.0f);
        v.duration = seconds > 0.0f ? static_cast<XrDuration>(static_cast<double>(seconds) * 1e9) : XR_MIN_HAPTIC_DURATION;
        v.frequency = XR_FREQUENCY_UNSPECIFIED;
        const XrResult r = p_xrApplyHapticFeedback(g_session, &hi, reinterpret_cast<const XrHapticBaseHeader *>(&v));
        if (r != XR_SESSION_NOT_FOCUSED)
            check(r, "xrApplyHapticFeedback");
    }

    std::string interactionProfile(int hand) { return hand == 0 || hand == 1 ? g_profile[hand] : std::string(); }

    uint64_t framesSubmitted() { return g_submitted; }

    std::string status()
    {
        auto shortProfile = [](const std::string &p) {
            const size_t slash = p.find("/interaction_profiles/");
            return p.empty() ? std::string("none") : slash == 0 ? p.substr(std::strlen("/interaction_profiles/")) : p;
        };
        char b[512];
        std::snprintf(b, sizeof(b),
                      "runtime=\"%s\" instance=%d session=%d state=%s running=%d hz=%.0f frames=%llu errors=%llu input=%s "
                      "left=%s right=%s",
                      g_runtime.c_str(), g_instance ? 1 : 0, g_session ? 1 : 0, sessionStateName(), running() ? 1 : 0,
                      refreshHz(), static_cast<unsigned long long>(g_submitted), static_cast<unsigned long long>(g_errors),
                      g_inputAttached ? "attached" : "none", shortProfile(g_profile[0]).c_str(), shortProfile(g_profile[1]).c_str());
        return b;
    }
}
