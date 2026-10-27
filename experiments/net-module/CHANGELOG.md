# Changelog

## [Unreleased]

### Added

- Owned bounded serializer and an adapter implementing MTA's actual
  NetBitStreamInterface. This preserves the public serialization API while
  avoiding any claim of official netc/RakNet wire compatibility. Golden/malformed
  tests and real MTA helper tests bracket the seam; server integration is pending.
- Native CRefCountable lock storage retained across the module boundary. A
  standard-library mutex would break calls compiled into other MTA modules.
- A real dynamically loaded factory DLL and independent host now exercise
  virtual MTA calls and host-inline reference counting across modules. Test
  threads auto-join on constructor failure, and every check has a deadline.
