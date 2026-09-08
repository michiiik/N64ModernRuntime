#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "recomp.h"

// From the runtime repository root in a VS developer PowerShell with Clang:
// clang-cl /std:c++20 /EHsc /I N64Recomp/include librecomp/src/mips_interp.cpp librecomp/tests/test_mips_interp_boundary.cpp /Fe:boundary-test.exe
// ./boundary-test.exe

extern "C" bool recomp_interpret_function(uint8_t* rdram, recomp_context* ctx, uint32_t start_pc);

namespace {
constexpr uint32_t kStart = 0x80001000u;
constexpr uint32_t kTarget = 0x80002000u;
constexpr uint32_t kCallerReturn = 0x8000F000u;

uint32_t expected_host_return;
bool native_called;
bool tailcall_drained;
uint32_t drained_host_return;

void fail(const char* message) {
    std::fprintf(stderr, "boundary regression: %s\n", message);
    std::exit(1);
}

void expect(bool condition, const char* message) {
    if (!condition) fail(message);
}

void put_insn(uint8_t* rdram, uint32_t pc, uint32_t insn) {
    *reinterpret_cast<uint32_t*>(rdram + (pc - 0x80000000u)) = insn;
}

uint32_t jal(uint32_t target) { return 0x0C000000u | ((target >> 2) & 0x03FFFFFFu); }
uint32_t jr(unsigned reg) { return (reg << 21) | 0x08u; }
uint32_t j(uint32_t target) { return 0x08000000u | ((target >> 2) & 0x03FFFFFFu); }
uint32_t jalr(unsigned rs) { return (rs << 21) | (31u << 11) | 0x09u; }

extern "C" void native_target(uint8_t*, recomp_context* ctx) {
    native_called = true;
    expect(ctx->host_return_target == expected_host_return,
           "native callee saw the wrong host_return_target");
    if (ctx->tailcall_target == 0xDEADu) {
        fail("test callback entered with stale tailcall state");
    }
    // A native callee may defer a tailcall; the interpreter boundary must drain it.
    ctx->tailcall_target = 0xDEADu;
    ctx->tailcall_func = nullptr;
    ctx->tailcall_pending = 1;
    ctx->r31 = kCallerReturn;

}

extern "C" recomp_func_t* recomp_lookup_function_or_null(int32_t vram) {
    return static_cast<uint32_t>(vram) == kTarget ? native_target : nullptr;
}

extern "C" int recomp_shadow_diff_active(void) { return 0; }
extern "C" void recomp_shadow_diff_note_native_call(void) {}
extern "C" void recomp_capture_interp_target(uint32_t) {}
extern "C" void recomp_handle_tailcalls(uint8_t*, recomp_context* ctx) {
    tailcall_drained = true;
    drained_host_return = ctx->host_return_target;
    ctx->tailcall_pending = 0;
    ctx->tailcall_target = 0;
    ctx->tailcall_func = nullptr;
}
}

void run_case(uint32_t first_insn, uint32_t expected_return) {
    alignas(4) uint8_t rdram[0x4000] = {};
    recomp_context ctx{};
    ctx.r1 = kTarget;
    ctx.r31 = kCallerReturn;
    ctx.host_return_target = 0x81234567u;
    expected_host_return = expected_return;
    native_called = false;
    tailcall_drained = false;
    drained_host_return = 0;
    put_insn(rdram, kStart, first_insn);
    put_insn(rdram, kStart + 4, 0); // delay slot
    put_insn(rdram, kStart + 8, jr(31));
    put_insn(rdram, kStart + 12, 0); // delay slot
    expect(recomp_interpret_function(rdram, &ctx, kStart),
           "interpreter rejected the native-boundary case");
    expect(native_called, "native target was not called");
    expect(tailcall_drained, "pending native tailcall was not drained");
    expect(drained_host_return == expected_return,
           "tailcall drain saw the wrong host_return_target");
    expect(ctx.host_return_target == 0x81234567u,
           "interpreter did not restore the caller host_return_target");
}

int main() {
    run_case(jal(kTarget), kStart + 8); // jal native: callee returns to link PC
    run_case(jalr(1), kStart + 8); // jalr native: register-indirect call
    run_case(j(kTarget), kCallerReturn); // j native: direct tail call

    // jr native: no link is made, so the native callee inherits this frame's
    // guest return target, exactly like a generated tailcall.
    alignas(4) uint8_t rdram[0x4000] = {};
    recomp_context ctx{};
    ctx.r1 = kTarget;
    ctx.r31 = kCallerReturn;
    ctx.host_return_target = 0x87654321u;
    expected_host_return = kCallerReturn;
    native_called = false;
    tailcall_drained = false;
    drained_host_return = 0;
    put_insn(rdram, kStart, jr(1));
    put_insn(rdram, kStart + 4, 0);
    expect(recomp_interpret_function(rdram, &ctx, kStart), "jr native case failed");
    expect(native_called && tailcall_drained, "jr native boundary was not exercised");
    expect(drained_host_return == kCallerReturn, "jr native used the wrong return target");
    expect(ctx.host_return_target == 0x87654321u, "jr native did not restore host return target");

    std::puts("mips interpreter native-boundary regression: PASS");
    return 0;

}
