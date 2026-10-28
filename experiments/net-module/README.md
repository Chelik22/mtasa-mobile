# Experimental MTA network module

This G3 work includes a serializer and an experimental `CNetServer` module.
`MtaBitStream` implements the real public `NetBitStreamInterface` from the pinned
MTA tree. Server handlers and Lua argument serializers can therefore use their
existing API. Closed official `netc`/RakNet wire compatibility is not claimed.

## Build and check

```powershell
cmake -S experiments/net-module -B experiments/net-module/build -G "Visual Studio 18 2026" -A x64
cmake --build experiments/net-module/build --config Release --parallel 1
ctest --test-dir experiments/net-module/build -C Release --output-on-failure
```

The small target uses existing MTA headers, the standard library and native lock
primitives plus pinned ENet. Session nonces use the OS random generator. No GUI
or closed networking binary is linked. The broad
`SharedUtil.hpp` implementation is deliberately avoided to keep this seam small.

## Owned format

| Operation | Encoding |
|---|---|
| Scalar | Fixed-width little-endian; IEEE754 binary32/binary64 |
| Raw partial bits | LSB-first in bytes; MTA partial native-number callers require little-endian hosts |
| Compressed unsigned | Canonical LEB128 with explicit destination-width bound |
| Compressed signed short/int | ZigZag + LEB128 |
| Compressed char | Unsigned byte value; independent of native char signedness |
| Normal vector | Largest index 2 bits, sign 1 bit, two signed16 components; zero marker index3 |
| Normal quaternion | Largest index 2 bits, sign 1 bit, three signed16 components |
| Vector | Binary32 magnitude + packed normalized vector |
| Orthogonal matrix | Proper-rotation validation, normalized quaternion, 51 bits |

Maximum buffer: 1MiB. This is a serializer allocation bound, not yet a measured
MTA application-packet limit. Each transport packet must carry/validate its
payload extent. Noncanonical integers, overflow, nonzero final padding and
out-of-range offsets are rejected. Composite varint/unit/vector readers restore
their offset on failure; individual MTA string helpers keep the existing MTA
behavior. Normalized zero quaternion, nonfinite values and improper matrices
are rejected.

`ModuleRuntime.cpp` preserves upstream native `CCriticalSection` storage because
`CRefCountable::AddRef/Release` can be inlined in another DLL. Replacing that
storage with a C++ mutex would be unsafe: the other DLL's `Lock` implementation
would interpret it as `CRITICAL_SECTION`/`pthread_mutex_t`.

## Evidence boundaries

Tests cover independent scalar/bit/varint golden bytes, malformed input and
limits, normalized basis/rotation cases, real MTA ElementID/string/ISyncStructure
helpers, and reference-count lifetime. A separate shared factory library is
dynamically loaded by a host executable: virtual/helper calls, host-inline
AddRef/Release from two threads, final DLL destruction and unload are exercised.
The factory test is a real native module boundary, distinct from actual MTA
server loading. Player creation and source server Lua are now separately proved
by H1 below; client Lua and gameplay remain later gates. Factory tests have a
15-second deadline so a broken native lock cannot hang
an unattended test run indefinitely.

## Opt-in server module

`net_experimental.dll` exports the real pinned `CNetServer` ABI. The core loader
uses it only when `MTA_EXPERIMENTAL_NET_LIBRARY=net_experimental.dll`; an unset
variable preserves the stock module, and other values fail startup. Copy the
module only into the isolated lab's `x64` directory. The baseline source build
does not include this loader change; it must use its own separately built core.

The module binds only `127.0.0.1`, with at most 32 peers and four ENet channels.
MTAX v1 uses a 32-byte little-endian header (see `Protocol.h`): magic/version,
kind, MTA packet ID, reliability, ordering channel, exact bit extent, random
server nonce, identity generation and per-(channel,reliability) sequence.
Hello contains a prototype 32-hex install identity and printable version. These
are not official serials or anti-cheat evidence. Reconnect gets a fresh nonce
and generation; duplicate live identity and foreign-session frames are rejected.

Incoming callbacks occur only in `DoPulse`, through the callback registered by
MTA's real buffer. Connect/quit/timeout are local notifications with valid empty
streams; clients cannot forge them. Plain source Lua buffers pass through the
synchronous deobfuscation API. Compiled Lua and HTTP queue operations fail
explicitly; official AC services are unavailable. This stage does not implement
client Lua, HTTP resource delivery or a graphical client.

Reliability 0 maps to ENet unsequenced, 1 to ENet unreliable sequenced, and 2–4
currently to reliable ordered delivery. MTAX rejects stale sequenced messages
and keeps sequence counters separate across all four channels/five modes.
Modes 2/4 currently receive stronger ordering than MTA requests; priority is
validated but sends are FIFO. Loss/reorder semantics, reliable-sequenced
coalescing, bounded outgoing queues and priority scheduling remain G3 work.
Do not claim the complete G3 contract from the component tests.

`server_module_tests` loads the actual DLL and exercises ENet peers, native API
calls and retained callback streams. It is a component test, not a running MTA
server: CPlayer and server Lua execution must still be proved by B0/H1.

Actual H1 run `20261028T000420Z-4d6737ba` now proves this module in the source-built
MTA server: Core selected this DLL with no official net DLL present, source Lua
created two real CPlayer elements and one model411 vehicle, three token/element
reference roundtrips were accepted, reconnect preserved the vehicle and both
clients received its deletion. Final players=0, all three processes exited0.
Independent raw-log/input-hash/PID review passed.
[Summary](headless/verification/windows-x64-h1.json). This does not prove client
Lua, HTTP/cache, the remaining reliability/priority/queue contract or gameplay.

### Validated integration contracts

1. `CNetServerBuffer::RegisterPacketHandler` installs its own callback in the real
   module. Deliver only during the module's `DoPulse`; preserve that wrapper
   rather than calling game handlers from a new socket thread.
2. `CNetServerBuffer::ProcessPacket` calls `BitStream->AddRef()` before enqueueing
   and similarly retains non-null `SNetExtraInfo`. Even connect notifications
   therefore need an empty valid stream. The module owns its initial reference
   and releases it after callback delivery; queued references survive separately.
3. `CLuaMain::LoadScriptFromBuffer` always calls the real module's
   `DeobfuscateScript`. Plain source can return the bounded input span unchanged
   for this synchronous call; a blanket false implementation would reject every
   source script. Compiled/obfuscated formats remain explicitly unsupported until
   implemented. Inspect resource metadata callers before deciding GetScriptInfo.

These facts were read in pinned `Server/mods/deathmatch/logic/net/CNetBuffer.cpp`
and `logic/lua/CLuaMain.cpp`. The separate H1 receipt above records actual server
use; these source contracts explain the callback and script lifetime rules.
