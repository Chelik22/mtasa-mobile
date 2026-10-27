#ifdef _WIN32
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif
#include "MtaBitStream.h"
#include <iostream>
#include <memory>
#include <thread>

void require(bool value, const char* reason)
{
    if (!value)
        throw std::runtime_error(reason);
}
int main(int argc, char** argv)
{
    try
    {
        require(argc == 2, "expected factory library path");
#ifdef _WIN32
        auto library = LoadLibraryA(argv[1]);
        require(library != nullptr, "LoadLibrary failed");
        auto                                                              unload = [](HMODULE module) { FreeLibrary(module); };
        std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(unload)> owner(library, unload);
        auto                                                              symbol = GetProcAddress(library, "MtaCreateExperimentalBitStream");
#else
        auto library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        require(library != nullptr, "dlopen failed");
        auto                                    unload = [](void* module) { dlclose(module); };
        std::unique_ptr<void, decltype(unload)> owner(library, unload);
        auto                                    symbol = dlsym(library, "MtaCreateExperimentalBitStream");
#endif
        require(symbol != nullptr, "factory export missing");
        using Create = NetBitStreamInterface* (*)(unsigned short);
        static_assert(sizeof(Create) == sizeof(symbol));
        Create create = nullptr;
        std::memcpy(&create, &symbol, sizeof(create));
        auto                                                      release = [](NetBitStreamInterface* stream) { stream->Release(); };
        std::unique_ptr<NetBitStreamInterface, decltype(release)> stream(create(0x30), release);
        require(stream != nullptr && stream->Version() == 0x30, "factory failed");
        stream->Write(ElementID{54321});
        stream->WriteString(std::string_view("across-dll"));
        stream->WriteCompressed(static_cast<int>(-31415));
        ElementID   element;
        std::string text;
        int         number = 0;
        require(stream->Read(element) && element.Value() == 54321 && stream->ReadString(text) && text == "across-dll" && stream->ReadCompressed(number) &&
                    number == -31415,
                "cross-module virtual/helper calls");
        // Host-inline AddRef invokes this executable's native Lock on the DLL's
        // lock storage. Keep the original reference until both threads finish.
        auto churn = [&]
        {
            for (int i = 0; i < 1000; ++i)
            {
                stream->AddRef();
                stream->Release();
            }
        };
        std::jthread first(churn), second(churn);
        first.join();
        second.join();
        auto* final = stream.release();
        require(final->Release() == 0, "cross-module final release");
        std::cout << "PASS dynamically loaded MTA bitstream, host-inline refcount and DLL destruction; CNetServer NOT RUN\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
