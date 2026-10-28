# Changes

- Added optional reuse of the locally built HTTP005 Release curl/zlib archives.
  Configure verifies recorded artifact hashes and the complete unchanged vendor
  input set; other configurations retain source compilation. Positive configure
  passed, and a deliberately changed copied archive was rejected before build.

- Added retained-byte snapshot validation with shared digest/format/path-entry
  rules and no filesystem reopen. Added changed-disk/removal, changed-byte/path
  and compiled-source snapshot cases. Manifest entries also reject Windows aliases
  and case-insensitive duplicates.

- Added context-bound, caller-manifest fixture validation using actual raw SHA256,
  explicit source/binary formats and path/URL binding. Added negative and
  known-digest native test sources; execution remains not_run pending resources.
- Recorded actual frozen-source Win32 curl/zlib/HTTP build and real localhost
  test PASS, raw logs/JUnit, input/artifact hashes and I386 PE metadata. No game,
  CNet integration or later policy execution is implied.

- Added a source-only shared SDK HTTP manager backed by the pinned vendored curl
  multi interface, owned Tx data, real file publication, bounded responses/options
  and explicit callback/cancellation/reset contracts.
- Added an isolated Win32 component build and actual localhost integration test
  sources. Cold dependency compilation and HTTP execution remain not_run pending
  the campaign resource window and independent review.
