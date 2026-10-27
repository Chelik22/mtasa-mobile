#include "MtaBitStream.h"

#ifdef _WIN32
    #define FACTORY_EXPORT extern "C" __declspec(dllexport)
#else
    #define FACTORY_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// This narrow DLL exercises the real interface/lifetime boundary before adding
// CNetServer services. It is not named net.dll and cannot replace MTA networking.
FACTORY_EXPORT NetBitStreamInterface* MtaCreateExperimentalBitStream(unsigned short version)
{
    try
    {
        return new experimental_net::MtaBitStream(version);
    }
    catch (...)
    {
        return nullptr;
    }
}
