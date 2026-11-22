#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "vk_api.h"

namespace vkr {

// Keeps the GPU busy between PyroWave decodes, so its governor keeps its clock up. Left to
// itself, the GPU clocks down until it's about 65% busy: on a Pixel 10 Pro decoding 60 PyroWave
// frames a second (a 60 fps stream, or a 120 fps one of a game running at 60), its clock moved
// between 512 and 1094 MHz and decodes took 8.1-16.8 ms (p10-p90) on the GPU, where with 120
// frames a second they took 7.9-8.3 ms. Its performance hint session couldn't raise the clock:
// its power HAL ignored the GPU time reported, even for a graphics pipeline session.
//
// Short compute dispatches run back to back, two in flight, on a queue of their own on a device
// of their own, at low global priority where the driver allows, so decodes and draws go ahead
// of them at the next dispatch at the latest. Each is sized to about chunkNs, from how often they
// finish. It costs power: the GPU runs at its top clock the whole time. The PyroWave GPU boost
// setting switches it on (on by default); adb shell setprop debug.moonlight.gpu_keep_busy
// overrides that setting.
//
// The priority doesn't keep a dispatch from running alongside a decode on the GPU's other cores:
// with 256 workgroups, enough for all of them, the Pixel 10 Pro stayed at 1094 MHz and decodes
// were steady, but took 13 ms (11.8-13.6 ms p10-p90) rather than 8. A few workgroups (groups)
// keep the GPU busy on a core or two and leave the rest to the decodes.
class GpuKeepBusy {
public:
    ~GpuKeepBusy();

    // Starts the work on a device of its own, made from instance and physicalDevice with one
    // queue of queueFamily. globalPriorityExt: the global priority extension to ask for low
    // priority with, or nullptr. groups: workgroups of 64 invocations in each dispatch. Returns
    // false if it couldn't start.
    bool start(const VkApi& vk, VkInstance instance, VkPhysicalDevice physicalDevice, uint32_t queueFamily,
               const char* globalPriorityExt, int64_t chunkNs, uint32_t groups);
    // Waits for the work in flight, then frees everything. Safe to call more than once.
    void stop();
    // Submits nothing more until resume(). Does not wait for chunks already on the GPU.
    void requestPause();
    // Finishes every chunk already submitted. Submits nothing until resume().
    void pause();
    void resume();
    // A shared-queue frame is queued and its draw fence is not armed yet. Chunks stay
    // unsubmitted while any such frame is outstanding. armBlitGate and releaseUnarmedBlit
    // each drop one. The frame's destructor releases a hold the draw never armed.
    void holdUnarmedBlit();
    void releaseUnarmedBlit();
    // Chunks started after resume wait until this fence signals. The fence belongs to
    // another device. retireBlit must run before that fence is reset, and it waits until
    // a wait already inside vkWaitForFences has returned, including while this thread is
    // stopping. generation is the submit's id. releaseHold drops one holdUnarmedBlit
    // under the same lock that publishes the fence.
    bool armBlitGate(PFN_vkWaitForFences wait, VkDevice device, VkFence fence, uint64_t generation,
                     bool releaseHold);
    void retireBlit(uint64_t generation);

private:
    static constexpr int kInFlight = 2;
    // Two sequential dispatches per submit keep Tegra X1 preemption at half-chunk granularity
    // while halving vkQueueSubmit and fence-wait frequency.
    static constexpr uint32_t kDispatchesPerSubmit = 2;
    static constexpr uint32_t kGroupSize = 64;  // keep_busy.comp's local_size_x

    bool createDevice(VkPhysicalDevice physicalDevice, uint32_t queueFamily, const char* globalPriorityExt);
    bool createResources(VkPhysicalDevice physicalDevice, uint32_t queueFamily);
    void destroy();
    void run();
    bool recordSlot(int slot, uint32_t iterations);
    bool submit(int slot);
    // Under pauseMutex_. Submits both slots. On failure marks the thread stopped.
    bool submitPair(std::unique_lock<std::mutex>& lock);
    // Under pauseMutex_. May unlock while a blit fence is waited. False when stopping.
    bool waitBlitGate(std::unique_lock<std::mutex>& lock);
    void stopRun(std::unique_lock<std::mutex>& lock);

    VkApi vk_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    bool lowPriority_ = false;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkShaderModule shader_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffers_[kInFlight] = {};
    VkFence fences_[kInFlight] = {};

    int64_t chunkNs_ = 0;
    uint32_t groups_ = 0;
    uint32_t iterations_ = 0;
    uint32_t recordedIterations_[kInFlight] = {};
    std::atomic<bool> quit_ {false};
    bool paused_ = false;  // Under pauseMutex_
    int inFlight_ = 0;     // Submitted chunks not yet waited. Under pauseMutex_
    std::mutex pauseMutex_;
    std::condition_variable pauseCv_;
    std::thread thread_;

    // Render-device fence the next chunks wait for. Under pauseMutex_.
    PFN_vkWaitForFences blitWait_ = nullptr;
    VkDevice blitDevice_ = VK_NULL_HANDLE;
    VkFence blitFence_ = VK_NULL_HANDLE;
    uint64_t blitGeneration_ = 0;
    uint64_t blitWaitedGeneration_ = 0;
    uint64_t blitWaitingGeneration_ = 0;
    int blitWaiters_ = 0;
    // Shared-queue frames queued whose draw fence is not armed yet. Under pauseMutex_.
    int unarmedBlits_ = 0;
};

}  // namespace vkr
