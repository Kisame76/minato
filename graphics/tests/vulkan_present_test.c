/* Test 6 of ADR-0030, the window's part against MoltenVK (graphics.vulkan_present), on graphics-run's window
   of 64 x 48 pixels (window_host.m, ID 1), with the Metal hook reading the first four drawables back
   (metal_hook.m, MINATO_METAL_HOOK=present) and the window host's background script after 70 presents:
   - a known pattern arrives bit for bit in the drawable: the direct way (B8G8R8A8_UNORM), and the copy way
     from R8G8B8A8_UNORM and R8G8B8A8_SRGB, whose channels the copy reorders (decision 12); the program prints
     the CRC-32 each drawable must have, the hook the one it has;
   - VK_SUBOPTIMAL_KHR, which MoltenVK answers for another size than the layer's, becomes VK_SUCCESS in
     acquire, present and its result (decision 12);
   - 60 presents in a row, without a CATransaction flush after each (decision 12);
   - in the background, "scheduled" through the runtime while the program keeps drawing, no command buffer
     of a guest thread after it, and VK_ERROR_SURFACE_LOST_KHR once the window is gone; the semaphore of a
     present that failed so can be used again (decision 13). */
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <android/native_window.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WIDTH 64
#define HEIGHT 48
#define MS 1000000u /* ns */

static VkInstance instance;
static VkPhysicalDevice physical;
static VkDevice device;
static VkQueue queue;
static VkSurfaceKHR surface;
static VkCommandPool pool;
static VkCommandBuffer commands;
static VkBuffer pattern;
static VkDeviceMemory pattern_memory;
static uint8_t *bytes;
static VkSemaphore acquired, rendered;

static uint64_t now(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t)time.tv_sec * 1000000000u + (uint64_t)time.tv_nsec;
}

static void check(const char *what, VkResult result) {
    if (result != VK_SUCCESS) {
        printf("%s: %d\n", what, result);
        exit(1);
    }
}

static const char *yes(int condition) {
    return condition ? "yes" : "no";
}

static uint32_t crc32_of(const uint8_t *data, size_t length) {
    uint32_t crc = 0xffffffffu;
    for (size_t index = 0; index < length; index++) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

/* The CRC-32 of the pattern's first `pixels` pixels, with the first and third byte of each swapped when
   `swapped`, as a B8G8R8A8 drawable holds an R8G8B8A8 image. */
static uint32_t expected(uint32_t pixels, int swapped) {
    uint8_t *copy = malloc(pixels * 4);
    memcpy(copy, bytes, pixels * 4);
    for (uint32_t pixel = 0; swapped && pixel < pixels; pixel++) {
        const uint8_t first = copy[pixel * 4];
        copy[pixel * 4] = copy[pixel * 4 + 2];
        copy[pixel * 4 + 2] = first;
    }
    const uint32_t crc = crc32_of(copy, pixels * 4);
    free(copy);
    return crc;
}

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags wanted) {
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (uint32_t type = 0; type < properties.memoryTypeCount; type++) {
        if ((bits & (1u << type)) && (properties.memoryTypes[type].propertyFlags & wanted) == wanted) {
            return type;
        }
    }
    printf("no memory type\n");
    exit(1);
}

static void setup(void) {
    const char *extensions[] = {"VK_KHR_surface", "VK_KHR_android_surface"};
    const VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "present",
                                           .apiVersion = VK_API_VERSION_1_1};
    const VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                       .pApplicationInfo = &application, .enabledExtensionCount = 2,
                                       .ppEnabledExtensionNames = extensions};
    check("vkCreateInstance", vkCreateInstance(&info, NULL, &instance));
    uint32_t count = 1;
    check("vkEnumeratePhysicalDevices", vkEnumeratePhysicalDevices(instance, &count, &physical));
    const float priority = 1;
    const VkDeviceQueueCreateInfo family = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1,
                                            .pQueuePriorities = &priority};
    const char *swapchain = "VK_KHR_swapchain";
    const VkDeviceCreateInfo created = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                                        .pQueueCreateInfos = &family, .enabledExtensionCount = 1,
                                        .ppEnabledExtensionNames = &swapchain};
    check("vkCreateDevice", vkCreateDevice(physical, &created, NULL, &device));
    vkGetDeviceQueue(device, 0, 0, &queue);
    const VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                               .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    check("vkCreateCommandPool", vkCreateCommandPool(device, &pool_info, NULL, &pool));
    const VkCommandBufferAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                  .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                  .commandBufferCount = 1};
    check("vkAllocateCommandBuffers", vkAllocateCommandBuffers(device, &allocate, &commands));

    /* The pattern: every value in every channel, a different sequence in each. */
    const VkBufferCreateInfo buffer = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = WIDTH * HEIGHT * 4,
                                       .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    check("vkCreateBuffer", vkCreateBuffer(device, &buffer, NULL, &pattern));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, pattern, &requirements);
    const VkMemoryAllocateInfo memory = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type(requirements.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    check("vkAllocateMemory", vkAllocateMemory(device, &memory, NULL, &pattern_memory));
    check("vkBindBufferMemory", vkBindBufferMemory(device, pattern, pattern_memory, 0));
    check("vkMapMemory", vkMapMemory(device, pattern_memory, 0, VK_WHOLE_SIZE, 0, (void **)&bytes));
    for (uint32_t pixel = 0; pixel < WIDTH * HEIGHT; pixel++) {
        bytes[pixel * 4] = (uint8_t)pixel;
        bytes[pixel * 4 + 1] = (uint8_t)(pixel * 7 + 13);
        bytes[pixel * 4 + 2] = (uint8_t)(255 - pixel);
        bytes[pixel * 4 + 3] = (uint8_t)(pixel * 3);
    }

    const VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    check("vkCreateSemaphore", vkCreateSemaphore(device, &semaphore, NULL, &acquired));
    check("vkCreateSemaphore", vkCreateSemaphore(device, &semaphore, NULL, &rendered));

    void *library = dlopen("libnativewindow.so", RTLD_NOW);
    struct ANativeWindow *(*create)(uint64_t, int32_t, int32_t) =
        library != NULL ? (struct ANativeWindow * (*)(uint64_t, int32_t, int32_t)) dlsym(library, "minato_ANativeWindow_create")
                        : NULL;
    if (create == NULL) {
        printf("no minato_ANativeWindow_create\n");
        exit(1);
    }
    struct ANativeWindow *window = create(1, WIDTH, HEIGHT);
    const VkAndroidSurfaceCreateInfoKHR surface_info = {.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
                                                        .window = window};
    check("vkCreateAndroidSurfaceKHR", vkCreateAndroidSurfaceKHR(instance, &surface_info, NULL, &surface));
    ANativeWindow_release(window);
}

static VkSwapchainKHR swapchain(VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage,
                                VkSwapchainKHR old) {
    const VkSwapchainCreateInfoKHR info = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surface,
                                           .minImageCount = 3, .imageFormat = format,
                                           .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
                                           .imageExtent = {width, height}, .imageArrayLayers = 1, .imageUsage = usage,
                                           .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                           .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                                           .compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                                           .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE,
                                           .oldSwapchain = old};
    VkSwapchainKHR made;
    check("vkCreateSwapchainKHR", vkCreateSwapchainKHR(device, &info, NULL, &made));
    return made;
}

struct frame {
    VkResult acquire, present, result;
};

/* One frame: the image acquired, filled with the pattern or only made presentable, and presented. */
static struct frame frame(VkSwapchainKHR which, uint32_t width, uint32_t height, int fill) {
    struct frame done = {VK_RESULT_MAX_ENUM, VK_RESULT_MAX_ENUM, VK_RESULT_MAX_ENUM};
    uint32_t index = 0, count = 8;
    VkImage images[8];
    check("vkQueueWaitIdle", vkQueueWaitIdle(queue));
    check("vkGetSwapchainImagesKHR", vkGetSwapchainImagesKHR(device, which, &count, images));
    done.acquire = vkAcquireNextImageKHR(device, which, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
    if (done.acquire != VK_SUCCESS) {
        return done;
    }
    const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    check("vkBeginCommandBuffer", vkBeginCommandBuffer(commands, &begin));
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                    .newLayout = fill ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                    .dstAccessMask = fill ? VK_ACCESS_TRANSFER_WRITE_BIT : 0,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = images[index],
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &barrier);
    if (fill) {
        const VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                          .imageExtent = {width, height, 1}};
        vkCmdCopyBufferToImage(commands, pattern, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = 0;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL,
                             0, NULL, 1, &barrier);
    }
    check("vkEndCommandBuffer", vkEndCommandBuffer(commands));
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkSubmitInfo batch = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
                                .pWaitSemaphores = &acquired, .pWaitDstStageMask = &stage, .commandBufferCount = 1,
                                .pCommandBuffers = &commands, .signalSemaphoreCount = 1, .pSignalSemaphores = &rendered};
    check("vkQueueSubmit", vkQueueSubmit(queue, 1, &batch, VK_NULL_HANDLE));
    const VkPresentInfoKHR info = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                                   .pWaitSemaphores = &rendered, .swapchainCount = 1, .pSwapchains = &which,
                                   .pImageIndices = &index, .pResults = &done.result};
    done.present = vkQueuePresentKHR(queue, &info);
    return done;
}

int main(void) {
    setup();
    const VkImageUsageFlags transfer = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkSwapchainKHR swapchains[6];
    unsigned made = 0;

    /* What each of the first four drawables must hold. */
    swapchains[made++] =
        swapchain(VK_FORMAT_B8G8R8A8_UNORM, WIDTH, HEIGHT, transfer | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_NULL_HANDLE);
    struct frame done = frame(swapchains[0], WIDTH, HEIGHT, 1);
    printf("direct: acquire %d, present %d, its result %d; the drawable's CRC-32 %08x\n", done.acquire, done.present,
           done.result, expected(WIDTH * HEIGHT, 0));
    swapchains[made] = swapchain(VK_FORMAT_R8G8B8A8_UNORM, WIDTH, HEIGHT, transfer, swapchains[made - 1]);
    done = frame(swapchains[made++], WIDTH, HEIGHT, 1);
    printf("copy from UNORM: acquire %d, present %d, its result %d; the drawable's CRC-32 %08x\n", done.acquire,
           done.present, done.result, expected(WIDTH * HEIGHT, 1));
    swapchains[made] = swapchain(VK_FORMAT_R8G8B8A8_SRGB, WIDTH, HEIGHT, transfer, swapchains[made - 1]);
    done = frame(swapchains[made++], WIDTH, HEIGHT, 1);
    printf("copy from SRGB: acquire %d, present %d, its result %d; the drawable's CRC-32 %08x\n", done.acquire,
           done.present, done.result, expected(WIDTH * HEIGHT, 1));
    swapchains[made] = swapchain(VK_FORMAT_R8G8B8A8_UNORM, WIDTH / 2, HEIGHT / 2, transfer, swapchains[made - 1]);
    done = frame(swapchains[made++], WIDTH / 2, HEIGHT / 2, 1);
    printf("32 x 24: acquire %d, present %d, its result %d; the drawable's CRC-32 %08x\n", done.acquire, done.present,
           done.result, expected(WIDTH * HEIGHT / 4, 1));

    /* 60 presents in a row. */
    swapchains[made] = swapchain(VK_FORMAT_R8G8B8A8_UNORM, WIDTH, HEIGHT, transfer, swapchains[made - 1]);
    const VkSwapchainKHR last = swapchains[made++];
    int all = 1;
    uint64_t start = now();
    for (int presents = 0; presents < 60; presents++) {
        done = frame(last, WIDTH, HEIGHT, 0);
        all = all && done.acquire == VK_SUCCESS && done.present == VK_SUCCESS && done.result == VK_SUCCESS;
    }
    printf("60 presents: all VK_SUCCESS: %s, within 3 s: %s\n", yes(all), yes(now() - start < 3000 * MS));

    /* Drawing on until the window goes in the background. */
    start = now();
    do {
        done = frame(last, WIDTH, HEIGHT, 0);
    } while (done.acquire == VK_SUCCESS && done.present == VK_SUCCESS && now() - start < 5000 * MS);
    const int lost = done.acquire == VK_ERROR_SURFACE_LOST_KHR ||
                     (done.acquire == VK_SUCCESS && done.present == VK_ERROR_SURFACE_LOST_KHR);
    printf("then the window went: VK_ERROR_SURFACE_LOST_KHR: %s, within 5 s: %s\n", yes(lost),
           yes(now() - start < 5000 * MS));

    /* The semaphores again, as the next frame would use them: a signal, then a wait. */
    const VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    check("vkCreateFence", vkCreateFence(device, &fence_info, NULL, &fence));
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    const VkSubmitInfo signal = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .signalSemaphoreCount = 1,
                                 .pSignalSemaphores = &rendered};
    const VkSubmitInfo wait = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
                               .pWaitSemaphores = &rendered, .pWaitDstStageMask = &stage};
    check("vkQueueSubmit", vkQueueSubmit(queue, 1, &signal, VK_NULL_HANDLE));
    check("vkQueueSubmit", vkQueueSubmit(queue, 1, &wait, fence));
    printf("the present's semaphore used again: %d\n", vkWaitForFences(device, 1, &fence, VK_TRUE, 1000 * MS));

    check("vkDeviceWaitIdle", vkDeviceWaitIdle(device));
    vkDestroyFence(device, fence, NULL);
    for (unsigned index = 0; index < made; index++) {
        vkDestroySwapchainKHR(device, swapchains[index], NULL);
    }
    vkDestroySurfaceKHR(instance, surface, NULL);
    vkDestroySemaphore(device, acquired, NULL);
    vkDestroySemaphore(device, rendered, NULL);
    vkDestroyBuffer(device, pattern, NULL);
    vkFreeMemory(device, pattern_memory, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    printf("done\n");
    return 0;
}
