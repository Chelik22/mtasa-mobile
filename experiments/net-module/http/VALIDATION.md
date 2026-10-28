# Component preparation evidence

2026-10-28, Windows host, installed MSVC 19.51.36257.0 x86 target,
Windows SDK 10.0.26100.0, CMake 4.3.3, Python 3.14.6.

- CMake standalone configure/generate: exit 0, Visual Studio 18 2026 / Win32.
- Own `HttpManager.cpp` and `http_integration_tests.cpp`: native MSVC `/Zs`
  syntax-only check, `/std:c++20 /EHsc /MP1 /utf-8`, exit 0 before review.
  Uses actual vendored curl headers and Shared SDK headers; no invented SDK typedefs.
- Python localhost fixture: AST parse passed; this does not start its server.
- Scoped clang-format `--dry-run --Werror` and Git whitespace checks passed.
- Vendored curl/zlib cold compilation: **not_run**, held for campaign memory and
  an independently running PC source build. No target DLL/executable was linked.
- Actual localhost native integration test: **not_run**. Test source exists;
  expected behaviors in README/test code are acceptance conditions, not results.
- CNet integration / MTA / GTA / GUI / client Lua / full G4: **not_run**.

The initial own-TU syntax check found the missing Windows IPv6 type include;
`ws2tcpip.h` corrected that before the successful checks. No global toolchain,
PATH, Windows settings, installed MTA/game or upstream source was modified.
Only ignored CMake compiler-detection artifacts were generated under build-win32.
Subsequent native build/test must preserve its actual command, PID/session,
stdout/stderr and terminal exit code; never infer completion from an empty
process-name snapshot or start a duplicate after an observation timeout.

Context-bound fixture policy follow-up: source and meaningful native test cases
were added on feat/http-fixture-policy in a separate worktree. Frozen e14 HTTP
runner sources remain unchanged. Policy runtime and syntax-only checks have not
yet run while G1 causes heavy memory paging. Tests include actual BCrypt known
digests, path/URL/size/hash mismatches, CRLF versus LF raw hashes, matching-hash
compiled Lua/native rejection, binary assets, source-only constraints and empty
files. Expected digest constants were independently calculated with Python hashlib.
