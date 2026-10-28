#include "AndroidInputAdapter.h"

namespace mta::mobile
{
    NativeInputResult HandleAndroidInput(AndroidInputState& state, const AInputEvent* event)
    {
        if (!event || AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION ||
            (AInputEvent_getSource(event) & AINPUT_SOURCE_TOUCHSCREEN) != AINPUT_SOURCE_TOUCHSCREEN)
            return NativeInputResult::NotHandled;
        const auto reject = [&state]
        {
            state.Reset(ResetReason::Malformed);
            return NativeInputResult::Rejected;
        };
        const auto raw_action = AMotionEvent_getAction(event);
        const auto action = raw_action & AMOTION_EVENT_ACTION_MASK;
        const bool captured_before = state.HasCapturedPointers();
        if (action == AMOTION_EVENT_ACTION_CANCEL)
        {
            state.Reset(ResetReason::Cancel);
            return captured_before ? NativeInputResult::Handled : NativeInputResult::NotHandled;
        }
        MotionAction translated;
        switch (action)
        {
            case AMOTION_EVENT_ACTION_DOWN:
                translated = MotionAction::Begin;
                break;
            case AMOTION_EVENT_ACTION_POINTER_DOWN:
                translated = MotionAction::PointerDown;
                break;
            case AMOTION_EVENT_ACTION_MOVE:
                translated = MotionAction::Move;
                break;
            case AMOTION_EVENT_ACTION_POINTER_UP:
                translated = MotionAction::PointerUp;
                break;
            case AMOTION_EVENT_ACTION_UP:
                translated = MotionAction::End;
                break;
            default:
                return NativeInputResult::NotHandled;
        }
        const auto count = AMotionEvent_getPointerCount(event);
        if (!count || count > AndroidInputState::MaxPointers)
            return reject();
        const auto changed_index = static_cast<std::size_t>((raw_action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        if (translated != MotionAction::Move && changed_index >= count)
            return reject();
        std::array<PointerSample, AndroidInputState::MaxPointers> samples{};
        for (std::size_t index = 0; index < count; ++index)
            samples[index] = {AMotionEvent_getPointerId(event, index), {AMotionEvent_getX(event, index), AMotionEvent_getY(event, index)}};
        const auto changed_id = translated == MotionAction::Move ? -1 : samples[changed_index].id;
        const bool applied = state.Apply(translated, std::span<const PointerSample>(samples.data(), count), changed_id);
        if (!applied)
            return state.Enabled() ? NativeInputResult::Rejected : NativeInputResult::NotHandled;
        return captured_before || state.HasCapturedPointers() ? NativeInputResult::Handled : NativeInputResult::NotHandled;
    }
}  // namespace mta::mobile
