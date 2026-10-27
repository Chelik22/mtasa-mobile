#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace experimental_net
{
    // A separate wire format lets owned server/PC/Android endpoints share MTA's public
    // packet code without pretending to implement the closed official net module.
    class BitBuffer
    {
    public:
        static constexpr std::size_t max_bytes = 1024 * 1024;
        explicit BitBuffer(std::uint16_t version = 0x30) : version_(version) {}
        BitBuffer(const void* source, std::size_t bytes, std::size_t bits, std::uint16_t version) : version_(version)
        {
            if (bytes > max_bytes || bits > bytes * 8 || bytes != (bits + 7) / 8 || (bytes && !source))
                throw std::invalid_argument("invalid bitstream input");
            if (bytes)
                data_.assign(static_cast<const std::uint8_t*>(source), static_cast<const std::uint8_t*>(source) + bytes);
            write_ = bits;
            if (bits % 8 && (data_.back() >> (bits % 8)) != 0)
                throw std::invalid_argument("nonzero bitstream padding");
        }

        void clear()
        {
            data_.clear();
            read_ = write_ = 0;
        }
        void rewind() { read_ = 0; }
        void seek(std::size_t offset)
        {
            if (offset > write_)
                throw std::out_of_range("read offset");
            read_ = offset;
        }
        std::size_t         offset() const { return read_; }
        std::size_t         bits() const { return write_; }
        std::size_t         bytes() const { return (write_ + 7) / 8; }
        std::size_t         remaining() const { return write_ - read_; }
        std::uint16_t       version() const { return version_; }
        const std::uint8_t* data() const { return data_.data(); }
        std::uint8_t*       data() { return data_.data(); }

        void write_bits(const void* source, std::size_t count)
        {
            if (count > max_bytes * 8 - write_ || (count && !source))
                throw std::length_error("bitstream write bound");
            const auto*               input = static_cast<const std::uint8_t*>(source);
            std::vector<std::uint8_t> snapshot;
            if (overlaps(source, (count + 7) / 8))
            {
                const auto offset = reinterpret_cast<std::uintptr_t>(source) - reinterpret_cast<std::uintptr_t>(data_.data());
                if (offset > data_.size() || (count + 7) / 8 > data_.size() - offset)
                    throw std::invalid_argument("aliased write exceeds owned storage");
                snapshot.assign(input, input + (count + 7) / 8);
                input = snapshot.data();
            }
            data_.resize((write_ + count + 7) / 8, 0);
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto bit = static_cast<std::uint8_t>((input[i / 8] >> (i % 8)) & 1);
                const auto mask = static_cast<std::uint8_t>(1u << ((write_ + i) % 8));
                auto&      byte = data_[(write_ + i) / 8];
                byte = static_cast<std::uint8_t>((byte & ~mask) | (bit ? mask : 0));
            }
            write_ += count;
        }
        bool read_bits(void* destination, std::size_t count)
        {
            if (count > remaining() || (count && !destination))
                return false;
            auto*                     output = static_cast<std::uint8_t*>(destination);
            std::vector<std::uint8_t> snapshot;
            const auto*               input = data_.data();
            if (overlaps(destination, (count + 7) / 8))
            {
                const auto offset = reinterpret_cast<std::uintptr_t>(destination) - reinterpret_cast<std::uintptr_t>(data_.data());
                if (offset > data_.size() || (count + 7) / 8 > data_.size() - offset)
                    return false;
                snapshot = data_;
                input = snapshot.data();
            }
            if (count)
                std::memset(output, 0, (count + 7) / 8);
            for (std::size_t i = 0; i < count; ++i)
                output[i / 8] |= static_cast<std::uint8_t>(((input[(read_ + i) / 8] >> ((read_ + i) % 8)) & 1) << (i % 8));
            read_ += count;
            return true;
        }
        void write_bit(bool bit)
        {
            const std::uint8_t value = bit ? 1 : 0;
            write_bits(&value, 1);
        }
        bool read_bit(bool& bit)
        {
            std::uint8_t value = 0;
            if (!read_bits(&value, 1))
                return false;
            bit = value != 0;
            return true;
        }
        void align_write()
        {
            const std::uint8_t zero = 0;
            write_bits(&zero, (8 - write_ % 8) % 8);
        }
        void align_read() { seek((read_ + 7) / 8 * 8); }

        template <class T>
        void write_unsigned(T value)
        {
            static_assert(std::is_unsigned_v<T>);
            std::array<std::uint8_t, sizeof(T)> bytes{};
            for (std::size_t i = 0; i < sizeof(T); ++i)
                bytes[i] = static_cast<std::uint8_t>(value >> (i * 8));
            write_bits(bytes.data(), sizeof(T) * 8);
        }
        template <class T>
        bool read_unsigned(T& value)
        {
            static_assert(std::is_unsigned_v<T>);
            std::array<std::uint8_t, sizeof(T)> bytes{};
            if (!read_bits(bytes.data(), sizeof(T) * 8))
                return false;
            T result = 0;
            for (std::size_t i = 0; i < sizeof(T); ++i)
                result = static_cast<T>(result | (static_cast<T>(bytes[i]) << (i * 8)));
            value = result;
            return true;
        }
        template <class T>
        void write_scalar(T value)
        {
            static_assert(std::is_integral_v<T> || std::is_floating_point_v<T>);
            using U = std::conditional_t<sizeof(T) == 1, std::uint8_t,
                                         std::conditional_t<sizeof(T) == 2, std::uint16_t, std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>>>;
            static_assert(sizeof(U) == sizeof(T));
            U raw = 0;
            std::memcpy(&raw, &value, sizeof(T));
            write_unsigned(raw);
        }
        template <class T>
        bool read_scalar(T& value)
        {
            using U = std::conditional_t<sizeof(T) == 1, std::uint8_t,
                                         std::conditional_t<sizeof(T) == 2, std::uint16_t, std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>>>;
            static_assert(sizeof(U) == sizeof(T));
            U raw = 0;
            if (!read_unsigned(raw))
                return false;
            std::memcpy(&value, &raw, sizeof(T));
            return true;
        }

        // Canonical unsigned LEB128 is intentionally independent of RakNet compression.
        template <class T>
        void write_varuint(T value)
        {
            static_assert(std::is_unsigned_v<T>);
            do
            {
                auto byte = static_cast<std::uint8_t>(value & 0x7f);
                value >>= 7;
                if (value)
                    byte |= 0x80;
                write_unsigned(byte);
            } while (value);
        }
        template <class T>
        bool read_varuint(T& output)
        {
            static_assert(std::is_unsigned_v<T>);
            constexpr unsigned width = sizeof(T) * 8;
            const auto         start = read_;
            T                  value = 0;
            for (unsigned shift = 0; shift < width; shift += 7)
            {
                std::uint8_t byte = 0;
                if (!read_unsigned(byte) || (shift + 7 > width && static_cast<unsigned>(byte & 0x7f) >= (1u << (width - shift))))
                {
                    read_ = start;
                    return false;
                }
                value = static_cast<T>(value | (static_cast<T>(byte & 0x7f) << shift));
                if (!(byte & 0x80))
                {
                    if (shift && !(byte & 0x7f))
                    {
                        read_ = start;
                        return false;
                    }
                    output = value;
                    return true;
                }
            }
            read_ = start;
            return false;
        }
        template <class T>
        void write_varint(T value)
        {
            static_assert(std::is_signed_v<T>);
            using U = std::make_unsigned_t<T>;
            const U sign = value < 0 ? std::numeric_limits<U>::max() : U{0};
            write_varuint(static_cast<U>((static_cast<U>(value) << 1) ^ sign));
        }
        template <class T>
        bool read_varint(T& output)
        {
            using U = std::make_unsigned_t<T>;
            U encoded = 0;
            if (!read_varuint(encoded))
                return false;
            const U decoded = static_cast<U>((encoded >> 1) ^ (U{0} - (encoded & 1)));
            std::memcpy(&output, &decoded, sizeof(T));
            return true;
        }

        // Largest-component packing bounds error without the near-zero reconstruction
        // instability of always omitting x/w. Vector=35 bits, quaternion=51 bits.
        template <std::size_t N>
        void write_unit(std::array<float, N> input)
        {
            static_assert(N == 3 || N == 4);
            double squared = 0;
            for (float value : input)
            {
                if (!std::isfinite(value))
                    throw std::invalid_argument("nonfinite normalized value");
                squared += double(value) * value;
            }
            if (squared == 0)
            {
                if constexpr (N == 3)
                {
                    const std::uint8_t zero_marker = 3;
                    write_bits(&zero_marker, 2);
                    write_bit(false);
                    write_scalar(std::int16_t{0});
                    write_scalar(std::int16_t{0});
                    return;
                }
                throw std::invalid_argument("zero quaternion");
            }
            const auto  scale = 1.0 / std::sqrt(squared);
            std::size_t largest = 0;
            for (std::size_t i = 0; i < N; ++i)
            {
                input[i] = static_cast<float>(input[i] * scale);
                if (std::abs(input[i]) > std::abs(input[largest]))
                    largest = i;
            }
            const auto index = static_cast<std::uint8_t>(largest);
            write_bits(&index, 2);
            write_bit(input[largest] < 0);
            for (std::size_t i = 0; i < N; ++i)
                if (i != largest)
                    write_scalar(static_cast<std::int16_t>(std::lround(input[i] * 46339.5357982792)));
        }
        template <std::size_t N>
        bool read_unit(std::array<float, N>& output)
        {
            const auto   start = read_;
            std::uint8_t index = 0;
            bool         negative = false;
            if (!read_bits(&index, 2) || !read_bit(negative))
            {
                read_ = start;
                return false;
            }
            std::array<float, N> result{};
            if constexpr (N == 3)
            {
                if (index == 3)
                {
                    std::int16_t a = 0, b = 0;
                    if (negative || !read_scalar(a) || !read_scalar(b) || a || b)
                    {
                        read_ = start;
                        return false;
                    }
                    output = result;
                    return true;
                }
            }
            double squared = 0;
            for (std::size_t i = 0; i < N; ++i)
                if (i != index)
                {
                    std::int16_t value = 0;
                    if (!read_scalar(value) || value == std::numeric_limits<std::int16_t>::min())
                    {
                        read_ = start;
                        return false;
                    }
                    result[i] = static_cast<float>(value / 46339.5357982792);
                    squared += double(result[i]) * result[i];
                }
            if (squared > 1.00005)
            {
                read_ = start;
                return false;
            }
            result[index] = static_cast<float>(std::sqrt(std::max(0.0, 1.0 - squared)) * (negative ? -1 : 1));
            for (std::size_t i = 0; i < N; ++i)
                if (std::abs(result[i]) > std::abs(result[index]) + 0.0001f)
                {
                    read_ = start;
                    return false;
                }
            output = result;
            return true;
        }

    private:
        bool overlaps(const void* pointer, std::size_t bytes) const
        {
            if (!bytes || data_.empty())
                return false;
            const auto address = reinterpret_cast<std::uintptr_t>(pointer);
            const auto begin = reinterpret_cast<std::uintptr_t>(data_.data());
            return address >= begin ? address - begin < data_.size() : begin - address < bytes;
        }
        std::vector<std::uint8_t> data_;
        std::size_t               read_ = 0;
        std::size_t               write_ = 0;
        std::uint16_t             version_;
    };
}  // namespace experimental_net
