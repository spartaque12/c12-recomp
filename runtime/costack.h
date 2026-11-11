// Coroutine stacks at fixed addresses (for the game's fibres and its main thread).
// Every stack lives in one reserved address range at a fixed place, slot by slot, so a save state can
// copy a stack's bytes and put them back later at the very same addresses (return addresses and frame
// pointers stay valid as long as it is the same c12.exe, which is linked without ASLR).
#pragma once
#include <stdint.h>
#include <stddef.h>

struct CoStack {
    void* rsp = nullptr;   // saved stack pointer while switched out
    int slot = -1;         // -1 = the thread's own stack (host)
    uint8_t* lo = nullptr; // stack bounds
    uint8_t* hi = nullptr;
};

enum { CO_SLOTS = 512, CO_MAIN_SLOT = 0 };

bool co_init();                                   // reserve the range, take over the calling thread as host
CoStack* co_host();
bool co_fixed();                                  // the range is at its fixed address (save states can be reloaded across runs)
void co_create(CoStack* s, int slot, void (*fn)(void*), void* arg);   // fresh stack in a free slot (slot < 0: any)
void co_free(CoStack* s);
void co_switch(CoStack* from, CoStack* to);
int co_alloc_slot();                              // lowest free slot (> CO_MAIN_SLOT), -1 if none
void co_claim_slot(CoStack* s, int slot);         // load: take a specific slot without initialising it
void co_release_all_but_main();
uint8_t* co_slot_hi(int slot);
uint8_t* co_slot_lo(int slot);
