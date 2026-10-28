#include "AndroidInputState.h"
#include <jni.h>

#include <array>
#include <limits>
#include <memory>
#include <mutex>

namespace
{
    using namespace mta::mobile;
    struct Context
    {
        std::mutex        mutex;
        AndroidInputState state;
        Context()
        {
            state.SetFocused(false);
            state.SetPaused(true);
        }
    };
    struct Slot
    {
        jlong                    id{};
        std::shared_ptr<Context> context;
    };
    std::mutex           registry_mutex;
    std::array<Slot, 16> slots;
    jlong                next_id = 1;

    std::shared_ptr<Context> Find(jlong id)
    {
        std::lock_guard lock(registry_mutex);
        for (const auto& slot : slots)
            if (id > 0 && slot.id == id)
                return slot.context;
        return {};
    }

    template <typename Function>
    jboolean With(jlong id, Function function) noexcept
    {
        try
        {
            auto context = Find(id);
            if (!context)
                return JNI_FALSE;
            std::lock_guard lock(context->mutex);
            return function(context->state) ? JNI_TRUE : JNI_FALSE;
        }
        catch (...)
        {
            return JNI_FALSE;
        }
    }

    jboolean Malformed(jlong id)
    {
        return With(id,
                    [](AndroidInputState& state)
                    {
                        state.Reset(ResetReason::Malformed);
                        return false;
                    });
    }
}  // namespace

extern "C"
{
    JNIEXPORT jlong JNICALL Java_org_mta_mobile_input_NativeInputBridge_nativeCreate(JNIEnv*, jclass)
    {
        try
        {
            std::lock_guard lock(registry_mutex);
            if (next_id == std::numeric_limits<jlong>::max())
                return 0;
            for (auto& slot : slots)
                if (!slot.context)
                {
                    auto context = std::make_shared<Context>();
                    slot = {next_id++, std::move(context)};
                    return slot.id;
                }
        }
        catch (...)
        {
        }
        return 0;
    }

    JNIEXPORT void JNICALL Java_org_mta_mobile_input_NativeInputBridge_nativeDestroy(JNIEnv*, jclass, jlong id)
    {
        try
        {
            std::lock_guard lock(registry_mutex);
            for (auto& slot : slots)
                if (slot.id == id)
                {
                    slot = {};
                    break;
                }
        }
        catch (...)
        {
        }
    }

    JNIEXPORT jboolean JNICALL Java_org_mta_mobile_input_NativeInputBridge_nativeConfigure(JNIEnv* env, jclass, jlong id, jfloatArray input)
    {
        std::array<jfloat, 20> values{};
        if (!input || env->GetArrayLength(input) != static_cast<jsize>(values.size()))
            return With(id, [](AndroidInputState& state) { return state.Configure({}); });
        env->GetFloatArrayRegion(input, 0, static_cast<jsize>(values.size()), values.data());
        if (env->ExceptionCheck())
            return With(id, [](AndroidInputState& state) { return state.Configure({}); });
        InputLayout layout{values[0],
                           values[1],
                           {values[2], values[3], values[4], values[5]},
                           {values[6], values[7], values[8], values[9]},
                           {values[10], values[11], values[12], values[13]},
                           {values[14], values[15], values[16], values[17]},
                           values[18],
                           values[19]};
        return With(id, [&](AndroidInputState& state) { return state.Configure(layout); });
    }

    JNIEXPORT jboolean JNICALL Java_org_mta_mobile_input_NativeInputBridge_nativeFeed(JNIEnv* env, jclass, jlong id, jint action, jint changed_id,
                                                                                      jintArray ids, jfloatArray xs, jfloatArray ys, jint count)
    {
        // Android MotionEvent action constants; no Java object is cast to AInputEvent.
        MotionAction translated;
        switch (action)
        {
            case 0:
                translated = MotionAction::Begin;
                break;
            case 1:
                translated = MotionAction::End;
                break;
            case 2:
                translated = MotionAction::Move;
                break;
            case 3:
            case 4:
                return With(id,
                            [](AndroidInputState& state)
                            {
                                state.Reset(ResetReason::Cancel);
                                return true;
                            });
            case 5:
                translated = MotionAction::PointerDown;
                break;
            case 6:
                translated = MotionAction::PointerUp;
                break;
            default:
                return Malformed(id);
        }
        if (count <= 0 || count > static_cast<jint>(AndroidInputState::MaxPointers) || !ids || !xs || !ys || env->GetArrayLength(ids) < count ||
            env->GetArrayLength(xs) < count || env->GetArrayLength(ys) < count)
            return Malformed(id);
        std::array<jint, AndroidInputState::MaxPointers>   identifiers{};
        std::array<jfloat, AndroidInputState::MaxPointers> x{}, y{};
        env->GetIntArrayRegion(ids, 0, count, identifiers.data());
        if (env->ExceptionCheck())
            return Malformed(id);
        env->GetFloatArrayRegion(xs, 0, count, x.data());
        if (env->ExceptionCheck())
            return Malformed(id);
        env->GetFloatArrayRegion(ys, 0, count, y.data());
        if (env->ExceptionCheck())
            return Malformed(id);
        std::array<PointerSample, AndroidInputState::MaxPointers> pointers{};
        for (jint index = 0; index < count; ++index)
            pointers[index] = {identifiers[index], {x[index], y[index]}};
        return With(id, [&](AndroidInputState& state)
                    { return state.Apply(translated, std::span<const PointerSample>(pointers.data(), static_cast<std::size_t>(count)), changed_id); });
    }

    JNIEXPORT jboolean JNICALL Java_org_mta_mobile_input_NativeInputBridge_nativeGate(JNIEnv*, jclass, jlong id, jint gate, jboolean enabled)
    {
        return With(id,
                    [&](AndroidInputState& state)
                    {
                        switch (gate)
                        {
                            case 0:
                                state.SetFocused(enabled == JNI_TRUE);
                                break;
                            case 1:
                                state.SetPaused(enabled == JNI_TRUE);
                                break;
                            case 2:
                                state.SetImeVisible(enabled == JNI_TRUE);
                                break;
                            default:
                                state.Reset(ResetReason::Malformed);
                                return false;
                        }
                        return true;
                    });
    }

    JNIEXPORT jboolean JNICALL Java_org_mta_mobile_input_NativeInputBridge_nativeReset(JNIEnv*, jclass, jlong id)
    {
        return With(id,
                    [](AndroidInputState& state)
                    {
                        state.Reset();
                        return true;
                    });
    }

    JNIEXPORT jboolean JNICALL Java_org_mta_mobile_input_NativeInputBridge_nativeConsume(JNIEnv* env, jclass, jlong id, jfloatArray axes, jlongArray metadata)
    {
        if (!axes || !metadata || env->GetArrayLength(axes) != 4 || env->GetArrayLength(metadata) != 4)
            return Malformed(id);
        InputFrame frame{};
        if (!With(id,
                  [&](AndroidInputState& state)
                  {
                      frame = state.ConsumeFrame();
                      return true;
                  }))
            return JNI_FALSE;
        const std::array<jfloat, 4> values{frame.movement.x, frame.movement.y, frame.look_delta.x, frame.look_delta.y};
        const std::array<jlong, 4>  tags{frame.action ? 1 : 0, frame.enabled ? 1 : 0, static_cast<jlong>(frame.epoch), static_cast<jlong>(frame.sequence)};
        // JNI array access happens outside reducer/registry locks; no JVM callbacks under those locks.
        env->SetFloatArrayRegion(axes, 0, static_cast<jsize>(values.size()), values.data());
        if (env->ExceptionCheck())
            return Malformed(id);
        env->SetLongArrayRegion(metadata, 0, static_cast<jsize>(tags.size()), tags.data());
        if (env->ExceptionCheck())
            return Malformed(id);
        return JNI_TRUE;
    }
}
