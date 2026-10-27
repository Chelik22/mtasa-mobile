#include "wire.hpp"

#include <enet/enet.h>

#include <chrono>
#include <iostream>
#include <map>
#include <memory>

using namespace mobile_net;
using Clock = std::chrono::steady_clock;
using Host = std::unique_ptr<ENetHost, decltype(&enet_host_destroy)>;

void require(bool condition, const char* reason)
{
    if (!condition)
        throw std::runtime_error(reason);
}

Message receive(ENetPacket* packet)
{
    std::unique_ptr<ENetPacket, decltype(&enet_packet_destroy)> owned(packet, enet_packet_destroy);
    return decode(packet->data, packet->dataLength);
}

void send(ENetPeer* peer, const Message& message)
{
    const auto  bytes = encode(message);
    ENetPacket* packet = enet_packet_create(bytes.data(), bytes.size(), ENET_PACKET_FLAG_RELIABLE);
    require(packet != nullptr, "packet allocation failed");
    if (enet_peer_send(peer, 0, packet) != 0)
    {
        enet_packet_destroy(packet);
        throw std::runtime_error("peer send failed");
    }
    enet_host_flush(peer->host);
}

void bound(ENetHost* host)
{
    // Limit reassembly allocation as well as the application-level envelope.
    host->maximumPacketSize = header_size + max_payload;
    host->maximumWaitingData = 16 * (header_size + max_payload);
}

struct PeerState
{
    std::uint64_t session = 0;
    std::uint32_t generation = 0;
    std::uint32_t sequence = 0;
};

struct SessionState
{
    std::uint32_t generation = 0;
    bool          active = false;
};

int server(std::uint16_t port)
{
    ENetAddress address{};
    require(enet_address_set_host_ip(&address, "127.0.0.1") == 0, "loopback address failed");
    address.port = port;
    Host host(enet_host_create(&address, 4, 1, 0, 0), enet_host_destroy);
    require(host != nullptr, "server bind failed");
    bound(host.get());
    std::map<ENetPeer*, PeerState>        peers;
    std::map<std::uint64_t, SessionState> sessions;
    unsigned                              connections = 0, accepted = 0, disconnected = 0;
    std::cout << "{\"event\":\"listening\",\"port\":" << port << "}" << std::endl;
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    while (Clock::now() < deadline && disconnected < 3)
    {
        ENetEvent event{};
        const int result = enet_host_service(host.get(), &event, 25);
        require(result >= 0, "server service failed");
        if (result == 0)
            continue;
        if (event.type == ENET_EVENT_TYPE_CONNECT)
        {
            peers.emplace(event.peer, PeerState{});
            ++connections;
        }
        else if (event.type == ENET_EVENT_TYPE_RECEIVE)
        {
            const auto message = receive(event.packet);
            auto&      peer = peers.at(event.peer);
            if (message.type == Type::hello)
            {
                require(peer.session == 0 && message.sequence == 1, "invalid hello sequence");
                require(sessions.count(message.session) != 0 || sessions.size() < 2, "session limit exceeded");
                auto& session = sessions[message.session];
                require(!session.active && message.generation > session.generation, "duplicate or stale session");
                session = {message.generation, true};
                peer = {message.session, message.generation, message.sequence};
            }
            else
            {
                require(message.type == Type::probe && peer.session != 0 && message.session == peer.session && message.generation == peer.generation &&
                            message.sequence == peer.sequence + 1,
                        "foreign or stale message");
                peer.sequence = message.sequence;
            }
            send(event.peer, {Type::ack, message.session, message.generation, message.sequence, message.payload});
            ++accepted;
        }
        else if (event.type == ENET_EVENT_TYPE_DISCONNECT)
        {
            const auto peer = peers.at(event.peer);
            require(peer.session != 0 && peer.sequence == 2, "disconnected before completed exchange");
            sessions.at(peer.session).active = false;
            peers.erase(event.peer);
            ++disconnected;
        }
    }
    require(connections == 3 && disconnected == 3 && accepted == 6 && sessions.size() == 2 && peers.empty(), "server smoke incomplete");
    unsigned reconnected = 0;
    for (const auto& session : sessions)
        reconnected += session.second.generation == 2 ? 1U : 0U;
    require(reconnected == 1, "reconnect generation missing");
    std::cout << "{\"event\":\"server_complete\",\"connections\":" << connections << ",\"disconnected\":" << disconnected << ",\"accepted\":" << accepted
              << ",\"sessions\":" << sessions.size() << ",\"reconnected\":" << reconnected << "}" << std::endl;
    return 0;
}

void exchange(ENetHost* host, ENetPeer* peer, const Message& request)
{
    send(peer, request);
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < deadline)
    {
        ENetEvent event{};
        const int result = enet_host_service(host, &event, 25);
        require(result >= 0, "client service failed");
        if (result > 0 && event.type == ENET_EVENT_TYPE_RECEIVE)
        {
            const auto reply = receive(event.packet);
            require(event.peer == peer && reply.type == Type::ack && reply.session == request.session && reply.generation == request.generation &&
                        reply.sequence == request.sequence && reply.payload == request.payload,
                    "ack mismatch");
            return;
        }
        require(result == 0 || event.type != ENET_EVENT_TYPE_DISCONNECT, "unexpected disconnect");
    }
    throw std::runtime_error("ack timeout");
}

int client(std::uint16_t port, std::uint64_t session, unsigned cycles)
{
    require(session != 0 && (cycles == 1 || cycles == 2), "invalid client identity or cycles");
    for (unsigned generation = 1; generation <= cycles; ++generation)
    {
        Host host(enet_host_create(nullptr, 1, 1, 0, 0), enet_host_destroy);
        require(host != nullptr, "client host failed");
        bound(host.get());
        ENetAddress address{};
        require(enet_address_set_host_ip(&address, "127.0.0.1") == 0, "client address failed");
        address.port = port;
        ENetPeer* peer = enet_host_connect(host.get(), &address, 1, 0);
        require(peer != nullptr, "connect allocation failed");
        ENetEvent event{};
        require(enet_host_service(host.get(), &event, 5000) > 0 && event.type == ENET_EVENT_TYPE_CONNECT, "connect timeout");
        exchange(host.get(), peer, {Type::hello, session, generation, 1, "hello"});
        exchange(host.get(), peer, {Type::probe, session, generation, 2, "transport-only"});
        enet_peer_disconnect(peer, 0);
        enet_host_flush(host.get());
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        bool       disconnected = false;
        while (Clock::now() < deadline && !disconnected)
        {
            const int result = enet_host_service(host.get(), &event, 25);
            require(result >= 0, "disconnect service failed");
            if (result > 0 && event.type == ENET_EVENT_TYPE_RECEIVE)
                enet_packet_destroy(event.packet);
            disconnected = result > 0 && event.type == ENET_EVENT_TYPE_DISCONNECT;
        }
        require(disconnected, "disconnect timeout");
    }
    std::cout << "{\"event\":\"client_complete\",\"session\":\"" << session << "\",\"completed_cycles\":" << cycles << "}" << std::endl;
    return 0;
}

int main(int argc, char** argv)
{
    try
    {
        require(argc == 3 || argc == 5, "usage: net_probe server PORT | client PORT SESSION CYCLES");
        const auto port = std::stoul(argv[2]);
        require(port > 0 && port <= 65535, "invalid port");
        require(enet_initialize() == 0, "ENet initialization failed");
        struct Cleanup
        {
            ~Cleanup() { enet_deinitialize(); }
        } cleanup;
        if (std::string(argv[1]) == "server" && argc == 3)
            return server(static_cast<std::uint16_t>(port));
        require(std::string(argv[1]) == "client" && argc == 5, "invalid mode");
        const auto cycles = std::stoul(argv[4]);
        require(cycles <= 2, "invalid cycles");
        return client(static_cast<std::uint16_t>(port), std::stoull(argv[3]), static_cast<unsigned>(cycles));
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
