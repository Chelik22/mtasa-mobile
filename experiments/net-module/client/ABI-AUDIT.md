# CNet PC module: source audit

Base: `326ab2bca930a35aea1ff4c4f67fd8082ac7c237`, read before implementing this
adapter. This is preparation for G4; no GTA, PC gameplay, resource download or
client Lua execution is proved by a component build.

## Loader and ownership

`Client/core/CCore.cpp:999–1015` casts the initializer to `T* (*)(U*)` and calls
`InitNetInterface` with its Core pointer. `CreateNetwork:1095–1117` then resolves
`CheckCompatibility` as `unsigned long (*)(unsigned long, unsigned long*)`, checks
`MTA_DM_CLIENT_NET_MODULE_VERSION` and reads revision/release/serial metadata.
These are cdecl C exports for the Windows target; Win32 requires explicit export
names, not an assumption that x64 symbol spelling proves the x86 loader ABI.

`CNet` has no virtual destructor. A host must not `delete CNet*`. Core calls
`Shutdown` before unloading its module (`CCore::DestroyNetwork`). Our optional
`ReleaseNetInterface` is module-side cleanup for component tests, after callbacks
and interface-owned objects have been released. No engine loader is modified here.

The actual public interface is `Client/sdk/net/CNet.h`; its callback type and
enum/struct layouts come from `Client/sdk/net/net_common.h`. Do not include the
server's `CNetServer.h` in the same translation unit: its global names conflict.
Bitstreams retain the existing real `NetBitStreamInterface`/`CRefCountable` ABI.

## Method contracts and first implementation boundary

| CNet method | Observed caller contract / first implementation |
|---|---|
| `StartNetwork` | `CConnectManager::Connect/DoPulse` starts and retries the same endpoint after 4s. Same live endpoint must be idempotent; a different endpoint needs Stop/Reset first. Real ENet + owned hello/welcome, not a synthetic MOD_NAME. |
| `StopNetwork` | Abort/graceful teardown; close only this connection and clear its queues/session. Best-effort bounded graceful disconnect, not proof of server quit. |
| `SetFakeLag` | Connect always calls `(0,0,0,0)` to disable. That case is supported; nonzero impairment is explicit unsupported. |
| `IsReady` | `CCore::IsNetworkReady` gates starting a connection. Means initialized transport/prototype identity, not HTTP/Lua/game readiness. |
| `IsConnected` | True only after real owned welcome establishes nonce/generation; false after stop/loss. |
| `DoPulse` | Core and game call repeatedly; `CModManager` may pulse while loading. Guard reentry; real callbacks may replace handler, reset or stop the connection. |
| `Shutdown` | Core calls before unload; destroy network resources and mark transport not ready. |
| `RegisterPacketHandler` | Connect handler receives MOD_NAME, then the deathmatch module installs its own handler. Never keep invoking a captured old handler after lifecycle/handler mutation. |
| `AllocateNetBitStream` | Caller expects a real writable owned stream; Version tracks selected server bitstream version. |
| `DeallocateNetBitStream` | Release the module's initial reference; callback consumers may retain with AddRef. |
| `SendPacket` | Caller commonly deallocates stream immediately after sending. Copy payload/bit extent into bounded module queue, preserve per-channel/mode sequences. FIFO priority scheduling is explicitly limited. |
| `SetClientPort` | Configure local UDP bind before connection; no live socket rebinding. |
| `GetConnectedServer` | Return stable address text, optionally port, for immediate caller copy. No fabricated successful endpoint. |
| `GetNetworkStatistics` | Complete RakNet statistics are unavailable; return false and cleared output, not synthetic successful stats. |
| `GetPacketStats` | Real per-packet send/receive counts/bytes collected by this module. |
| `GetPing` | Actual ENet roundTripTime when connected, otherwise 0; not a probe success claim. |
| `GetTime` | Monotonic millisecond clock with the public unsigned-long width. |
| `GetLocalIP` | Actual bound loopback address for the currently limited loopback-only transport. |
| `GetSerial` | Core reads into caller-sized buffer at startup. Explicit prototype identity from environment, never an invented official/hardware serial. |
| `GetConnectionError` / `SetConnectionError` | Core/game inspect real error codes before abort; use declared RID values, clear only when requested/reset. |
| `Reset` | `CConnectManager:98–99` and `CModManager:277–278` assert server bitstream version becomes 0. Stop and invalidate old session/queues. |
| `GetHTTPDownloadManager` | Core/game dereference without null checks. Must return a valid failure-capable manager for every download mode, not nullptr. |
| `SetServerBitStreamVersion` / `GetServerBitStreamVersion` | MOD_NAME callback selects server version; inherited CanServerBitStream reads it. Reset restores 0. |
| `GetStatus` | Bounded caller output; state and limitations, no successful resource claim. |
| `GetNetRev` / `GetNetRel` | Build/report metadata; experimental 0/0, not official netc revision. |
| `GetNextBuffer` | `CClientGame:1165` parses nonempty strings as AC reports. Empty means no reports; never inject an unsupported-service message here. AC is unavailable. |
| `GetDiagnosticStatus` | `CClientGame:1206–1214` forwards changes as PLAYER_DIAGNOSTIC; report actual prototype state/limitations here. |
| `UpdatePingStatus` | Server browser asks for verification. Leave verification false; do not certify unsigned ASE data. |
| `VerifySignature` | VersionUpdater trusts true; unsupported must be false. |
| `ResetStub(DWORD,...)` / `(DWORD,va_list)` | DX/debug private hooks are not reproduced. Explicit diagnostic, no AC/success fabrication. |
| `GetCurrentServerId` | `CServerIdManager` uses it for per-server cache identity. Empty unsupported value, not a forged trusted ID. |
| `CheckFile` | Model loaders trust true for DFF/TXD/COL; unsupported must return false. |
| `GetExtendedErrorCode` | Nonzero diagnostic category for unsupported/error cases, not an official netc code. |
| `SetTimeoutTime` | Bound real transport timeout; no infinite wait. |
| `ValidateBinaryFileName` | Protected cache validation unsupported; false. |
| `AllocateBinaryFile` | `CScriptFile:37–55` dereferences without null checks. Return an object whose FOpen fails, with safe other operations; never open plaintext as trusted cache. Caller deletes via its virtual destructor before module unload. |
| `EncryptDumpfile` | False; no successful encryption claim or plaintext output labelled encrypted. |
| `DeobfuscateScript` | `CLuaMain:214–240` expects an output pointer/size before real Lua loading. Plain source pass-through is valid; optional BOM then ESC/compiled data must fail. Decoder success is not Lua execution. |
| `PostCrash` | Unsupported report/crash service, no upload/GUI. |
| `SendTo` | Core's ASE query wrapper has private wire behavior; return SOCKET_ERROR/unsupported rather than imply official query compatibility. |

## HTTP failure behavior needs deferred callbacks

`CResourceFileDownloadManager::BeginResourceFileDownload:261–269` interprets a
false QueueFile return as duplicate/piggybacking and still returns true. Merely
returning false can leave an active resource download waiting forever. A bounded
failure-capable manager should accept a unique failure notification and invoke
its completion callback asynchronously from ProcessQueuedFiles with
`bSuccess=false`, no data and an explicit unsupported error. It must never create
an output file or announce a successful resource. Callback reentry/cancellation
must not invalidate the drain iterator.

`CScriptFile` deletes a failed binary-file object itself; returning a singleton
there would be incorrect ownership. HTTP managers, conversely, are module-owned
and remain valid until module shutdown/unload.

## Thread/lifecycle boundary

No worker or input thread is required for this slice. Core lifecycle/DoPulse are
owned by the calling game thread; sends copy into a bounded protected queue.
Receive callbacks are invoked only from DoPulse. A session epoch prevents using
old ENet pointers or handlers after callback-driven Stop/Reset/Start. Retained
streams can outlive a callback, but must be released before unloading the DLL.
Internal standard-library locks are private; cross-DLL CRefCountable still uses
the existing native lock representation in ModuleRuntime.cpp.

Win32 LoadLibrary/export calls and native transport component tests must precede
any engine opt-in loader patch. Neither a fake Core pointer in an opaque-argument
test nor a transport callback substitutes for actual G4 gameplay/client Lua.
