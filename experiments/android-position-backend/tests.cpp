#include "PositionSnapshot.h"

#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>

using namespace mobile::position;
namespace
{
    constexpr std::uint64_t bias = 0x10000000, ped = 0x20000000, matrix = 0x30000000;
    struct TestMemory       final : Memory
    {
        mutable std::map<std::uint64_t, unsigned char> bytes;
        mutable unsigned                               matrixReads{};
        bool                                           replaceMatrix{};
        bool                                           Read(std::uint64_t address, void* output, std::size_t length) const override
        {
            if (replaceMatrix && address == ped + 0x18 && ++matrixReads == 2)
                return PutRead<std::uint64_t>(matrix + 4096, output, length);
            auto* result = static_cast<unsigned char*>(output);
            for (std::size_t i = 0; i < length; ++i)
            {
                auto it = bytes.find(address + i);
                if (it == bytes.end())
                    return false;
                result[i] = it->second;
            }
            return true;
        }
        template <typename T>
        static bool PutRead(T value, void* output, std::size_t length)
        {
            if (length != sizeof(value))
                return false;
            std::memcpy(output, &value, sizeof(value));
            return true;
        }
        template <typename T>
        void Put(std::uint64_t address, T value)
        {
            const auto* source = reinterpret_cast<const unsigned char*>(&value);
            for (std::size_t i = 0; i < sizeof(value); ++i)
                bytes[address + i] = source[i];
        }
        TestMemory()
        {
            Put(bias + kFocusGot, bias + kFocus);
            Put(bias + kInstanceGot, bias + kInstance);
            Put(bias + kPlayersGot, bias + kPlayers);
            Put<std::uint8_t>(bias + kFocus, 0);
            Put<std::uint32_t>(bias + kInstance, 0);
            Put(bias + kPlayers, ped);
            Put(bias + kPlayers + 0x1d8, ped);
            Put<std::uint64_t>(ped + 0x18, 0);
            Put(ped + 0x08, std::array<float, 3>{1, 2, 3});
            Put(matrix + 0x30, std::array<float, 3>{4, 5, 6});
        }
    };
    void Expect(Result expected, TestMemory& memory)
    {
        Candidate    output{{99, 99, 99}, 99, 99, 99};
        const Result actual = ReadCandidate(memory, bias, output);
        if (actual != expected)
            throw std::runtime_error("unexpected candidate result");
        if (expected != Result::candidate_unverified && (output.ped || output.matrix || output.xyz != std::array<float, 3>{}))
            throw std::runtime_error("failure retained stale position");
    }
}
int main()
{
    TestMemory a;
    Candidate  output;
    Expect(Result::candidate_unverified, a);
    ReadCandidate(a, bias, output);
    if (output.xyz != std::array<float, 3>{1, 2, 3})
        return 1;
    a.Put(ped + 0x18, matrix);
    ReadCandidate(a, bias, output);
    if (output.xyz != std::array<float, 3>{4, 5, 6})
        return 2;
    a.replaceMatrix = true;
    Expect(Result::changed, a);
    TestMemory b;
    b.Put<std::uint8_t>(bias + kFocus, 2);
    Expect(Result::invalid_selector, b);
    b.Put<std::uint32_t>(bias + kInstance, 1);
    Expect(Result::candidate_unverified, b);
    TestMemory c;
    c.Put<std::uint64_t>(bias + kPlayers, 0);
    Expect(Result::no_player, c);
    c.Put<std::uint64_t>(bias + kPlayers, 1);
    Expect(Result::invalid_pointer, c);
    TestMemory d;
    d.Put<std::uint64_t>(bias + kPlayersGot, bias + kPlayers + 8);
    Expect(Result::invalid_binding, d);
    TestMemory e;
    e.bytes.erase(ped + 8);
    Expect(Result::read_unavailable, e);
    TestMemory f;
    f.Put(ped + 8, std::numeric_limits<float>::quiet_NaN());
    Expect(Result::nonfinite, f);
    std::cout << "candidate boundary cases passed; Android/game/lifetime not tested\n";
}
