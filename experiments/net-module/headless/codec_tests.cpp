#include "PacketCodec.h"
#include <iostream>

using experimental_net::MtaBitStream;
using headless::require;

namespace
{
    template <class Action>
    void rejects(Action action)
    {
        try
        {
            action();
        }
        catch (const std::exception&)
        {
            return;
        }
        throw std::runtime_error("malformed selected MTA payload accepted");
    }

    void string_argument(NetBitStreamInterface& stream, const std::string& text)
    {
        // Construct the server writer's documented short-string shape independently.
        const unsigned char type = headless::source::lua_string;
        stream.WriteBits(&type, 4);
        stream.WriteCompressed(static_cast<unsigned short>(text.size()));
        stream.Write(text.data(), static_cast<int>(text.size()));
    }
}

int main()
{
    try
    {
        MtaBitStream join;
        headless::write_join_data(join, "Headless_A");
        unsigned short net = 0, mta = 0, bit = 0;
        unsigned char  game = 0;
        bool           update = true;
        require(join.Read(net) && net == MTA_DM_NETCODE_VERSION && join.Read(mta) && mta == MTA_DM_VERSION && join.Read(bit) &&
                    bit == static_cast<unsigned short>(MTA_DM_BITSTREAM_VERSION),
                "actual join reader version fields");
        require(headless::read_string(join) == headless::version_string() && join.ReadBit(update) && !update && join.Read(game) &&
                    game == headless::source::game_version_us10,
                "actual join reader update/game fields");
        const auto nick = headless::read_characters(join, MAX_PLAYER_NICK_LENGTH);
        require(nick.substr(0, 10) == "Headless_A" && nick.substr(10).find_first_not_of('\0') == std::string::npos, "fixed nickname padding");
        require(headless::read_characters(join, 16) == std::string(16, '\0'), "empty password hash");
        require(headless::read_characters(join, MAX_SERIAL_LENGTH) == std::string(MAX_SERIAL_LENGTH, '\0') && join.GetNumberOfUnreadBits() == 0,
                "removed community bytes");

        MtaBitStream joined;
        joined.Write(ElementID{123});
        joined.Write(static_cast<unsigned char>(1));
        joined.Write(ElementID{1});
        joined.Write(0);
        joined.WriteBit(false);
        const unsigned char sample = 1, quality = 4;
        joined.WriteBits(&sample, 2);
        joined.WriteBits(&quality, 4);
        joined.WriteCompressed(0u);
        joined.WriteBit(false);
        joined.Write(4);
        joined.Write(static_cast<unsigned char>(HTTPDownloadType::HTTP_DOWNLOAD_DISABLED));
        joined.WriteString(std::string_view("source-server"));
        const auto info = headless::read_joined(joined);
        require(info.player.Value() == 123 && info.root.Value() == 1 && info.server_name == "source-server", "joined-game actual writer order");

        MtaBitStream probe;
        probe.Write(ElementID{1});
        probe.WriteCompressed(3u);
        string_argument(probe, "token-1");
        string_argument(probe, "vehicle-1");
        const unsigned char userdata = headless::source::lua_userdata;
        probe.WriteBits(&userdata, 4);
        probe.Write(ElementID{500});
        const auto decoded = headless::read_probe(probe);
        require(decoded.source.Value() == 1 && decoded.vehicle.Value() == 500 && decoded.token == "token-1" && decoded.correlation == "vehicle-1",
                "actual Lua event arguments");
        for (int bytes = 0; bytes < probe.GetNumberOfBytesUsed(); ++bytes)
        {
            MtaBitStream truncated(probe.GetData(), static_cast<std::size_t>(bytes), static_cast<std::size_t>(bytes) * 8, 0x30);
            rejects([&] { headless::read_probe(truncated); });
        }
        MtaBitStream invalid;
        invalid.Write(ElementID{1});
        invalid.WriteCompressed(4u);
        rejects([&] { headless::read_probe(invalid); });

        MtaBitStream ack;
        headless::write_ack(ack, ElementID{123}, decoded);
        unsigned short event_size = 0;
        ElementID      actual_source;
        unsigned int   arguments = 0;
        require(ack.ReadCompressed(event_size) && headless::read_characters(ack, event_size) == "headlesswitness:ack" && ack.Read(actual_source) &&
                    actual_source.Value() == 123 && ack.ReadCompressed(arguments) && arguments == 3,
                "ACK actual event envelope and own-player source");
        require(headless::read_lua_value(ack).text == "token-1" && headless::read_lua_value(ack).text == "vehicle-1" &&
                    headless::read_lua_value(ack).element.Value() == 500 && ack.GetNumberOfUnreadBits() == 0,
                "ACK preserves actual element reference");
        std::cout << "PASS selected MTA packet codecs and malformed payloads; server/H1 NOT RUN\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
