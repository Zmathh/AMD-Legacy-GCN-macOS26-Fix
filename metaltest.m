// Minimal Metal reproducer: device -> fixed-function clear -> shader compile -> pipeline -> draw.
// Distinguishes "the GPU draws nothing at all" from "only pipeline/shader state is broken".
//
// Build:  clang -fobjc-arc -framework Metal -framework Foundation -o metaltest metaltest.m
//
// Expected on a healthy legacy-GCN setup (e.g. macOS 15.8 + AMDMTLBronzeDriver 12.5):
//   step 1 clear : R=255 G=0 B=0 A=255  -> OK (red)
//   step 2 draw  : R=0 G=255 B=0 A=255  -> OK (green, triangle rasterized)
// Observed on macOS 26.0 (25A354) with AMDMTLBronzeDriver 12.5-24:
//   step 2 draw  : R=0 G=0 B=255 A=255  -> FAIL (clear colour untouched)

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

static const char *kShaders =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "vertex float4 v_main(uint vid [[vertex_id]]) {\n"
    "    float2 p[3] = { float2(-3.0, -1.0), float2(1.0, 3.0), float2(1.0, -1.0) };\n"
    "    return float4(p[vid], 0.0, 1.0);\n"
    "}\n"
    "fragment float4 f_main() { return float4(0.0, 1.0, 0.0, 1.0); }\n";

static void readPixel(id<MTLTexture> tex, uint8_t *out) {
    [tex getBytes:out bytesPerRow:64 * 4 fromRegion:MTLRegionMake2D(32, 32, 1, 1) mipmapLevel:0];
}

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { printf("FAIL: no Metal device\n"); return 1; }
        printf("device        : %s\n", dev.name.UTF8String);
        printf("low power     : %d | removable : %d\n", (int)dev.isLowPower, (int)dev.isRemovable);

        MTLTextureDescriptor *td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:64 height:64 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeManaged;
        id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        if (!tex || !queue) { printf("FAIL: null texture or command queue\n"); return 1; }

        // --- step 1: clear to red (fixed-function path, no shaders involved) ---
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = tex;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(1.0, 0.0, 0.0, 1.0);
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc endEncoding];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit synchronizeResource:tex];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.error) printf("command buffer error (clear): %s\n", cb.error.description.UTF8String);
        uint8_t px[4] = {0};
        readPixel(tex, px);
        printf("step 1 clear  : R=%d G=%d B=%d A=%d  -> %s\n", px[0], px[1], px[2], px[3],
               (px[0] > 200 && px[1] < 50) ? "OK (red)" : "FAIL");

        // --- step 2: render pipeline + shaders (what the impostor shim translates) ---
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:@(kShaders) options:nil error:&err];
        if (!lib) { printf("step 2 : shader compile FAILED: %s\n", err.description.UTF8String); return 2; }
        printf("step 2 shaders: compiled OK\n");

        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"v_main"];
        pd.fragmentFunction = [lib newFunctionWithName:@"f_main"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!pso) { printf("step 2 : pipeline creation FAILED: %s\n", err.description.UTF8String); return 3; }
        printf("step 2 pso    : created OK\n");

        rp.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 1.0, 1.0); // blue
        cb = [queue commandBuffer];
        enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:pso];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
        blit = [cb blitCommandEncoder];
        [blit synchronizeResource:tex];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.error) printf("command buffer error (draw): %s\n", cb.error.description.UTF8String);
        readPixel(tex, px);
        printf("step 2 draw   : R=%d G=%d B=%d A=%d  -> %s\n", px[0], px[1], px[2], px[3],
               (px[1] > 200 && px[0] < 50) ? "OK (green, triangle rasterized)"
                                           : (px[2] > 200 ? "FAIL (still blue: nothing was drawn)"
                                                          : "FAIL (unexpected colour)"));
        return 0;
    }
}
