// In-game settings menu (Dear ImGui), live setting changes, classic/remastered switch.
#pragma once
#include "psx.h"

struct ID3D11Device;
struct ID3D11DeviceContext;

extern const char* const g_btn_label[16];   // PSX button names, bit order of the pad word
extern const char* const g_btn_key[16];     // ini key suffixes

void ui_init(void* hwnd, ID3D11Device* dev, ID3D11DeviceContext* ctx);
void ui_render();                 // call with the back buffer bound, right before Present
bool ui_menu_open();
void ui_toggle_menu();
void ui_notify(const char* text);
bool ui_wndproc(void* hwnd, unsigned msg, unsigned long long wp, long long lp);   // true = consumed

void settings_defaults();         // default key bindings
void settings_apply();            // push g_cfg into the running subsystems
void settings_toggle_classic();   // one-key original <-> remastered presentation
bool settings_is_classic();
void settings_save(const char* path);

// implemented by the subsystems
void hw_set_scale(int scale);
void core_set_bias(double bias);
const char* plat_key_name(int vk);
int plat_scan_key();              // first key/mouse button currently down, 0 if none
void plat_toggle_fullscreen();
