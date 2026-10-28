#include "AndroidInputState.h"
#ifdef __ANDROID__
    #include "AndroidInputAdapter.h"
#endif

#include <cmath>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace mta::mobile;

namespace
{
    void Require(bool value, const char* message)
    {
        if (!value)
            throw std::runtime_error(message);
    }
    void Near(float actual, float expected, const char* message)
    {
        Require(std::abs(actual - expected) < 0.0001f, message);
    }
    InputLayout Layout()
    {
        return {1000, 1000, {100, 100, 900, 900}, {0, 0, 0.45f, 1}, {0.55f, 0, 1, 0.7f}, {0.55f, 0.75f, 1, 1}, 0.25f, 0.2f};
    }
    void Apply(AndroidInputState& state, MotionAction action, std::initializer_list<PointerSample> samples, std::int32_t changed = -1)
    {
        Require(state.Apply(action, std::span<const PointerSample>(samples.begin(), samples.size()), changed), "event rejected");
    }
    void Neutral(InputFrame frame)
    {
        Near(frame.movement.x, 0, "movement x stuck");
        Near(frame.movement.y, 0, "movement y stuck");
        Near(frame.look_delta.x, 0, "look x stuck");
        Near(frame.look_delta.y, 0, "look y stuck");
        Require(!frame.action, "action stuck");
    }

    void MultiTouchTrace()
    {
        AndroidInputState state;
        Require(state.Configure(Layout()), "layout rejected");
        Apply(state, MotionAction::Begin, {{7, {260, 500}}}, 7);
        Apply(state, MotionAction::PointerDown, {{7, {260, 500}}, {42, {740, 260}}}, 42);
        Apply(state, MotionAction::PointerDown, {{3, {740, 780}}, {42, {740, 260}}, {7, {260, 500}}}, 3);
        // Index 0 changes from movement to action to look. Ownership must follow IDs.
        Apply(state, MotionAction::Move, {{42, {660, 340}}, {7, {380, 500}}, {3, {740, 780}}});
        auto frame = state.ConsumeFrame();
        Near(frame.movement.x, 0.5f, "radial deadzone/rescale");
        Near(frame.movement.y, 0, "move axis");
        Near(frame.look_delta.x, -0.1f, "look x by pointer ID");
        Near(frame.look_delta.y, 0.1f, "look y by pointer ID");
        Require(frame.action && frame.enabled, "third finger action missing");
        const auto next = state.ConsumeFrame();
        Near(next.look_delta.x, 0, "delta consumed twice");
        Near(next.look_delta.y, 0, "delta consumed twice");
        Near(next.movement.x, 0.5f, "held movement lost");
        Require(next.action && next.sequence > frame.sequence, "held action/sequence");
        Apply(state, MotionAction::PointerUp, {{3, {740, 780}}, {7, {380, 500}}, {42, {660, 340}}}, 42);
        Apply(state, MotionAction::Move, {{3, {740, 780}}, {7, {420, 500}}});
        frame = state.ConsumeFrame();
        Near(frame.movement.x, 0.75f, "index reorder after lift stole movement");
        Near(frame.look_delta.x, 0, "lifted look still active");
        Require(frame.action, "lifting camera released action");
        Apply(state, MotionAction::PointerUp, {{7, {420, 500}}, {3, {740, 780}}}, 7);
        Apply(state, MotionAction::End, {{3, {740, 780}}}, 3);
        Neutral(state.ConsumeFrame());
    }

    void CancellationAndLifecycle()
    {
        AndroidInputState state;
        Require(state.Configure(Layout()), "layout");
        auto held = [&]
        {
            Apply(state, MotionAction::Begin, {{1, {260, 500}}}, 1);
            Apply(state, MotionAction::PointerDown, {{1, {380, 500}}, {2, {740, 260}}}, 2);
            Apply(state, MotionAction::PointerDown, {{1, {380, 500}}, {2, {660, 340}}, {8, {740, 780}}}, 8);
        };
        held();
        const auto old_epoch = state.ConsumeFrame().epoch;
        Apply(state, MotionAction::Cancel, {});
        auto frame = state.ConsumeFrame();
        Neutral(frame);
        Require(frame.epoch > old_epoch, "cancel epoch");
        held();
        state.SetFocused(false);
        Neutral(state.ConsumeFrame());
        Require(!state.Enabled(), "lost focus enabled");
        Require(!state.Apply(MotionAction::Begin, std::array{PointerSample{1, {260, 500}}}, 1), "background accepted input");
        state.SetFocused(true);
        Neutral(state.ConsumeFrame());
        held();
        state.SetPaused(true);
        Neutral(state.ConsumeFrame());
        Require(!state.Enabled(), "paused enabled");
        state.SetPaused(false);
        Neutral(state.ConsumeFrame());
        held();
        state.SetImeVisible(true);
        Neutral(state.ConsumeFrame());
        Require(!state.Enabled(), "IME enabled gameplay");
        state.SetImeVisible(false);
        Neutral(state.ConsumeFrame());
        // ACTION_MOVE after resume must not reacquire fingers from an old gesture.
        Apply(state, MotionAction::Move, {{1, {380, 500}}, {2, {660, 340}}, {8, {740, 780}}});
        Neutral(state.ConsumeFrame());
        held();
        state.Reset();
        Neutral(state.ConsumeFrame());  // host Android back/UI transition
    }

    void ViewportAndRotation()
    {
        AndroidInputState state;
        Require(state.Configure(Layout()), "layout");
        Apply(state, MotionAction::Begin, {{1, {40, 500}}}, 1);  // outside safe area, no capture
        Apply(state, MotionAction::Move, {{1, {260, 500}}});
        Neutral(state.ConsumeFrame());
        Apply(state, MotionAction::Begin, {{2, {260, 500}}}, 2);
        Apply(state, MotionAction::Move, {{2, {280, 500}}});
        Neutral(state.ConsumeFrame());  // within radial deadzone
        Apply(state, MotionAction::Move, {{2, {9000, -1000}}});
        auto frame = state.ConsumeFrame();
        Near(std::hypot(frame.movement.x, frame.movement.y), 1, "diagonal movement not unit bounded");
        auto rotated = Layout();
        rotated.width = 1400;
        rotated.height = 800;
        rotated.safe_area = {100, 100, 1300, 700};
        Require(state.Configure(rotated), "rotation layout");
        Neutral(state.ConsumeFrame());
        Apply(state, MotionAction::Move, {{2, {400, 400}}});
        Neutral(state.ConsumeFrame());  // stale pointer cannot rebind
        Apply(state, MotionAction::Begin, {{9, {1060, 220}}}, 9);
        Apply(state, MotionAction::Move, {{9, {1180, 280}}});
        frame = state.ConsumeFrame();
        Near(frame.look_delta.x, 0.1f, "rotated width normalization");
        Near(frame.look_delta.y, 0.1f, "rotated height normalization");
        auto invalid = rotated;
        invalid.action_zone = invalid.look_zone;
        Require(!state.Configure(invalid), "overlapping zones accepted");
        Neutral(state.ConsumeFrame());
        Require(!state.Enabled(), "invalid layout enabled");
    }

    void MalformedAndMissingUp()
    {
        AndroidInputState state;
        Require(state.Configure(Layout()), "layout");
        Apply(state, MotionAction::Begin, {{11, {740, 780}}}, 11);
        Apply(state, MotionAction::Move, {{70, {300, 300}}});
        Neutral(state.ConsumeFrame());  // lost up heals held action
        Apply(state, MotionAction::Begin, {{4, {260, 500}}}, 4);
        const std::array duplicate{PointerSample{4, {260, 500}}, PointerSample{4, {740, 260}}};
        Require(!state.Apply(MotionAction::Move, duplicate), "duplicate pointer ID accepted");
        Neutral(state.ConsumeFrame());
        Apply(state, MotionAction::Begin, {{4, {260, 500}}}, 4);
        const std::array nan{PointerSample{4, {std::numeric_limits<float>::quiet_NaN(), 0}}};
        Require(!state.Apply(MotionAction::Move, nan), "NaN accepted");
        Neutral(state.ConsumeFrame());
        Require(state.LastReset() == ResetReason::Malformed, "malformed reset missing");
        const std::array one{PointerSample{4, {260, 500}}};
        Require(!state.Apply(MotionAction::PointerUp, one, 999), "unknown changed pointer accepted");
        std::array<PointerSample, AndroidInputState::MaxPointers + 1> too_many{};
        Require(!state.Apply(MotionAction::Move, too_many), "unbounded pointer count");
    }
}  // namespace

int main()
{
    try
    {
#ifdef __ANDROID__
        // Pull the real adapter object into this ELF so its NDK imports must resolve at link time.
        AndroidInputState no_event_state;
        Require(HandleAndroidInput(no_event_state, nullptr) == NativeInputResult::NotHandled, "null native event");
#endif
        MultiTouchTrace();
        CancellationAndLifecycle();
        ViewportAndRotation();
        MalformedAndMissingUp();
        std::cout << "PASS portable multitouch traces; native AInputEvent delivery/game/U1 NOT RUN\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
