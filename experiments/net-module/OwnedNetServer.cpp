#ifdef _WIN32
    #include <winsock2.h>
    #include <windows.h>
    #include <bcrypt.h>
#endif
#include "MtaBitStream.h"
#include "Protocol.h"
#include <SharedUtil.Time.h>
#include <net/CNetServer.h>
#include <net/Packets.h>
#include <enet/enet.h>
#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <random>

namespace experimental_net
{
    using Clock = std::chrono::steady_clock;
    class DisabledHttp final : public CNetHTTPDownloadManagerInterface
    {
    public:
        uint        GetDownloadSizeNow() override { return 0; }
        void        ResetDownloadSize() override {}
        const char* GetError() override { return "Owned prototype: HTTP downloads are not implemented yet"; }
        bool        ProcessQueuedFiles() override { return true; }
        bool        QueueFile(const char*, const char*, void*, PFN_DOWNLOAD_FINISHED_CALLBACK, const SHttpRequestOptionsTx&) override { return false; }
        void        SetMaxConnections(int) override {}
        void        Reset() override {}
        bool        CancelDownload(void*, PFN_DOWNLOAD_FINISHED_CALLBACK) override { return false; }
        bool        GetDownloadStatus(void*, PFN_DOWNLOAD_FINISHED_CALLBACK, SDownloadStatus&) override { return false; }
    };

    class OwnedNetServer final : public CNetServer
    {
        struct Peer
        {
            ENetPeer*                                   native = nullptr;
            std::string                                 identity, version;
            std::uint64_t                               nonce = 0;
            std::uint32_t                               generation = 0;
            unsigned short                              bitstream = static_cast<unsigned short>(eBitStreamVersion::Latest);
            bool                                        ready = false;
            Clock::time_point                           accepted = Clock::now();
            std::array<std::array<std::uint32_t, 5>, 4> incoming{}, outgoing{};
            NetStatistics                               stats{};
        };

    public:
        OwnedNetServer()
        {
            if (enet_initialize() != 0)
                throw std::runtime_error("ENet initialization failed");
            log_ = "Owned ENet prototype: official AC/compiled Lua/HTTP unavailable; only the MTAX protocol is accepted.";
        }
        ~OwnedNetServer()
        {
            StopNetwork();
            enet_deinitialize();
        }
        bool StartNetwork(const char* ip, unsigned short port, unsigned int players, const char*) override
        {
            std::lock_guard lock(mutex_);
            if (host_ || !ip || std::string(ip) != "127.0.0.1" || !players || players > 32)
                return false;
            ENetAddress address{};
            if (enet_address_set_host_ip(&address, ip) != 0)
                return false;
            address.port = port;
            host_ = enet_host_create(&address, players, 4, 0, 0);
            if (!host_)
                return false;
            host_->maximumPacketSize = frame_header_bytes + BitBuffer::max_bytes;
            host_->maximumWaitingData = 16 * (frame_header_bytes + BitBuffer::max_bytes);
            maximum_ = players;
            started_ = Clock::now();
            return true;
        }
        void StopNetwork() override
        {
            std::lock_guard lock(mutex_);
            if (host_)
                enet_host_destroy(host_);
            host_ = nullptr;
            peers_.clear();
        }
        void RegisterPacketHandler(PPACKETHANDLER callback) override
        {
            std::lock_guard lock(mutex_);
            callback_ = callback;
        }
        void DoPulse() override
        {
            std::lock_guard lock(mutex_);
            if (!host_)
                return;
            for (unsigned count = 0; count < 128; ++count)
            {
                ENetEvent event{};
                const int result = enet_host_service(host_, &event, 0);
                if (result < 0)
                {
                    log_ = "ENet service failed";
                    break;
                }
                if (!result)
                    break;
                const auto id = socket_id(event.peer);
                if (event.type == ENET_EVENT_TYPE_CONNECT)
                {
                    // ENet connect IDs can distinguish two sessions using the
                    // same UDP endpoint; MTA's socket ID cannot. Keep the live
                    // mapping until its own disconnect instead of overwriting it.
                    if (peers_.contains(id))
                    {
                        log_ = "Rejected owned connection: occupied UDP endpoint";
                        enet_peer_disconnect_now(event.peer, 0);
                        continue;
                    }
                    if (peers_.size() >= maximum_)
                    {
                        enet_peer_disconnect_now(event.peer, 0);
                        continue;
                    }
                    Peer peer;
                    peer.native = event.peer;
                    peers_.insert_or_assign(id, std::move(peer));
                }
                else if (event.type == ENET_EVENT_TYPE_RECEIVE)
                {
                    std::unique_ptr<ENetPacket, decltype(&enet_packet_destroy)> packet(event.packet, enet_packet_destroy);
                    auto                                                        found = peers_.find(id);
                    if (found == peers_.end() || found->second.native != event.peer)
                    {
                        enet_peer_disconnect_now(event.peer, 0);
                        continue;
                    }
                    try
                    {
                        receive(id, found->second, event.channelID, event.packet);
                    }
                    catch (const std::exception& error)
                    {
                        log_ = std::string("Rejected owned packet: ") + error.what();
                        disconnect(id, PACKET_ID_PLAYER_QUIT);
                        enet_peer_disconnect_now(event.peer, 0);
                    }
                }
                else if (event.type == ENET_EVENT_TYPE_DISCONNECT)
                {
                    const auto found = peers_.find(id);
                    if (found != peers_.end() && found->second.native == event.peer)
                        disconnect(id, event.data == 1 ? PACKET_ID_PLAYER_QUIT : PACKET_ID_PLAYER_TIMEOUT);
                }
            }
            std::vector<NetServerPlayerID> expired;
            for (const auto& [id, peer] : peers_)
                if (!peer.ready && Clock::now() - peer.accepted > std::chrono::seconds(5))
                    expired.push_back(id);
            for (const auto& id : expired)
            {
                auto found = peers_.find(id);
                if (found != peers_.end())
                    enet_peer_disconnect_now(found->second.native, 0);
                disconnect(id, PACKET_ID_PLAYER_TIMEOUT);
            }
            enet_host_flush(host_);
        }
        NetBitStreamInterface* AllocateNetServerBitStream(unsigned short version, const void* data, uint bytes, bool) override
        {
            try
            {
                return bytes ? new MtaBitStream(data, bytes, version) : new MtaBitStream(version);
            }
            catch (const std::exception& error)
            {
                std::lock_guard lock(mutex_);
                log_ = error.what();
                return nullptr;
            }
        }
        void DeallocateNetServerBitStream(NetBitStreamInterface* stream) override
        {
            if (stream)
                stream->Release();
        }
        bool SendPacket(unsigned char packet, const NetServerPlayerID& id, NetBitStreamInterface* stream, bool broadcast, NetServerPacketPriority priority,
                        NetServerPacketReliability reliability, ePacketOrdering ordering) override
        {
            std::lock_guard lock(mutex_);
            if (!host_ || !stream || packet >= NUM_PACKETS || priority < 0 || priority >= PACKET_PRIORITY_COUNT || reliability < 0 || reliability > 4 ||
                ordering < 0 || ordering > 3)
                return false;
            bool sent = false;
            for (auto& [target, peer] : peers_)
            {
                if (!peer.ready || (!broadcast && target != id))
                    continue;
                Frame frame;
                frame.packet = packet;
                frame.reliability = static_cast<std::uint8_t>(reliability);
                frame.ordering = static_cast<std::uint8_t>(ordering);
                frame.nonce = peer.nonce;
                frame.generation = peer.generation;
                auto& sequence = peer.outgoing[ordering][reliability];
                if (sequence == std::numeric_limits<std::uint32_t>::max())
                    return false;
                frame.sequence = sequence + 1;
                const int bytes = stream->GetNumberOfBytesUsed(), bits = stream->GetNumberOfBitsUsed();
                if (bytes < 0 || bits < 0 || static_cast<std::size_t>(bytes) > BitBuffer::max_bytes)
                    return false;
                frame.bits = static_cast<std::uint32_t>(bits);
                if (bytes)
                    frame.payload.assign(stream->GetData(), stream->GetData() + bytes);
                if (send(peer, frame))
                {
                    ++sequence;
                    sent = true;
                    ++packet_stats_[packet].iCount;
                    packet_stats_[packet].iTotalBytes += bytes;
                }
            }
            return sent;
        }
        bool IsValidSocket(const NetServerPlayerID& id) override
        {
            std::lock_guard lock(mutex_);
            auto            found = peers_.find(id);
            return found != peers_.end() && found->second.ready;
        }
        void Kick(const NetServerPlayerID& id) override
        {
            std::lock_guard lock(mutex_);
            auto            found = peers_.find(id);
            if (found != peers_.end())
                enet_peer_disconnect(found->second.native, 1);
        }
        void GetPlayerIP(const NetServerPlayerID& id, char output[22], unsigned short* port) override
        {
            std::lock_guard lock(mutex_);
            ENetAddress     address{static_cast<enet_uint32>(id.GetBinaryAddress()), id.GetPort()};
            if (enet_address_get_host_ip(&address, output, 22) != 0)
                output[0] = 0;
            if (port)
                *port = id.GetPort();
        }
        void GetClientSerialAndVersion(const NetServerPlayerID& id, SFixedString<32>& identity, SFixedString<64>& extra, SFixedString<32>& version) override
        {
            std::lock_guard lock(mutex_);
            auto            found = peers_.find(id);
            identity = found == peers_.end() ? "" : found->second.identity.c_str();
            version = found == peers_.end() ? "" : found->second.version.c_str();
            extra = "owned-protocol prototype; not official AC";
        }
        void SetClientBitStreamVersion(const NetServerPlayerID& id, unsigned short version) override
        {
            std::lock_guard lock(mutex_);
            auto            found = peers_.find(id);
            if (found != peers_.end())
                found->second.bitstream = version;
        }
        void ClearClientBitStreamVersion(const NetServerPlayerID& id) override
        {
            SetClientBitStreamVersion(id, static_cast<unsigned short>(eBitStreamVersion::Latest));
        }
        void SetPassword(const char* password) override
        {
            std::lock_guard lock(mutex_);
            password_ = password ? password : "";
        }
        void SetMaximumIncomingConnections(unsigned short count) override
        {
            std::lock_guard lock(mutex_);
            maximum_ = std::min<unsigned>(32, count);
        }
        CNetHTTPDownloadManagerInterface* GetHTTPDownloadManager(EDownloadModeType) override { return &http_; }
        bool                              GetScriptInfo(const char*, uint, SScriptInfo*) override { return false; }
        bool                              DeobfuscateScript(const char* data, uint size, const char** output, uint* output_size, const char*) override
        {
            if (!output || !output_size || (!data && size) || size > 16 * 1024 * 1024)
                return false;
            // CLuaMain strips a UTF-8 BOM before calling Lua's loader. Inspect
            // the same first meaningful byte so binary chunks cannot bypass
            // this source-only boundary by prepending that BOM.
            const uint prefix = size >= 3 && std::memcmp(data, "\xEF\xBB\xBF", 3) == 0 ? 3 : 0;
            if (size > prefix && static_cast<unsigned char>(data[prefix]) == 0x1b)
                return false;
            *output = data ? data : "";
            *output_size = size;
            return true;
        }
        void GenerateRandomData(void* output, uint size) override
        {
            if (!output && size)
                throw std::invalid_argument("null random buffer");
#ifdef _WIN32
            if (BCryptGenRandom(nullptr, static_cast<PUCHAR>(output), size, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
                throw std::runtime_error("OS random failed");
#else
            std::random_device random;
            auto*              bytes = static_cast<unsigned char*>(output);
            for (uint i = 0; i < size; ++i)
                bytes[i] = static_cast<unsigned char>(random());
#endif
        }
        bool InitServerId(const char* path) override
        {
            if (!path || !*path)
                return false;
            std::ifstream existing(path);
            std::string   value;
            if (existing >> value)
                return valid_identity(value);
            std::array<unsigned char, 16> random{};
            GenerateRandomData(random.data(), static_cast<uint>(random.size()));
            const char* hex = "0123456789ABCDEF";
            for (auto byte : random)
            {
                value.push_back(hex[byte >> 4]);
                value.push_back(hex[byte & 15]);
            }
            std::ofstream file(path, std::ios::out | std::ios::trunc);
            file << value << '\n';
            return bool(file);
        }
        bool GetNetworkStatistics(NetStatistics* output, const NetServerPlayerID& id) override
        {
            if (!output)
                return false;
            std::lock_guard lock(mutex_);
            auto            found = peers_.find(id);
            if (found == peers_.end())
                return false;
            *output = found->second.stats;
            output->packetlossTotal = 100.0f * found->second.native->packetLoss / static_cast<float>(ENET_PEER_PACKET_LOSS_SCALE);
            output->packetlossLastSecond = output->packetlossTotal;
            return true;
        }
        const SPacketStat* GetPacketStats() override
        {
            thread_local std::array<SPacketStat, 256> copy{};
            std::lock_guard                           lock(mutex_);
            copy = packet_stats_;
            return copy.data();
        }
        bool GetBandwidthStatistics(SBandwidthStatistics* output) override
        {
            if (!output)
                return false;
            std::lock_guard lock(mutex_);
            *output = {};
            output->llOutgoingUDPByteCount = sent_bytes_;
            output->llIncomingUDPByteCount = received_bytes_;
            output->llOutgoingUDPPacketCount = sent_packets_;
            output->llIncomingUDPPacketCount = received_packets_;
            return true;
        }
        bool GetNetPerformanceStatistics(SNetPerformanceStatistics*, bool) override { return false; }
        bool GetSyncThreadStatistics(SSyncThreadStatistics*, bool) override { return false; }
        void GetPingStatus(SFixedString<32>* output) override
        {
            if (output)
                *output = "ENet RTT; prototype";
        }
        unsigned int GetPendingPacketCount() override { return 0; }  // Callbacks are delivered synchronously during DoPulse.
        void         GetNetRoute(SFixedString<32>* output) override
        {
            if (output)
                *output = "MTAX/ENet loopback";
        }
        void SetChecks(const char*, const char*, const char*, int, bool, const char*) override
        {
            std::lock_guard lock(mutex_);
            log_ = "Owned protocol: official AC checks are unavailable";
        }
        void SetNetOptions(const SNetOptions& options) override
        {
            std::lock_guard lock(mutex_);
            if (options.netSim.bValid || options.netFilter.bValid || options.netOptimize.bValid)
                log_ = "Owned protocol: network option tuning is not implemented";
        }
        void ResendModPackets(const NetServerPlayerID&) override {}
        void ResendACPackets(const NetServerPlayerID&) override {}
        void SetMinClientRequirement(const char* requirement) override
        {
            std::lock_guard lock(mutex_);
            requirement_ = requirement ? requirement : "";
        }
        const char* GetLogOutput() override
        {
            thread_local std::string copy;
            std::lock_guard          lock(mutex_);
            // CGame drains this method every pulse; returning the same text
            // repeatedly would flood the server log while otherwise idle.
            copy.clear();
            copy.swap(log_);
            return copy.c_str();
        }

    private:
        static NetServerPlayerID socket_id(ENetPeer* peer) { return {peer->address.host, peer->address.port}; }
        bool                     send(Peer& peer, const Frame& frame)
        {
            try
            {
                const auto  data = encode_frame(frame);
                enet_uint32 flags = frame.reliability >= 2 ? ENET_PACKET_FLAG_RELIABLE : (frame.reliability == 0 ? ENET_PACKET_FLAG_UNSEQUENCED : 0);
                ENetPacket* packet = enet_packet_create(data.data(), data.size(), flags);
                if (!packet)
                    return false;
                if (enet_peer_send(peer.native, frame.ordering, packet) != 0)
                {
                    enet_packet_destroy(packet);
                    return false;
                }
                ++peer.stats.packetsSent;
                peer.stats.bytesSent += data.size();
                ++sent_packets_;
                sent_bytes_ += data.size();
                return true;
            }
            catch (const std::exception& error)
            {
                log_ = error.what();
                return false;
            }
        }
        void receive(const NetServerPlayerID& id, Peer& peer, unsigned channel, ENetPacket* packet)
        {
            const auto frame = decode_frame(packet->data, packet->dataLength);
            ++peer.stats.packetsReceived;
            peer.stats.bytesReceived += packet->dataLength;
            ++received_packets_;
            received_bytes_ += packet->dataLength;
            if (channel != frame.ordering)
                throw std::invalid_argument("ordering channel mismatch");
            if (!peer.ready)
            {
                const auto hello = read_hello(frame);
                for (const auto& [other, candidate] : peers_)
                    if (other != id && candidate.ready && candidate.identity == hello.identity)
                        throw std::invalid_argument("duplicate live identity");
                // A bounded history preserves reconnect generations without allowing
                // unlimited identities to grow the long-running lab process.
                if (!generations_.contains(hello.identity) && generations_.size() >= 4096)
                    throw std::invalid_argument("prototype identity history capacity exhausted");
                auto& generation = generations_[hello.identity];
                if (generation == std::numeric_limits<std::uint32_t>::max())
                    throw std::invalid_argument("generation exhausted");
                peer.identity = hello.identity;
                peer.version = hello.version;
                peer.generation = ++generation;
                do
                {
                    GenerateRandomData(&peer.nonce, sizeof(peer.nonce));
                } while (!peer.nonce);
                Frame welcome;
                welcome.kind = FrameKind::welcome;
                welcome.nonce = peer.nonce;
                welcome.generation = peer.generation;
                if (!send(peer, welcome))
                    throw std::runtime_error("welcome send failed");
                peer.ready = true;
                deliver(PACKET_ID_PLAYER_JOIN, id, nullptr, 0, peer.bitstream);
                return;
            }
            if (frame.kind != FrameKind::packet || frame.nonce != peer.nonce || frame.generation != peer.generation || frame.packet >= NUM_PACKETS)
                throw std::invalid_argument("foreign session or packet");
            // These notifications belong to the transport, never to remote clients.
            if (frame.packet == PACKET_ID_PLAYER_JOIN || frame.packet == PACKET_ID_PLAYER_QUIT || frame.packet == PACKET_ID_PLAYER_TIMEOUT ||
                frame.packet == PACKET_ID_SERVER_DISCONNECTED)
                throw std::invalid_argument("remote transport notification");
            auto& sequence = peer.incoming[channel][frame.reliability];
            if (frame.reliability == 3 && frame.sequence != sequence + 1)
                throw std::invalid_argument("ordered sequence gap");
            if (frame.reliability != 0 && frame.sequence <= sequence)
                return;
            sequence = std::max(sequence, frame.sequence);
            deliver(frame.packet, id, frame.payload.data(), frame.bits, peer.bitstream);
        }
        void deliver(unsigned char packet, const NetServerPlayerID& id, const void* data, std::size_t bits, unsigned short version)
        {
            if (!callback_)
                return;
            auto* stream = new MtaBitStream(data, (bits + 7) / 8, bits, version);
            // The real CNetServerBuffer callback retains an extra reference when queued.
            auto                                                      release = [](NetBitStreamInterface* value) { value->Release(); };
            std::unique_ptr<NetBitStreamInterface, decltype(release)> owner(stream, release);
            callback_(packet, id, stream, nullptr);
        }
        void disconnect(const NetServerPlayerID& id, unsigned char packet)
        {
            auto found = peers_.find(id);
            if (found == peers_.end())
                return;
            const bool ready = found->second.ready;
            peers_.erase(found);
            // Preserve transport event order: a reused IP/port's old player must
            // quit before a subsequent hello can enqueue the replacement join.
            if (ready)
                deliver(packet, id, nullptr, 0, 0);
        }
        std::recursive_mutex                 mutex_;
        ENetHost*                            host_ = nullptr;
        PPACKETHANDLER                       callback_ = nullptr;
        std::map<NetServerPlayerID, Peer>    peers_;
        std::map<std::string, std::uint32_t> generations_;
        std::array<SPacketStat, 256>         packet_stats_{};
        std::uint64_t                        sent_bytes_ = 0, received_bytes_ = 0, sent_packets_ = 0, received_packets_ = 0;
        unsigned                             maximum_ = 0;
        Clock::time_point                    started_ = Clock::now();
        std::string                          log_, password_, requirement_;
        DisabledHttp                         http_;
    };
}  // namespace experimental_net

#ifdef _WIN32
    #define NET_EXPORT extern "C" __declspec(dllexport)
#else
    #define NET_EXPORT extern "C" __attribute__((visibility("default")))
#endif
namespace
{
    experimental_net::OwnedNetServer* instance = nullptr;
}
NET_EXPORT unsigned long CheckCompatibility(unsigned long requested, unsigned long* output)
{
    constexpr unsigned long server_abi = 0x0ad;  // pinned Shared/sdk/version.h
    if (requested == 1)
    {
        if (output)
            *output = server_abi;
        return 0;
    }
    return requested == server_abi;
}
NET_EXPORT CNetServer* InitNetServerInterface()
{
    if (instance)
        return instance;
    try
    {
        instance = new experimental_net::OwnedNetServer;
        return instance;
    }
    catch (...)
    {
        return nullptr;
    }
}
NET_EXPORT void ReleaseNetServerInterface()
{
    delete instance;
    instance = nullptr;
}
