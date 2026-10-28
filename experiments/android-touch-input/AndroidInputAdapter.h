#pragma once

#include "AndroidInputState.h"
#include <android/input.h>

namespace mta::mobile
{
    enum class NativeInputResult
    {
        NotHandled,
        Handled,
        Rejected
    };
    // Borrowed event: consumed synchronously, never retained. Host still owns AInputQueue_finishEvent.
    NativeInputResult HandleAndroidInput(AndroidInputState& state, const AInputEvent* event);
}  // namespace mta::mobile
