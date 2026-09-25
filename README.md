# AMD Legacy GCN on macOS 26: the pipeline descriptor bitfield moved

On an **iMac15,1 (Radeon R9 M295X, Tonga / AMD Legacy GCN)** running **macOS 26.0 (25A354)** with the
OCLP `AMD Legacy GCN` payload root-patched in, the whole graphics stack comes up — kernel drivers
attach, `SkyLight` logs *Metal compositor activated*, `system_profiler` reports **Metal 2** and the
built-in 5K panel as online and main — yet **nothing is ever drawn**: a solid-colour screen with a
working (hardware) cursor, no crash, no error in any log.

The cause is a **bitfield re-pack inside the private render pipeline descriptor** between macOS 15
and macOS 26. `impostor.dylib` (shipped in `12.5-24`) translates the macOS 15 layout only, so the
Monterey Bronze driver reads `isRasterizationEnabled` from a bit that now holds `alphaToOne`.
Rasterization is silently disabled and every draw is a no-op.

A fix is proposed below. **It has not been validated at runtime** — see *Validation status*.

---

## 1. Environment

| | |
|---|---|
| Machine | iMac15,1, i7-4790K, AMD Radeon R9 M295X (`0x1002:0x6938`, Tonga, Legacy GCN) |
| Target OS | macOS 26.0 (25A354), Darwin 25.0.0, `xnu-12377.1.9~3/RELEASE_X86_64` |
| Bootloader | OpenCore 1.0.6 built by OCLP 2.5.0, plus the two config changes in §6 |
| Root patch | `AMD Legacy GCN` payload from PatcherSupportPkg 1.9.x, applied manually: `12.5` kexts and bundles, `AMDFramebuffer` `12.5-GCN`, `AMDRadeonX4000` `12.5-23.4`, `AMDMTLBronzeDriver` `12.5-24`, merged with KDK 25A353, `kmutil create --allow-missing-kdk --volume-root … --update-all`, then `bless --create-snapshot` |
| Reference | macOS 15.8 on the same machine, OCLP 2.5.0, 3802 route (stock `12.5` Bronze, Ventura `Metal.framework`) |

Note: on this model OCLP takes the 3802 route under Sequoia, because the Haswell iGPU is present.
The `impostor` shim is therefore only reachable on the 31001 route, which is what was used here on 26.

## 2. What already works on macOS 26

Kernel side is complete:

```
AMD9000ControllerWrangler / AMDSupport / AMD9000Controller published
AMDRadeonX4000_AMDRadeonHWServicesVI published
ATY,Basset + AMDFramebufferVI ×4 published
(AMDSupport) Accelerator successfully registered with controller.
AMDRadeonX4000_AMDTongaGraphicsAccelerator published
[com.apple.GPUWrangler:default] (reg) gpu 0x91a5 flags 0x20 (DG) vid.did=1002.6938
```

User side reports a healthy device:

```
Chipset Model: AMD Radeon R9 M295X      Metal Support: Metal 2
Resolution: Retina 5K (5120 x 2880)     Main Display: Yes    Online: Yes
Framebuffer Depth: 30-Bit Color (ARGB2101010)
```

`IOAccelerator` exposes the full set (`AMDAccelDevice`, `AMDAccelCommandQueue`, `AMDAccelSurface`,
`IOAccelDisplayPipeUserClient2`), `WindowServer` logs *Metal compositor activated*, `loginwindow`
reaches `LoginComplete`. Every CoreDisplay warning seen on 26 (`Setting offline display … main`,
`capabilities with no devices`, `Provided pixel encoding value (1) is not supported`) appears
identically on the working 15.8 reference and is noise.

## 3. Minimal reproducer

`metaltest.m` (included) creates a device, clears a texture red through the fixed-function path,
then compiles a trivial shader pair, builds a pipeline and draws a full-screen triangle in green.

macOS 26.0, patched as above:

```
device        : AMD Radeon R9 M295X
step 1 clear  : R=255 G=0 B=0 A=255   -> OK (red)
step 2 shaders: compiled OK
step 2 pso    : created OK
step 2 draw   : R=0 G=0 B=255 A=255   -> FAIL (clear colour untouched, nothing rasterized)
```

The same binary on macOS 15.8 with the same `12.5` driver draws green. So: clears work, the Metal
compiler works, `newRenderPipelineStateWithDescriptor:` *succeeds*, and draws produce nothing.

## 4. Root cause

`impostor.dylib` swizzles `-[MTLRenderPipelineDescriptorInternal _descriptorPrivate]` and
`-[MTLComputePipelineDescriptorInternal _descriptorPrivate]`. When the return address is below
`0x700000000000` (i.e. the caller is not inside the dyld shared cache, so it is the Bronze driver),
it returns a thread-local 0x190-byte copy rebuilt in the Monterey layout: 0x48 bytes verbatim, then
42 individually moved fields.

Two of those moves are `movl 0xe0 → 0xd0` and `movl 0xe4 → 0xd4`. Those 8 bytes are a **bitfield
block**, and macOS 26 re-packs it — `alphaToCoverage` and `alphaToOne` widen from 1 to 2 bits,
everything above shifts, and `logicOperation` moves into the second dword.

| field | macOS 15.8 bit | macOS 26.0 bit |
|---|---|---|
| `isAlphaToCoverageEnabled` | 0 (1 bit) | 0–1 (2 bits) |
| `isAlphaToOneEnabled` | 1 (assumed) | 2–3 (2 bits) |
| `isRasterizationEnabled` | **2** | **4** |
| `inputPrimitiveTopology` | 3–4 | 5–6 |
| `isDepthStencilWriteDisabled` | 6 | 8 |
| `openGLModeEnabled` | 7 | 9 |
| `sampleCoverageInvert` | 8 | 10 |
| `vertexAmplificationMode` | 10 | 12 |
| `isTwoSideEnabled` | 11 | 13 |
| `isPointSizeOutputVS` | 12 | 14 |
| `isPointCoordLowerLeft` | 13 | 15 |
| `isPointSmoothEnabled` | 14 | 16 |
| `clipDistanceEnableMask` | 15–22 | 17–24 |
| `alphaTestFunction` | 23–25 | 25–27 |
| `isAlphaTestEnabled` | 26 | 28 |
| `logicOperation` | **27–30** | **32–35** |
| `isLogicOperationEnabled` | 31 | 36 |
| `forceResourceIndex` | 32 | 37 |
| `objectThreadgroupSizeIsMultipleOfThreadExecutionWidth` | 34 | 39 |
| `meshThreadgroupSizeIsMultipleOfThreadExecutionWidth` | 35 | 40 |
| `internalPipeline` | 36 | not found on 26 |

With the verbatim copy, the Monterey driver reads bit 2 as `isRasterizationEnabled`, but on 26 that
bit belongs to `alphaToOne` — 0 in normal use. **Rasterization is therefore reported as disabled**,
which matches the reproducer exactly: a valid pipeline that writes no pixels, and a compositor that
presents empty frames over a working scanout.

Everything else lines up: **85 of the 88 resolvable accessors** of
`MTLRenderPipelineDescriptorInternal` keep the same offset across 15.8 and 26.0, the three that move
are the ones inside this block, and `MTLComputePipelineDescriptorInternal` is **unchanged**
(29 of 29 accessors identical) — so the compute half of the shim needs no change at all.

## 5. Proposed fix

Replace the verbatim copy of the two dwords with a bit re-pack. Full source in `impostor_tahoe.c`;
the relevant function is:

```c
static uint64_t repack_flags(uint64_t t) {          // macOS 26 layout -> Monterey layout
    uint64_t m = 0;
    m |= (uint64_t)(((t >> 0) & 3ULL) != 0) << 0;   // isAlphaToCoverageEnabled
    m |= (uint64_t)(((t >> 2) & 3ULL) != 0) << 1;   // isAlphaToOneEnabled
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
    m |= ((t >> 17) & 0xFFULL) << 15;               // clipDistanceEnableMask
    m |= ((t >> 25) & 7ULL)    << 23;               // alphaTestFunction
    m |= ((t >> 28) & 1ULL)    << 26;               // isAlphaTestEnabled
    m |= ((t >> 32) & 0xFULL)  << 27;               // logicOperation
    m |= ((t >> 36) & 1ULL)    << 31;               // isLogicOperationEnabled
    m |= ((t >> 37) & 1ULL)    << 32;               // forceResourceIndex
    m |= ((t >> 39) & 1ULL)    << 34;               // objectThreadgroupSizeIsMultipleOf…
    m |= ((t >> 40) & 1ULL)    << 35;               // meshThreadgroupSizeIsMultipleOf…
    return m;
}
```

Known gaps: Monterey bits 5, 9, 33 and 36 (`internalPipeline`) have no identified source on 26 and
are left at zero; `isAlphaToOneEnabled` on 15.8 is inferred (bit 1) rather than measured, its getter
did not match the parser.

## 6. Validation status

- **Layout mapping** — measured, by disassembling both `Metal.framework` binaries and reading every
  accessor of the class (see §7). High confidence.
- **Conversion logic** — unit-tested against synthetic descriptors; all fields round-trip to the
  expected Monterey positions.
- **Runtime** — **not validated**. A locally built `impostor.dylib` cannot be loaded:

  ```
  AMFI: Library Validation failed: Rejecting
  '/System/Library/Extensions/AMDMTLBronzeDriver.bundle/Contents/MacOS/impostor.dylib'
  (Team ID: none, platform: no) for process 'WindowServer' (Team ID: N/A, platform: yes),
  reason: mapping process is a platform binary, but mapped file is not
  ```

  The shipped shim is signed by *OpenCore Legacy Patcher Software Signing* / *Dortania Root CA*,
  which AMFIPass trusts; an ad-hoc signed rebuild is rejected and takes the Bronze driver down with
  it (`dlopen … Library not loaded`), after which `WindowServer` aborts in
  `CoreDisplay_CreateDisplayForCGXDisplayDevice`. **Someone able to sign with the project's chain
  needs to run the test.** The reproducer in §3 is the fastest check: green instead of blue.

## 7. Method

1. Mounted the target's OS cryptex (`…/cryptex1/current/os.dmg`) and extracted
   `dyld_shared_cache_x86_64h` with `/usr/lib/dsc_extractor.bundle`, on both systems.
2. `otool -tV` on both `Metal` binaries.
3. Parsed every `-[MTLRenderPipelineDescriptorInternal <getter>]`: base
   `_OBJC_IVAR_$_MTLRenderPipelineDescriptorInternal._private`, then the `(offset, shift, mask)`
   triple of the first access, giving an absolute bit position per field.
4. Diffed the two maps; cross-referenced with the field list that `impostor.dylib`'s `_fake_render`
   actually copies (from its own disassembly).

## 8. Two other macOS 26 findings from the same machine

Both were needed just to reach the state described above; they may be useful independently.

1. **USB / no keyboard.** `iMac15,1` is in `Missing_USB_Map_Ventura`, so OCLP injects `USB-Map.kext`.
   macOS 26 renamed the per-port key in `AppleUSBHostPlatformProperties` from `port` to
   `usb-port-number` (verified by diffing Apple's own `iMac20,1-XHC1` personality between 15.8 and
   26.0). With the old key, `XHC1` is opened then put in D3 and **no USB device ever enumerates** —
   no keyboard, no mouse, no internal Bluetooth. Injecting the same map with the new key, gated
   `MinKernel 25.0.0`, restores USB. This matches the `Add USB mappings for macOS 26` line in the
   3.0.0 changelog.
2. **Wired Ethernet.** The `IOSkywalkFamily` block (`MinKernel 23.0.0`, no `MaxKernel`) plus the
   Ventura `IOSkywalkFamily` injection leave macOS 26 with no `IOSkywalkFamily` at all, so
   `AppleBCM5701Ethernet`, `IOTimeSyncFamily` and `AppleIPAppender` fail with *library kext
   com.apple.iokit.IOSkywalkFamily not found*. Capping the block and the injected wireless kexts at
   `MaxKernel 24.99.99` restores wired Ethernet on 26.
3. **FileVault, for the record.** macOS 26 turned FileVault on by itself on this install; the
   pre-boot unlock screen then rejected the correct password and the pointer only moved on one axis.
   The `fv2` approach (`EnableJumpstart=false` + `apfs_aligned.efi`) was tried with the macOS 15.8
   `apfs_aligned.efi` from `/usr/standalone/i386` and did **not** help here; turning FileVault off
   was the only way through.

## Files

| file | what it is |
|---|---|
| `impostor_tahoe.c` | drop-in replacement for `impostor.dylib`, macOS 26 layout |
| `metaltest.m` | the minimal reproducer of §3 |
