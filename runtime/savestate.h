// Save states: everything the running game consists of, written to / read from one file.
#pragma once
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include <deque>
#include "psx.h"

struct StateIO {
    FILE* f = nullptr;
    bool load = false;
    bool ok = true;
    void raw(void* p, size_t n) {
        if (!n) return;
        if (load) ok = ok && fread(p, 1, n, f) == n;
        else ok = ok && fwrite(p, 1, n, f) == n;
    }
    template <class T> void pod(T& v) { raw(&v, sizeof v); }
    template <class T> void vec(std::vector<T>& v) {
        u32 n = (u32)v.size();
        pod(n);
        if (load) { if (!ok || n > (64u << 20)) { ok = false; return; } v.resize(n); }
        raw(v.data(), n * sizeof(T));
    }
    template <class T> void deq(std::deque<T>& d) {
        std::vector<T> v(d.begin(), d.end());
        vec(v);
        if (load) d.assign(v.begin(), v.end());
    }
    void str(std::string& s) {
        u32 n = (u32)s.size();
        pod(n);
        if (load) { if (!ok || n > 4096) { ok = false; return; } s.resize(n); }
        raw(&s[0], n);
    }
    // section marker: catches a module reading a different amount than was written
    void tag(const char* name) {
        u32 t = 0;
        memcpy(&t, name, strlen(name) < 4 ? strlen(name) : 4);
        u32 v = t;
        pod(v);
        if (load && v != t) { if (ok) logf("save state: section %s out of step\n", name); ok = false; }
    }
};

// per-module state (each in its own file)
void core_state(StateIO& s);
void bios_state(StateIO& s);
void cdrom_state(StateIO& s);
void hw_state(StateIO& s);
void spu_state(StateIO& s);
void gpu_state(StateIO& s);
void memcard_state(StateIO& s);
void fibres_state(StateIO& s);
void hdtex_state(StateIO& s);

// runs the game (guest main) on the main coroutine stack and serves save/load requests in between
void state_run_game(void (*entry)(void*));
// called by the game at the start of each frame wait: a pending save/load is carried out there
void state_point();
void state_request_save(const char* path);
void state_request_load(const char* path);
const char* state_quick_path();
