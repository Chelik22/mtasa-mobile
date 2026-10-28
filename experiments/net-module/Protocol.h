#pragma once
#include "BitBuffer.h"
#include <algorithm>
#include <array>
#include <string>

namespace experimental_net
{
    constexpr std::size_t   frame_header_bytes = 32;
    constexpr std::uint16_t owned_protocol_version = 1;
    enum class FrameKind : std::uint8_t
    {
        hello = 1,
        packet = 2,
        welcome = 3
    };
    struct Frame
    {
        FrameKind                 kind = FrameKind::packet;
        std::uint8_t              packet = 0;
        std::uint8_t              reliability = 3;
        std::uint8_t              ordering = 0;
        std::uint32_t             bits = 0;
        std::uint64_t             nonce = 0;
        std::uint32_t             generation = 0;
        std::uint32_t             sequence = 0;
        std::vector<std::uint8_t> payload;
    };
    inline void validate_frame(const Frame& frame)
    {
        if (frame.payload.size() > BitBuffer::max_bytes || frame.bits > frame.payload.size() * 8 ||
            frame.payload.size() != (static_cast<std::size_t>(frame.bits) + 7) / 8 || frame.reliability > 4 || frame.ordering > 3)
            throw std::invalid_argument("invalid owned frame fields");
        if (frame.bits % 8 && (frame.payload.back() >> (frame.bits % 8)))
            throw std::invalid_argument("nonzero owned frame padding");
        if (frame.kind == FrameKind::hello)
        {
            if (frame.packet || frame.nonce || frame.generation || frame.sequence || frame.reliability != 3 || frame.ordering || frame.payload.size() > 128 ||
                frame.bits % 8)
                throw std::invalid_argument("invalid hello frame");
        }
        else if (frame.kind == FrameKind::welcome)
        {
            if (frame.packet || !frame.nonce || !frame.generation || frame.sequence || frame.bits || frame.reliability != 3 || frame.ordering)
                throw std::invalid_argument("invalid welcome frame");
        }
        else if (frame.kind != FrameKind::packet || !frame.nonce || !frame.generation || !frame.sequence)
            throw std::invalid_argument("invalid packet session");
    }
    inline std::vector<std::uint8_t> encode_frame(const Frame& frame)
    {
        validate_frame(frame);
        BitBuffer header;
        header.write_unsigned(std::uint32_t{0x5841544d});  // MTAX bytes, independent of official netc.
        header.write_unsigned(owned_protocol_version);
        header.write_unsigned(static_cast<std::uint8_t>(frame.kind));
        header.write_unsigned(frame.packet);
        header.write_unsigned(frame.reliability);
        header.write_unsigned(frame.ordering);
        header.write_unsigned(std::uint16_t{0});
        header.write_unsigned(frame.bits);
        header.write_unsigned(frame.nonce);
        header.write_unsigned(frame.generation);
        header.write_unsigned(frame.sequence);
        std::vector<std::uint8_t> result(header.data(), header.data() + frame_header_bytes);
        result.insert(result.end(), frame.payload.begin(), frame.payload.end());
        return result;
    }
    inline Frame decode_frame(const void* data, std::size_t size)
    {
        if (!data || size < frame_header_bytes || size > frame_header_bytes + BitBuffer::max_bytes)
            throw std::invalid_argument("owned frame size");
        BitBuffer     header(data, frame_header_bytes, frame_header_bytes * 8, 0);
        std::uint32_t magic = 0;
        std::uint16_t version = 0, reserved = 0;
        std::uint8_t  kind = 0;
        Frame         result;
        header.read_unsigned(magic);
        header.read_unsigned(version);
        header.read_unsigned(kind);
        header.read_unsigned(result.packet);
        header.read_unsigned(result.reliability);
        header.read_unsigned(result.ordering);
        header.read_unsigned(reserved);
        header.read_unsigned(result.bits);
        header.read_unsigned(result.nonce);
        header.read_unsigned(result.generation);
        header.read_unsigned(result.sequence);
        if (magic != 0x5841544d || version != owned_protocol_version || reserved)
            throw std::invalid_argument("owned frame protocol");
        result.kind = static_cast<FrameKind>(kind);
        // Reject unauthenticated oversized hello bodies before copying them.
        if (result.kind == FrameKind::hello && size > frame_header_bytes + 128)
            throw std::invalid_argument("hello body bound");
        const auto* first = static_cast<const std::uint8_t*>(data);
        result.payload.assign(first + frame_header_bytes, first + size);
        validate_frame(result);
        return result;
    }
    inline bool valid_identity(const std::string& identity)
    {
        return identity.size() == 32 && std::all_of(identity.begin(), identity.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'); });
    }
    struct Hello
    {
        std::string identity;
        std::string version;
    };
    inline Frame make_hello(const Hello& hello)
    {
        if (!valid_identity(hello.identity) || hello.version.empty() || hello.version.size() > 63 ||
            !std::all_of(hello.version.begin(), hello.version.end(), [](char c) { return c >= 32 && c <= 126; }))
            throw std::invalid_argument("invalid hello identity/version");
        BitBuffer body;
        body.write_bits(hello.identity.data(), 32 * 8);
        body.write_unsigned(static_cast<std::uint16_t>(hello.version.size()));
        body.write_bits(hello.version.data(), hello.version.size() * 8);
        Frame result;
        result.kind = FrameKind::hello;
        result.bits = static_cast<std::uint32_t>(body.bits());
        result.payload.assign(body.data(), body.data() + body.bytes());
        return result;
    }
    inline Hello read_hello(const Frame& frame)
    {
        validate_frame(frame);
        if (frame.kind != FrameKind::hello)
            throw std::invalid_argument("expected hello");
        BitBuffer            body(frame.payload.data(), frame.payload.size(), frame.bits, 0);
        std::array<char, 32> identity{};
        std::uint16_t        length = 0;
        if (!body.read_bits(identity.data(), 32 * 8) || !body.read_unsigned(length) || !length || length > 63 || body.remaining() != length * 8)
            throw std::invalid_argument("invalid hello body");
        Hello result{std::string(identity.data(), identity.size()), std::string(length, '\0')};
        if (!body.read_bits(result.version.data(), length * 8))
            throw std::invalid_argument("truncated hello");
        make_hello(result);  // Apply exactly the same character and identity constraints.
        return result;
    }
}  // namespace experimental_net
