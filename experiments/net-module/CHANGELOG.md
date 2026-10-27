# Changelog

## [Unreleased]

### Added

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
