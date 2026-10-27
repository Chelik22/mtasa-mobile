#include "PacketCodec.h"
#include <Protocol.h>
#include <enet/enet.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <sstream>

namespace
{
    using Clock = std::chrono::steady_clock;
    using experimental_net::Frame;
    using experimental_net::FrameKind;
    using experimental_net::MtaBitStream;
    using headless::require;

    std::string quote(const std::string& text)
    {
        std::ostringstream out;
        out << '"';
        for (unsigned char c : text)
        {
            if (c == '"' || c == '\\')
                out << '\\' << static_cast<char>(c);
            else if (c < 32)
            {
                const char hex[] = "0123456789abcdef";
                out << "\\u00" << hex[c >> 4] << hex[c & 15];
            }
            else
                out << static_cast<char>(c);
        }
        out << '"';
        return out.str();
    }

    struct Options
    {
        std::string           host = "127.0.0.1", identity, nickname;
        std::filesystem::path commands;
        unsigned short        port = 22003;
        unsigned              duration = 60;
    };

    unsigned parse_number(const std::string& text, unsigned maximum)
    {
        require(!text.empty() && text.find_first_not_of("0123456789") == std::string::npos, "invalid numeric option");
        const auto number = std::stoul(text);
        require(number > 0 && number <= maximum, "numeric option outside bound");
        return static_cast<unsigned>(number);
    }

    Options options(int argc, char** argv)
    {
        Options result;
        for (int i = 1; i < argc; i += 2)
        {
            require(i + 1 < argc, "option requires a value");
            const std::string name = argv[i], value = argv[i + 1];
            if (name == "--host")
                result.host = value;
            else if (name == "--port")
                result.port = static_cast<unsigned short>(parse_number(value, 65535));
            else if (name == "--identity")
                result.identity = value;
            else if (name == "--nickname")
                result.nickname = value;
            else if (name == "--duration")
                result.duration = parse_number(value, 3600);
            else if (name == "--commands")
                result.commands = value;
            else
                throw std::invalid_argument("unknown command-line option");
        }
        require(experimental_net::valid_identity(result.identity), "identity must be exactly 32 uppercase hex characters");
        require(!result.nickname.empty() && result.nickname.size() <= MAX_PLAYER_NICK_LENGTH &&
                    result.nickname.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos,
                "nickname must be bounded ASCII letters/digits/_/-");
        return result;
    }

    class Client
    {
    public:
        explicit Client(Options settings) : settings_(std::move(settings)), host_(enet_host_create(nullptr, 1, 4, 0, 0), enet_host_destroy)
        {
            require(host_ != nullptr, "ENet host allocation failed");
            host_->maximumPacketSize = experimental_net::frame_header_bytes + experimental_net::BitBuffer::max_bytes;
            host_->maximumWaitingData = host_->maximumPacketSize * 16;
        }

        int run()
        {
            emit("client_start", {{"scope", "actual MTA protocol; no GTA or native world"},
                                  {"identity", settings_.identity},
                                  {"version", headless::version_string()},
                                  {"host", settings_.host},
                                  {"port", std::to_string(settings_.port)}});
            connect();
            const auto deadline = Clock::now() + std::chrono::seconds(settings_.duration);
            while (!quit_ || peer_)
            {
                poll_commands();
                if (!quit_ && Clock::now() >= deadline)
                {
                    emit("duration_elapsed");
                    quit_ = true;
                    disconnect(false);
                }
                ENetEvent event{};
                const int serviced = enet_host_service(host_.get(), &event, 25);
                require(serviced >= 0, "ENet service failed");
                if (serviced > 0)
                    process(event);
                if (peer_ && Clock::now() >= phase_deadline_ && state_ == "disconnecting")
                {
                    emit("disconnect_forced", {{"reason", "graceful event timeout; server quit not proven"}});
                    failed_ = true;
                    enet_peer_reset(peer_);
                    peer_ = nullptr;
                    state_ = "disconnected";
                    if (reconnect_ && !quit_)
                        connect();
                }
                else if (peer_ && state_ != "initial_data_sent" && state_ != "disconnecting" && Clock::now() >= phase_deadline_)
                    throw std::runtime_error("connection/join phase timeout: " + state_);
            }
            emit("client_stop");
            return failed_ ? 1 : 0;
        }

    private:
        void emit(const std::string& event, std::initializer_list<std::pair<std::string, std::string>> fields = {})
        {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_).count();
            std::cout << "{\"event\":" << quote(event) << ",\"elapsed_ms\":" << elapsed << ",\"nickname\":" << quote(settings_.nickname)
                      << ",\"state\":" << quote(state_);
            for (const auto& field : fields)
                std::cout << ',' << quote(field.first) << ':' << quote(field.second);
            std::cout << "}" << std::endl;
        }

        void connect()
        {
            if (peer_)
            {
                emit("command_ignored", {{"reason", "connection already active"}});
                return;
            }
            ENetAddress address{};
            require(enet_address_set_host(&address, settings_.host.c_str()) == 0, "host resolution failed");
            address.port = settings_.port;
            peer_ = enet_host_connect(host_.get(), &address, 4, 0);
            require(peer_ != nullptr, "connect allocation failed");
            nonce_ = 0;
            generation_ = 0;
            send_sequence_ = {};
            receive_sequence_ = {};
            player_ = ElementID{INVALID_ELEMENT_ID};
            root_ = ElementID{INVALID_ELEMENT_ID};
            vehicles_.clear();
            acked_.clear();
            reconnect_ = false;
            state_ = "connecting";
            phase_deadline_ = Clock::now() + std::chrono::seconds(10);
            emit("connect_requested");
        }

        void disconnect(bool reconnect)
        {
            reconnect_ = reconnect;
            if (!peer_)
            {
                if (reconnect && !quit_)
                    connect();
                return;
            }
            if (state_ == "disconnecting")
                return;
            state_ = "disconnecting";
            enet_peer_disconnect(peer_, 1);  // Owned server maps this to genuine PLAYER_QUIT, not timeout.
            enet_host_flush(host_.get());
            phase_deadline_ = Clock::now() + std::chrono::seconds(3);
            emit("disconnect_requested");
        }

        void send_frame(const Frame& frame)
        {
            const auto  bytes = experimental_net::encode_frame(frame);
            ENetPacket* packet = enet_packet_create(bytes.data(), bytes.size(), ENET_PACKET_FLAG_RELIABLE);
            require(packet != nullptr, "packet allocation failed");
            if (enet_peer_send(peer_, frame.ordering, packet) != 0)
            {
                enet_packet_destroy(packet);
                throw std::runtime_error("ENet send failed");
            }
            enet_host_flush(host_.get());
        }

        void send_packet(unsigned char id, const MtaBitStream& stream)
        {
            Frame frame;
            frame.kind = FrameKind::packet;
            frame.packet = id;
            frame.nonce = nonce_;
            frame.generation = generation_;
            auto& sequence = send_sequence_[0][3];
            require(sequence != std::numeric_limits<std::uint32_t>::max(), "outgoing sequence exhausted");
            frame.sequence = ++sequence;
            frame.bits = static_cast<std::uint32_t>(stream.GetNumberOfBitsUsed());
            if (stream.GetNumberOfBytesUsed())
                frame.payload.assign(stream.GetData(), stream.GetData() + stream.GetNumberOfBytesUsed());
            send_frame(frame);
            emit("packet_sent", {{"packet_id", std::to_string(id)}, {"bits", std::to_string(frame.bits)}, {"sequence", std::to_string(frame.sequence)}});
        }

        void rpc(unsigned char id)
        {
            MtaBitStream body;
            body.Write(id);
            send_packet(PACKET_ID_RPC, body);
        }

        void process(const ENetEvent& event)
        {
            require(event.peer == peer_, "event from unknown peer");
            if (event.type == ENET_EVENT_TYPE_CONNECT)
            {
                require(state_ == "connecting", "duplicate ENet connect");
                send_frame(experimental_net::make_hello({settings_.identity, headless::version_string()}));
                state_ = "awaiting_welcome";
                phase_deadline_ = Clock::now() + std::chrono::seconds(10);
                emit("enet_connected");
            }
            else if (event.type == ENET_EVENT_TYPE_DISCONNECT)
            {
                const bool expected = state_ == "disconnecting";
                peer_ = nullptr;
                state_ = "disconnected";
                emit("enet_disconnected", {{"data", std::to_string(event.data)}, {"requested", expected ? "true" : "false"}});
                require(expected, "unexpected server disconnect");
                if (reconnect_ && !quit_)
                    connect();
            }
            else if (event.type == ENET_EVENT_TYPE_RECEIVE)
            {
                std::unique_ptr<ENetPacket, decltype(&enet_packet_destroy)> owner(event.packet, enet_packet_destroy);
                const auto frame = experimental_net::decode_frame(event.packet->data, event.packet->dataLength);
                if (frame.kind == FrameKind::welcome)
                {
                    require(state_ == "awaiting_welcome" && event.channelID == 0, "unexpected owned welcome");
                    nonce_ = frame.nonce;
                    generation_ = frame.generation;
                    state_ = "awaiting_mod_name";
                    phase_deadline_ = Clock::now() + std::chrono::seconds(10);
                    emit("owned_welcome", {{"nonce", std::to_string(nonce_)}, {"generation", std::to_string(generation_)}});
                    return;
                }
                require(
                    frame.kind == FrameKind::packet && nonce_ && frame.nonce == nonce_ && frame.generation == generation_ && event.channelID == frame.ordering,
                    "foreign frame session/channel");
                auto& last = receive_sequence_[frame.ordering][frame.reliability];
                if (frame.reliability == 3)
                    require(frame.sequence == last + 1, "reliable-ordered sequence gap");
                else if (frame.reliability != 0 && frame.sequence <= last)
                {
                    emit("stale_frame_dropped");
                    return;
                }
                if (frame.reliability != 0)
                    last = frame.sequence;
                emit("packet_received", {{"packet_id", std::to_string(frame.packet)},
                                         {"bits", std::to_string(frame.bits)},
                                         {"ordering", std::to_string(frame.ordering)},
                                         {"reliability", std::to_string(frame.reliability)},
                                         {"sequence", std::to_string(frame.sequence)}});
                MtaBitStream body(frame.payload.data(), frame.payload.size(), frame.bits, static_cast<unsigned short>(MTA_DM_BITSTREAM_VERSION));
                handle(frame.packet, body);
            }
        }

        void handle(unsigned char id, MtaBitStream& body)
        {
            if (id == PACKET_ID_MOD_NAME)
            {
                require(state_ == "awaiting_mod_name", "unexpected mod-name phase");
                unsigned short version = 0;
                require(body.Read(version) && version == static_cast<unsigned short>(MTA_DM_BITSTREAM_VERSION), "unsupported bitstream version");
                require(headless::read_string(body) == "deathmatch" && body.GetNumberOfUnreadBits() == 0, "unsupported mod name");
                MtaBitStream join;
                headless::write_join_data(join, settings_.nickname);
                send_packet(PACKET_ID_PLAYER_JOINDATA, join);
                state_ = "awaiting_connect_complete";
                phase_deadline_ = Clock::now() + std::chrono::seconds(10);
            }
            else if (id == PACKET_ID_SERVER_JOIN_COMPLETE)
            {
                require(state_ == "awaiting_connect_complete", "unexpected connect-complete phase");
                const auto banner = headless::read_string(body), version = headless::read_string(body);
                require(body.GetNumberOfUnreadBits() == 0, "connect-complete trailing data");
                emit("server_connected", {{"banner", banner}, {"version", version}});
                rpc(headless::source::rpc_ingame);
                state_ = "awaiting_joined_game";
                phase_deadline_ = Clock::now() + std::chrono::seconds(10);
            }
            else if (id == PACKET_ID_SERVER_JOINEDGAME)
            {
                require(state_ == "awaiting_joined_game", "unexpected joined-game phase");
                const auto joined = headless::read_joined(body);
                player_ = joined.player;
                root_ = joined.root;
                emit("joined_game_received", {{"player_id", std::to_string(player_.Value())},
                                              {"root_id", std::to_string(root_.Value())},
                                              {"server_name", joined.server_name},
                                              {"http_type", std::to_string(joined.http_type)}});
                rpc(headless::source::rpc_initial_data);
                state_ = "initial_data_sent";
                emit("initial_data_requested", {{"note", "actual server onPlayerJoin still requires independent witness evidence"}});
            }
            else if (id == PACKET_ID_ENTITY_ADD)
            {
                try
                {
                    const auto prefix = headless::read_entity_prefix(body);
                    if (prefix.vehicle_prefix)
                        vehicles_[prefix.element.Value()] = prefix.model;
                    emit("entity_prefix", {{"element_id", std::to_string(prefix.element.Value())},
                                           {"type", std::to_string(prefix.type)},
                                           {"packet_count", std::to_string(prefix.count)},
                                           {"vehicle_prefix", prefix.vehicle_prefix ? "true" : "false"},
                                           {"model", prefix.vehicle_prefix ? std::to_string(prefix.model) : "not_decoded"},
                                           {"position", prefix.vehicle_prefix ? std::to_string(prefix.position[0]) + "," + std::to_string(prefix.position[1]) +
                                                                                    "," + std::to_string(prefix.position[2])
                                                                              : "not_decoded"},
                                           {"remaining_bits", std::to_string(body.GetNumberOfUnreadBits())},
                                           {"scope", "first entity prefix only; remaining body/entities not decoded"}});
                }
                catch (const std::exception& error)
                {
                    emit("entity_prefix_unsupported", {{"reason", error.what()}, {"scope", "not a complete entity decoder"}});
                }
            }
            else if (id == PACKET_ID_ENTITY_REMOVE)
            {
                constexpr auto bits = NumberOfSignificantBits<(MAX_SERVER_ELEMENTS - 1)>::COUNT;
                while (body.GetNumberOfUnreadBits() >= bits)
                {
                    ElementID removed;
                    require(body.Read(removed), "truncated removed element");
                    vehicles_.erase(removed.Value());
                    emit("entity_removed", {{"element_id", std::to_string(removed.Value())}});
                }
                require(body.GetNumberOfUnreadBits() == 0, "entity-remove trailing partial ID");
            }
            else if (id == PACKET_ID_LUA_EVENT)
            {
                unsigned short length = 0;
                require(body.ReadCompressed(length), "truncated Lua event name");
                const auto name = headless::read_characters(body, length);
                if (name != "headlesswitness:probe")
                {
                    emit("lua_event_unsupported", {{"name", name}});
                    return;
                }
                require(state_ == "initial_data_sent", "probe before actual player identity");
                const auto probe = headless::read_probe(body);
                require(probe.source == root_, "witness event source is not root");
                require(acked_.count(probe.token) == 0, "duplicate witness token");
                const auto known = vehicles_.find(probe.vehicle.Value());
                emit("witness_probe_received", {{"token", probe.token},
                                                {"correlation", probe.correlation},
                                                {"vehicle_id", std::to_string(probe.vehicle.Value())},
                                                {"source_id", std::to_string(probe.source.Value())},
                                                {"entity_prefix_model", known == vehicles_.end() ? "not_observed" : std::to_string(known->second)}});
                MtaBitStream reply;
                headless::write_ack(reply, player_, probe);
                send_packet(PACKET_ID_LUA_EVENT, reply);
                acked_.insert(probe.token);
                emit("witness_ack_sent", {{"token", probe.token},
                                          {"correlation", probe.correlation},
                                          {"source_player", std::to_string(player_.Value())},
                                          {"vehicle_id", std::to_string(probe.vehicle.Value())},
                                          {"scope", "element-reference roundtrip; no native world or server acceptance claim"}});
            }
            else if (id == PACKET_ID_SERVER_DISCONNECTED || id == PACKET_ID_DISCONNECT_MESSAGE)
                throw std::runtime_error("server rejected/disconnected client; inspect packet_received and server logs");
            else
                emit("packet_not_implemented", {{"packet_id", std::to_string(id)}});
        }

        void poll_commands()
        {
            if (quit_ || settings_.commands.empty() || !std::filesystem::exists(settings_.commands))
                return;
            std::ifstream input(settings_.commands, std::ios::binary);
            require(input.good(), "cannot read command file");
            require(std::filesystem::file_size(settings_.commands) <= 65536, "command file exceeds bound");
            const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            require(text.size() <= 65536 && text.size() >= command_offset_, "command file truncated or exceeds bound");
            auto newline = text.find('\n', command_offset_);
            while (newline != std::string::npos)
            {
                auto command = text.substr(command_offset_, newline - command_offset_);
                command_offset_ = newline + 1;
                if (!command.empty() && command.back() == '\r')
                    command.pop_back();
                if (command == "connect")
                    connect();
                else if (command == "disconnect")
                    disconnect(false);
                else if (command == "reconnect")
                    disconnect(true);
                else if (command == "quit")
                {
                    quit_ = true;
                    disconnect(false);
                }
                else if (command == "status")
                    emit("status", {{"player_id", std::to_string(player_.Value())},
                                    {"root_id", std::to_string(root_.Value())},
                                    {"nonce", std::to_string(nonce_)},
                                    {"generation", std::to_string(generation_)}});
                else if (!command.empty())
                    emit("command_rejected", {{"command", command}});
                if (quit_)
                    break;
                newline = text.find('\n', command_offset_);
            }
        }

        Options                                                 settings_;
        std::unique_ptr<ENetHost, decltype(&enet_host_destroy)> host_;
        ENetPeer*                                               peer_ = nullptr;
        Clock::time_point                                       started_ = Clock::now(), phase_deadline_{};
        std::string                                             state_ = "disconnected";
        std::uint64_t                                           nonce_ = 0;
        std::uint32_t                                           generation_ = 0;
        std::array<std::array<std::uint32_t, 5>, 4>             send_sequence_{}, receive_sequence_{};
        ElementID                                               player_{INVALID_ELEMENT_ID}, root_{INVALID_ELEMENT_ID};
        std::map<unsigned int, unsigned short>                  vehicles_;
        std::set<std::string>                                   acked_;
        std::size_t                                             command_offset_ = 0;
        bool                                                    reconnect_ = false, quit_ = false, failed_ = false;
    };
}  // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "--help")
    {
        std::cout << "headless_client --host 127.0.0.1 --port 22003 --identity 32UPPERHEX --nickname H1_A --duration 60 --commands FILE\n"
                     "Append newline-delimited connect/status/disconnect/reconnect/quit to FILE; no blocking stdin thread.\n"
                     "Actual owned MTA protocol only; no GTA, Lua VM, resource downloader or native world.\n";
        return 0;
    }
    try
    {
        auto settings = options(argc, argv);
        require(enet_initialize() == 0, "ENet initialization failed");
        struct Runtime
        {
            ~Runtime() { enet_deinitialize(); }
        } runtime;
        Client client(std::move(settings));
        return client.run();
    }
    catch (const std::exception& error)
    {
        std::cout << "{\"event\":\"fatal_error\",\"reason\":" << quote(error.what()) << ",\"scope\":\"no H1 success claim\"}" << std::endl;
        return 1;
    }
}
