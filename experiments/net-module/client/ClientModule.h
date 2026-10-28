#pragma once

#ifdef _WIN32
    #include <winsock2.h>
    #include <windows.h>
#endif
#include <MtaBitStream.h>
#include <net/CNet.h>
#ifdef _WIN32
    #define EXPERIMENTAL_CLIENT_CALL __cdecl
#else
    #define EXPERIMENTAL_CLIENT_CALL
#endif

class CCoreInterface;

namespace experimental_client
{
    using Initialize = CNet*(EXPERIMENTAL_CLIENT_CALL*)(CCoreInterface*);
    using Compatibility = unsigned long(EXPERIMENTAL_CLIENT_CALL*)(unsigned long, unsigned long*);
    using Release = void(EXPERIMENTAL_CLIENT_CALL*)();
    inline constexpr const char* identity_environment = "MTA_EXPERIMENTAL_CLIENT_IDENTITY";
}
