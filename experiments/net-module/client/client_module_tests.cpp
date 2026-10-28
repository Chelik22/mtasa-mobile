#include "ClientModule.h"
#include <Protocol.h>
#include <net/Packets.h>
#include <version.h>
#include <enet/enet.h>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace experimental_net;
using Clock = std::chrono::steady_clock;

void require(bool value, const char* reason)
{
    if (!value)
        throw std::runtime_error(reason);
}
template <typename Function>
Function symbol(HMODULE library, const char* name)
{
    auto address = GetProcAddress(library, name);
    require(address != nullptr, name);
    Function result = nullptr;
    static_assert(sizeof(result) == sizeof(address));
    std::memcpy(&result, &address, sizeof(result));
    return result;
}

struct CallbackState
{
    CNet*                  net = nullptr;
    NetBitStreamInterface* retained = nullptr;
    unsigned               count = 0;
    bool                   bad = false;
    bool                   reset = false;
    ~CallbackState()
    {
        if (retained)
            retained->Release();
    }
} callbacks;

bool packet_callback(unsigned char id, NetBitStreamInterface& stream)
{
    ++callbacks.count;
    callbacks.bad |= id != PACKET_ID_MOD_NAME || stream.Version() != 0x30 || stream.GetNumberOfBitsUsed() != 3;
    unsigned char bits = 0;
    callbacks.bad |= !stream.ReadBits(&bits, 3) || bits != 5;
    if (!callbacks.retained)
    {
        stream.AddRef();
        callbacks.retained = &stream;
    }
    // The loader may recursively pulse while a packet loads the game module.
    callbacks.net->DoPulse();
    if (callbacks.reset)
        callbacks.net->Reset();
    return true;
}

struct HttpContext
{
    CNetHTTPDownloadManagerInterface* manager = nullptr;
    unsigned                          count = 0;
    bool                              bad = false;
    bool                              reset = false;
};
void http_callback(const SHttpDownloadResult& result)
{
    auto& context = *static_cast<HttpContext*>(result.pObj);
    ++context.count;
    context.bad |= result.bSuccess || result.pData || result.dataSize || !result.iErrorCode || result.uiContentLength;
    if (context.reset)
        context.manager->Reset();
}

// A native ENet peer is a transport test fixture, not a CNetServer or game server.
struct Peer
{
    ENetHost*          host = nullptr;
    ENetPeer*          peer = nullptr;
    std::uint64_t      nonce = 0x1122334455667788;
    std::uint32_t      generation = 1;
    unsigned           hellos = 0;
    std::vector<Frame> received;
    Peer()
    {
        ENetAddress address{};
        require(enet_address_set_host_ip(&address, "127.0.0.1") == 0, "loopback resolution");
        host = enet_host_create(&address, 1, 4, 0, 0);
        require(host != nullptr, "native peer bind");
    }
    ~Peer()
    {
        if (host)
            enet_host_destroy(host);
    }
    void send(Frame frame)
    {
        auto  bytes = encode_frame(frame);
        auto* packet = enet_packet_create(bytes.data(), bytes.size(), ENET_PACKET_FLAG_RELIABLE);
        require(packet != nullptr, "native packet allocation");
        if (enet_peer_send(peer, frame.ordering, packet) != 0)
        {
            enet_packet_destroy(packet);
            throw std::runtime_error("native send");
        }
        enet_host_flush(host);
    }
    void pulse()
    {
        ENetEvent event{};
        for (unsigned count = 0; count < 64 && enet_host_service(host, &event, 0) > 0; ++count)
        {
            if (event.type == ENET_EVENT_TYPE_CONNECT)
                peer = event.peer;
            else if (event.type == ENET_EVENT_TYPE_RECEIVE)
            {
                std::unique_ptr<ENetPacket, decltype(&enet_packet_destroy)> packet(event.packet, enet_packet_destroy);
                auto                                                        frame = decode_frame(packet->data, packet->dataLength);
                if (frame.kind == FrameKind::hello)
                {
                    const auto hello = read_hello(frame);
                    require(hello.identity == "0123456789ABCDEF0123456789ABCDEF" && !hello.version.empty(), "actual hello identity/version");
                    require(event.channelID == 0, "hello channel");
                    ++hellos;
                    Frame welcome;
                    welcome.kind = FrameKind::welcome;
                    welcome.nonce = nonce;
                    welcome.generation = generation;
                    send(welcome);
                }
                else
                    received.push_back(std::move(frame));
            }
            else if (event.type == ENET_EVENT_TYPE_DISCONNECT)
                peer = nullptr;
        }
    }
    Frame incoming(unsigned sequence = 1)
    {
        Frame frame;
        frame.packet = PACKET_ID_MOD_NAME;
        frame.nonce = nonce;
        frame.generation = generation;
        frame.sequence = sequence;
        frame.bits = 3;
        frame.payload = {5};
        return frame;
    }
};

template <typename Predicate>
void pump(CNet& net, Peer& peer, Predicate done)
{
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (!done() && Clock::now() < deadline)
    {
        peer.pulse();
        net.DoPulse();
        peer.pulse();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(done(), "bounded transport observation timed out");
}

void unsupported_contracts(CNet& net)
{
    char serial[6]{'x', 'x', 'x', 'x', 'x', 'x'};
    net.GetSerial(serial, sizeof(serial));
    require(std::string(serial) == "01234", "bounded prototype serial");
    char status[1]{'x'};
    net.GetStatus(status, sizeof(status));
    require(!status[0] && !std::strlen(net.GetNextBuffer()), "bounded status/no fake AC report");
    require(!net.VerifySignature("x", 1) && !net.CheckFile("dff", "missing") && !net.ValidateBinaryFileName("missing") &&
                !net.EncryptDumpfile("missing", "must-not-create"),
            "unsupported trust services must fail");
    require(!std::strlen(net.GetCurrentServerId(false)), "no invented trusted server ID");
    ushort reference = 77;
    bool   verified = true;
    net.UpdatePingStatus("", 0, reference, verified);
    require(!verified && reference == 77, "ASE cannot be certified");
    NetStatistics statistics{};
    statistics.bytesSent = 99;
    require(!net.GetNetworkStatistics(&statistics) && !statistics.bytesSent, "unsupported statistics must fail/clear");
    auto* file = net.AllocateBinaryFile();
    require(file != nullptr, "cache caller requires failure object");
    std::unique_ptr<CBinaryFileInterface> owned(file);
    require(!file->FOpen("missing", "r", true) && file->FEof() && file->FTell() == -1, "protected cache fails safely");
    const char  source[] = "return 1";
    const char* output = nullptr;
    uint        size = 0;
    require(net.DeobfuscateScript(source, sizeof(source) - 1, &output, &size, "source.lua") && output == source && size == sizeof(source) - 1,
            "plain source decode is passthrough only");
    const char compiled[] = "\xef\xbb\xbf\x1bLua";
    require(!net.DeobfuscateScript(compiled, sizeof(compiled) - 1, &output, &size, "compiled.lua") && !output && !size,
            "optional BOM cannot bypass compiled Lua rejection");
    auto* http = net.GetHTTPDownloadManager(EDownloadMode::RESOURCE_INITIAL_FILES_INTERNAL);
    require(http != nullptr && net.GetHTTPDownloadManager(static_cast<EDownloadModeType>(-1)) != nullptr, "HTTP managers cannot be null");
    HttpContext first{http}, cancelled{http};
    require(http->QueueFile("http://invalid/", "must-not-create", &first, http_callback), "failure notification queued");
    require(!http->QueueFile("http://invalid/", "must-not-create", &first, http_callback), "duplicate rejection");
    require(http->QueueFile("http://invalid/", "must-not-create", &cancelled, http_callback) && http->CancelDownload(&cancelled, http_callback),
            "HTTP cancellation");
    require(first.count == 0 && http->ProcessQueuedFiles() && first.count == 1 && !first.bad && !cancelled.count, "HTTP settles with actual deferred failure");
    first.reset = true;
    require(http->QueueFile("x", "x", &first, http_callback) && http->QueueFile("y", "y", &cancelled, http_callback), "reentrant reset setup");
    require(http->ProcessQueuedFiles() && first.count == 2 && !cancelled.count, "callback reset cancels remaining requests");
    require(net.GetExtendedErrorCode() != 0, "unsupported service diagnostic");
}

int main(int argc, char** argv)
{
    try
    {
        require(argc == 2, "expected experimental CNet DLL path");
        require(SetEnvironmentVariableA(experimental_client::identity_environment, "0123456789ABCDEF0123456789ABCDEF") != 0, "set prototype identity");
        auto library = LoadLibraryA(argv[1]);
        require(library != nullptr, "LoadLibrary experimental client");
        auto                                                              unload = [](HMODULE module) { FreeLibrary(module); };
        std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(unload)> owner(library, unload);
        const auto                                                        initialize = symbol<experimental_client::Initialize>(library, "InitNetInterface");
        const auto compatibility = symbol<experimental_client::Compatibility>(library, "CheckCompatibility");
        const auto release = symbol<experimental_client::Release>(library, "ReleaseNetInterface");
        struct Cleanup
        {
            experimental_client::Release release;
            ~Cleanup()
            {
                if (callbacks.retained)
                {
                    callbacks.retained->Release();
                    callbacks.retained = nullptr;
                }
                callbacks.net = nullptr;
                release();
            }
        } cleanup{release};
        unsigned long module_version = 0;
        require(compatibility(MTA_DM_CLIENT_NET_MODULE_VERSION, &module_version) && module_version == MTA_DM_CLIENT_NET_MODULE_VERSION &&
                    !compatibility(MTA_DM_CLIENT_NET_MODULE_VERSION ^ 1, nullptr),
                "real client ABI compatibility check");
        // Core is deliberately opaque; this checks the real signature, not Core behavior.
        auto* net = initialize(nullptr);
        require(net && initialize(nullptr) == net && net->IsReady() && !net->IsConnected(), "module initialization/lifecycle");
        callbacks.net = net;
        unsupported_contracts(*net);
        net->SetServerBitStreamVersion(0x30);
        net->Reset();
        require(net->GetServerBitStreamVersion() == 0 && net->GetConnectionError() == 0, "Reset clears version/error");
        net->SetFakeLag(0, 0, 0, 0);
        require(!net->StartNetwork("192.0.2.1", 22003) && !net->StartNetwork("127.0.0.1", 22003, true), "scope/tag unsupported must fail");
        net->Reset();
        net->SetServerBitStreamVersion(0x30);
        net->RegisterPacketHandler(packet_callback);
        require(enet_initialize() == 0, "test ENet initialization");
        struct EnetCleanup
        {
            ~EnetCleanup() { enet_deinitialize(); }
        } enet_cleanup;
        {
            Peer peer;
            require(net->StartNetwork("127.0.0.1", peer.host->address.port) && net->StartNetwork("127.0.0.1", peer.host->address.port),
                    "start retry idempotence");
            pump(*net, peer, [&] { return net->IsConnected(); });
            require(peer.hellos == 1 && std::strlen(net->GetConnectedServer(true)), "one real welcome session");
            auto* stream = net->AllocateNetBitStream();
            require(stream && stream->Version() == 0x30, "versioned actual bitstream");
            stream->Write(static_cast<unsigned char>(0x42));
            require(net->SendPacket(PACKET_ID_MOD_NAME, stream, PACKET_PRIORITY_HIGH, PACKET_RELIABILITY_RELIABLE_ORDERED, PACKET_ORDERING_CHAT),
                    "real outgoing queue");
            net->DeallocateNetBitStream(stream);  // Payload must have been copied.
            pump(*net, peer, [&] { return !peer.received.empty(); });
            const auto& sent = peer.received.back();
            require(sent.nonce == peer.nonce && sent.generation == peer.generation && sent.sequence == 1 && sent.ordering == 1 && sent.bits == 8 &&
                        sent.payload == std::vector<std::uint8_t>{0x42},
                    "real outgoing session/channel/payload");
            peer.send(peer.incoming());
            pump(*net, peer, [&] { return callbacks.count == 1; });
            require(!callbacks.bad && callbacks.retained && callbacks.retained->GetNumberOfBitsUsed() == 3, "actual callback/exact extent/retained stream");
            callbacks.retained->Release();
            callbacks.retained = nullptr;
            // Two different channels may both begin at sequence one.
            auto channel = peer.incoming();
            channel.ordering = 1;
            peer.send(channel);
            pump(*net, peer, [&] { return callbacks.count == 2; });
            callbacks.reset = true;
            peer.send(peer.incoming(2));
            peer.send(peer.incoming(3));
            pump(*net, peer, [&] { return !net->IsConnected(); });
            require(callbacks.count == 3 && net->GetServerBitStreamVersion() == 0 && !callbacks.bad, "callback Reset invalidates pending old session");
            callbacks.reset = false;
            if (callbacks.retained)
            {
                callbacks.retained->Release();
                callbacks.retained = nullptr;
            }
            // Retain no server peer handles while reconnecting this independent ENet host.
        }
        {
            Peer peer;
            ++peer.nonce;
            ++peer.generation;
            net->SetServerBitStreamVersion(0x30);
            require(net->StartNetwork("127.0.0.1", peer.host->address.port), "reconnect start");
            pump(*net, peer, [&] { return net->IsConnected(); });
            auto stale = peer.incoming();
            --stale.nonce;
            peer.send(stale);
            pump(*net, peer, [&] { return !net->IsConnected(); });
            require(net->GetConnectionError() != 0 && callbacks.count == 3, "stale session rejected without callback");
        }
        net->Shutdown();
        require(!net->IsReady() && !net->IsConnected(), "shutdown closes readiness");
        std::cout << "PASS dynamic CNet transport/contracts; pointer_bits=" << sizeof(void*) * 8 << "; game/Core/HTTP/Lua/G4 NOT RUN\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
