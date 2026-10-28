#include "ClientModule.h"
#ifdef MTA_WITH_CLIENT_HTTP
    #include "HttpManagerSlot.h"
#endif
#include <Protocol.h>
#include <net/Packets.h>
#include <version.h>
#include <enet/enet.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>

namespace experimental_client
{
    using Clock = std::chrono::steady_clock;
    using experimental_net::Frame;
    using experimental_net::FrameKind;
    using experimental_net::MtaBitStream;
    constexpr uint unsupported_error = 0x4d540001;
    constexpr uint transport_error = 0x4d540002;
    constexpr uint queue_error = 0x4d540003;

    class UnsupportedHttp final : public CNetHTTPDownloadManagerInterface
    {
        struct Failure
        {
            void*                          object;
            PFN_DOWNLOAD_FINISHED_CALLBACK callback;
        };

    public:
        uint        GetDownloadSizeNow() override { return 0; }
        void        ResetDownloadSize() override {}
        const char* GetError() override { return "Owned client: HTTP/resource downloads are unsupported"; }
        bool        ProcessQueuedFiles() override
        {
            {
                std::lock_guard lock(mutex_);
                if (processing_)
                    return false;
                processing_ = true;
            }
            struct Scope
            {
                UnsupportedHttp& owner;
                ~Scope()
                {
                    std::lock_guard lock(owner.mutex_);
                    owner.processing_ = false;
                }
            } scope{*this};
            // Pop individually: a callback may cancel/reset the remaining objects.
            // A copied drain list would still call a now-destroyed request owner.
            for (unsigned count = 0; count < 128; ++count)
            {
                Failure failure{};
                {
                    std::lock_guard lock(mutex_);
                    if (pending_.empty())
                        break;
                    failure = pending_.front();
                    pending_.pop_front();
                }
                if (failure.callback)
                {
                    const SHttpDownloadResult result{nullptr, 0, failure.object, false, 10001, "", 0, 0};
                    failure.callback(result);
                }
            }
            std::lock_guard lock(mutex_);
            return pending_.empty();  // Settled failures, never successful files.
        }
        bool QueueFile(const char*, const char*, void* object, PFN_DOWNLOAD_FINISHED_CALLBACK callback, const SHttpRequestOptionsTx&) override
        {
            std::lock_guard lock(mutex_);
            if (closed_)
                return false;
            for (const auto& item : pending_)
                if (item.object == object && item.callback == callback)
                    return false;
            if (pending_.size() >= 128)
                return false;
            pending_.push_back({object, callback});
            return true;  // The failure callback is deferred until the caller has registered the request.
        }
        void SetMaxConnections(int) override {}
        void Reset() override
        {
            std::lock_guard lock(mutex_);
            pending_.clear();
        }
        void Close()
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
            pending_.clear();
        }
        bool CanRelease()
        {
            std::lock_guard lock(mutex_);
            return !processing_;
        }
        bool CancelDownload(void* object, PFN_DOWNLOAD_FINISHED_CALLBACK callback) override
        {
            std::lock_guard lock(mutex_);
            for (auto it = pending_.begin(); it != pending_.end(); ++it)
                if (it->object == object && it->callback == callback)
                {
                    pending_.erase(it);
                    return true;
                }
            return false;
        }
        bool GetDownloadStatus(void* object, PFN_DOWNLOAD_FINISHED_CALLBACK callback, SDownloadStatus& status) override
        {
            status = {};
            std::lock_guard lock(mutex_);
            for (const auto& item : pending_)
                if (item.object == object && item.callback == callback)
                    return true;
            return false;
        }

    private:
        std::mutex          mutex_;
        std::deque<Failure> pending_;
        bool                processing_ = false;
        bool                closed_ = false;
    };

#ifdef MTA_WITH_CLIENT_HTTP
    using ClientHttp = HttpManagerSlot;
#else
    using ClientHttp = UnsupportedHttp;
#endif

    class UnsupportedFile final : public CBinaryFileInterface
    {
    public:
        bool FOpen(const char*, const char*, bool) override { return false; }
        void FClose() override {}
        bool FEof() override { return true; }
        void FFlush() override {}
        int  FTell() override { return -1; }
        void FSeek(int, int) override {}
        int  FRead(void*, uint) override { return 0; }
        int  FWrite(const void*, uint) override { return 0; }
    };

    class OwnedNetClient final : public CNet
    {
    public:
        OwnedNetClient()
        {
#ifdef _WIN32
            // Read the process environment, not a potentially stale per-CRT copy.
            char identity[34]{};
            if (GetEnvironmentVariableA(identity_environment, identity, sizeof(identity)) == 32 && experimental_net::valid_identity(identity))
                identity_ = identity;
#else
            const char* identity = std::getenv(identity_environment);
            if (identity && experimental_net::valid_identity(identity))
                identity_ = identity;
#endif
            initialized_ = enet_initialize() == 0;
            if (!initialized_)
                error("ENet initialization failed", transport_error);
            else if (identity_.empty())
                error("Set MTA_EXPERIMENTAL_CLIENT_IDENTITY to a 32-character uppercase hex prototype ID", unsupported_error);
            else
                detail_ = "transport initialized; disconnected";
        }
        ~OwnedNetClient() { Shutdown(); }

        bool StartNetwork(const char* host, unsigned short port, bool packet_tag) override
        {
            std::lock_guard lock(mutex_);
            try
            {
                if (!IsReady() || !host || !port)
                    return error("Transport/endpoint is not ready", transport_error);
                if (packet_tag)
                    return error("Official packet tagging is unsupported", unsupported_error);
                ENetAddress target{}, local{};
                if (enet_address_set_host(&target, host) != 0 || enet_address_set_host_ip(&local, "127.0.0.1") != 0)
                    return error("Endpoint resolution failed", transport_error);
                target.port = port;
                if (target.host != local.host)
                    return error("This component is loopback-only", unsupported_error);
                if (host_)
                    return target.host == target_.host && target.port == target_.port ? true : error("Stop/Reset before changing endpoint", transport_error);
                local.port = client_port_;
                host_ = enet_host_create(&local, 1, 4, 0, 0);
                if (!host_)
                    return error("Local ENet bind failed", transport_error);
                host_->maximumPacketSize = experimental_net::frame_header_bytes + experimental_net::BitBuffer::max_bytes;
                host_->maximumWaitingData = 8 * host_->maximumPacketSize;
                peer_ = enet_host_connect(host_, &target, 4, 0);
                if (!peer_)
                {
                    stop(false);
                    return error("ENet connect allocation failed", transport_error);
                }
                target_ = target;
                host_text_ = host;
                endpoint_text_ = host_text_ + ":" + std::to_string(port);
                nonce_ = 0;
                generation_ = 0;
                incoming_sequence_ = {};
                outgoing_sequence_ = {};
                connection_error_ = 0;
                extended_error_ = 0;
                connected_ = false;
                deadline_ = Clock::now() + std::chrono::milliseconds(timeout_ms_);
                ++epoch_;
                detail_ = "connecting to owned MTAX server";
                return true;
            }
            catch (const std::exception&)
            {
                stop(false);
                return error("StartNetwork failed", transport_error);
            }
        }
        void StopNetwork() override
        {
            std::lock_guard lock(mutex_);
            stop(true);
        }
        void SetFakeLag(unsigned short loss, unsigned short ping, unsigned short variance, int limit) override
        {
            std::lock_guard lock(mutex_);
            if (loss || ping || variance || limit)
                error("Fake-lag impairment is unsupported", unsupported_error);
        }
        bool IsReady() override
        {
            std::lock_guard lock(mutex_);
            return initialized_ && !shutdown_ && !identity_.empty();
        }
        bool IsConnected() override
        {
            std::lock_guard lock(mutex_);
            return connected_;
        }

        void DoPulse() override
        {
            std::unique_lock lock(mutex_);
            if (pulsing_ || !initialized_ || shutdown_)
                return;
            pulsing_ = true;
            struct Scope
            {
                OwnedNetClient& owner;
                ~Scope()
                {
                    std::lock_guard guard(owner.mutex_);
                    owner.pulsing_ = false;
                }
            } scope{*this};
            try
            {
                for (unsigned count = 0; count < 64 && host_; ++count)
                {
                    const auto epoch = epoch_;
                    while (!outgoing_.empty())
                    {
                        auto frame = std::move(outgoing_.front());
                        outgoing_.pop_front();
                        outgoing_bytes_ -= frame.payload.size();
                        send_native(frame);
                    }
                    if (!incoming_.empty() && handler_)
                    {
                        auto frame = std::move(incoming_.front());
                        incoming_.pop_front();
                        incoming_bytes_ -= frame.payload.size();
                        auto* stream = new MtaBitStream(frame.payload.data(), frame.payload.size(), frame.bits, server_version_);
                        auto  release = [](NetBitStreamInterface* value) { value->Release(); };
                        std::unique_ptr<NetBitStreamInterface, decltype(release)> owner(stream, release);
                        const auto                                                callback = handler_;
                        const auto                                                began = Clock::now();
                        lock.unlock();
                        try
                        {
                            callback(frame.packet, *stream);
                        }
                        catch (...)
                        {
                            lock.lock();
                            throw;
                        }
                        lock.lock();
                        stats_[frame.packet].totalTime +=
                            static_cast<TIMEUS>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - began).count());
                        if (epoch_ != epoch)
                            break;
                        continue;
                    }
                    ENetEvent event{};
                    const int serviced = enet_host_service(host_, &event, 0);
                    if (serviced < 0)
                        throw std::runtime_error("ENet service failed");
                    if (!serviced)
                        break;
                    if (event.peer != peer_)
                        throw std::runtime_error("Foreign ENet peer");
                    if (event.type == ENET_EVENT_TYPE_CONNECT)
                    {
                        enet_peer_timeout(peer_, 32, timeout_ms_, timeout_ms_);
                        send_native(experimental_net::make_hello({identity_, version_string()}));
                        detail_ = "awaiting owned welcome";
                    }
                    else if (event.type == ENET_EVENT_TYPE_DISCONNECT)
                    {
                        connection_error_ = static_cast<unsigned char>(event.data == 1 ? RID_DISCONNECTION_NOTIFICATION : RID_CONNECTION_LOST);
                        stop(false);
                        error("Transport disconnected", transport_error);
                        break;
                    }
                    else if (event.type == ENET_EVENT_TYPE_RECEIVE)
                    {
                        std::unique_ptr<ENetPacket, decltype(&enet_packet_destroy)> packet(event.packet, enet_packet_destroy);
                        receive(experimental_net::decode_frame(packet->data, packet->dataLength), event.channelID);
                    }
                }
                if (host_ && !connected_ && Clock::now() >= deadline_)
                {
                    connection_error_ = static_cast<unsigned char>(RID_CONNECTION_ATTEMPT_FAILED);
                    stop(false);
                    error("Owned handshake timeout", transport_error);
                }
            }
            catch (...)
            {
                connection_error_ = static_cast<unsigned char>(RID_CONNECTION_LOST);
                stop(false);
                error("Receive/send/callback failure; connection stopped", transport_error);
            }
        }
        void Shutdown() override
        {
            {
                std::lock_guard lock(mutex_);
                stop(true);
                shutdown_ = true;
                if (initialized_)
                {
                    enet_deinitialize();
                    initialized_ = false;
                }
            }
            // HTTP callbacks may enter CNet; never wait for their lock while
            // holding the transport lock in the opposite direction.
            for (auto& http : http_)
                http.Close();
            invalid_http_.Close();
        }
        void RegisterPacketHandler(PPACKETHANDLER handler) override
        {
            std::lock_guard lock(mutex_);
            handler_ = handler;
        }
        NetBitStreamInterface* AllocateNetBitStream() override
        {
            std::lock_guard lock(mutex_);
            try
            {
                return new MtaBitStream(server_version_);
            }
            catch (...)
            {
                error("Bitstream allocation failed", transport_error);
                return nullptr;
            }
        }
        void DeallocateNetBitStream(NetBitStreamInterface* stream) override
        {
            if (stream)
                stream->Release();
        }
        bool SendPacket(unsigned char id, NetBitStreamInterface* stream, NetPacketPriority priority, NetPacketReliability reliability,
                        ePacketOrdering ordering) override
        {
            std::lock_guard lock(mutex_);
            try
            {
                if (!connected_ || !stream)
                    return error("Send requires an established owned session and stream", transport_error);
                const int mode = static_cast<int>(reliability), channel = static_cast<int>(ordering);
                if (id >= NUM_PACKETS || priority < PACKET_PRIORITY_HIGH || priority >= PACKET_PRIORITY_COUNT || mode < 0 || mode > 4 || channel < 0 ||
                    channel > 3)
                    return error("Invalid packet metadata", transport_error);
                const int bits = stream->GetNumberOfBitsUsed(), bytes = stream->GetNumberOfBytesUsed();
                if (bits < 0 || bytes < 0 || static_cast<std::size_t>(bytes) > experimental_net::BitBuffer::max_bytes ||
                    static_cast<std::size_t>(bytes) != (static_cast<std::size_t>(bits) + 7) / 8)
                    return error("Invalid outgoing bit extent", transport_error);
                if (outgoing_.size() >= 128 || outgoing_bytes_ + static_cast<std::size_t>(bytes) > 8 * experimental_net::BitBuffer::max_bytes)
                    return error("Outgoing queue bound exceeded", queue_error);
                Frame frame;
                frame.packet = id;
                frame.reliability = static_cast<unsigned char>(mode);
                frame.ordering = static_cast<unsigned char>(channel);
                frame.nonce = nonce_;
                frame.generation = generation_;
                auto& sequence = outgoing_sequence_[channel][mode];
                if (sequence == 0xffffffffu)
                    return error("Outgoing sequence exhausted", transport_error);
                frame.sequence = sequence + 1;
                frame.bits = static_cast<std::uint32_t>(bits);
                if (bytes)
                {
                    if (!stream->GetData())
                        return error("Null outgoing data", transport_error);
                    frame.payload.assign(stream->GetData(), stream->GetData() + bytes);
                }
                experimental_net::validate_frame(frame);
                outgoing_.push_back(std::move(frame));
                ++sequence;
                outgoing_bytes_ += static_cast<std::size_t>(bytes);
                return true;
            }
            catch (...)
            {
                return error("Packet queue/serialization failed", transport_error);
            }
        }
        void SetClientPort(unsigned short port) override
        {
            std::lock_guard lock(mutex_);
            if (host_)
                error("Cannot rebind an active client port", transport_error);
            else
                client_port_ = port;
        }
        const char* GetConnectedServer(bool include_port) override
        {
            std::lock_guard lock(mutex_);
            return include_port ? endpoint_text_.c_str() : host_text_.c_str();
        }
        bool GetNetworkStatistics(NetStatistics* value) override
        {
            std::lock_guard lock(mutex_);
            if (value)
                *value = {};
            return error("Complete RakNet statistics are unsupported; packet counters are available", unsupported_error);
        }
        const SPacketStat* GetPacketStats() override { return stats_.data(); }
        int                GetPing() override
        {
            std::lock_guard lock(mutex_);
            return connected_ && peer_ ? static_cast<int>(peer_->roundTripTime) : 0;
        }
        unsigned long GetTime() override
        {
            return static_cast<unsigned long>(
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count()) & 0xffffffffu);
        }
        const char* GetLocalIP() override
        {
            std::lock_guard lock(mutex_);
            return host_ ? "127.0.0.1" : "";
        }
        void GetSerial(char* output, size_t size) override
        {
            std::lock_guard lock(mutex_);
            copy(output, size, identity_);
        }
        unsigned char GetConnectionError() override
        {
            std::lock_guard lock(mutex_);
            return connection_error_;
        }
        void SetConnectionError(unsigned char value) override
        {
            std::lock_guard lock(mutex_);
            connection_error_ = value;
        }
        void Reset() override
        {
            {
                std::lock_guard lock(mutex_);
                stop(true);
                server_version_ = 0;
                connection_error_ = 0;
                extended_error_ = 0;
                detail_ = "reset; disconnected";
            }
            for (auto& http : http_)
                http.Reset();
        }
        CNetHTTPDownloadManagerInterface* GetHTTPDownloadManager(EDownloadModeType mode) override
        {
            const auto index = static_cast<int>(mode);
            return index >= 0 && static_cast<std::size_t>(index) < http_.size() ? &http_[index] : &invalid_http_;
        }
        void SetServerBitStreamVersion(unsigned short version) override
        {
            std::lock_guard lock(mutex_);
            server_version_ = version;
        }
        unsigned short GetServerBitStreamVersion() override
        {
            std::lock_guard lock(mutex_);
            return server_version_;
        }
        void GetStatus(char* output, size_t size) override
        {
            std::lock_guard lock(mutex_);
            copy(output, size, diagnostic());
        }
        unsigned short GetNetRev() override { return 0; }
        unsigned short GetNetRel() override { return 0; }
        const char*    GetNextBuffer() override { return ""; }  // No invented AC report.
        const char*    GetDiagnosticStatus() override
        {
            std::lock_guard lock(mutex_);
            return diagnostic().c_str();
        }
        void UpdatePingStatus(const char*, size_t, ushort&, bool& verified) override { verified = false; }
        bool VerifySignature(const char*, unsigned long) override
        {
            std::lock_guard lock(mutex_);
            return error("Signature verification unsupported", unsupported_error);
        }
        void ResetStub(DWORD, ...) override
        {
            std::lock_guard lock(mutex_);
            error("Private netc debug/AC stub unsupported", unsupported_error);
        }
        void ResetStub(DWORD, va_list) override
        {
            std::lock_guard lock(mutex_);
            error("Private netc debug/AC stub unsupported", unsupported_error);
        }
        const char* GetCurrentServerId(bool) override { return ""; }
        bool        CheckFile(const char*, const char*, const void*, size_t) override
        {
            std::lock_guard lock(mutex_);
            return error("Trusted model/cache validation unsupported", unsupported_error);
        }
        uint GetExtendedErrorCode() override
        {
            std::lock_guard lock(mutex_);
            return extended_error_;
        }
        void SetTimeoutTime(uint milliseconds) override
        {
            std::lock_guard lock(mutex_);
            if (milliseconds < 100 || milliseconds > 120000)
            {
                error("Timeout outside supported bounds", unsupported_error);
                return;
            }
            timeout_ms_ = milliseconds;
            if (peer_)
                enet_peer_timeout(peer_, 32, timeout_ms_, timeout_ms_);
        }
        bool ValidateBinaryFileName(const char*) override
        {
            std::lock_guard lock(mutex_);
            return error("Protected cache filename validation unsupported", unsupported_error);
        }
        CBinaryFileInterface* AllocateBinaryFile() override
        {
            std::lock_guard lock(mutex_);
            error("Protected binary cache unsupported; FOpen will fail", unsupported_error);
            return new UnsupportedFile;
        }
        bool EncryptDumpfile(const char*, const char*) override
        {
            std::lock_guard lock(mutex_);
            return error("Dump encryption unsupported", unsupported_error);
        }
        bool DeobfuscateScript(const char* input, uint size, const char** output, uint* output_size, const char*) override
        {
            std::lock_guard lock(mutex_);
            if (output)
                *output = nullptr;
            if (output_size)
                *output_size = 0;
            if (!input || !output || !output_size || size > experimental_net::BitBuffer::max_bytes)
                return error("Invalid source-script buffer", unsupported_error);
            const uint offset = size >= 3 && static_cast<unsigned char>(input[0]) == 0xef && static_cast<unsigned char>(input[1]) == 0xbb &&
                                        static_cast<unsigned char>(input[2]) == 0xbf
                                    ? 3
                                    : 0;
            if (size > offset && static_cast<unsigned char>(input[offset]) == 0x1b)
                return error("Compiled/obfuscated Lua unsupported", unsupported_error);
            *output = input;
            *output_size = size;
            return true;  // Source pass-through only; the caller's real Lua loader still validates/executes it.
        }
        void PostCrash() override
        {
            std::lock_guard lock(mutex_);
            error("Crash reporting/upload unsupported", unsupported_error);
        }
        int SendTo(SOCKET, const char*, int, int, const struct sockaddr*, int) override
        {
            std::lock_guard lock(mutex_);
            error("Official ASE query wrapper unsupported", unsupported_error);
#ifdef _WIN32
            WSASetLastError(WSAEOPNOTSUPP);
#endif
            return -1;
        }
        bool CanRelease()
        {
            {
                std::lock_guard lock(mutex_);
                if (pulsing_)
                    return false;
            }
            for (auto& http : http_)
                if (!http.CanRelease())
                    return false;
            return invalid_http_.CanRelease();
        }

    private:
        static std::string version_string()
        {
            char text[64]{};
            std::snprintf(text, sizeof(text), "%d.%d.%d-%d.%05d.%d", MTASA_VERSION_MAJOR, MTASA_VERSION_MINOR, MTASA_VERSION_MAINTENANCE, MTASA_VERSION_TYPE,
                          MTASA_VERSION_BUILD, 0);
            return text;
        }
        static void copy(char* output, size_t size, const std::string& value)
        {
            if (!output || !size)
                return;
            const auto count = std::min(size - 1, value.size());
            std::memcpy(output, value.data(), count);
            output[count] = '\0';
        }
        bool error(const char* message, uint code)
        {
            detail_ = message;
            extended_error_ = code;
            return false;
        }
        const std::string& diagnostic()
        {
            status_ = "Owned MTAX client; ";
            status_ += connected_ ? "connected; " : "not connected; ";
            status_ += "HTTP/cache/AC/signatures/compiled-Lua unavailable; priority FIFO; ";
            status_ += detail_;
            return status_;
        }
        void stop(bool graceful)
        {
            ++epoch_;
            incoming_.clear();
            outgoing_.clear();
            incoming_bytes_ = outgoing_bytes_ = 0;
            if (host_)
            {
                if (peer_ && graceful)
                {
                    enet_peer_disconnect(peer_, 1);
                    enet_host_flush(host_);
                    ENetEvent  event{};
                    const auto until = Clock::now() + std::chrono::milliseconds(NET_DISCONNECT_DELAY);
                    while (Clock::now() < until)
                    {
                        const auto serviced = enet_host_service(host_, &event, 1);
                        if (serviced < 0)
                            break;
                        if (!serviced)
                            continue;
                        if (event.type == ENET_EVENT_TYPE_RECEIVE)
                            enet_packet_destroy(event.packet);
                        if (event.type == ENET_EVENT_TYPE_DISCONNECT)
                            break;
                    }
                }
                enet_host_destroy(host_);
            }
            host_ = nullptr;
            peer_ = nullptr;
            connected_ = false;
            nonce_ = 0;
            generation_ = 0;
            incoming_sequence_ = {};
            outgoing_sequence_ = {};
            host_text_.clear();
            endpoint_text_.clear();
        }
        void send_native(const Frame& frame)
        {
            const auto        bytes = experimental_net::encode_frame(frame);
            const enet_uint32 flags = frame.reliability >= 2 ? ENET_PACKET_FLAG_RELIABLE : (frame.reliability == 0 ? ENET_PACKET_FLAG_UNSEQUENCED : 0);
            ENetPacket*       packet = enet_packet_create(bytes.data(), bytes.size(), flags);
            if (!packet)
                throw std::runtime_error("Packet allocation failed");
            if (enet_peer_send(peer_, frame.ordering, packet) != 0)
            {
                enet_packet_destroy(packet);
                throw std::runtime_error("Native send failed");
            }
            enet_host_flush(host_);
            if (frame.kind == FrameKind::packet)
            {
                count_packet(frame);
            }
        }
        void receive(Frame frame, enet_uint8 channel)
        {
            if (frame.kind == FrameKind::welcome)
            {
                if (connected_ || nonce_ || channel != 0)
                    throw std::runtime_error("Unexpected welcome");
                nonce_ = frame.nonce;
                generation_ = frame.generation;
                connected_ = true;
                detail_ = "owned welcome accepted; no gameplay/resource readiness implied";
                return;
            }
            if (!connected_ || frame.kind != FrameKind::packet || frame.nonce != nonce_ || frame.generation != generation_ || frame.ordering != channel)
                throw std::runtime_error("Foreign owned packet/session/channel");
            if (frame.packet >= NUM_PACKETS)
                throw std::runtime_error("Unknown packet ID range");
            auto& last = incoming_sequence_[frame.ordering][frame.reliability];
            if (frame.reliability == 3 && frame.sequence != last + 1)
                throw std::runtime_error("Ordered sequence gap");
            if (frame.reliability != 0 && frame.reliability != 3 && frame.sequence <= last)
                return;
            if (frame.reliability != 0)
                last = frame.sequence;
            if (incoming_.size() >= 128 || incoming_bytes_ + frame.payload.size() > 8 * experimental_net::BitBuffer::max_bytes)
                throw std::runtime_error("Incoming queue bound exceeded");
            count_packet(frame);
            incoming_bytes_ += frame.payload.size();
            incoming_.push_back(std::move(frame));
        }

        void count_packet(const Frame& frame)
        {
            auto& value = stats_[frame.packet];
            if (value.iCount < std::numeric_limits<int>::max())
                ++value.iCount;
            value.iTotalBytes +=
                static_cast<int>(std::min(frame.payload.size(), static_cast<std::size_t>(std::numeric_limits<int>::max() - value.iTotalBytes)));
        }

        std::recursive_mutex                        mutex_;
        ENetHost*                                   host_ = nullptr;
        ENetPeer*                                   peer_ = nullptr;
        ENetAddress                                 target_{};
        PPACKETHANDLER                              handler_ = nullptr;
        std::string                                 identity_, host_text_, endpoint_text_, detail_, status_;
        bool                                        initialized_ = false, shutdown_ = false, connected_ = false, pulsing_ = false;
        unsigned short                              client_port_ = 0, server_version_ = 0;
        unsigned char                               connection_error_ = 0;
        uint                                        extended_error_ = 0, timeout_ms_ = 7000;
        std::uint64_t                               epoch_ = 0, nonce_ = 0;
        std::uint32_t                               generation_ = 0;
        Clock::time_point                           deadline_{};
        std::array<std::array<std::uint32_t, 5>, 4> incoming_sequence_{}, outgoing_sequence_{};
        std::deque<Frame>                           incoming_, outgoing_;
        std::size_t                                 incoming_bytes_ = 0, outgoing_bytes_ = 0;
        std::array<SPacketStat, 256>                stats_{};
        std::array<ClientHttp, static_cast<std::size_t>(EDownloadMode::CALL_REMOTE_ANY_HOST_LAST) + 1> http_;
        UnsupportedHttp                                                                                invalid_http_;
    };
}

#ifdef _WIN32
    #define CLIENT_EXPORT extern "C" __declspec(dllexport)
#else
    #define CLIENT_EXPORT extern "C" __attribute__((visibility("default")))
#endif
#if defined(_MSC_VER) && defined(_M_IX86)
    // The real Win32 Core uses these undecorated names with GetProcAddress.
    #pragma comment(linker, "/EXPORT:InitNetInterface=_InitNetInterface")
    #pragma comment(linker, "/EXPORT:CheckCompatibility=_CheckCompatibility")
    #pragma comment(linker, "/EXPORT:ReleaseNetInterface=_ReleaseNetInterface")
#endif

namespace
{
    experimental_client::OwnedNetClient* instance = nullptr;
}
CLIENT_EXPORT unsigned long EXPERIMENTAL_CLIENT_CALL CheckCompatibility(unsigned long requested, unsigned long* output)
{
    if (output)
        *output = MTA_DM_CLIENT_NET_MODULE_VERSION;
    return requested == MTA_DM_CLIENT_NET_MODULE_VERSION;
}
CLIENT_EXPORT CNet* EXPERIMENTAL_CLIENT_CALL InitNetInterface(CCoreInterface*)
{
    if (instance)
        return instance;
    try
    {
        instance = new experimental_client::OwnedNetClient;
        return instance;
    }
    catch (...)
    {
        return nullptr;
    }
}
CLIENT_EXPORT void EXPERIMENTAL_CLIENT_CALL ReleaseNetInterface()
{
    if (instance && instance->CanRelease())
    {
        delete instance;
        instance = nullptr;
    }
}
