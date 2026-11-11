/* Example C-12 mod: counts game frames by hooking the main-loop frame limiter (VSync @ 800a0fb0)
 * and logs once in a while. Build (from a VS x64 prompt):
 *     cl /LD /O2 /GS- /I ..\runtime example_mod.c /Fe:..\mods\example_mod.dll
 * (/GS-: see c12_mod_api.h - save states)
 */
#include <stdio.h>
#include "c12_mod_api.h"

static const C12ModApi* api;
static unsigned calls;

static void vsync_hook(uint32_t* r, C12OrigFn orig) {
    if (r[4] != 0xFFFFFFFFu && (++calls % 500) == 0) {
        char msg[96];
        snprintf(msg, sizeof msg, "example_mod: %u VSync waits so far", calls);
        api->log(msg);
    }
    orig(r);
}

__declspec(dllexport) int c12_mod_init(const C12ModApi* a) {
    if (a->version < 1) return 0;
    api = a;
    return api->hook(0x800a0fb0, vsync_hook);
}
