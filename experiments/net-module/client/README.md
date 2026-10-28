# Experimental PC CNet component (G4 preparation)

Source-only component; do not install/rename it over production `netc.dll`.
No Client engine source is changed. The root component build must link
`OwnedNetClient.cpp` with the existing `mta_bitstream` and pinned ENet, put
`Client/sdk` before any `Server/sdk` include directory, and include this parent
`net-module` directory and `Shared`. Build the library as `netc_experimental`.
`client_module_tests.cpp` links the same bitstream/ENet and receives that DLL's
absolute path. It uses real LoadLibrary/virtual CNet calls and native loopback
ENet exchange; no GTA, Core instance, CClientGame or CNetServer is started.

The intended engine ABI is Windows x86/cdecl with matching MTA compiler/runtime
and public headers. A 64-bit component pass only proves that target; record
`pointer_bits=32` for the Win32 gate. The initializer accepts the real opaque
`CCoreInterface*`; no fake Core implementation is called. Optional
`ReleaseNetInterface` performs module-side destruction because CNet lacks a
virtual destructor. Release retained bitstreams/binary-file objects before DLL
unload; stop callbacks before releasing the module.

Set `MTA_EXPERIMENTAL_CLIENT_IDENTITY` to a distinct 32-character uppercase hex
prototype identity before initialization. It is not a hardware/official serial.
Only IPv4 loopback endpoints and untagged owned MTAX v1 traffic are supported.
Transport `IsConnected` follows a real welcome, not MOD_NAME, resource readiness
or joined game. `DoPulse` owns networking and callbacks; there is no worker.
Send copies the payload before returning. Sequence counters are per channel and
reliability; ENet reliable mode2 currently has stronger ordering than the public
contract. Priority metadata is validated but queued FIFO. Packet counters combine
real sends/receives and saturate at the public signed-int limit; full RakNet
statistics are unsupported. Graceful disconnect is bounded/best effort; actual
server quit needs separate observation.

HTTP never creates files, returns successful content or claims resource readiness:
unique requests queue explicit unsupported failure callbacks, drained by
`ProcessQueuedFiles`. Callback reset/cancellation removes remaining requests.
The bounded 128-request failure queue rejects overload explicitly; engine opt-in
requires a real downloader/resource-cache design and overload handling first.
Binary-file allocations are real failure objects, separately owned by callers.
Protected cache/model validation, compiled/obfuscated Lua, AC, signatures, trusted
server IDs, official ASE wrapper, crash upload and impairment simulation are
unsupported. Plain source Lua passthrough only returns the original buffer; the
engine's actual Lua loader still decides validity and execution.

See [ABI-AUDIT.md](ABI-AUDIT.md) for loader and all virtual-method contracts.
Before engine opt-in, require independent code review, Win32 DLL loading/tests,
then a reviewed explicit development loader seam, actual PC game packet flow,
resource failures handled visibly, and actual client Lua proof. G4 remains
unpassed until its real gameplay acceptance gate. Android UI/input/game backend
and existing RP resources are outside this module change.
