// Compile the real SDK file-routing definitions without MTA_CLIENT. The lab
// namespace deliberately does not exist here; unguarded client-only calls must
// fail compilation instead of reaching an ordinary Windows server build.
#ifdef MTA_CLIENT
    #error This regression must use a server, not client, preprocessor context
#endif
#include <winsock2.h>
#include <windows.h>
#include <filesystem>
#include <SharedUtil.h>
#include <SharedUtil.File.hpp>
