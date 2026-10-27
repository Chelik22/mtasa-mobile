#pragma once

#include <MtaBitStream.h>
#include <net/Packets.h>
#include <version.h>
#include <enums/HTTPDownloadType.h>
#include "SourceConstants.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>

namespace headless
{
    inline void require(bool condition, const char* message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    inline std::string version_string()
    {
        std::array<char, 64> text{};
        std::snprintf(text.data(), text.size(), "%d.%d.%d-%d.%05d.%d", MTASA_VERSION_MAJOR, MTASA_VERSION_MINOR,
                      MTASA_VERSION_MAINTENANCE, MTASA_VERSION_TYPE, MTASA_VERSION_BUILD, 0);
        return text.data();
    }

    inline std::string read_characters(NetBitStreamInterface& stream, std::size_t length, std::size_t maximum = 4096)
    {
        require(length <= maximum && length * 8 <= static_cast<std::size_t>(stream.GetNumberOfUnreadBits()), "bounded string exceeds payload");
        std::string value(length, '\0');
        require(!length || stream.Read(value.data(), static_cast<int>(length)), "truncated characters");
        return value;
    }

    inline std::string read_string(NetBitStreamInterface& stream)
    {
        unsigned short length = 0;
        require(stream.Read(length), "truncated string length");
        return read_characters(stream, length);
    }

    inline void write_join_data(NetBitStreamInterface& stream, const std::string& nickname)
    {
        // CPlayerJoinDataPacket::Read and Client CClientGame::Connect define this order.
        require(!nickname.empty() && nickname.size() <= MAX_PLAYER_NICK_LENGTH, "invalid nickname length");
        stream.Write(static_cast<unsigned short>(MTA_DM_NETCODE_VERSION));
        stream.Write(static_cast<unsigned short>(MTA_DM_VERSION));
        stream.Write(static_cast<unsigned short>(MTA_DM_BITSTREAM_VERSION));
        stream.WriteString(std::string_view(version_string()));
        stream.WriteBit(false);
        stream.Write(source::game_version_us10);
        std::array<char, MAX_PLAYER_NICK_LENGTH> nick{};
        std::copy(nickname.begin(), nickname.end(), nick.begin());
        stream.Write(nick.data(), static_cast<int>(nick.size()));
        const std::array<char, 16> password{};  // Isolated no-password server only.
        stream.Write(password.data(), static_cast<int>(password.size()));
        const std::array<char, MAX_SERIAL_LENGTH> removed_community{};
        stream.Write(removed_community.data(), static_cast<int>(removed_community.size()));
    }

    struct Joined
    {
        ElementID player{INVALID_ELEMENT_ID};
        ElementID root{INVALID_ELEMENT_ID};
        std::string server_name;
        unsigned char http_type = 0;
    };

    inline Joined read_joined(NetBitStreamInterface& stream)
    {
        // Server CPlayerJoinCompletePacket::Write is authoritative, including optional HTTP port.
        Joined result;
        unsigned char compatibility = 0, sample_rate = 0, quality = 0;
        int checks = 0, http_connections = 0;
        unsigned int bitrate = 0;
        bool voice = false, fake_lag = false;
        require(stream.Read(result.player) && stream.Read(compatibility) && stream.Read(result.root) && stream.Read(checks)
                    && stream.ReadBit(voice) && stream.ReadBits(&sample_rate, 2) && stream.ReadBits(&quality, 4)
                    && stream.ReadCompressed(bitrate) && stream.ReadBit(fake_lag) && stream.Read(http_connections) && stream.Read(result.http_type),
                "truncated joined-game header");
        require(result.player != INVALID_ELEMENT_ID && result.root != INVALID_ELEMENT_ID && compatibility != 0, "invalid joined-game element IDs");
        require(result.http_type <= HTTPDownloadType::HTTP_DOWNLOAD_ENABLED_URL, "unsupported HTTP download type");
        if (result.http_type != HTTPDownloadType::HTTP_DOWNLOAD_DISABLED)
        {
            unsigned short port = 0;
            require(stream.Read(port), "truncated HTTP port");
            if (result.http_type == HTTPDownloadType::HTTP_DOWNLOAD_ENABLED_URL)
                read_string(stream);  // Record limitation; never fetch remote content.
        }
        result.server_name = read_string(stream);
        require(stream.GetNumberOfUnreadBits() == 0, "joined-game unexpected trailing data");
        return result;
    }

    struct LuaValue
    {
        unsigned char type = source::lua_nil;
        std::string text;
        ElementID element{INVALID_ELEMENT_ID};
    };

    inline LuaValue read_lua_value(NetBitStreamInterface& stream)
    {
        // Selected scalar types only; tables/functions are explicitly unsupported.
        LuaValue value;
        require(stream.ReadBits(&value.type, 4), "truncated Lua type");
        if (value.type == source::lua_string)
        {
            unsigned short length = 0;
            require(stream.ReadCompressed(length), "truncated Lua string length");
            value.text = read_characters(stream, length);
        }
        else if (value.type == source::lua_userdata || value.type == source::lua_lightuserdata)
            require(stream.Read(value.element) && value.element != INVALID_ELEMENT_ID, "invalid Lua element reference");
        else if (value.type == source::lua_boolean)
        {
            bool bit = false;
            require(stream.ReadBit(bit), "truncated Lua boolean");
        }
        else if (value.type == source::lua_number)
        {
            bool floating = false, wide = false;
            require(stream.ReadBit(floating), "truncated Lua number kind");
            if (!floating)
            {
                int number = 0;
                require(stream.ReadCompressed(number), "truncated Lua integer");
            }
            else
            {
                require(stream.ReadBit(wide), "truncated Lua number width");
                if (wide)
                {
                    double number = 0;
                    require(stream.Read(number) && std::isfinite(number), "invalid Lua double");
                }
                else
                {
                    float number = 0;
                    require(stream.Read(number) && std::isfinite(number), "invalid Lua float");
                }
            }
        }
        else
            require(value.type == source::lua_nil, "unsupported Lua argument type");
        return value;
    }

    inline void write_lua_string(NetBitStreamInterface& stream, const std::string& value)
    {
        require(value.size() <= 4096, "outgoing Lua string bound");
        const auto type = source::lua_string;
        stream.WriteBits(&type, 4);
        stream.WriteCompressed(static_cast<unsigned short>(value.size()));
        stream.Write(value.data(), static_cast<int>(value.size()));
    }

    struct Probe
    {
        ElementID source{INVALID_ELEMENT_ID};
        std::string token, correlation;
        ElementID vehicle{INVALID_ELEMENT_ID};
    };

    inline Probe read_probe(NetBitStreamInterface& stream)
    {
        // Event name was read by the dispatcher, per CLuaEventPacket::Write.
        Probe result;
        unsigned int count = 0;
        require(stream.Read(result.source) && stream.ReadCompressed(count) && count == 3, "invalid witness event envelope");
        const auto token = read_lua_value(stream);
        const auto correlation = read_lua_value(stream);
        const auto vehicle = read_lua_value(stream);
        require(token.type == source::lua_string && correlation.type == source::lua_string && vehicle.type == source::lua_userdata,
                "unexpected witness argument types");
        require(!token.text.empty() && !correlation.text.empty() && stream.GetNumberOfUnreadBits() == 0, "invalid witness event extent");
        result.token = token.text;
        result.correlation = correlation.text;
        result.vehicle = vehicle.element;
        return result;
    }

    inline void write_ack(NetBitStreamInterface& stream, ElementID own_player, const Probe& probe)
    {
        const std::string name = "headlesswitness:ack";
        stream.WriteCompressed(static_cast<unsigned short>(name.size()));
        stream.Write(name.data(), static_cast<int>(name.size()));
        stream.Write(own_player);  // Server's client == source validation uses this actual ID.
        stream.WriteCompressed(3u);
        write_lua_string(stream, probe.token);
        write_lua_string(stream, probe.correlation);
        const auto type = source::lua_userdata;
        stream.WriteBits(&type, 4);
        stream.Write(probe.vehicle);
    }

    struct EntityPrefix
    {
        unsigned int count = 0;
        ElementID element{INVALID_ELEMENT_ID};
        unsigned char type = 0;
        unsigned short model = 0;
        std::array<float, 3> position{};
        bool vehicle_prefix = false;
    };

    inline float read_fixed_position(NetBitStreamInterface& stream)
    {
        // SPositionSync(false) -> SFloatSync<14,10>, signed 24-bit / 1024.
        unsigned int bits = 0;
        require(stream.ReadBits(&bits, 24), "truncated fixed position");
        const int signed_value = bits & 0x800000 ? static_cast<int>(bits) - 0x1000000 : static_cast<int>(bits);
        return static_cast<float>(signed_value) / 1024.0f;
    }

    inline EntityPrefix read_entity_prefix(NetBitStreamInterface& stream)
    {
        // Only the FIRST entity's common prefix and vehicle position/model are decoded.
        EntityPrefix result;
        ElementID parent;
        unsigned char interior = 0, context = 0;
        unsigned short dimension = 0, name_length = 0;
        bool attached = false, collisions = false, propagation = false;
        unsigned int custom_count = 0;
        require(stream.ReadCompressed(result.count) && result.count > 0 && result.count <= MAX_SERVER_ELEMENTS
                    && stream.Read(result.element) && stream.Read(result.type) && stream.Read(parent) && stream.Read(interior)
                    && stream.ReadCompressed(dimension) && stream.ReadBit(attached), "truncated entity common prefix");
        if (attached)
        {
            ElementID target;
            require(stream.Read(target), "truncated attached target");
            read_fixed_position(stream);
            read_fixed_position(stream);
            float z = 0;
            unsigned short rotation = 0;
            require(stream.Read(z) && stream.Read(rotation) && stream.Read(rotation) && stream.Read(rotation), "truncated attachment");
        }
        require(stream.ReadBit(collisions) && stream.ReadBit(propagation) && stream.ReadCompressed(custom_count) && custom_count <= 128,
                "invalid entity custom data count");
        for (unsigned int index = 0; index < custom_count; ++index)
        {
            unsigned char length = 0;
            require(stream.Read(length), "truncated custom data key");
            read_characters(stream, length);
            read_lua_value(stream);
        }
        require(stream.ReadCompressed(name_length), "truncated entity name");
        read_characters(stream, name_length);
        require(stream.Read(context), "truncated entity sync context");
        if (result.type == source::entity_vehicle)
        {
            result.position[0] = read_fixed_position(stream);
            result.position[1] = read_fixed_position(stream);
            unsigned short rotation = 0;
            require(stream.Read(result.position[2]) && stream.Read(rotation) && stream.Read(rotation) && stream.Read(rotation)
                        && stream.Read(result.model), "truncated vehicle prefix");
            require(std::isfinite(result.position[2]), "invalid vehicle position");
            result.vehicle_prefix = true;
        }
        return result;
    }
}  // namespace headless
