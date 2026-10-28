#ifdef _WIN32
    #include <winsock2.h>
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif
#include "MtaBitStream.h"
#include "Protocol.h"
#include <SharedUtil.Time.h>
#include <net/CNetServer.h>
#include <net/Packets.h>
#include <enet/enet.h>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

using namespace experimental_net;
void require(bool value, const char* reason)
{
    if (!value)
        throw std::runtime_error(reason);
}
struct Received
{
    unsigned char          packet;
    NetServerPlayerID      id;
    NetBitStreamInterface* stream;
};
std::vector<Received> received;
bool                  callback(unsigned char packet, const NetServerPlayerID& id, NetBitStreamInterface* stream, SNetExtraInfo*)
{
    // Mirror the real buffer's ownership, so use-after-callback is exercised.
    stream->AddRef();
    received.push_back({packet, id, stream});
    return true;
}
void clear_received()
{
    for (auto& item : received)
        item.stream->Release();
    received.clear();
}
struct Client
{
    ENetHost*          host = nullptr;
    ENetPeer*          peer = nullptr;
    std::vector<Frame> frames;
    bool               connected = false, disconnected = false;
    explicit Client(unsigned short port, unsigned short local_port = 0)
    {
        ENetAddress local{};
        enet_address_set_host_ip(&local, "127.0.0.1");
        local.port = local_port;
        host = enet_host_create(&local, 1, 4, 0, 0);
        require(host != nullptr, "client host");
        // Distinct connect IDs must not depend on allocator reuse or timer resolution.
        static enet_uint32 connect_seed = 0x12340000;
        host->randomSeed = ++connect_seed;
        ENetAddress address{};
        enet_address_set_host_ip(&address, "127.0.0.1");
        address.port = port;
        peer = enet_host_connect(host, &address, 4, 0);
        require(peer != nullptr, "connect allocation");
    }
    ~Client() { close(); }
    void close()
    {
        if (host)
            enet_host_destroy(host);
        host = nullptr;
        peer = nullptr;
    }
    void pulse()
    {
        ENetEvent event{};
        while (enet_host_service(host, &event, 0) > 0)
        {
            if (event.type == ENET_EVENT_TYPE_CONNECT)
                connected = true;
            if (event.type == ENET_EVENT_TYPE_DISCONNECT)
                disconnected = true;
            if (event.type == ENET_EVENT_TYPE_RECEIVE)
            {
                std::unique_ptr<ENetPacket, decltype(&enet_packet_destroy)> owner(event.packet, enet_packet_destroy);
                frames.push_back(decode_frame(event.packet->data, event.packet->dataLength));
            }
        }
    }
    void send(const Frame& frame)
    {
        const auto bytes = encode_frame(frame);
        auto*      packet = enet_packet_create(bytes.data(), bytes.size(), frame.reliability >= 2 ? ENET_PACKET_FLAG_RELIABLE : 0);
        require(packet != nullptr, "packet allocation");
        if (enet_peer_send(peer, frame.ordering, packet) != 0)
        {
            enet_packet_destroy(packet);
            throw std::runtime_error("peer send");
        }
        enet_host_flush(host);
    }
};
template <class Predicate>
void until(CNetServer* server, std::initializer_list<Client*> clients, Predicate done)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done())
    {
        require(std::chrono::steady_clock::now() < end, "network deadline");
        server->DoPulse();
        for (auto* client : clients)
            client->pulse();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
template <class Function, class Symbol>
Function function(Symbol symbol)
{
    require(symbol != nullptr, "missing module export");
    Function result = nullptr;
    static_assert(sizeof(result) == sizeof(symbol));
    std::memcpy(&result, &symbol, sizeof(result));
    return result;
}
int main(int argc, char** argv)
{
    try
    {
        require(argc == 2, "module path required");
#ifdef _WIN32
        const auto library = LoadLibraryA(argv[1]);
        require(library != nullptr, "module LoadLibrary");
        auto                                                              unload = [](HMODULE value) { FreeLibrary(value); };
        std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(unload)> library_owner(library, unload);
        auto                                                              symbol = [&](const char* name) { return GetProcAddress(library, name); };
#else
        auto library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        require(library != nullptr, "module dlopen");
        auto                                    unload = [](void* value) { dlclose(value); };
        std::unique_ptr<void, decltype(unload)> library_owner(library, unload);
        auto                                    symbol = [&](const char* name) { return dlsym(library, name); };
#endif
        auto check = function<unsigned long (*)(unsigned long, unsigned long*)>(symbol("CheckCompatibility"));
        require(check(0xad, reinterpret_cast<unsigned long*>(3)) == 1, "ABI version-type sentinel must not be dereferenced");
        unsigned long abi = 0;
        require(!check(1, &abi) && abi == 0xad && !check(0xae, nullptr), "ABI query/mismatch");
        auto        create = function<CNetServer* (*)()>(symbol("InitNetServerInterface"));
        auto        release = function<void (*)()>(symbol("ReleaseNetServerInterface"));
        CNetServer* server = create();
        require(server && server == create(), "module singleton");
        auto stop = [&](CNetServer*)
        {
            clear_received();
            release();
        };
        std::unique_ptr<CNetServer, decltype(stop)> server_owner(server, stop);
        const char                                  lua[] = "return 42";
        const char*                                 output = nullptr;
        uint                                        length = 0;
        require(server->DeobfuscateScript(lua, sizeof(lua) - 1, &output, &length, "plain") && output == lua && length == sizeof(lua) - 1,
                "plain source Lua buffer");
        require(!server->DeobfuscateScript("\x1bLua", 4, &output, &length, "compiled"), "compiled Lua must be rejected");
        const char bom_binary[] =
            "\xEF\xBB\xBF"
            "\x1bLua";
        require(!server->DeobfuscateScript(bom_binary, sizeof(bom_binary) - 1, &output, &length, "bom-compiled"),
                "compiled Lua behind UTF-8 BOM must be rejected before CLuaMain strips the BOM");
        const char bom_source[] =
            "\xEF\xBB\xBF"
            "return 42";
        require(server->DeobfuscateScript(bom_source, sizeof(bom_source) - 1, &output, &length, "bom-source") && output == bom_source &&
                    length == sizeof(bom_source) - 1,
                "UTF-8 BOM source remains a synchronous pass-through");
        require(!server->GetHTTPDownloadManager(EDownloadMode::CALL_REMOTE)->QueueFile("http://127.0.0.1/", "unused"), "unsupported HTTP must report failure");
        require(!server->StartNetwork("0.0.0.0", 23803, 4, "test"), "lab must stay loopback");
        unsigned short port = 30000;
        while (port < 30100 && !server->StartNetwork("127.0.0.1", port, 4, "module-test"))
            ++port;
        require(port < 30100, "test port unavailable");
        server->RegisterPacketHandler(callback);
        require(enet_initialize() == 0, "host ENet init");
        auto                                    deinit = [](void*) { enet_deinitialize(); };
        std::unique_ptr<void, decltype(deinit)> enet_owner(reinterpret_cast<void*>(1), deinit);
        Client                                  first(port);
        until(server, {&first}, [&] { return first.connected; });
        first.send(make_hello({"11111111111111111111111111111111", "1.6.0-9.99999"}));
        until(server, {&first}, [&] { return !first.frames.empty() && !received.empty(); });
        const Frame original = first.frames.front();
        require(original.kind == FrameKind::welcome && received[0].packet == PACKET_ID_PLAYER_JOIN, "hello must yield transport join");
        const auto id = received[0].id;
        require(server->IsValidSocket(id), "live socket");
        SFixedString<32> serial, version;
        SFixedString<64> extra;
        server->GetClientSerialAndVersion(id, serial, extra, version);
        require(std::string(serial) == "11111111111111111111111111111111", "prototype identity");
        clear_received();
        Frame incoming;
        incoming.packet = PACKET_ID_PLAYER_PURESYNC;
        incoming.nonce = original.nonce;
        incoming.generation = original.generation;
        incoming.sequence = 1;
        incoming.bits = 13;
        incoming.payload = {0xa5, 0x13};
        first.send(incoming);
        until(server, {&first}, [&] { return !received.empty(); });
        require(received[0].stream->GetNumberOfBitsUsed() == 13, "exact incoming bit extent");
        unsigned char value[2]{};
        require(received[0].stream->ReadBits(value, 13) && value[0] == 0xa5 && value[1] == 0x13, "retained callback stream lifetime");
        clear_received();
        // Sequences are independent across reliability modes and ordering channels.
        incoming.reliability = 2;
        incoming.sequence = 1;
        first.send(incoming);
        incoming.reliability = 3;
        incoming.ordering = 1;
        first.send(incoming);
        until(server, {&first}, [&] { return received.size() == 2; });
        clear_received();
        auto* outgoing = server->AllocateNetServerBitStream(0x30);
        require(outgoing != nullptr, "allocate server stream");
        outgoing->WriteBits(value, 13);
        require(server->SendPacket(PACKET_ID_MOD_NAME, id, outgoing, false, PACKET_PRIORITY_HIGH, PACKET_RELIABILITY_RELIABLE_ORDERED), "server send");
        server->DeallocateNetServerBitStream(outgoing);
        until(server, {&first}, [&] { return first.frames.size() == 2; });
        require(first.frames[1].bits == 13 && first.frames[1].payload == std::vector<std::uint8_t>({0xa5, 0x13}), "outgoing payload");
        Client duplicate(port);
        until(server, {&first, &duplicate}, [&] { return duplicate.connected; });
        duplicate.send(make_hello({"11111111111111111111111111111111", "1.6.0-9.99999"}));
        // A second connection with the same identity never becomes a player.
        until(server, {&first, &duplicate}, [&] { return std::string(server->GetLogOutput()).find("duplicate live identity") != std::string::npos; });
        require(received.empty() && server->IsValidSocket(id), "duplicate must not displace live peer");
        enet_peer_disconnect(first.peer, 1);
        enet_host_flush(first.host);
        until(server, {&first}, [&] { return !received.empty(); });
        require(received[0].packet == PACKET_ID_PLAYER_QUIT && !server->IsValidSocket(id), "graceful quit");
        const auto reused_port = first.host->address.port;
        first.close();
        Client reconnect(port, reused_port);
        until(server, {&reconnect}, [&] { return reconnect.connected; });
        reconnect.send(make_hello({"11111111111111111111111111111111", "1.6.0-9.99999"}));
        until(server, {&reconnect}, [&] { return !reconnect.frames.empty() && received.size() == 2; });
        require(reconnect.frames[0].generation == original.generation + 1 && reconnect.frames[0].nonce != original.nonce, "fresh reconnect session");
        require(received[0].packet == PACKET_ID_PLAYER_QUIT && received[1].packet == PACKET_ID_PLAYER_JOIN && received[1].id == id,
                "same endpoint must quit before replacement join");
        const auto reconnect_id = received[1].id;
        clear_received();
        incoming.ordering = 0;
        incoming.reliability = 3;
        incoming.sequence = 1;
        incoming.nonce = original.nonce;
        incoming.generation = original.generation;
        reconnect.send(incoming);
        until(server, {&reconnect}, [&] { return !received.empty(); });
        require(received.size() == 1 && received[0].packet == PACKET_ID_PLAYER_QUIT && !server->IsValidSocket(reconnect_id),
                "stale session must disconnect without payload callback");
        clear_received();

        Client victim(port);
        until(server, {&victim}, [&] { return victim.connected; });
        victim.send(make_hello({"22222222222222222222222222222222", "1.6.0-9.99999"}));
        until(server, {&victim}, [&] { return !victim.frames.empty() && !received.empty(); });
        const auto victim_id = received[0].id;
        const auto occupied_port = victim.host->address.port;
        clear_received();
        victim.close();  // Abrupt socket loss leaves the original server ENet session alive.
        Client collision(port, occupied_port);
        until(server, {&collision}, [&] { return std::string(server->GetLogOutput()).find("occupied UDP endpoint") != std::string::npos; });
        require(received.empty() && server->IsValidSocket(victim_id), "overlapping ENet connect must not overwrite the existing MTA socket");
        server->StopNetwork();
        std::cout << "PASS actual CNetServer DLL/ENet ABI, plain Lua buffer, callback lifetime, exact bits, identity, per-channel sequences, "
                     "quit/reconnect/stale rejection; MTA CPlayer NOT RUN\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
