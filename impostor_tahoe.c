// impostor.dylib for macOS 26 (Tahoe) — macOS 26 layout -> Monterey layout.
//
// AMDMTLBronzeDriver (12.5) reads the private structure behind
// -[MTLRenderPipelineDescriptorInternal _descriptorPrivate] in its own, Monterey-era layout.
// Every macOS release reshuffles that structure, so the shim shipped in 12.5-24 translates
// macOS 15 -> Monterey. This one translates macOS 26 -> Monterey.
//
// Measured differences between macOS 15.8 and macOS 26.0 (25A354), by disassembling both
// Metal.framework binaries and reading every accessor of the class:
//   - MTLComputePipelineDescriptorInternal : 29 accessors, no difference at all
//       -> the compute path below is a faithful port of the original shim
//   - MTLRenderPipelineDescriptorInternal  : 85 of 88 resolvable accessors keep their offset;
//     the exception is the 8-byte bitfield block at +0xe0, which macOS 26 re-packs:
//     alphaToCoverage and alphaToOne widen from 1 to 2 bits, everything above shifts up, and
//     logicOperation moves from +0xe0 bits 27-30 to +0xe4 bits 0-3.
//     Consequence of copying those 8 bytes verbatim: bit 2, read by the Monterey driver as
//     isRasterizationEnabled, lands on alphaToOne under macOS 26. It is 0 in normal use, so the
//     driver believes rasterization is off and writes no pixels — a valid pipeline that draws
//     nothing, which is exactly the observed symptom.
//
// Build:
//   clang -O2 -dynamiclib -framework Foundation \
//     -install_name /System/Library/Extensions/AMDMTLBronzeDriver.bundle/Contents/MacOS/impostor.dylib \
//     -o impostor.dylib impostor_tahoe.c
//
// NOTE: must be signed with the project's signing chain. An ad-hoc signed build is rejected by
// Library Validation when WindowServer maps it ("mapping process is a platform binary, but mapped
// file is not"), which takes the Bronze driver down with it.

#include <objc/runtime.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// The dyld shared cache is mapped above this address. A caller above it is Metal itself and gets
// the structure untouched; anything below is the Bronze driver and gets the translated copy.
#define DSC_BASE 0x700000000000ULL

#define RENDER_SIZE  0x190
#define COMPUTE_SIZE 0xa8

extern char __NSDictionary0__struct;

typedef void *(*orig_imp_t)(id, SEL);
static orig_imp_t real_render, real_compute;

static pthread_key_t key_render, key_compute;
static pthread_once_t once_render = PTHREAD_ONCE_INIT, once_compute = PTHREAD_ONCE_INIT;
static void make_render(void) { pthread_key_create(&key_render, free); }
static void make_compute(void) { pthread_key_create(&key_compute, free); }

static void *thread_storage(pthread_key_t *key, pthread_once_t *once, void (*init)(void), size_t size) {
    pthread_once(once, init);
    void *buf = pthread_getspecific(*key);
    if (!buf) { buf = calloc(1, size); pthread_setspecific(*key, buf); }
    return buf;
}

#define RD8(o)   (*(uint8_t  *)(s + (o)))
#define RD16(o)  (*(uint16_t *)(s + (o)))
#define RD32(o)  (*(uint32_t *)(s + (o)))
#define RD64(o)  (*(uint64_t *)(s + (o)))
#define WR8(o,v)  (*(uint8_t  *)(d + (o)) = (uint8_t)(v))
#define WR16(o,v) (*(uint16_t *)(d + (o)) = (uint16_t)(v))
#define WR32(o,v) (*(uint32_t *)(d + (o)) = (uint32_t)(v))
#define WR64(o,v) (*(uint64_t *)(d + (o)) = (uint64_t)(v))

// The 64-bit flag block: macOS 26 bit positions -> Monterey bit positions.
// Monterey bits 5, 9, 33 and 36 (internalPipeline) have no identified source on 26 and stay 0.
static uint64_t repack_flags(uint64_t t) {
    uint64_t m = 0;
    m |= (uint64_t)(((t >> 0) & 3ULL) != 0) << 0;   // isAlphaToCoverageEnabled  (2 bits -> 1)
    m |= (uint64_t)(((t >> 2) & 3ULL) != 0) << 1;   // isAlphaToOneEnabled       (2 bits -> 1)
    m |= ((t >> 4)  & 1ULL)    << 2;                // isRasterizationEnabled
    m |= ((t >> 5)  & 3ULL)    << 3;                // inputPrimitiveTopology
    m |= ((t >> 8)  & 1ULL)    << 6;                // isDepthStencilWriteDisabled
    m |= ((t >> 9)  & 1ULL)    << 7;                // openGLModeEnabled
    m |= ((t >> 10) & 1ULL)    << 8;                // sampleCoverageInvert
    m |= ((t >> 12) & 1ULL)    << 10;               // vertexAmplificationMode
    m |= ((t >> 13) & 1ULL)    << 11;               // isTwoSideEnabled
    m |= ((t >> 14) & 1ULL)    << 12;               // isPointSizeOutputVS
    m |= ((t >> 15) & 1ULL)    << 13;               // isPointCoordLowerLeft
    m |= ((t >> 16) & 1ULL)    << 14;               // isPointSmoothEnabled
    m |= ((t >> 17) & 0xFFULL) << 15;               // clipDistanceEnableMask (8 bits)
    m |= ((t >> 25) & 7ULL)    << 23;               // alphaTestFunction
    m |= ((t >> 28) & 1ULL)    << 26;               // isAlphaTestEnabled
    m |= ((t >> 32) & 0xFULL)  << 27;               // logicOperation
    m |= ((t >> 36) & 1ULL)    << 31;               // isLogicOperationEnabled
    m |= ((t >> 37) & 1ULL)    << 32;               // forceResourceIndex
    m |= ((t >> 39) & 1ULL)    << 34;               // objectThreadgroupSizeIsMultipleOf...
    m |= ((t >> 40) & 1ULL)    << 35;               // meshThreadgroupSizeIsMultipleOf...
    return m;
}

static void *fake_render(id self, SEL sel) {
    uint8_t *s = (uint8_t *)real_render(self, sel);
    if ((uintptr_t)__builtin_return_address(0) >= DSC_BASE) return s;  // internal Metal call
    uint8_t *d = (uint8_t *)thread_storage(&key_render, &once_render, make_render, RENDER_SIZE);

    memcpy(d, s, 0x48);                       // header is identical in both layouts
    WR64(0x48, RD64(0x48)); WR64(0x50, RD64(0x50)); WR64(0x58, RD64(0x58)); WR64(0x60, RD64(0x60));
    WR8 (0x68, RD8 (0x68)); WR64(0x70, RD64(0x70)); WR64(0x78, RD64(0x78)); WR64(0x80, RD64(0x80));
    WR64(0x88, RD64(0x88)); WR64(0x90, RD64(0x90)); WR8 (0x98, RD8 (0x98));
    WR64(0xa0, RD64(0xb0)); WR64(0xa8, RD64(0xb8)); WR64(0xb0, RD64(0xc0)); WR32(0xb8, RD32(0xc8));
    WR64(0xc8, RD64(0xd8));

    WR64(0xd0, repack_flags(RD64(0xe0)));     // <-- the macOS 26 fix (was: verbatim 0xe0/0xe4)

    WR32(0xd8, RD32(0xe8)); WR32(0xdc, RD32(0xec));
    WR64(0xe0, RD64(0xf0)); WR64(0xe8, RD64(0xf8)); WR64(0xf0, RD64(0x100));
    WR64(0xf8, RD64(0x108)); WR64(0x100, RD64(0x110));
    WR64(0x108, RD64(0x198)); WR64(0x110, RD64(0x1a0)); WR64(0x118, RD64(0x1a8));
    WR64(0x120, RD64(0x1b8)); WR64(0x138, RD64(0x1d0));
    WR8 (0x140, RD8 (0x1d8)); WR32(0x144, RD32(0x1dc));
    WR64(0x148, RD64(0x1e0)); WR64(0x150, RD64(0x1e8)); WR64(0x158, RD64(0x1f0));
    WR64(0x160, RD64(0x1f8)); WR64(0x168, RD64(0x210)); WR64(0x170, RD64(0x218));
    WR64(0x178, RD64(0x230)); WR64(0x180, RD64(0x238));
    WR8 (0x188, RD8 (0x240)); WR8 (0x189, RD8 (0x241));
    return d;
}

static void *fake_compute(id self, SEL sel) {
    uint8_t *s = (uint8_t *)real_compute(self, sel);
    if ((uintptr_t)__builtin_return_address(0) >= DSC_BASE) return s;
    uint8_t *d = (uint8_t *)thread_storage(&key_compute, &once_compute, make_compute, COMPUTE_SIZE);

    WR64(0x00, RD64(0x00)); WR64(0x08, RD64(0x08)); WR8(0x10, RD8(0x10)); WR16(0x12, RD16(0x12));
    WR64(0x18, RD64(0x18)); WR64(0x20, RD64(0x20)); WR64(0x28, RD64(0x30)); WR64(0x30, RD64(0x38));
    WR64(0x38, RD64(0x40)); WR64(0x40, RD64(0x48)); WR8 (0x48, RD8 (0x50)); WR64(0x50, RD64(0x68));
    WR8 (0x58, RD8 (0x70)); WR64(0x60, RD64(0x78)); WR64(0x68, RD64(0x80)); WR8 (0x70, RD8 (0x88));
    {   // two single bits: source +0x89 bits 0 and 1 -> destination +0x71 bits 0 and 1
        uint8_t v = *(uint8_t *)(d + 0x71);
        v = (uint8_t)((v & ~1u) | (RD8(0x89) & 1u));
        v = (uint8_t)((v & ~2u) | (((RD8(0x89) >> 1) & 1u) << 1));
        *(uint8_t *)(d + 0x71) = v;
    }
    WR64(0x78, RD64(0x90));
    WR64(0x80, (uint64_t)(uintptr_t)&__NSDictionary0__struct);  // empty dictionary, as in the original
    WR64(0x88, RD64(0x98)); WR64(0x90, RD64(0xa0)); WR8(0x98, RD8(0xa8)); WR64(0xa0, RD64(0xb0));
    return d;
}

static void hook(const char *cls_name, IMP replacement, orig_imp_t *saved) {
    Class cls = objc_getClass(cls_name);
    if (!cls) return;
    Method m = class_getInstanceMethod(cls, sel_registerName("_descriptorPrivate"));
    if (!m) return;
    *saved = (orig_imp_t)method_getImplementation(m);
    method_setImplementation(m, replacement);
}

__attribute__((constructor)) static void impostor_tahoe_init(void) {
    hook("MTLRenderPipelineDescriptorInternal", (IMP)fake_render, &real_render);
    hook("MTLComputePipelineDescriptorInternal", (IMP)fake_compute, &real_compute);
}
