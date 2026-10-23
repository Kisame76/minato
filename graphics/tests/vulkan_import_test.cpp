// graphics.vulkan_import: graphics/host's import of a hardware buffer into an image's dedicated memory (vulkan.m,
// ADR-0058 decision 7) against a hostile guest, which calls the host's slots of libvulkan's table directly and so
// skips the guest libvulkan's own checks (graphics/vulkan/external_memory.c). The image's shape is the guest's: an
// import into an image whose type, depth, samples, tiling, levels, layers, extent or format is not the buffer's, or
// into an image that has a buffer's memory, must answer VK_ERROR_INVALID_EXTERNAL_HANDLE before MoltenVK sees the
// IOSurface. Android 15's driver on the emulator takes all of these (measured for this step, 2026-10-23), the
// specification's valid usage it does not check; MoltenVK would wrap the IOSurface in a texture of another shape,
// in the host's process. MoltenVK is a fake here, with vkUseIOSurfaceMVK of this program's; the buffers are
// graphics/host's IOSurfaces with the runtime's IDs. Exits 1 after the cases when one failed.
#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"
#include "host_protocol.h"
#include "minato_graphics.h"
#include "syscall_frame.h"
#include "vulkan_table.h"
extern "C" {
#include "vulkan_host.h"
}

#include <IOSurface/IOSurfaceRef.h>
#include <dlfcn.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <thread>
#include <vector>

namespace {

constexpr std::uint32_t rgba8 = 1, rgbx8 = 2, rgb565 = 4, rgba16f = 0x16, blob = 0x21, rgb10a2 = 0x2b;
constexpr std::uint64_t cpu = 3 | (3 << 4), gpu_sampled = 1 << 8;
constexpr std::uint32_t side = 8;

std::atomic<int> failures{0};
std::atomic<std::uint64_t> next_handle{0x1000};
std::atomic<int> surfaces_used{0}, allocations{0}, destroyed{0};
std::atomic<int> failing_allocations{0}, failing_surfaces{0};
// The handle the next image gets when not 0, as MoltenVK gives a freed image's address to a new one.
std::atomic<std::uint64_t> handle_again{0};
// Runs inside the next vkUseIOSurfaceMVK that fails, before it answers.
std::function<void()> while_failing;

void fail(const char *name, const char *what, long long value) {
    std::fprintf(stderr, "FAILED  %s: %s %lld\n", name, what, value);
    failures.fetch_add(1);
}

// The fake MoltenVK: images and memory are numbers; an allocation fails while failing_allocations is above 0.
VkResult image_created(VkDevice, const VkImageCreateInfo *, const VkAllocationCallbacks *, VkImage *image) {
    const std::uint64_t again = handle_again.exchange(0);
    *image = reinterpret_cast<VkImage>(again != 0 ? again : next_handle.fetch_add(1));
    return VK_SUCCESS;
}

void device_destroyed(VkDevice, const VkAllocationCallbacks *) {}

void image_destroyed(VkDevice, VkImage, const VkAllocationCallbacks *) {
    destroyed.fetch_add(1);
}

VkResult memory_allocated(VkDevice, const VkMemoryAllocateInfo *, const VkAllocationCallbacks *,
                          VkDeviceMemory *memory) {
    allocations.fetch_add(1);
    if (failing_allocations.load() > 0) {
        failing_allocations.fetch_sub(1);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    *memory = reinterpret_cast<VkDeviceMemory>(next_handle.fetch_add(1));
    return VK_SUCCESS;
}

void *table[MINATO_VULKAN_TABLE_LENGTH];

std::int64_t hardware_buffer_call(std::uint64_t op, std::uint64_t argument) {
    apkrt_syscall_frame frame{};
    frame.x[0] = op;
    frame.x[1] = argument;
    frame.x[8] = APKRT_NR_HARDWARE_BUFFER;
    apkrt_syscall_dispatch(&frame);
    return static_cast<std::int64_t>(frame.x[0]);
}

// A buffer of the runtime's with graphics/host's IOSurface; its ID, or 0.
std::uint64_t buffer(std::uint32_t width, std::uint32_t height, std::uint32_t layers, std::uint32_t format,
                     std::uint64_t usage) {
    apkrt_hardware_buffer made{};
    made.width = width;
    made.height = height;
    made.layers = layers;
    made.format = format;
    made.usage = usage;
    const std::int64_t answer = hardware_buffer_call(APKRT_HARDWARE_BUFFER_ALLOCATE,
                                                     reinterpret_cast<std::uint64_t>(&made));
    if (answer != 0) {
        std::fprintf(stderr, "allocate %u x %u x %u of format %u: %lld\n", width, height, layers, format,
                     static_cast<long long>(answer));
    }
    return answer == 0 ? made.id : 0;
}

void release(std::uint64_t id) {
    hardware_buffer_call(APKRT_HARDWARE_BUFFER_RELEASE, id);
}

VkDevice const device = reinterpret_cast<VkDevice>(0x10), other_device = reinterpret_cast<VkDevice>(0x20);

// The image's shape, as the guest asks the host for it.
struct shape {
    VkImageType type = VK_IMAGE_TYPE_2D;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkExtent3D extent = {side, side, 1};
    std::uint32_t levels = 1, layers = 1;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
    VkImageCreateFlags flags = 0;
};

VkImage image_of(const shape &shape, VkDevice on = device) {
    const VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                    nullptr,
                                    shape.flags,
                                    shape.type,
                                    shape.format,
                                    shape.extent,
                                    shape.levels,
                                    shape.layers,
                                    shape.samples,
                                    shape.tiling,
                                    VK_IMAGE_USAGE_SAMPLED_BIT,
                                    VK_SHARING_MODE_EXCLUSIVE,
                                    0,
                                    nullptr,
                                    VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage image = VK_NULL_HANDLE;
    const VkResult result =
        reinterpret_cast<PFN_vkCreateImage>(table[MINATO_VULKAN_SLOT_vkCreateImage])(on, &info, nullptr, &image);
    return result == VK_SUCCESS ? image : VK_NULL_HANDLE;
}

void destroy(VkImage image) {
    reinterpret_cast<PFN_vkDestroyImage>(table[MINATO_VULKAN_SLOT_vkDestroyImage])(device, image, nullptr);
}

// The host's vkAllocateMemory as the guest's libvulkan calls it: the buffer's ID first in the chain, then the
// dedicated image.
VkResult imported(std::uint64_t id, VkImage image) {
    const VkMemoryDedicatedAllocateInfo dedicated = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, image,
                                                     VK_NULL_HANDLE};
    const minato_hardware_buffer named = {MINATO_STRUCTURE_TYPE_HARDWARE_BUFFER, &dedicated, id};
    const VkMemoryAllocateInfo info = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &named, 4096, 0};
    VkDeviceMemory memory = VK_NULL_HANDLE;
    return reinterpret_cast<PFN_vkAllocateMemory>(table[MINATO_VULKAN_SLOT_vkAllocateMemory])(device, &info, nullptr,
                                                                                             &memory);
}

// An import of buffer `id` into a new image of `shape` must answer `expected`; a refusal must leave MoltenVK
// untouched, an import reach vkUseIOSurfaceMVK and the allocation once each.
void check_import(const char *name, std::uint64_t id, const shape &shape, VkResult expected) {
    const VkImage image = image_of(shape);
    if (image == VK_NULL_HANDLE) {
        fail(name, "no image", 0);
        return;
    }
    const int used = surfaces_used.load(), allocated = allocations.load();
    const VkResult result = imported(id, image);
    const int reached = expected == VK_SUCCESS ? 1 : 0;
    if (result != expected) {
        fail(name, "answer", result);
    }
    if (surfaces_used.load() - used != reached || allocations.load() - allocated != reached) {
        fail(name, "MoltenVK's calls", (surfaces_used.load() - used) * 10 + allocations.load() - allocated);
    }
    destroy(image);
    std::printf("checked  %s\n", name);
}

}  // namespace

// MoltenVK's vkUseIOSurfaceMVK, which the host finds with dlsym in the library it gets: this program. It fails while
// failing_surfaces is above 0.
extern "C" __attribute__((visibility("default"), used)) VkResult vkUseIOSurfaceMVK(VkImage, IOSurfaceRef surface) {
    surfaces_used.fetch_add(1);
    if (failing_surfaces.load() > 0) {
        failing_surfaces.fetch_sub(1);
        std::function<void()> run;
        run.swap(while_failing);
        if (run) {
            run();
        }
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return surface != nullptr ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
}

int main() {
    if (apkrt_set_hardware_buffers(minato_graphics_hardware_buffers()) != 0) {
        std::fprintf(stderr, "FAILED  no hardware buffers\n");
        return 1;
    }
    table[MINATO_VULKAN_SLOT_vkCreateImage] = reinterpret_cast<void *>(image_created);
    table[MINATO_VULKAN_SLOT_vkDestroyImage] = reinterpret_cast<void *>(image_destroyed);
    table[MINATO_VULKAN_SLOT_vkAllocateMemory] = reinterpret_cast<void *>(memory_allocated);
    table[MINATO_VULKAN_SLOT_vkDestroyDevice] = reinterpret_cast<void *>(device_destroyed);
    minato_graphics_vulkan_wrap(table, dlopen(nullptr, RTLD_NOW));

    const std::uint64_t rgba = buffer(side, side, 1, rgba8, gpu_sampled | cpu);
    const std::uint64_t rgbx = buffer(side, side, 1, rgbx8, gpu_sampled | cpu);
    // graphics/host makes a buffer of layers or a BLOB only for the CPU; a hostile guest imports it all the same.
    const std::uint64_t two_layers = buffer(side, side, 2, rgba8, cpu);
    const std::uint64_t data = buffer(side * side * 4, 1, 1, blob, cpu);
    if (rgba == 0 || rgbx == 0 || two_layers == 0 || data == 0) {
        std::fprintf(stderr, "FAILED  no buffers\n");
        return 1;
    }
    const shape own;
    check_import("the buffer's own shape imports", rgba, own, VK_SUCCESS);
    check_import("R8G8B8X8 imports as R8G8B8A8", rgbx, own, VK_SUCCESS);

    const VkResult refused = VK_ERROR_INVALID_EXTERNAL_HANDLE;
    shape changed = own;
    changed.levels = 2;
    check_import("two levels", rgba, changed, refused);
    changed = own;
    changed.layers = 2;
    check_import("two layers", rgba, changed, refused);
    check_import("two layers of a buffer of two layers", two_layers, changed, refused);
    check_import("one layer of a buffer of two layers", two_layers, own, refused);
    changed = own;
    changed.extent.width = 2 * side;
    check_import("a wider extent", rgba, changed, refused);
    changed = own;
    changed.extent.height = 2 * side;
    check_import("a taller extent", rgba, changed, refused);
    changed = own;
    changed.extent = {side / 2, side / 2, 1};
    check_import("a smaller extent", rgba, changed, refused);
    changed = own;
    changed.extent.depth = 2;
    check_import("a depth of 2", rgba, changed, refused);
    changed = own;
    changed.type = VK_IMAGE_TYPE_3D;
    check_import("a 3D image", rgba, changed, refused);
    changed = own;
    changed.type = VK_IMAGE_TYPE_1D;
    changed.extent = {side, 1, 1};
    check_import("a 1D image", rgba, changed, refused);
    changed = own;
    changed.format = VK_FORMAT_R5G6B5_UNORM_PACK16;
    check_import("R5G6B5 for R8G8B8A8", rgba, changed, refused);
    changed = own;
    changed.format = VK_FORMAT_B8G8R8A8_UNORM;
    check_import("B8G8R8A8 for R8G8B8A8, its bytes per texel", rgba, changed, refused);
    changed = own;
    changed.format = VK_FORMAT_R32_UINT;
    check_import("R32_UINT for R8G8B8A8, its bytes per texel", rgba, changed, refused);
    changed = own;
    changed.format = VK_FORMAT_UNDEFINED;
    check_import("an undefined format", rgba, changed, refused);
    changed = own;
    changed.samples = VK_SAMPLE_COUNT_4_BIT;
    check_import("four samples", rgba, changed, refused);
    changed = own;
    changed.tiling = VK_IMAGE_TILING_LINEAR;
    check_import("linear tiling", rgba, changed, refused);
    changed = own;
    changed.flags = VK_IMAGE_CREATE_SPARSE_BINDING_BIT;
    check_import("a sparse image", rgba, changed, refused);
    changed.flags = VK_IMAGE_CREATE_PROTECTED_BIT;
    check_import("a protected image", rgba, changed, refused);
    changed.flags = VK_IMAGE_CREATE_DISJOINT_BIT;
    check_import("a disjoint image", rgba, changed, refused);
    changed.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    check_import("a mutable format imports", rgba, changed, VK_SUCCESS);
    changed = own;
    changed.extent = {side * side * 4, 1, 1};
    check_import("a BLOB, whose size an image of one row has", data, changed, refused);
    changed.format = VK_FORMAT_UNDEFINED;
    check_import("an undefined format for a BLOB, which has no VkFormat", data, changed, refused);
    // The table's other formats import where the buffer is one.
    const std::uint64_t rgb565_buffer = buffer(side, side, 1, rgb565, gpu_sampled | cpu);
    const std::uint64_t rgba16f_buffer = buffer(side, side, 1, rgba16f, gpu_sampled | cpu);
    const std::uint64_t rgb10a2_buffer = buffer(side, side, 1, rgb10a2, gpu_sampled | cpu);
    changed = own;
    changed.format = VK_FORMAT_R5G6B5_UNORM_PACK16;
    check_import("R5G6B5 for R5G6B5 imports", rgb565_buffer, changed, VK_SUCCESS);
    changed.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    check_import("R16G16B16A16_SFLOAT for R16G16B16A16_FLOAT imports", rgba16f_buffer, changed, VK_SUCCESS);
    changed.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    check_import("A2B10G10R10_UNORM for R10G10B10A2_UNORM imports", rgb10a2_buffer, changed, VK_SUCCESS);

    // An image the host did not make, and one it destroyed.
    {
        const char *name = "an image the host never made";
        const int used = surfaces_used.load();
        const VkImage never = reinterpret_cast<VkImage>(UINT64_C(0xdead0000));
        if (imported(rgba, never) != refused || surfaces_used.load() != used) {
            fail(name, "answer", 0);
        }
        std::printf("checked  %s\n", name);
        name = "a destroyed image";
        const VkImage image = image_of(own);
        destroy(image);
        if (imported(rgba, image) != refused || surfaces_used.load() != used) {
            fail(name, "answer", 0);
        }
        std::printf("checked  %s\n", name);
    }
    // A second import into an image that has a buffer's memory; a failed allocation leaves it to a later import.
    {
        const char *name = "a second import into the same image";
        const VkImage image = image_of(own);
        const int used = surfaces_used.load();
        const VkResult first = imported(rgba, image), second = imported(rgba, image);
        if (first != VK_SUCCESS || second != refused || surfaces_used.load() != used + 1) {
            fail(name, "answers", surfaces_used.load() - used);
        }
        destroy(image);
        std::printf("checked  %s\n", name);
        name = "an import after MoltenVK's failed allocation";
        const VkImage again = image_of(own);
        failing_allocations.store(1);
        if (imported(rgba, again) != VK_ERROR_OUT_OF_DEVICE_MEMORY || imported(rgba, again) != VK_SUCCESS) {
            fail(name, "answers", 0);
        }
        destroy(again);
        std::printf("checked  %s\n", name);
    }
    // MoltenVK gives a handle again, which the host had for an image MoltenVK destroyed past it: the new shape counts.
    {
        const char *name = "a handle again, of another shape";
        const VkImage first = image_of(own);
        image_destroyed(device, first, nullptr);  // MoltenVK's own, past the host
        handle_again.store(reinterpret_cast<std::uint64_t>(first));
        shape levels = own;
        levels.levels = 2;
        const VkImage again = image_of(levels);
        const int used = surfaces_used.load();
        if (again != first || imported(rgba, again) != refused || surfaces_used.load() != used) {
            fail(name, "answer", again == first);
        }
        destroy(again);
        std::printf("checked  %s\n", name);
    }
    // The images of a destroyed device go with it, also those the app did not destroy.
    {
        const char *name = "an image of a destroyed device";
        const VkImage left = image_of(own, other_device);
        const VkImage kept = image_of(own);
        reinterpret_cast<PFN_vkDestroyDevice>(table[MINATO_VULKAN_SLOT_vkDestroyDevice])(other_device, nullptr);
        const int used = surfaces_used.load();
        if (imported(rgba, left) != refused || surfaces_used.load() != used || imported(rgba, kept) != VK_SUCCESS) {
            fail(name, "answers", surfaces_used.load() - used);
        }
        destroy(kept);
        std::printf("checked  %s\n", name);
    }
    // vkUseIOSurfaceMVK's failure leaves the image to a later import.
    {
        const char *name = "an import after vkUseIOSurfaceMVK failed";
        const VkImage image = image_of(own);
        failing_surfaces.store(1);
        const int allocated = allocations.load();
        const VkResult first = imported(rgba, image);
        if (first != refused || allocations.load() != allocated || imported(rgba, image) != VK_SUCCESS) {
            fail(name, "answers", first);
        }
        destroy(image);
        std::printf("checked  %s\n", name);
    }
    // While an import fails, the app destroys the image and MoltenVK gives its handle to a new one, which another
    // import takes: the failed one gives back only its own take.
    {
        const char *name = "a failed import gives back only its own take";
        const VkImage image = image_of(own);
        VkImage again = VK_NULL_HANDLE;
        VkResult taken = VK_ERROR_UNKNOWN;
        while_failing = [&again, &taken, image, rgba, own] {
            destroy(image);
            handle_again.store(reinterpret_cast<std::uint64_t>(image));
            again = image_of(own);
            taken = imported(rgba, again);
        };
        failing_surfaces.store(1);
        const VkResult failed = imported(rgba, image);
        const VkResult after = imported(rgba, again);
        if (failed != refused || again != image || taken != VK_SUCCESS || after != refused) {
            fail(name, "answers", after);
        }
        destroy(again);
        std::printf("checked  %s\n", name);
    }
    // Two imports into one image at once: one of them takes it.
    {
        const char *name = "two imports into one image at once";
        int wrong = 0;
        for (int round = 0; round < 200; round++) {
            const VkImage image = image_of(own);
            VkResult answers[2];
            std::thread one([&answers, image, rgba] { answers[0] = imported(rgba, image); });
            std::thread two([&answers, image, rgba] { answers[1] = imported(rgba, image); });
            one.join();
            two.join();
            if ((answers[0] == VK_SUCCESS) + (answers[1] == VK_SUCCESS) != 1) {
                wrong++;
            }
            destroy(image);
        }
        if (wrong != 0) {
            fail(name, "rounds with other than one import", wrong);
        }
        std::printf("checked  %s\n", name);
    }
    // Every image the host's vkDestroyImage gets reaches MoltenVK's, VK_NULL_HANDLE too.
    {
        const char *name = "destroys reach MoltenVK";
        const int before = destroyed.load();
        destroy(VK_NULL_HANDLE);
        destroy(image_of(own));
        if (destroyed.load() != before + 2) {
            fail(name, "destroys", destroyed.load() - before);
        }
        std::printf("checked  %s\n", name);
    }
    // Images made and destroyed on several threads while others import.
    {
        const char *name = "threads that make, import and destroy";
        std::vector<std::thread> threads;
        std::atomic<int> wrong{0};
        for (int index = 0; index < 4; index++) {
            threads.emplace_back([&wrong, rgba, own, index] {
                shape mine = own;
                mine.levels = index % 2 == 0 ? 1 : 2;
                for (int round = 0; round < 500; round++) {
                    const VkImage image = image_of(mine);
                    const VkResult result = imported(rgba, image);
                    if (result != (mine.levels == 1 ? VK_SUCCESS : VK_ERROR_INVALID_EXTERNAL_HANDLE)) {
                        wrong.fetch_add(1);
                    }
                    destroy(image);
                }
            });
        }
        for (std::thread &thread : threads) {
            thread.join();
        }
        if (wrong.load() != 0) {
            fail(name, "wrong answers", wrong.load());
        }
        std::printf("checked  %s\n", name);
    }
    // An allocation without a buffer reaches MoltenVK as it is.
    {
        const char *name = "an allocation without a buffer";
        const VkMemoryAllocateInfo info = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, 4096, 0};
        VkDeviceMemory memory = VK_NULL_HANDLE;
        const int allocated = allocations.load(), used = surfaces_used.load();
        const VkResult result = reinterpret_cast<PFN_vkAllocateMemory>(table[MINATO_VULKAN_SLOT_vkAllocateMemory])(
            device, &info, nullptr, &memory);
        if (result != VK_SUCCESS || allocations.load() != allocated + 1 || surfaces_used.load() != used) {
            fail(name, "answer", result);
        }
        std::printf("checked  %s\n", name);
    }
    for (std::uint64_t id : {rgba, rgbx, two_layers, data, rgb565_buffer, rgba16f_buffer, rgb10a2_buffer}) {
        release(id);
    }
    if (failures.load() != 0) {
        std::fprintf(stderr, "%d failed\n", failures.load());
        return 1;
    }
    std::printf("all passed\n");
    return 0;
}
