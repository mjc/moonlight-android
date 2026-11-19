LOCAL_PATH := $(call my-dir)

# libpyrowave-shared.so is packaged for arm64-v8a only.
ifeq ($(TARGET_ARCH_ABI),arm64-v8a)

include $(CLEAR_VARS)

LOCAL_MODULE := vulkan_renderer
LOCAL_SRC_FILES := \
    frame_pacer.cpp \
    gpu_keep_busy.cpp \
    ndk_api.cpp \
    pacer_trace.cpp \
    perf_hint.cpp \
    present_scheduler.cpp \
    pyrowave_decoder.cpp \
    vk_api.cpp \
    vulkan_bridge.cpp \
    vulkan_renderer.cpp \

LOCAL_C_INCLUDES := $(LOCAL_PATH) $(LOCAL_PATH)/../moonlight-core/moonlight-common-c/src
LOCAL_CPPFLAGS += -std=c++17 -Wall

# For LiSetPartialFrameDeadline(), which tells moonlight-common-c when to cut short PyroWave frames
# still arriving (VulkanRenderer::updatePartialDeadline()). MoonBridge loads it first.
LOCAL_SHARED_LIBRARIES := moonlight-core

# Vulkan and the API 26+ NDK libraries are loaded at runtime (see ndk_api.h and vk_api.h),
# since the app's native code is built for API 21
LOCAL_LDLIBS := -llog -landroid -ldl

include $(BUILD_SHARED_LIBRARY)

endif
