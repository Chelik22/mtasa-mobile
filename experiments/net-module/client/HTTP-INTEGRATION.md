# PC HTTP integration prerequisites

Source audit at `966fbdd27`, 2026-10-28. This is an integration contract, not a
runtime result. Standalone HTTP compile/link/tests and Core gameplay are pending.

`CResourceFileDownloadManager::BeginResourceFileDownload` always sets
`bCheckContents=true` before `QueueFile`. `CSingularFileDownload` does likewise.
The current backend requires an explicit validator for that flag; simply wiring
the default manager into CNet would therefore reject every resource download.
Do not replace this with unconditional success or silently ignore the flag.

The SDK describes this flag as checking for naughty contents before saving; its
private implementation is unavailable. Our validator must document its own
policy and test actual downloaded bytes before atomic publication. It cannot
claim equivalence to the official validator. The engine separately compares the
downloaded resource checksum against the server value in
`CResourceFileDownloadManager::DownloadFinished`; preserve that code and its
fallback/error behavior. A content policy does not replace this checksum.

Integration must preserve these ownership rules:

- Stable borrowed manager pointers while CNet is initialized; Reset cancels
  requests and retains the managers.
- Destroy concrete managers, curl multi handles and runtime leases during normal
  Shutdown before DLL unload. The SDK base has no virtual destructor.
- Defer Shutdown requested by a completion callback until that callback/pulse
  unwinds. Do not destroy a manager inside its own callback.
- After Shutdown return a closed, module-owned failure manager for getters that
  callers dereference without checking for null. Never return freed pointers.
- Repeated Init of the same closed module must not revive old callbacks.

Required proof after integration: actual HTTP fixture requests and file hashes,
native CNet reset/shutdown/reinitialization tests, real resource download through
Core, and execution of client Lua. None of these follows from syntax checking or
from a successful headless server event roundtrip.
