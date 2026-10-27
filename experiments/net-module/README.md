# Experimental MTA network module — serialization layer

This is the first G3 implementation layer, not a replacement `net.dll` yet.
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
primitives. No crypto, GUI or closed networking binary is linked. The broad
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
This is a real native module boundary, not CNetServer loading. Player creation,
server/client Lua execution, transport behavior and gameplay remain G3/H1 and
later gates. Tests have a 15-second deadline so a broken native lock cannot hang
an unattended test run indefinitely.

Next: implement the opt-in CNetServer ABI with ENet delivery, thread-safe queues,
prototype identity, source Lua support, honest unsupported HTTP services, and
loader checks before attempting two real headless players.

### Verified integration constraints for the next change

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
and `logic/lua/CLuaMain.cpp`; they are implementation prerequisites, not evidence
that the server has used the new module.
