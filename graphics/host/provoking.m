/* ADR-0044 a)'s probe and the run-time check before the ES 3.1 path (decision 1, c)); see minato_graphics.h. Host
   code only: one triangle in an offscreen Metal pass, set up as MoltenVK 1.4.2 sets up its passes with the private
   API (MVKRenderPass.mm, MVKCommandEncoderState.mm). */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>

#include "minato_graphics.h"

/* Apple's private calls as MoltenVK declares them, and MoltenVK's value for the last vertex (mvk_datatypes.h:
   MTLProvokingVertexModeFirst, then MTLProvokingVertexModeLast). */
@interface MTLRenderPassDescriptor (MinatoPrivate)
- (void)setOpenGLModeEnabled:(BOOL)enabled;
@end
@protocol MinatoRenderCommandEncoder <MTLRenderCommandEncoder>
- (void)setProvokingVertexMode:(NSUInteger)mode;
@end
enum { kLastVertex = 1 };

/* The target is kSize pixels square. The triangle covers its center whichever way OpenGL mode might turn y, and each
   vertex has a flat color of its own: red, green, blue. */
enum { kSize = 16 };
static NSString *const kSource =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct Vertex { float4 position [[position]]; float4 color [[flat]]; };\n"
     "vertex Vertex probe_vertex(uint id [[vertex_id]]) {\n"
     "    const float2 positions[3] = { float2(-0.75, -0.75), float2(0.75, -0.75), float2(0.0, 0.75) };\n"
     "    const float4 colors[3] = { float4(1, 0, 0, 1), float4(0, 1, 0, 1), float4(0, 0, 1, 1) };\n"
     "    return Vertex { float4(positions[id], 0.5, 1), colors[id] };\n"
     "}\n"
     "fragment float4 probe_fragment(Vertex in [[stage_in]]) { return in.color; }\n";

/* What a draw gave: the vertex whose color the center pixel has, or kOther and the color, or kFailed and why. */
enum { kOther = -1, kFailed = -2 };
typedef struct {
    int vertex;
    char detail[160];
} Drawn;

/* An error's description for a line. */
static const char *described(NSError *error) {
    return error != nil ? error.localizedDescription.UTF8String : "no error given";
}

static Drawn draw(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLRenderPipelineState> pipeline,
                  BOOL opengl_mode, BOOL last_vertex) {
    Drawn drawn = {.vertex = kFailed};
    MTLTextureDescriptor *texture = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                       width:kSize
                                                                                      height:kSize
                                                                                   mipmapped:NO];
    texture.usage = MTLTextureUsageRenderTarget;
    texture.storageMode = MTLStorageModePrivate;
    id<MTLTexture> target = [device newTextureWithDescriptor:texture];
    id<MTLBuffer> pixels = [device newBufferWithLength:kSize * kSize * 4 options:MTLResourceStorageModeShared];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    // Each call only when Metal responds to it, as MoltenVK makes them.
    if (opengl_mode && [pass respondsToSelector:@selector(setOpenGLModeEnabled:)]) {
        [pass setOpenGLModeEnabled:YES];
    }
    id<MTLCommandBuffer> buffer = [queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [buffer renderCommandEncoderWithDescriptor:pass];
    if (last_vertex && [encoder respondsToSelector:@selector(setProvokingVertexMode:)]) {
        [(id<MinatoRenderCommandEncoder>)encoder setProvokingVertexMode:kLastVertex];
    }
    [encoder setRenderPipelineState:pipeline];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    id<MTLBlitCommandEncoder> blit = [buffer blitCommandEncoder];
    [blit copyFromTexture:target
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(kSize, kSize, 1)
                        toBuffer:pixels
               destinationOffset:0
          destinationBytesPerRow:kSize * 4
        destinationBytesPerImage:kSize * kSize * 4];
    [blit endEncoding];
    [buffer commit];
    [buffer waitUntilCompleted];
    if (buffer.status != MTLCommandBufferStatusCompleted) {
        snprintf(drawn.detail, sizeof drawn.detail, "the GPU failed: %s", described(buffer.error));
        return drawn;
    }
    const uint8_t *center = (const uint8_t *)pixels.contents + ((kSize / 2) * kSize + kSize / 2) * 4;
    static const uint8_t colors[3][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}};
    drawn.vertex = kOther;
    for (int vertex = 0; vertex < 3; vertex++) {
        if (memcmp(center, colors[vertex], 4) == 0) {
            drawn.vertex = vertex;
        }
    }
    if (drawn.vertex == kOther) {
        snprintf(drawn.detail, sizeof drawn.detail, "the center has another color: %u %u %u %u", center[0], center[1],
                 center[2], center[3]);
    }
    return drawn;
}

static int failed(const char *what, NSError *error) {
    printf("provoking probe: %s failed: %s\n", what, described(error));
    return 1;
}

/* The triangle's pipeline, nil with `failure` naming the step that failed. */
static id<MTLRenderPipelineState> pipeline_of(id<MTLDevice> device, const char **failure, NSError **error) {
    id<MTLLibrary> library = [device newLibraryWithSource:kSource options:nil error:error];
    if (library == nil) {
        *failure = "compiling the shaders";
        return nil;
    }
    MTLRenderPipelineDescriptor *description = [[MTLRenderPipelineDescriptor alloc] init];
    description.vertexFunction = [library newFunctionWithName:@"probe_vertex"];
    description.fragmentFunction = [library newFunctionWithName:@"probe_fragment"];
    description.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:description error:error];
    if (pipeline == nil) {
        *failure = "making the pipeline";
    }
    return pipeline;
}

int minato_graphics_angle_vulkan_check(int forced, char *reason, size_t size) {
    if (forced) {
        snprintf(reason, size, "the run-time check was forced to fail");
        return 0;
    }
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            snprintf(reason, size, "no Metal device");
            return 0;
        }
        const char *failure = NULL;
        NSError *error = nil;
        id<MTLRenderPipelineState> pipeline = pipeline_of(device, &failure, &error);
        if (pipeline == nil) {
            snprintf(reason, size, "the check's triangle: %s failed: %s", failure, described(error));
            return 0;
        }
        id<MTLCommandQueue> queue = [device newCommandQueue];
        const Drawn without = draw(device, queue, pipeline, NO, NO);
        const Drawn with = draw(device, queue, pipeline, YES, YES);
        if (without.vertex != 0) {
            snprintf(reason, size, "without Apple's calls the first vertex does not provoke: %s",
                     without.vertex >= 0 ? "another vertex does" : without.detail);
            return 0;
        }
        if (with.vertex != 2) {
            snprintf(reason, size, "the last vertex does not provoke in a pass as MoltenVK sets it up: %s",
                     with.vertex >= 0 ? "another vertex does" : with.detail);
            return 0;
        }
        return 1;
    }
}

int minato_provoking_probe(void) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            printf("provoking probe: no Metal device\n");
            return 1;
        }
        printf("provoking probe: GPU %s\n", device.name.UTF8String);
        const char *failure = NULL;
        NSError *error = nil;
        id<MTLRenderPipelineState> pipeline = pipeline_of(device, &failure, &error);
        if (pipeline == nil) {
            return failed(failure, error);
        }
        id<MTLCommandQueue> queue = [device newCommandQueue];

        // Whether Metal responds to the two calls, asked as MoltenVK asks: the pass's descriptor and an encoder.
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        MTLTextureDescriptor *texture =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:1
                                                              height:1
                                                           mipmapped:NO];
        texture.usage = MTLTextureUsageRenderTarget;
        texture.storageMode = MTLStorageModePrivate;
        pass.colorAttachments[0].texture = [device newTextureWithDescriptor:texture];
        id<MTLCommandBuffer> buffer = [queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [buffer renderCommandEncoderWithDescriptor:pass];
        printf("provoking probe: setOpenGLModeEnabled: %s by %s, setProvokingVertexMode: %s by %s\n",
               [pass respondsToSelector:@selector(setOpenGLModeEnabled:)] ? "offered" : "not offered",
               NSStringFromClass([pass class]).UTF8String,
               [encoder respondsToSelector:@selector(setProvokingVertexMode:)] ? "offered" : "not offered",
               NSStringFromClass([encoder class]).UTF8String);
        [encoder endEncoding];

        static const struct {
            const char *name;
            BOOL opengl_mode, last_vertex;
        } draws[] = {
            {"without either call", NO, NO},
            {"with the mode only", NO, YES},
            {"with OpenGL mode only", YES, NO},
            {"with both, as MoltenVK", YES, YES},
        };
        Drawn drawn[4];
        for (size_t i = 0; i < 4; i++) {
            drawn[i] = draw(device, queue, pipeline, draws[i].opengl_mode, draws[i].last_vertex);
            if (drawn[i].vertex >= 0) {
                printf("provoking probe: %s, vertex %d provokes\n", draws[i].name, drawn[i].vertex);
            } else {
                printf("provoking probe: %s, %s\n", draws[i].name, drawn[i].detail);
            }
        }
        if (drawn[0].vertex != 0) {
            printf("provoking probe: without either call the first vertex does not provoke, so the draw is wrong\n");
            return 1;
        }
        if (drawn[3].vertex != 2) {
            printf("provoking probe: the last vertex does not provoke in a pass as MoltenVK sets it up\n");
            return 1;
        }
        printf("provoking probe: the last vertex provokes in a pass as MoltenVK sets it up\n");
        return 0;
    }
}
