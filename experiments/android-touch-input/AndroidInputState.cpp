#include "AndroidInputState.h"

#include <algorithm>
#include <cmath>

namespace mta::mobile
{
    namespace
    {
        bool Finite(Point p)
        {
            return std::isfinite(p.x) && std::isfinite(p.y);
        }
        bool Valid(Rect r)
        {
            return Finite({r.left, r.top}) && Finite({r.right, r.bottom}) && r.left < r.right && r.top < r.bottom;
        }
        bool Unit(Rect r)
        {
            return Valid(r) && r.left >= 0 && r.top >= 0 && r.right <= 1 && r.bottom <= 1;
        }
        bool Overlaps(Rect a, Rect b)
        {
            return std::max(a.left, b.left) < std::min(a.right, b.right) && std::max(a.top, b.top) < std::min(a.bottom, b.bottom);
        }
        bool Contains(Rect r, Point p)
        {
            return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
        }
    }  // namespace

    bool AndroidInputState::Configure(const InputLayout& layout)
    {
        // A resize/rotation invalidates old pointer coordinates even if the new layout is rejected.
        Reset(ResetReason::Layout);
        configured_ = false;
        if (!std::isfinite(layout.width) || !std::isfinite(layout.height) || layout.width < 1 || layout.height < 1 || layout.width > 1000000 ||
            layout.height > 1000000 || !Valid(layout.safe_area) || layout.safe_area.left < 0 || layout.safe_area.top < 0 ||
            layout.safe_area.right > layout.width || layout.safe_area.bottom > layout.height || layout.safe_area.right - layout.safe_area.left < 1 ||
            layout.safe_area.bottom - layout.safe_area.top < 1 || !Unit(layout.movement_zone) || !Unit(layout.look_zone) || !Unit(layout.action_zone) ||
            Overlaps(layout.movement_zone, layout.look_zone) || Overlaps(layout.movement_zone, layout.action_zone) ||
            Overlaps(layout.look_zone, layout.action_zone) || !std::isfinite(layout.movement_radius) || layout.movement_radius < 0.001f ||
            layout.movement_radius > 1 || !std::isfinite(layout.deadzone) || layout.deadzone < 0 || layout.deadzone >= 1)
            return false;
        layout_ = layout;
        configured_ = true;
        return true;
    }

    void AndroidInputState::ClearOwners()
    {
        owners_ = {};
        movement_ = {};
    }

    void AndroidInputState::Reset(ResetReason reason)
    {
        ClearOwners();
        look_delta_ = {};
        reset_reason_ = reason;
        ++epoch_;
    }

    void AndroidInputState::SetFocused(bool focused)
    {
        if (focused_ != focused)
        {
            focused_ = focused;
            Reset(ResetReason::Focus);
        }
    }
    void AndroidInputState::SetPaused(bool paused)
    {
        if (paused_ != paused)
        {
            paused_ = paused;
            Reset(ResetReason::Pause);
        }
    }
    void AndroidInputState::SetImeVisible(bool visible)
    {
        if (ime_visible_ != visible)
        {
            ime_visible_ = visible;
            Reset(ResetReason::Ime);
        }
    }

    Point AndroidInputState::Normalized(Point pixel) const
    {
        const auto& r = layout_.safe_area;
        return {(pixel.x - r.left) / (r.right - r.left), (pixel.y - r.top) / (r.bottom - r.top)};
    }

    void AndroidInputState::Release(std::int32_t id)
    {
        for (std::size_t role = 0; role < owners_.size(); ++role)
            if (owners_[role].id == id)
            {
                owners_[role] = {};
                if (role == 0)
                    movement_ = {};
            }
    }

    void AndroidInputState::Update(const PointerSample& pointer)
    {
        const auto& r = layout_.safe_area;
        Point       clamped{std::clamp(pointer.position.x, r.left, r.right), std::clamp(pointer.position.y, r.top, r.bottom)};
        if (owners_[0].id == pointer.id)
        {
            const auto& anchor = owners_[0].anchor;
            const float radius = layout_.movement_radius * std::min(r.right - r.left, r.bottom - r.top);
            Point       delta{(clamped.x - anchor.x) / radius, (clamped.y - anchor.y) / radius};
            const float magnitude = std::hypot(delta.x, delta.y);
            const float strength = (std::min(magnitude, 1.0f) - layout_.deadzone) / (1.0f - layout_.deadzone);
            movement_ = magnitude > layout_.deadzone ? Point{delta.x / magnitude * strength, delta.y / magnitude * strength} : Point{};
        }
        if (owners_[1].id == pointer.id)
        {
            auto& previous = owners_[1].previous;
            // Bound backlog if the consumer temporarily stops pulsing; lifecycle resets still discard it.
            look_delta_.x = std::clamp(look_delta_.x + (clamped.x - previous.x) / (r.right - r.left), -8.0f, 8.0f);
            look_delta_.y = std::clamp(look_delta_.y + (clamped.y - previous.y) / (r.bottom - r.top), -8.0f, 8.0f);
            previous = clamped;
        }
    }

    bool AndroidInputState::Apply(MotionAction action, std::span<const PointerSample> pointers, std::int32_t changed_id)
    {
        if (action == MotionAction::Cancel)
        {
            Reset(ResetReason::Cancel);
            return true;
        }
        if (!Enabled())
            return false;
        auto malformed = [this]
        {
            Reset(ResetReason::Malformed);
            return false;
        };
        if (pointers.empty() || pointers.size() > MaxPointers)
            return malformed();
        const PointerSample* changed = nullptr;
        for (std::size_t i = 0; i < pointers.size(); ++i)
        {
            const auto& p = pointers[i];
            if (p.id < 0 || !Finite(p.position))
                return malformed();
            for (std::size_t j = 0; j < i; ++j)
                if (p.id == pointers[j].id)
                    return malformed();
            if (p.id == changed_id)
                changed = &p;
        }
        if (action != MotionAction::Move && !changed)
            return malformed();
        if ((action == MotionAction::Begin || action == MotionAction::End) && pointers.size() != 1)
            return malformed();
        if (action == MotionAction::Begin)
            ClearOwners();
        // Full snapshots heal a missing up without assigning another finger to a released control.
        for (const auto& owner : owners_)
        {
            if (owner.id < 0)
                continue;
            bool present = false;
            for (const auto& p : pointers)
                present |= p.id == owner.id;
            if (!present)
                Release(owner.id);
        }
        for (const auto& p : pointers)
            Update(p);
        if (action == MotionAction::Begin || action == MotionAction::PointerDown)
        {
            for (const auto& owner : owners_)
                if (owner.id == changed_id)
                    return malformed();
            if (!Contains(layout_.safe_area, changed->position))
                return true;
            const Point normalized = Normalized(changed->position);
            const Rect  zones[]{layout_.movement_zone, layout_.look_zone, layout_.action_zone};
            for (std::size_t role = 0; role < owners_.size(); ++role)
                if (owners_[role].id < 0 && Contains(zones[role], normalized))
                {
                    owners_[role] = {changed_id, changed->position, changed->position};
                    break;
                }
        }
        if (action == MotionAction::PointerUp)
            Release(changed_id);
        if (action == MotionAction::End)
            ClearOwners();
        return true;
    }

    InputFrame AndroidInputState::ConsumeFrame()
    {
        InputFrame frame{movement_, look_delta_, owners_[2].id >= 0, Enabled(), epoch_, ++sequence_};
        look_delta_ = {};
        return frame;
    }

    bool AndroidInputState::HasCapturedPointers() const
    {
        for (const auto& owner : owners_)
            if (owner.id >= 0)
                return true;
        return false;
    }
}  // namespace mta::mobile
