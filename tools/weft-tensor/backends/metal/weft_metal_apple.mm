// weft_metal_apple.mm — RFC-0017 §4 device roads (Apple-only leg).
//
// COMPILED ONLY ON __APPLE__ (the Makefile gate; the WeftMetalZeroCopy
// / gpu_ring-METAL precedent). Executable-verified on the apple CI leg;
// DECLARED for the x86_64 sandbox (every claim carries the label).
//
// The implementation keeps the Series-10 Swift bridge's contracts:
//   - bytesNoCopy: >= 16 bytes, 16-byte alignment (rings are 64-byte
//     aligned by construction; a foreign base that is not, refuses)
//   - discrete-GPU Macs: shared-storage compute support is the gate;
//     wrap REFUSES rather than silently staging (Law 4)
//   - the caller owns wrapped memory for the handle's lifetime
//
// The CVPixelBuffer road: CVPixelBufferCreateWithBytes NEVER copies when
// the geometry is exact — it wraps with a release callback. The surface
// must outlive model inference; the callback ties release to the view's
// epoch bookkeeping in the caller (we pass a context block the caller
// can key; v1 passes the epoch value for diagnostics).
//
// The IOSurface road: IOSurfaceCreate with kIOSurfaceAllocSize gives the
// ring device-shareable, ANE-consumable storage whose bytes the CPU
// writes directly — the reverse-ownership road (the ring's slots ARE
// surface bytes; CoreML consumes them through the surface handle with
// zero transforms).

#import "weft_metal_bridge.h"

#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

#include "weft/weft_accel_common.h"

// ---------------------------------------------------------------------------
// Probe
// ---------------------------------------------------------------------------

weft_metal_caps_t weft_metal_probe(void) {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return WEFT_METAL_UNSUPPORTED;
    if (device.hasUnifiedMemory) return WEFT_METAL_UNIFIED;
    // Discrete Macs: tier-2/tier-1 read-write support is the honest gate
    // (the Swift bridge's stance); anything less and wraps refuse.
    if (@available(macOS 10.15, iOS 13.0, *)) {
        if (device.readWriteTextureSupport == MTLReadWriteTextureTier2 ||
            device.readWriteTextureSupport == MTLReadWriteTextureTier1) {
            return WEFT_METAL_DISCRETE;
        }
    }
    return WEFT_METAL_UNSUPPORTED;
}

// ---------------------------------------------------------------------------
// MTLBuffer wrap (bytesNoCopy — the compute road)
// ---------------------------------------------------------------------------

int weft_metal_wrap_mtlbuffer(const weft_tensor_view_t* v,
                              void** out_buffer) {
    if (!v || !out_buffer) return -1;
    *out_buffer = NULL;

    weft_metal_geometry_t geo;
    if (weft_metal_geometry_for_view(v, &geo) != 0) return -1;

    // bytesNoCopy contract (the Swift bridge's documented boundary).
    if (v->byte_len < 16) return -1;
    if (((uintptr_t)v->data & 15u) != 0) return -1;

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return -1;
    if (!device.hasUnifiedMemory) {
        // Discrete-only Macs: refuse rather than silently stage (Law 4 —
        // the exact WeftMetalZeroCopy.swift gate).
        if (@available(macOS 10.15, iOS 13.0, *)) {
            if (device.readWriteTextureSupport == MTLReadWriteTextureTierNone)
                return -1;
        } else {
            return -1;
        }
    }

    id<MTLBuffer> buffer = [device
        newBufferWithBytesNoCopy:v->data
                          length:(NSUInteger)v->byte_len
                         options:MTLResourceStorageModeShared
                     deallocator:^(void* p, NSUInteger n) {
                         // The caller's memory — nothing to free; the
                         // block exists so Metal NEVER claims ownership.
                         (void)p; (void)n;
                     }];
    if (!buffer) return -1;
    CFRetain((__bridge CFTypeRef)buffer);
    *out_buffer = (__bridge void*)buffer;  // owned by the caller via
                                           // weft_metal_release
    return 0;
}

// ---------------------------------------------------------------------------
// CVPixelBuffer wrap (CoreML / Vision / ANE model input)
// ---------------------------------------------------------------------------

int weft_metal_wrap_cvpixelbuffer(const weft_tensor_view_t* v,
                                  void** out_pb) {
    if (!v || !out_pb) return -1;
    *out_pb = NULL;

    weft_metal_geometry_t geo;
    if (weft_metal_geometry_for_view(v, &geo) != 0) return -1;

    OSType format;
    if (v->dtype == (uint8_t)WEFT_TENSOR_U8) {
        format = kCVPixelFormatType_32RGBA;
    } else {
        format = kCVPixelFormatType_128RGBAFloat;  // the spectrogram road
    }

    // The release context: the view's epoch (diagnostics; the producer
    // owns the memory — the callback must never free it).
    uint64_t* epoch_ctx = (uint64_t*)malloc(sizeof(uint64_t));
    if (!epoch_ctx) return -1;
    *epoch_ctx = v->epoch;

    CVPixelBufferRef pb = NULL;
    CVReturn rv = CVPixelBufferCreateWithBytes(
        NULL,                             // default allocator
        geo.width, geo.height,
        format,
        v->data,
        (size_t)geo.bytes_per_row,
        ^(void* releaseRefCon, const void* baseAddress) {
            // Pixel buffer released: drop the epoch context only.
            (void)baseAddress;
            free(releaseRefCon);
        },
        epoch_ctx,
        NULL,                             // no extra CVPixelBuffer attributes
        &pb);
    if (rv != kCVReturnSuccess || !pb) {
        free(epoch_ctx);
        return -1;
    }
    *out_pb = pb;  // CF-owned; caller releases via weft_metal_release
    return 0;
}

// ---------------------------------------------------------------------------
// IOSurface span (reverse ownership — ring storage FROM the surface)
// ---------------------------------------------------------------------------

int weft_metal_iosurface_span(uint32_t width, uint32_t height,
                              uint32_t bytes_per_element, void** out_surface,
                              void** out_bytes, uint64_t* out_span) {
    if (!out_surface || !out_bytes || !out_span) return -1;
    *out_surface = NULL;
    *out_bytes = NULL;
    *out_span = 0;
    if (width == 0 || height == 0 ||
        (bytes_per_element != 1 && bytes_per_element != 2 &&
         bytes_per_element != 4 && bytes_per_element != 8)) {
        return -1;
    }

    size_t row_bytes = (size_t)width * bytes_per_element;
    row_bytes = (row_bytes + 63) & ~((size_t)63);  // the 64-alignment law
    size_t span = row_bytes * (size_t)height;

    NSDictionary* attrs = @{
        (id)kIOSurfaceWidth: @(width),
        (id)kIOSurfaceHeight: @(height),
        (id)kIOSurfaceBytesPerElement: @(bytes_per_element),
        (id)kIOSurfaceBytesPerRow: @(row_bytes),
        (id)kIOSurfaceAllocSize: @(span),
    };
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)attrs);
    if (!surface) return -1;

    void* bytes = IOSurfaceGetBytePtr(surface);
    if (!bytes) {
        CFRelease(surface);
        return -1;
    }
    // The surface's bytes are coherent CPU memory on Apple Silicon — the
    // ring engine may allocate its slots inside this span (Engineer 1's
    // arena accepts an external span; the view ABI flags PINNED).
    *out_surface = surface;
    *out_bytes = bytes;
    *out_span = (uint64_t)span;
    return 0;
}

void weft_metal_release(void* handle) {
    if (!handle) return;
    CFTypeRef t = (CFTypeRef)handle;
    // MTLBuffer arrives bridged (CFRetain'd at wrap) — one release fits
    // every handle kind (IOSurface/CVPixelBuffer are CF too).
    CFRelease(t);
}

// ---------------------------------------------------------------------------
// Pooled compute (frozen kernel; Law 1 — one pipeline, reused commands)
// ---------------------------------------------------------------------------

struct weft_metal_pool {
    id<MTLDevice> device;
    id<MTLComputePipelineState> pipeline;
    id<MTLCommandQueue> queue;
    dispatch_semaphore_t slot;   // paces the small reuse ring
};

int weft_metal_pool_create(void** out_pool, const char* msl_source,
                           size_t msl_len, uint64_t frozen_id_expect) {
    if (!out_pool || !msl_source || msl_len == 0) return -1;
    *out_pool = NULL;

    // Law 3: the frozen-ID check runs BEFORE the compiler.
    if (frozen_id_expect != 0) {
        uint64_t id = weft_accel_frozen_id(msl_source, msl_len);
        if (id != frozen_id_expect) return -1;
    }

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return -1;

    NSString* src = [[NSString alloc] initWithBytes:msl_source
                                             length:msl_len
                                           encoding:NSUTF8StringEncoding];
    NSError* err = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:src options:nil
                                                 error:&err];
    if (!lib) return -1;
    id<MTLFunction> fn = [lib newFunctionWithName:@"weft_preprocess"];
    if (!fn) return -1;
    id<MTLComputePipelineState> pipe =
        [device newComputePipelineStateWithFunction:fn error:&err];
    if (!pipe) return -1;

    struct weft_metal_pool* p =
        (struct weft_metal_pool*)calloc(1, sizeof(*p));
    if (!p) return -1;
    p->device = device;
    p->pipeline = pipe;
    p->queue = [device newCommandQueue];
    p->slot = dispatch_semaphore_create(4);  // 4-deep reuse ring
    *out_pool = p;
    return 0;
}

int weft_metal_pool_preprocess(void* pool, void* src_buffer,
                               uint64_t src_byte_off, void* dst_buffer,
                               uint64_t dst_byte_off,
                               const weft_metal_preprocess_push_t* push) {
    struct weft_metal_pool* p = (struct weft_metal_pool*)pool;
    if (!p || !src_buffer || !dst_buffer || !push) return -1;

    dispatch_semaphore_wait(p->slot, DISPATCH_TIME_FOREVER);
    @autoreleasepool {
        id<MTLCommandBuffer> cmd = [p->queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
        [enc setComputePipelineState:p->pipeline];
        [enc setBuffer:(__bridge id<MTLBuffer>)src_buffer
                 offset:(NSUInteger)src_byte_off
                atIndex:0];
        [enc setBuffer:(__bridge id<MTLBuffer>)dst_buffer
                 offset:(NSUInteger)dst_byte_off
                atIndex:1];
        [enc setBytes:push
               length:sizeof(*push)
              atIndex:2];
        // One pixel per thread; 64 per threadgroup (the frozen shape).
        MTLSize tg = MTLSizeMake(64, 1, 1);
        MTLSize grid = MTLSizeMake(
            ((push->n_pixels + 63) / 64) * 64, 1, 1);
        [enc dispatchThreads:grid threadsPerThreadgroup:tg];
        [enc endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];
    }
    dispatch_semaphore_signal(p->slot);
    return 0;
}
