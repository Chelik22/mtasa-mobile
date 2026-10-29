#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace mobile::position
{
    // These addresses belong only to the fingerprint documented in android-position-map.
    constexpr std::uint64_t kFocusGot = 0x837378;
    constexpr std::uint64_t kInstanceGot = 0x839a08;
    constexpr std::uint64_t kPlayersGot = 0x837358;
    constexpr std::uint64_t kFocus = 0xa1a8d0;
    constexpr std::uint64_t kInstance = 0xd0df30;
    constexpr std::uint64_t kPlayers = 0xa1a520;

    enum class Result
    {
        read_unavailable,
        invalid_binding,
        invalid_selector,
        no_player,
        invalid_pointer,
        changed,
        nonfinite,
        candidate_unverified,
    };

    struct Candidate
    {
        std::array<float, 3> xyz{};
        std::uint64_t        ped{};
        std::uint64_t        matrix{};
        std::uint8_t         slot{};
    };

    class Memory
    {
    public:
        virtual ~Memory() = default;
        // A failed or partial kernel copy must return false; no direct pointer dereference.
        virtual bool Read(std::uint64_t address, void* output, std::size_t bytes) const = 0;
    };

    // Internal candidate decoder. A load bias is not an identity credential: production
    // callers must obtain it from the Android module verifier below, never from a server.
    // This intentionally does not expose a gameplay-valid position or call game methods.
    Result ReadCandidate(const Memory& memory, std::uint64_t bias, Candidate& output);
}
