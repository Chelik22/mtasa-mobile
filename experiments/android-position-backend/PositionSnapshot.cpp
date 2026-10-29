#include "PositionSnapshot.h"

#include <cmath>
#include <limits>

namespace mobile::position
{
    namespace
    {
        bool Add(std::uint64_t base, std::uint64_t offset, std::uint64_t& result)
        {
            if (offset > std::numeric_limits<std::uint64_t>::max() - base)
                return false;
            result = base + offset;
            return true;
        }

        template <typename T>
        bool Read(const Memory& memory, std::uint64_t base, std::uint64_t offset, T& value)
        {
            std::uint64_t address;
            return Add(base, offset, address) && memory.Read(address, &value, sizeof(value));
        }

        bool Pointer(std::uint64_t address)
        {
            // Tagged/non-canonical and tiny addresses are rejected, not stripped or repaired.
            return address >= 4096 && address < (std::uint64_t{1} << 56) && address % 8 == 0;
        }

        struct Selection
        {
            std::uint64_t focusAddress{}, instanceAddress{}, playersAddress{}, ped{};
            std::uint32_t instance{};
            std::uint8_t  focus{}, slot{};
        };

        Result Select(const Memory& memory, std::uint64_t bias, Selection& value)
        {
            if (!Read(memory, bias, kFocusGot, value.focusAddress) || !Read(memory, bias, kInstanceGot, value.instanceAddress) ||
                !Read(memory, bias, kPlayersGot, value.playersAddress))
                return Result::read_unavailable;
            std::uint64_t focus, instance, players;
            if (!Add(bias, kFocus, focus) || !Add(bias, kInstance, instance) || !Add(bias, kPlayers, players) || value.focusAddress != focus ||
                value.instanceAddress != instance || value.playersAddress != players)
                return Result::invalid_binding;
            if (!memory.Read(focus, &value.focus, sizeof(value.focus)) || !memory.Read(instance, &value.instance, sizeof(value.instance)))
                return Result::read_unavailable;
            // FindPlayerPed(-1): instance==1 overrides focus. A server ElementID is never an index.
            value.slot = value.instance == 1 ? 1 : value.focus;
            if (value.slot > 1)
                return Result::invalid_selector;
            if (!Read(memory, players, std::uint64_t{value.slot} * 0x1d8, value.ped))
                return Result::read_unavailable;
            if (!value.ped)
                return Result::no_player;
            return Pointer(value.ped) ? Result::candidate_unverified : Result::invalid_pointer;
        }
    }

    Result ReadCandidate(const Memory& memory, std::uint64_t bias, Candidate& output)
    {
        output = {};
        Selection before{}, after{};
        Result    result = Select(memory, bias, before);
        if (result != Result::candidate_unverified)
            return result;
        std::uint64_t matrix, address;
        if (!Read(memory, before.ped, 0x18, matrix))
            return Result::read_unavailable;
        if (matrix && !Pointer(matrix))
            return Result::invalid_pointer;
        if (!Add(matrix ? matrix : before.ped, matrix ? 0x30 : 0x08, address))
            return Result::invalid_pointer;
        std::array<float, 3> xyz;
        if (!memory.Read(address, xyz.data(), sizeof(xyz)))
            return Result::read_unavailable;
        std::uint64_t finalMatrix;
        result = Select(memory, bias, after);
        if (result != Result::candidate_unverified)
            return result;
        if (!Read(memory, before.ped, 0x18, finalMatrix))
            return Result::read_unavailable;
        if (before.focus != after.focus || before.instance != after.instance || before.slot != after.slot || before.ped != after.ped || matrix != finalMatrix)
            return Result::changed;
        for (float component : xyz)
            if (!std::isfinite(component))
                return Result::nonfinite;
        // This rejects observed changes, but cannot detect ABA or prove object lifetime.
        output = {xyz, before.ped, matrix, before.slot};
        return Result::candidate_unverified;
    }
}
