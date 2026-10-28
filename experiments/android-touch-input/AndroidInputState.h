#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace mta::mobile
{
    struct Point
    {
        float x{}, y{};
    };

    struct Rect
    {
        float left{}, top{}, right{}, bottom{};
    };

    // Pixel coordinates share the native input surface origin. Zones use [0,1] within safe_area.
    struct InputLayout
    {
        float width{}, height{};
        Rect  safe_area, movement_zone, look_zone, action_zone;
        float movement_radius{0.15f};  // fraction of the shorter safe-area dimension
        float deadzone{0.15f};         // radial fraction of movement radius
    };

    struct PointerSample
    {
        std::int32_t id;
        Point        position;
    };

    enum class MotionAction
    {
        Begin,
        PointerDown,
        Move,
        PointerUp,
        End,
        Cancel
    };
    enum class ResetReason
    {
        Explicit,
        Cancel,
        Malformed,
        Layout,
        Focus,
        Pause,
        Ime
    };

    struct InputFrame
    {
        Point         movement;    // [-1,1], x right / y down; engine-specific signs are a later mapping
        Point         look_delta;  // relative to safe-area width/height, consumed once
        bool          action{};
        bool          enabled{};
        std::uint64_t epoch{}, sequence{};
    };

    class AndroidInputState
    {
    public:
        static constexpr std::size_t MaxPointers = 32;
        bool                         Configure(const InputLayout& layout);
        void                         SetFocused(bool focused);
        void                         SetPaused(bool paused);
        void                         SetImeVisible(bool visible);
        void                         Reset(ResetReason reason = ResetReason::Explicit);
        // Complete native pointer snapshot, including the pointer being lifted for PointerUp/End.
        bool        Apply(MotionAction action, std::span<const PointerSample> pointers, std::int32_t changed_id = -1);
        InputFrame  ConsumeFrame();
        bool        Enabled() const { return configured_ && focused_ && !paused_ && !ime_visible_; }
        bool        HasCapturedPointers() const;
        ResetReason LastReset() const { return reset_reason_; }

    private:
        struct Owner
        {
            std::int32_t id{-1};
            Point        anchor{}, previous{};
        };
        void                 ClearOwners();
        void                 Release(std::int32_t id);
        void                 Update(const PointerSample& pointer);
        Point                Normalized(Point pixel) const;
        InputLayout          layout_{};
        std::array<Owner, 3> owners_{};  // movement, look, action
        Point                movement_{}, look_delta_{};
        bool                 configured_{}, focused_{true}, paused_{}, ime_visible_{};
        std::uint64_t        epoch_{}, sequence_{};
        ResetReason          reset_reason_{ResetReason::Explicit};
    };
}  // namespace mta::mobile
