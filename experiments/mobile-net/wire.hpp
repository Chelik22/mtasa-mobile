#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace mobile_net
{
    constexpr std::size_t   header_size = 28;
    constexpr std::size_t   max_payload = 1024;
    constexpr std::uint16_t protocol_version = 1;
    enum class Type : std::uint16_t
    {
        hello = 1,
        probe = 2,
        ack = 3
    };

    struct Message
    {
        Type          type;
        std::uint64_t session;
        std::uint32_t generation;
        std::uint32_t sequence;
        std::string   payload;
    };

    inline bool valid(Type type)
    {
        return type == Type::hello || type == Type::probe || type == Type::ack;
    }

    inline void put(std::vector<std::uint8_t>& out, std::uint64_t value, unsigned bytes)
    {
        for (unsigned i = bytes; i != 0; --i)
            out.push_back(static_cast<std::uint8_t>(value >> ((i - 1) * 8)));
    }

    inline std::uint64_t get(const std::uint8_t* data, std::size_t& offset, unsigned bytes)
    {
        std::uint64_t value = 0;
        for (unsigned i = 0; i < bytes; ++i)
            value = (value << 8) | data[offset++];
        return value;
    }

    inline std::vector<std::uint8_t> encode(const Message& message)
    {
        if (!valid(message.type) || message.session == 0 || message.generation == 0 || message.sequence == 0 || message.payload.size() > max_payload)
            throw std::invalid_argument("invalid wire message");
        std::vector<std::uint8_t> out{'M', 'N', 'E', 'T'};
        out.reserve(header_size + message.payload.size());
        put(out, protocol_version, 2);
        put(out, static_cast<std::uint16_t>(message.type), 2);
        put(out, message.session, 8);
        put(out, message.generation, 4);
        put(out, message.sequence, 4);
        put(out, message.payload.size(), 4);
        out.insert(out.end(), message.payload.begin(), message.payload.end());
        return out;
    }

    inline Message decode(const std::uint8_t* data, std::size_t size)
    {
        // Validate the entire envelope before allocating payload memory or reading fields.
        if (data == nullptr || size < header_size || size > header_size + max_payload || data[0] != 'M' || data[1] != 'N' || data[2] != 'E' || data[3] != 'T')
            throw std::invalid_argument("invalid envelope size or magic");
        std::size_t offset = 4;
        if (get(data, offset, 2) != protocol_version)
            throw std::invalid_argument("unsupported wire version");
        Message message{};
        message.type = static_cast<Type>(get(data, offset, 2));
        message.session = get(data, offset, 8);
        message.generation = static_cast<std::uint32_t>(get(data, offset, 4));
        message.sequence = static_cast<std::uint32_t>(get(data, offset, 4));
        const auto payload_size = get(data, offset, 4);
        if (!valid(message.type) || message.session == 0 || message.generation == 0 || message.sequence == 0 || payload_size > max_payload ||
            payload_size != size - header_size)
            throw std::invalid_argument("invalid fields or payload length");
        message.payload.assign(reinterpret_cast<const char*>(data + offset), static_cast<std::size_t>(payload_size));
        return message;
    }
}  // namespace mobile_net
