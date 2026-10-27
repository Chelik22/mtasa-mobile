#include "BitBuffer.h"
#include <algorithm>
#include <iostream>
#include <string>

using experimental_net::BitBuffer;
void check(bool condition, const char* reason)
{
    if (!condition)
        throw std::runtime_error(reason);
}
template <class F>
void rejects(F action)
{
    bool rejected = false;
    try
    {
        action();
    }
    catch (const std::exception&)
    {
        rejected = true;
    }
    check(rejected, "expected rejection");
}

int main()
{
    try
    {
        BitBuffer scalar;
        scalar.write_scalar(std::uint32_t{0x12345678});
        scalar.write_scalar(-1.0f);
        const std::vector<std::uint8_t> golden{0x78, 0x56, 0x34, 0x12, 0, 0, 0x80, 0xbf};
        check(scalar.bytes() == golden.size() && std::equal(golden.begin(), golden.end(), scalar.data()), "scalar golden");
        std::uint32_t integer = 0;
        float         floating = 0;
        check(scalar.read_scalar(integer) && integer == 0x12345678 && scalar.read_scalar(floating) && floating == -1, "scalar read");
        check(!scalar.read_scalar(integer), "truncated scalar");
        BitBuffer var;
        var.write_varuint(std::uint32_t{300});
        var.write_varint(std::int32_t{-1});
        var.write_varint(std::numeric_limits<std::int32_t>::min());
        const std::vector<std::uint8_t> var_golden{0xac, 0x02, 0x01, 0xff, 0xff, 0xff, 0xff, 0x0f};
        check(var.bytes() == var_golden.size() && std::equal(var_golden.begin(), var_golden.end(), var.data()), "varint golden");
        std::int32_t signed_value = 0;
        check(var.read_varuint(integer) && integer == 300, "varuint read");
        check(var.read_varint(signed_value) && signed_value == -1, "negative read");
        check(var.read_varint(signed_value) && signed_value == std::numeric_limits<std::int32_t>::min(), "signed min");
        const std::array<std::uint8_t, 2> overlong{0x80, 0};
        BitBuffer                         invalid(overlong.data(), 2, 16, 0x30);
        check(!invalid.read_varuint(integer) && invalid.offset() == 0, "noncanonical varuint");
        const std::array<std::uint8_t, 5> overflow{0xff, 0xff, 0xff, 0xff, 0x10};
        BitBuffer                         too_wide(overflow.data(), 5, 40, 0x30);
        check(!too_wide.read_varuint(integer) && too_wide.offset() == 0, "overflow varuint");
        for (std::size_t size = 0; size < var_golden.size(); ++size)
        {
            BitBuffer short_buffer(var_golden.data(), size, size * 8, 0x30);
            if (size < 2)
                check(!short_buffer.read_varuint(integer) && short_buffer.offset() == 0, "truncated varuint rollback");
        }
        BitBuffer bits;
        bits.write_bit(true);
        const std::uint8_t three = 3;
        bits.write_bits(&three, 2);
        bits.write_scalar(std::uint16_t{0x1234});
        bits.align_write();
        const std::vector<std::uint8_t> bit_golden{0xa7, 0x91, 0};
        check(bits.bits() == 24 && std::equal(bit_golden.begin(), bit_golden.end(), bits.data()), "unaligned golden");
        bool          bit = false;
        std::uint8_t  part = 0;
        std::uint16_t word = 0;
        check(bits.read_bit(bit) && bit && bits.read_bits(&part, 2) && part == 3 && bits.read_scalar(word) && word == 0x1234, "unaligned read");
        bits.align_read();
        check(bits.remaining() == 0, "read alignment");
        rejects([&] { bits.seek(25); });
        const std::uint8_t padding = 0x80;
        rejects([&] { BitBuffer bad(&padding, 1, 1, 0x30); });
        rejects([&] { BitBuffer bad(nullptr, 1, 8, 0x30); });
        rejects([&] { BitBuffer bad(nullptr, BitBuffer::max_bytes + 1, 0, 0x30); });
        for (const auto& input : std::array<std::array<float, 3>, 4>{{{1, 0, 0}, {0, -1, 0}, {0, 0, 1}, {0.577350269f, 0.577350269f, 0.577350269f}}})
        {
            BitBuffer normalized;
            normalized.write_unit(input);
            check(normalized.bits() == 35, "vector size");
            std::array<float, 3> output{};
            check(normalized.read_unit(output), "vector read");
            for (std::size_t i = 0; i < 3; ++i)
                check(std::abs(input[i] - output[i]) < 0.00004f, "vector precision");
            BitBuffer truncated(normalized.data(), normalized.bytes() - 1, (normalized.bytes() - 1) * 8, 0x30);
            check(!truncated.read_unit(output) && truncated.offset() == 0, "vector truncated rollback");
        }
        BitBuffer quaternion;
        quaternion.write_unit(std::array<float, 4>{0.707106781f, 0, 0.707106781f, 0});
        check(quaternion.bits() == 51, "quaternion size");
        std::array<float, 4> rotation{};
        check(quaternion.read_unit(rotation) && std::abs(rotation[0] - 0.707106781f) < 0.00004f && std::abs(rotation[2] - 0.707106781f) < 0.00004f,
              "quaternion precision");
        BitBuffer zero;
        zero.write_unit(std::array<float, 3>{0, 0, 0});
        std::array<float, 3> zero_read{1, 1, 1};
        check(zero.read_unit(zero_read) && zero_read == std::array<float, 3>{0, 0, 0}, "zero vector marker");
        const std::vector<std::uint8_t> zero_golden{3, 0, 0, 0, 0};
        check(zero.bytes() == 5 && std::equal(zero_golden.begin(), zero_golden.end(), zero.data()), "zero vector golden");
        rejects(
            [&]
            {
                BitBuffer b;
                b.write_unit(std::array<float, 4>{0, 0, 0, 0});
            });
        rejects(
            [&]
            {
                BitBuffer b;
                b.write_unit(std::array<float, 3>{std::numeric_limits<float>::infinity(), 0, 0});
            });
        BitBuffer                       limit;
        const std::vector<std::uint8_t> full(BitBuffer::max_bytes, 0);
        limit.write_bits(full.data(), full.size() * 8);
        rejects([&] { limit.write_bit(true); });
        check(limit.bytes() == BitBuffer::max_bytes, "write bound unchanged");
        BitBuffer self;
        self.write_scalar(std::uint32_t{0x44332211});
        for (int repeat = 0; repeat < 10; ++repeat)
        {
            const auto previous = self.bytes();
            self.write_bits(self.data(), previous * 8);
            check(self.bytes() == previous * 2, "self append extent");
        }
        for (std::size_t index = 0; index < self.bytes(); ++index)
            check(self.data()[index] == static_cast<std::uint8_t>(0x11 * ((index % 4) + 1)), "self append snapshot");
        BitBuffer overlap;
        overlap.write_scalar(std::uint32_t{0x44332211});
        check(overlap.read_bits(overlap.data() + 1, 24), "overlapping read accepted");
        const std::vector<std::uint8_t> overlap_golden{0x11, 0x11, 0x22, 0x33};
        check(std::equal(overlap_golden.begin(), overlap_golden.end(), overlap.data()), "overlapping read snapshot");
        rejects([&] { overlap.write_bits(overlap.data() + 3, 16); });
        overlap.rewind();
        check(!overlap.read_bits(overlap.data() + 3, 16) && overlap.offset() == 0, "overlap extent rejection");
        std::cout << "PASS scalar/bit/varint golden vectors, malformed bounds, normalized values and aliasing; MTA integration NOT RUN\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
