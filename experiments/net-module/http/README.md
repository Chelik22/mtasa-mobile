# Owned HTTP component

Preparation from MTA fork `6083de505d1b2f38161015b096fe48c816254950`.
Only this directory changes. No CNet integration, engine change, game/GUI launch,
RP rewrite, successful client Lua execution or G4 completion is claimed.

`HttpManager` implements the shared SDK HTTP interface, without including either
client/server CNet headers or their conflicting enums. The concrete type owns its
destructor: never delete through the SDK base (which has no virtual destructor).
Root integration must **destroy all managers during ordinary Shutdown before DLL
unload**, not merely Reset them. Curl's multi/resolver state must be cleaned before
its module code disappears. `curl_global_init/cleanup` run under an explicit shared
runtime lease in ordinary constructors/destructors, never DllMain or a global
loader-time constructor. All callbacks execute in `ProcessQueuedFiles`, under a
recursive lock; no added polling/worker thread exists. The pinned curl resolver
may create its own DNS threads. Do not destroy the manager from its callback.

Required CNet integration lifecycle (documented contract; CNet is not edited here):

- While initialized, GetHTTPDownloadManager returns a borrowed, stable per-mode
  manager. Reset cancels its requests and retains that object and pointer.
- Shutdown first prevents further pulses/queues, settles ownership by cancellation
  without user callbacks, then destroys all real HTTP managers before returning.
  Owners must discard every previously borrowed HTTP pointer when Shutdown begins;
  no pointer is valid across Shutdown or module unload. A Shutdown requested from
  an HTTP callback must be deferred until ProcessQueuedFiles has unwound; destroying
  a manager during its own callback is forbidden.
- Because real callers dereference GetHTTPDownloadManager without null checks,
  calls to that getter after Shutdown must return a module-owned closed failure
  object, never a freed pointer or nullptr. That object performs no curl calls,
  rejects QueueFile with a shutdown diagnostic, and has no pending callbacks.
- Repeated InitNetInterface in the same live DLL is idempotent. After Shutdown,
  it must keep returning the closed instance (IsReady=false), not silently revive
  old managers or callbacks. A fresh module-side Release/Init cycle, or full unload
  and reload by Core, creates a new generation. All old borrowed pointers remain
  invalid; consumers reacquire managers and no old callback crosses generations.

The root CNet adapter/factory must implement and test this seam before integrating
this component. The standalone HttpManager itself supports Reset/reuse, not a
synthetic CNet Shutdown or factory generation.

## Caller contracts

Pinned source references:

- `Shared/sdk/net/CNetHTTPDownloadManagerInterface.h`: Tx strings/maps borrow
  memory; maps count alternating keys/values. Callback buffers are borrowed for
  the callback only. HTTP 400–599 is transport success with body in modern mode,
  failure/empty body in legacy mode. `uiAttemptNumber=0` means queued.
- `Client/mods/deathmatch/logic/CRemoteCalls.cpp:168–242` and server equivalent
  `:213–287`: Queue return is ignored; callbacks consume body/headers immediately,
  may run Lua, delete their request, enqueue new calls or cancel other calls.
  Empty/failure buffers must still be valid strings. Numeric HTTP status remains
  available even for modern 404; application success is decided by the caller.
- `Client/mods/deathmatch/logic/CResourceFileDownloadManager.cpp:254–269,300–343`:
  false QueueFile means duplicate/piggyback, not a useful permanent failure.
  Accepted validation/setup failures therefore receive a deferred failure callback.
  Real resource checksum comparison happens in this caller after download; HTTP
  success itself certifies neither content, cache trust nor script execution.
- `Client/mods/deathmatch/logic/CSingularFileDownload.cpp:35–40,60–82` requests
  checked output files; the actual resource event must follow real file completion.
- `Client/core/CConnectManager.cpp:478–504` may queue two distinct fire-and-forget
  requests with null object/callback. These are not duplicates merely because
  their object/callback pair matches. Cancel/status use the SDK object/callback pair.
- `Client/core/CVersionUpdater.cpp:3768–3819` uses `bResumeFile`, polls actual
  received bytes, then applies its separate downloaded-package checks.
- `vendor/curl/premake5.lua`, `lib/config-win32.h`, `include/curl/curlver.h` pin
  curl 8.20.0 / HTTP_ONLY / Schannel+SSPI+Win32IDN / zlib / threaded DNS. This
  component mirrors those sources and configuration without patching vendor code.

## Behavior and deliberate limits

All Tx strings/maps are copied before QueueFile returns. Method/header names are
validated; binary POST preserves NULs, nonbinary POST truncates at the first NUL,
MIME form fields take precedence over POST data. Username/password, custom method,
headers, redirect limit, connection attempts, connect timeout and low-speed timeout
are applied to curl. Schannel certificate/hostname verification stays enabled.
There is no environment proxy use. HTTP/HTTPS are the only protocols for initial
and redirect requests. Default `loopback_only=true` validates each opened IPv4/IPv6
socket, including redirected destinations. Production/restricted-mode host policy
is not claimed: root must explicitly choose that boundary before external use.

At most 64 requests and 4 simultaneous handles by default (configurable 1–16).
Default memory response bound is 8 MiB, aggregate response buffers 32 MiB, output
file 64 MiB, per-request copied options 1 MiB, maps 64 pairs, headers 64 KiB per
response / 256 KiB across redirects. Limits reject overload; they do not truncate
and report success. Queue overflow/allocation failures return
false with GetError; callers that ignore nonduplicate false need an engine-level
fix before unrestricted use. Ordinary invalid options and unsupported checks are
accepted as deferred failures so those callers do not wait for a fabricated success.

Attempts are bounded to 1–10, connect timeout 100–120000 ms, redirects 0–16, total
request lifetime five minutes and each attempt at most two minutes. Nonlocal
downloads abort below 10 bytes/s after the configured rounded-up timeout. Local
mode disables this low-speed rule, not the hard lifetime. GET/HEAD transport faults
may retry; possibly processed POST/PUT requests never repeat after receive/send
failure. DNS/connect failures may retry any method. Status and counters report
actual transferred bytes/attempts; ResetDownloadSize resets only its counter.

Files use an exclusive adjacent owned temporary, preserve the destination on any
failure, and publish by replace/rename only after successful transfer and required
validation. Temporary files are removed on cancel/reset/failure. Resume GET copies
an existing destination prefix into the temporary and requires a final HTTP 206
Content-Range matching that prefix through the complete representation; an ignored
range or incomplete body cannot publish. Each retry restarts from that original
destination prefix; failed temporary bytes are not a persistent resume cache.
The caller remains responsible for expected checksum/content identity and allowed
output paths. This is not a protected-cache/encryption/signature implementation.

`bCheckContents` has no public implementation in the pinned repository. Without an
injected `FileValidator` it fails before network/file publication. A supplied hook
receives FileValidationContext (temporary/final paths, requested/effective URL,
HTTP status, actual representation bytes) and checks the closed temporary; no stub true validator
is supplied. Tests use an explicitly named fixture-byte validator, not an alleged
implementation of official netc validation. Source Lua has its own later loader.

FixtureContentPolicy is an explicitly temporary G4 policy, not general RP/private
netc compatibility. Its trusted caller supplies cache root and a manifest list of
destination_relative/url/raw_sha256/max_bytes/format entries. The destination key
is exact and root-relative; URL and effective URL must both match (no fixture
redirects), the temporary must be a distinct adjacent ordinary file, and raw bytes
are hashed through BCrypt SHA256 with concurrent writers refused during reading.
No newline/BOM normalization changes the hash. max_bytes=0 permits only empty
files. The manifest must come from the controlled fixture/package receipt, not
merely an untrusted server's claimed checksum. Existing engine CRC/MD5 checks remain.

BinaryAsset accepts pinned binary/text assets without UTF8/NUL requirements;
LuaSourceUtf8 additionally rejects NUL, malformed UTF8 and an optional UTF8 BOM
followed by ESC. Native MZ/ELF and compiled-Lua magic are rejected for fixture
entries even if a caller accidentally pinned them. Lua source validation does not
claim syntax or execution. The policy is currently Win32, source entries are at
most 1 MiB and any entry at most 256 MiB; body limits apply independently.
Local file hashing is bounded synchronous work: keep the first fixture tiny.

ValidateReadSnapshot(canonical_path, span<const unsigned char>, error) validates
the caller's exact retained bytes without reopening a file or inventing an HTTP
context. The caller supplies the canonical identity captured from its verified
read handle. Manifest lookup, bound, SHA256 and format rules are shared with the
download check; URL/status/temp-file rules belong only to the download path.
Successful snapshot validation certifies those bytes, not an HTTP transfer/event.

Packaging schema aligns with the caller's content_manifest.py: destination_relative,
url, raw_sha256, max_bytes, format (lua_source_utf8/binary_asset). Runtime binding
supplies cache root and HTTP origin. The algorithm contains no hardcoded resource,
filename or digest. A package generator must hash actual checkout bytes (including
CRLF), not the normalized Git blob. The old client.lua cache=false stream does not
exercise HTTP; the separately declared file http-witness.txt does.

Callbacks run at most once after removal from the pending list. A callback may
cancel/reset other pending/ready requests, enqueue, and recursively pulse; a nested
pulse does not dispatch callbacks. Cancel/reset suppress cancelled owners and clean
their handles/files. A shared request keeps callback buffers alive throughout
reentry. Validator-triggered cancel/reset also suppresses publish/callback.
Manager lifetime remains the owner's responsibility. C++ callback exceptions are
caught and diagnosed, not retried. Callers copy GetError/buffer strings immediately.

Error code 1007 is local file create/write/close/publish failure (a real caller
special-cases it); 10001/10002/10003 are unsupported/options/bounds; 20000+CURLcode
and 21000+CURLMcode preserve actual transport errors. HTTP status is used when a
complete HTTP reply or legacy HTTP failure is received. These are documented
owned-module codes, not claims about private official netc error numbering.

## Scoped build and proof

Use the installed MSVC/Windows SDK, Win32, one worker, no downloads/installers:

```powershell
cmake -S experiments/net-module/http -B experiments/net-module/http/build-win32 `
  -G 'Visual Studio 18 2026' -A Win32
cmake --build experiments/net-module/http/build-win32 --config Release --parallel 1 -- `
  /nr:false /p:MultiProcessorCompilation=false
ctest --test-dir experiments/net-module/http/build-win32 -C Release --output-on-failure
```

Each C/C++ target also receives `/MP1`. Curl uses BUILDING_LIBCURL, CURL_STATICLIB,
HTTP_ONLY, USE_ZLIB, HAVE_LIBZ, HAVE_ZLIB_H, HAVE_CONFIG_H,
CURL_DISABLE_LIBCURL_OPTION, USE_SCHANNEL, USE_WINDOWS_SSPI, USE_WIN32_IDN;
system links: ws2_32, crypt32, Normaliz, secur32, bcrypt, ncrypt, advapi32, wldap32.
Vendor/zlib is built from its existing sources, not downloaded. CMake and object
outputs are ignored inside `build-win32`; do not import them into Git.

`http_fixture.py` starts an actual localhost ThreadingHTTPServer and the real native
test executable, with bounded process observation and temporary test output. It
checks real GET/binary/empty/404/redirect/auth/method/form/header responses, copied
Tx inputs, range resume and ignored ranges, atomic files, real write failures,
gzip/body bounds, retry versus unsafe POST, low-speed timeout, and callback
reset/cancel/reentry. Server-side request counts assert unsupported validation
never reaches HTTP and a failed POST is not repeated. This is component evidence,
not a game, public service, resource-cache or Lua execution gate.
