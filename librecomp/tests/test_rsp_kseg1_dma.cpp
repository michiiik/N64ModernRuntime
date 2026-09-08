#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "librecomp/rsp.hpp"

// Standalone test: compile as C++20 with N64Recomp/include, librecomp/include,
// and ultramodern/include on the include path. No runtime library is needed.
// Requires a 64-bit host: the CPU memory contract places KSEG1 at offset 512 MiB.

uint8_t dmem[0x1000] = {};
uint16_t rspReciprocals[512] = {};
uint16_t rspInverseSquareRoots[512] = {};

namespace recomp::rsp {
void trace_dma(bool, uint32_t, uint32_t, uint32_t) {}
}

static void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "RSP KSEG1 DMA regression: %s\n", message);
        std::exit(1);
    }
}

int main() {
    // KSEG0/physical addresses retain the low 24-bit RDRAM offset.
    expect(rsp_dram_offset(0x80001000u) == 0x00001000u,
           "KSEG0 address changed physical mapping");
    expect(rsp_dram_offset(0x00123456u) == 0x00123456u,
           "physical address changed mapping");

    // Uncached KSEG1 addresses must resolve through the CPU's mapped window:
    // 0xA0000000 + physical 0x1000 -> mapped RDRAM offset 0x20001000.
    const uint32_t kseg1 = rsp_dram_offset(0xA0001000u);
    expect(kseg1 == 0x20001000u,
           "KSEG1 address did not map to the CPU-visible 0x20000000 window");
    expect(kseg1 != rsp_dram_offset(0x00001000u),
           "KSEG1 source aliased the physical source offset");

    // The DMA helper masks the returned offset only for alignment, preserving
    // this distinction before it reads rdram + cur_dram.
    expect((kseg1 & ~7u) == 0x20001000u,
           "KSEG1 DMA source was not 8-byte aligned as expected");

    // Use distinct bytes in the physical and uncached windows. Reserve the
    // address range but initialize only the small regions exercised here.
    auto* rdram = static_cast<uint8_t*>(std::malloc(0x20002000u));
    expect(rdram != nullptr, "cannot allocate test address range");
    std::memset(rdram + 0x1000, 0x11, 32);
    std::memset(rdram + 0x20001000, 0x22, 32);
    std::memset(dmem, 0, sizeof(dmem));
    dma_rdram_to_dmem(rdram, 0x80, 0xA0001000u, 15);
    for (unsigned i = 0; i < 16; ++i)
        expect(dmem[0x80 + i] == 0x22, "KSEG1 DMA read selected physical window");
    expect(dmem[0x90] == 0, "DMA read exceeded requested block");
    std::memset(dmem + 0x80, 0x33, 16);
    dma_dmem_to_rdram(rdram, 0x80, 0xA0001000u, 15);
    for (unsigned i = 0; i < 16; ++i) {
        expect(rdram[0x20001000 + i] == 0x33, "KSEG1 DMA write missed uncached window");
        expect(rdram[0x1000 + i] == 0x11, "KSEG1 DMA write corrupted physical window");
    }
    expect(rdram[0x20001010] == 0x22, "DMA write exceeded requested block");
    dma_rdram_to_dmem(rdram, 0x80, 0x80001000u, 15);
    for (unsigned i = 0; i < 16; ++i)
        expect(dmem[0x80 + i] == 0x11, "KSEG0 DMA no longer selects physical window");
    std::free(rdram);
    std::puts("RSP KSEG1 DMA regression: PASS");
    return 0;
}
