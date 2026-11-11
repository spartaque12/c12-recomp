// Coroutine stacks at fixed addresses, see costack.h.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string.h>
#include "costack.h"
#include "psx.h"

extern "C" void co_swap(void** save_rsp, void* new_rsp);
extern "C" void co_start();

// slot 0 is the game's main thread (as big as the original thread stack), the others hold fibres
static const size_t MAIN_SIZE = 64u << 20;
static const size_t SLOT_SIZE = 1u << 20;
static uint8_t* const FIXED_BASE = (uint8_t*)0x0000200000000000ull;
static uint8_t* g_base;
static bool g_fixed;
static bool g_committed[CO_SLOTS];
static bool g_used[CO_SLOTS];
static CoStack g_host;
static void* g_host_dealloc;   // TEB.DeallocationStack of the real thread stack

uint8_t* co_slot_lo(int slot) { return slot == 0 ? g_base : g_base + MAIN_SIZE + (size_t)(slot - 1) * SLOT_SIZE; }
uint8_t* co_slot_hi(int slot) { return co_slot_lo(slot) + (slot == 0 ? MAIN_SIZE : SLOT_SIZE); }

bool co_init() {
    size_t total = MAIN_SIZE + (size_t)(CO_SLOTS - 1) * SLOT_SIZE;
    g_base = (uint8_t*)VirtualAlloc(FIXED_BASE, total, MEM_RESERVE, PAGE_READWRITE);
    g_fixed = g_base != nullptr;
    if (!g_base) g_base = (uint8_t*)VirtualAlloc(nullptr, total, MEM_RESERVE, PAGE_READWRITE);
    if (!g_base) return false;
    if (!g_fixed) logf("coroutine stacks: fixed address unavailable, save states only within this run\n");
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    g_host.slot = -1;
    g_host.lo = (uint8_t*)tib->StackLimit;
    g_host.hi = (uint8_t*)tib->StackBase;
    g_host_dealloc = *(void**)((uint8_t*)tib + 0x1478);
    return true;
}
CoStack* co_host() { return &g_host; }
bool co_fixed() { return g_fixed; }

static void commit(int slot) {
    if (g_committed[slot]) return;
    if (!VirtualAlloc(co_slot_lo(slot), co_slot_hi(slot) - co_slot_lo(slot), MEM_COMMIT, PAGE_READWRITE)) psx_fatal("stack commit failed (slot %d)", slot);
    g_committed[slot] = true;
}

int co_alloc_slot() {
    for (int i = 1; i < CO_SLOTS; i++) if (!g_used[i]) return i;
    return -1;
}

void co_claim_slot(CoStack* s, int slot) {
    commit(slot);
    g_used[slot] = true;
    s->slot = slot;
    s->lo = co_slot_lo(slot);
    s->hi = co_slot_hi(slot);
}

void co_create(CoStack* s, int slot, void (*fn)(void*), void* arg) {
    if (slot < 0) slot = co_alloc_slot();
    if (slot < 0) psx_fatal("out of coroutine stacks");
    co_claim_slot(s, slot);
    // The first switch to it "returns" into co_start with r12 = fn, r13 = arg (see ctxswitch.asm):
    // R = top - 8 holds the return address, the 8 pushed registers below it, the 168-byte XMM area below
    // those. The words above the co_start frame are zero so stack walks end there.
    uint8_t* top = s->hi;
    memset(top - 512, 0, 512);
    uint64_t* r = (uint64_t*)(top - 8);
    r[0] = (uint64_t)co_start;
    r[-1] = 0;                  // rbp
    r[-2] = 0;                  // rbx
    r[-3] = 0;                  // rdi
    r[-4] = 0;                  // rsi
    r[-5] = (uint64_t)fn;       // r12
    r[-6] = (uint64_t)arg;      // r13
    r[-7] = 0;                  // r14
    r[-8] = 0;                  // r15
    uint8_t* xmm = (uint8_t*)(r - 8) - 168;
    *(uint32_t*)(xmm + 160) = 0x1F80;   // MXCSR default
    *(uint16_t*)(xmm + 164) = 0x027F;   // x87 control word default
    s->rsp = xmm;
}

void co_free(CoStack* s) {
    if (s->slot > 0) g_used[s->slot] = false;
    s->slot = -1;
    s->rsp = nullptr;
}

void co_release_all_but_main() {
    for (int i = 1; i < CO_SLOTS; i++) g_used[i] = false;
}

void co_switch(CoStack* from, CoStack* to) {
    // keep the thread's idea of its stack in step (stack probes, unwinding, longjmp check it)
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    tib->StackBase = to->hi;
    tib->StackLimit = to->lo;
    *(void**)((uint8_t*)tib + 0x1478) = to == &g_host ? g_host_dealloc : (void*)to->lo;   // TEB.DeallocationStack
    co_swap(&from->rsp, to->rsp);
}
