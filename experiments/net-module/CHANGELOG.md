# Changelog

## [Unreleased]

### Fixed

- Reject binary Lua after an optional UTF-8 BOM, matching CLuaMain's later BOM
  stripping. Checking only the raw first byte allowed compiled chunks through
  a module that explicitly supports source scripts only. BOM-prefixed source
  remains unchanged for the real Lua loader; DLL-boundary regression covers both.

### Added

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
