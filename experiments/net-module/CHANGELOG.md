# Changelog

## Server HTTP cache preparation

- Реальный gameplay server preflight выявил отсутствующий filename validator.
  Добавлен узкий immutable root/allowlist validator для двух fixture cache paths;
  это не official content scanner и не atomic filesystem sandbox.
- Отсутствующая policy сохраняет прежний отказ filename API без изменения H1
  transport initialization. Native build и actual server replay ещё not_run.

## [Unreleased]

### Fixed

- Use separate SDK-base returns for HTTP manager lookup: the configured facade
  and invalid-mode failure manager have unrelated concrete pointer types, which
  the conditional operator could not combine. Actual configured compile found
  this error; its failed run is retained before the repair.

- Track the imported fixture producer as a CMake configure dependency, so changes
  to its shared path validation regenerate approved entries. Independent review
  found that JSON and generator tracking alone left this input untracked.

- Enforce the prepared read-only file backend's 64 MiB hard cap even when a caller
  requests a larger configured limit; independent review found the missing cap.
  Added a negative configured-limit assertion; native execution is pending.

- Reject binary Lua after an optional UTF-8 BOM, matching CLuaMain's later BOM
  stripping. Checking only the raw first byte allowed compiled chunks through
  a module that explicitly supports source scripts only. BOM-prefixed source
  remains unchanged for the real Lua loader; DLL-boundary regression covers both.

### Added

- Actual configured Win32 CNet service integration passed build and CTest 1/1:
  two real checked HTTP downloads into an empty cache, SDK file read/snapshot,
  changed-disk rejection and callback Shutdown/Release. I386 artifacts, source
  hashes and raw logs are captured. Core/player/client Lua/G4 remain not_run.

- Wired approved CNet binary-file allocations to the tested read-only snapshot
  component and shared immutable manifest policy. The default/no-policy and
  shutdown paths still return explicit failure objects. Configured native CNet
  build/runtime remains pending; HTTP-OFF transport regression passed 1/1.

- Prepared build-time trusted manifest configuration and per-CNet immutable cache
  root policy binding. Generated typed entries avoid a runtime JSON parser;
  missing/invalid roots fail initialization in approved builds. Generator Python
  tests passed 50/50; native configured client build/runtime remains pending.

- Construct per-instance concrete HTTP manager owners before publishing the CNet
  SDK pointer, preparing immutable policy injection without replacing borrowed
  managers. Transport-mode regression build/runtime verification is pending.

- Prepared a real SDK read-only resource-file implementation retaining the bytes
  accepted by an explicit path/content policy. Missing validation, write modes
  and oversized files fail; no protected-cache trust is claimed. Native tests
  cover non-truncation, path replacement after validation and rejected reopen.
  Win32 configure/build and the named native CTest passed 1/1 with an I386
  executable; receipt records hashes/logs. This backend is not wired into CNet.

- Prepared opt-in `MTA_WITH_CLIENT_HTTP=ON` CNet wiring to the real curl backend
  and stable HTTP facade. Reset/Close run outside the transport lock; module
  release checks active HTTP callbacks. Component assertions cover missing
  validator failure and closed-state queue rejection. Build/runtime pending;
  the default remains the previously tested transport-only configuration.

- Prepared a concrete HTTP manager ownership facade for CNet: stable borrowed
  interface, Reset/reuse, closed-state failures, and callback-safe deferred curl
  destruction after a Close request. It is not wired into CNet yet; native review,
  build and lifecycle runtime tests remain pending.

- Prepared fixed-name PC Core loader opt-in via
  `MTA_EXPERIMENTAL_CLIENT_NET_MODULE=netc_experimental`, preserving the stock
  default and SDK ABI check. Invalid overrides fail before loading; a missing
  compatibility export no longer causes a null function call in the error path.
  Independent source/native environment review passed. CMake's Debug postfix
  matches the existing `_d.dll` loader. Core build/game runtime verification and
  the Debug component build are pending.

- Win32 CNet component with the real client SDK interface and undecorated cdecl
  exports. Native DLL/transport tests passed with I386/pointer_bits32, including
  callback lifetime, reset/reconnect and explicit unsupported-service failures.
  This supplies the client transport seam; Core/game/resource integration is not
  yet proved. The real SDK time header is included before packet-stat definitions.

- Actual source-server H1 evidence: the owned DLL served two genuine CPlayer
  clients, server Lua created one vehicle and accepted three token/reference
  roundtrips, reconnect and zero-player/entity cleanup passed with clean exits.
  Independent raw logs/input hashes/PID audit passed; gameplay remains untested.

- An owned MTAX session envelope and opt-in CNetServer DLL over pinned ENet.
  Exact payload bit counts, nonce/generation checks and callback reference
  ownership keep MTA's existing packet API usable without pretending to speak
  the closed official protocol. The stock server module remains the default;
  source Lua is passed through synchronously, unsupported HTTP/compiled Lua/AC
  operations are reported explicitly. Actual server player creation is still a
  separate runtime gate.

- Owned bounded serializer and an adapter implementing MTA's actual
  NetBitStreamInterface. This preserves the public serialization API while
  avoiding any claim of official netc/RakNet wire compatibility. Golden/malformed
  tests and real MTA helper tests bracket the seam; server integration is pending.
- Native CRefCountable lock storage retained across the module boundary. A
  standard-library mutex would break calls compiled into other MTA modules.
- A real dynamically loaded factory DLL and independent host now exercise
  virtual MTA calls and host-inline reference counting across modules. Test
  threads auto-join on constructor failure, and every check has a deadline.
