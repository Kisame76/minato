/* Test 6 of ADR-0030 without the window, the queue's part against MoltenVK (graphics.vulkan_queue), with
   graphics-run's Metal hook in its "scheduled" mode, which prints to stderr what Metal sees between the
   program's markers there:
   - "scheduled" (decision 13): the program submits a batch that waits for a timeline semaphore at
     SCHEDULED_VALUE and waits for the value after it. On a thread of its own, the hook asks graphics/host
     for "scheduled" while the batch waits, lets the batch go on, asks again, prints both answers and then
     sets the value after it, which lets the program go on: no guest thread and no main thread take part;
   - the waits in slices (decision 4): a signal sent 50 ms into vkWaitForFences, vkWaitSemaphores,
     vkQueueWaitIdle, vkDeviceWaitIdle or vkGetQueryPoolResults is handled within about 10 ms (40 ms here,
     for a loaded machine), and the wait goes on to its end;
   - vkQueueWaitIdle and vkDeviceWaitIdle commit nothing (decision 13): the hook prints nothing between
     their markers;
   - a debug messenger hears the app's message on the app's thread, through graphics/host (decision 5). */
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The value graphics-run's Metal hook watches for (metal_hook.m). */
#define SCHEDULED_VALUE 1000

static VkDevice device;
static VkQueue queue;
static pthread_t program;
static volatile uint64_t handled_at;  /* when the handler of SIGUSR1 last ran */

static uint64_t now(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t)time.tv_sec * 1000000000u + (uint64_t)time.tv_nsec;
}

static void handled(int signal) {
    (void)signal;
    handled_at = now();
}

static void check(const char *what, VkResult result) {
    if (result != VK_SUCCESS) {
        printf("%s: %d\n", what, result);
        exit(1);
    }
}

/* A thread that sends the program's thread SIGUSR1 after 50 ms and then, when given, signals a timeline
   semaphore to 1 after 150 ms. */
struct nudge {
    VkSemaphore timeline;
    uint64_t sent_at;
};

static void *nudging(void *context) {
    struct nudge *nudge = context;
    usleep(50000);
    nudge->sent_at = now();
    pthread_kill(program, SIGUSR1);
    if (nudge->timeline != VK_NULL_HANDLE) {
        usleep(100000);
        const VkSemaphoreSignalInfo signal = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
                                              .semaphore = nudge->timeline, .value = 1};
        check("vkSignalSemaphore", vkSignalSemaphore(device, &signal));
    }
    return NULL;
}

/* A wait with a nudge meanwhile: its answer, whether the signal was handled within 40 ms of its sending,
   and whether the wait lasted at least `least` ms. */
static void nudged(const char *what, VkResult (*wait)(void *), void *context, VkSemaphore timeline, unsigned least) {
    struct nudge nudge = {timeline, 0};
    pthread_t thread;
    handled_at = 0;
    fprintf(stderr, "vulkan-queue-test: %s\n", what);
    const uint64_t start = now();
    pthread_create(&thread, NULL, nudging, &nudge);
    const VkResult result = wait(context);
    const uint64_t end = now();
    fprintf(stderr, "vulkan-queue-test: %s returned\n", what);
    pthread_join(thread, NULL);
    printf("%s: %d; the signal handled within 40 ms: %s; at least %u ms: %s\n", what, result,
           handled_at != 0 && handled_at - nudge.sent_at < 40000000u ? "yes" : "no", least,
           end - start >= least * 1000000ull ? "yes" : "no");
}

static VkFence never;  /* a fence no submit signals */

static VkResult wait_fence(void *unused) {
    (void)unused;
    return vkWaitForFences(device, 1, &never, VK_TRUE, 300000000u);
}

static VkResult wait_semaphore(void *context) {
    const uint64_t one = 1;
    const VkSemaphoreWaitInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                                      .pSemaphores = context, .pValues = &one};
    return vkWaitSemaphores(device, &info, 300000000u);
}

static VkResult wait_queue(void *unused) {
    (void)unused;
    return vkQueueWaitIdle(queue);
}

static VkResult wait_device(void *unused) {
    (void)unused;
    return vkDeviceWaitIdle(device);
}

struct query {
    VkQueryPool pool;
    uint64_t result[2];  /* the timestamp and its availability */
};

static VkResult wait_query(void *context) {
    struct query *query = context;
    return vkGetQueryPoolResults(device, query->pool, 0, 1, sizeof query->result, query->result, sizeof query->result,
                                 VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT |
                                     VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
}

static VkSemaphore semaphores[8];  /* the timeline semaphores made, for their destruction */
static unsigned made;

static VkSemaphore timeline(void) {
    const VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    const VkSemaphoreCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
    check("vkCreateSemaphore", vkCreateSemaphore(device, &info, NULL, &semaphores[made]));
    return semaphores[made++];
}

/* A batch of `commands` that waits until `semaphore` reaches `value`. */
static void submit_waiting(VkSemaphore semaphore, uint64_t value, VkCommandBuffer commands, VkFence fence) {
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    const VkTimelineSemaphoreSubmitInfo values = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                                  .waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &value};
    const VkSubmitInfo batch = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &values, .waitSemaphoreCount = 1,
                                .pWaitSemaphores = &semaphore, .pWaitDstStageMask = &stage, .commandBufferCount = 1,
                                .pCommandBuffers = &commands};
    fprintf(stderr, "vulkan-queue-test: vkQueueSubmit, waiting for %llu\n", (unsigned long long)value);
    check("vkQueueSubmit", vkQueueSubmit(queue, 1, &batch, fence));
}

static VkBool32 messages(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
                         const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    (void)severity, (void)types;
    printf("the app's messenger: \"%s\", on the app's thread: %s, its context: %s\n", data->pMessage,
           pthread_equal(pthread_self(), program) ? "yes" : "no", user == &program ? "yes" : "no");
    return VK_FALSE;
}

int main(void) {
    program = pthread_self();
    signal(SIGUSR1, handled);
    const VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                           .pApplicationName = "vulkan-queue-test", .apiVersion = VK_API_VERSION_1_3};
    const char *extensions[] = {"VK_EXT_debug_utils"};
    const VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &application,
                                       .enabledExtensionCount = 1, .ppEnabledExtensionNames = extensions};
    VkInstance instance;
    check("vkCreateInstance", vkCreateInstance(&info, NULL, &instance));
    VkPhysicalDevice physical;
    uint32_t count = 1;
    VkResult result = vkEnumeratePhysicalDevices(instance, &count, &physical);
    if (result != VK_INCOMPLETE) {
        check("vkEnumeratePhysicalDevices", result);
    }
    const float priority = 1;
    const VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                                .queueCount = 1, .pQueuePriorities = &priority};
    const VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                                            .pQueueCreateInfos = &queue_info};
    check("vkCreateDevice", vkCreateDevice(physical, &device_info, NULL, &device));
    vkGetDeviceQueue(device, 0, 0, &queue);

    /* A fill into a small buffer, and a timestamp. */
    const VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 256,
                                            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buffer;
    check("vkCreateBuffer", vkCreateBuffer(device, &buffer_info, NULL, &buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    const VkMemoryAllocateInfo allocate_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                                .allocationSize = requirements.size,
                                                .memoryTypeIndex = (uint32_t)__builtin_ctz(requirements.memoryTypeBits)};
    VkDeviceMemory memory;
    check("vkAllocateMemory", vkAllocateMemory(device, &allocate_info, NULL, &memory));
    check("vkBindBufferMemory", vkBindBufferMemory(device, buffer, memory, 0));
    const VkQueryPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                             .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 1};
    struct query query = {0};
    check("vkCreateQueryPool", vkCreateQueryPool(device, &pool_info, NULL, &query.pool));
    const VkCommandPoolCreateInfo command_pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandPool command_pool;
    check("vkCreateCommandPool", vkCreateCommandPool(device, &command_pool_info, NULL, &command_pool));
    const VkCommandBufferAllocateInfo command_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                      .commandPool = command_pool,
                                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2};
    VkCommandBuffer commands[2];
    check("vkAllocateCommandBuffers", vkAllocateCommandBuffers(device, &command_info, commands));
    const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check("vkBeginCommandBuffer", vkBeginCommandBuffer(commands[0], &begin));
    vkCmdFillBuffer(commands[0], buffer, 0, 256, 0x01020304);
    check("vkEndCommandBuffer", vkEndCommandBuffer(commands[0]));
    check("vkBeginCommandBuffer", vkBeginCommandBuffer(commands[1], &begin));
    vkCmdResetQueryPool(commands[1], query.pool, 0, 1);
    vkCmdWriteTimestamp(commands[1], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query.pool, 0);
    check("vkEndCommandBuffer", vkEndCommandBuffer(commands[1]));
    const VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence done;
    check("vkCreateFence", vkCreateFence(device, &fence_info, NULL, &done));
    check("vkCreateFence", vkCreateFence(device, &fence_info, NULL, &never));

    /* "Scheduled", while the program's only thread waits for the hook. */
    const VkSemaphore gate = timeline();
    submit_waiting(gate, SCHEDULED_VALUE, commands[0], done);
    const uint64_t after = SCHEDULED_VALUE + 1;
    const VkSemaphoreWaitInfo gate_wait = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                                           .pSemaphores = &gate, .pValues = &after};
    result = vkWaitSemaphores(device, &gate_wait, 10000000000u);
    printf("the hook let the program go on: %s", result == VK_SUCCESS ? "yes" : "no");
    printf(", the batch done: %s\n", vkWaitForFences(device, 1, &done, VK_TRUE, 1000000000u) == VK_SUCCESS ? "yes" : "no");

    /* The waits in slices, each nudged after 50 ms: a fence no submit signals, a timeline semaphore nothing
       signals, and batches that wait for a timeline semaphore the nudge signals after 150 ms. */
    nudged("vkWaitForFences", wait_fence, NULL, VK_NULL_HANDLE, 300);
    VkSemaphore semaphore = timeline();
    nudged("vkWaitSemaphores", wait_semaphore, &semaphore, VK_NULL_HANDLE, 300);
    semaphore = timeline();
    submit_waiting(semaphore, 1, commands[0], VK_NULL_HANDLE);
    nudged("vkQueueWaitIdle", wait_queue, NULL, semaphore, 150);
    semaphore = timeline();
    submit_waiting(semaphore, 1, commands[0], VK_NULL_HANDLE);
    nudged("vkDeviceWaitIdle", wait_device, NULL, semaphore, 150);
    semaphore = timeline();
    submit_waiting(semaphore, 1, commands[1], VK_NULL_HANDLE);
    nudged("vkGetQueryPoolResults", wait_query, &query, semaphore, 150);
    printf("the timestamp available: %s\n", query.result[1] == 1 ? "yes" : "no");

    /* A message of the app's, through MoltenVK's messenger and graphics/host's callback. */
    const VkDebugUtilsMessengerCreateInfoEXT messenger_info = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, .pfnUserCallback = messages,
        .pUserData = &program};
    PFN_vkCreateDebugUtilsMessengerEXT create_messenger =
        (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger =
        (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
    PFN_vkSubmitDebugUtilsMessageEXT submit_message =
        (PFN_vkSubmitDebugUtilsMessageEXT)vkGetInstanceProcAddr(instance, "vkSubmitDebugUtilsMessageEXT");
    VkDebugUtilsMessengerEXT messenger;
    check("vkCreateDebugUtilsMessengerEXT", create_messenger(instance, &messenger_info, NULL, &messenger));
    const VkDebugUtilsMessengerCallbackDataEXT data = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT,
                                                       .pMessage = "a message of the app"};
    submit_message(instance, VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
                   &data);
    destroy_messenger(instance, messenger, NULL);

    check("vkDeviceWaitIdle", vkDeviceWaitIdle(device));
    while (made > 0) {
        vkDestroySemaphore(device, semaphores[--made], NULL);
    }
    vkDestroyFence(device, never, NULL);
    vkDestroyFence(device, done, NULL);
    vkDestroyCommandPool(device, command_pool, NULL);
    vkDestroyQueryPool(device, query.pool, NULL);
    vkDestroyBuffer(device, buffer, NULL);
    vkFreeMemory(device, memory, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    printf("done\n");
    return 0;
}
