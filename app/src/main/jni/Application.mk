# Application.mk for Moonlight

# Our minimum version is Android 5.0
APP_PLATFORM := android-21

# The Vulkan renderer is C++. moonlight-core stays C.
APP_STL := c++_static

# We support 16KB pages
APP_SUPPORT_FLEXIBLE_PAGE_SIZES := true
