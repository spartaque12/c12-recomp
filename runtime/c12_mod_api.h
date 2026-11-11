/* C-12 recompiled: native mod API (plain C ABI, x64).
 *
 * Build a DLL exporting
 *     __declspec(dllexport) int c12_mod_init(const C12ModApi* api);
 * and drop it into the mods\ folder next to c12.exe.
 *
 * Every function of the original game is a hookable native function identified by its original
 * PSX address (see gen\main_map.txt for the list, re\main_decomp.c for decompiled reference code).
 * A hook receives the guest register file and the original implementation:
 *
 *     static void my_hook(uint32_t* r, void (*orig)(uint32_t* r)) {
 *         // r[4..7] = a0..a3, r[2] = v0, r[29] = sp, r[31] = ra
 *         orig(r);            // optional: run the original
 *         r[2] = 99;          // e.g. change the return value
 *     }
 *     api->hook(0x80012345, my_hook);
 */
#pragma once
#include <stdint.h>

#define C12_MOD_API_VERSION 2   /* check api->version >= the version your mod was written for */

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*C12OrigFn)(uint32_t* regs);
typedef void (*C12HookFn)(uint32_t* regs, C12OrigFn orig);

typedef struct C12ModApi {
    uint32_t version;
    uint8_t* ram;        /* 2 MB guest RAM; guest address 0x80000000+x lives at ram[x] */
    uint32_t* regs;      /* guest registers r0..r31 */
    void (*log)(const char* msg);
    int (*hook)(uint32_t guest_addr, C12HookFn fn);              /* 1 on success */
    void (*call)(uint32_t guest_addr);                           /* call a game function (set regs[4..7] first) */
    uint32_t (*read32)(uint32_t guest_addr);
    void (*write32)(uint32_t guest_addr, uint32_t value);
    void (*on_frame)(void (*cb)(void));                          /* called once per displayed frame */
    int (*override_file)(const char* disc_path, uint32_t offset, const void* data, uint32_t len);
    /* ---- version 2 ---- */
    /* Show the mod in the settings menu (tab "Mods") with an on/off switch bound to *enabled.
     * Call once from c12_mod_init, before registering settings. */
    void (*mod_info)(const char* name, const char* description, int* enabled);
    /* Settings shown under the mod's name; values live in the mod, are loaded from c12.ini
     * (mod.<name>.<key>=...) when registered and written back by "Save settings". */
    void (*setting_bool)(const char* key, const char* label, int* value);
    void (*setting_float)(const char* key, const char* label, float* value, float min, float max);
    /* Field of view: 1.0 = the game's own, 1.25 = 25% wider. Visibility culling follows. */
    void (*set_fov)(float multiplier);
    /* Change one of the runtime's own options for as long as the mod needs it; returns the previous
     * value (-1 = unknown key) so the mod can put it back. Keys: "cam_autocenter" (0/1),
     * "mouse_direct_cam" (0/1), "interp" (0/1). */
    int (*game_option)(const char* key, int value);
} C12ModApi;

#ifdef __cplusplus
}
#endif
