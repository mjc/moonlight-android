#pragma once

#include <Limelight.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*PyrowaveFrameSubmitFn)(PDECODE_UNIT decodeUnit, void* context);

// Hands PyroWave frames to the Vulkan renderer without copying them through Java.
// moonlight-core cannot call the renderer, which is a separate library, so the renderer
// registers this instead. context remains valid until a later call with a null submit
// for that same context returns. That call waits until a submit already running has
// returned. One decode thread submits at a time; direct submit stays off for PyroWave.
void LiSetPyrowaveFrameSubmit(PyrowaveFrameSubmitFn submit, void* context);

#ifdef __cplusplus
}
#endif
