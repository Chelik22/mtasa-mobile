#include "ClientModule.h"
#include <version.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#include <cstring>
#include <iterator>

namespace
{
    using Clock = std::chrono::steady_clock;
    constexpr auto mode = EDownloadMode::RESOURCE_INITIAL_FILES_INTERNAL;
    void           require(bool value, const char* reason)
    {
        if (!value)
            throw std::runtime_error(reason);
    }
    template <class Function>
    Function symbol(HMODULE library, const char* name)
    {
        auto address = GetProcAddress(library, name);
        require(address != nullptr, name);
        Function result = nullptr;
        static_assert(sizeof(result) == sizeof(address));
        std::memcpy(&result, &address, sizeof(result));
        return result;
    }
    std::vector<unsigned char> read(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        require(input.good(), "cannot read expected/actual fixture bytes");
        return std::vector<unsigned char>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    std::string utf8(const std::filesystem::path& path)
    {
        const auto value = path.u8string();
        return {reinterpret_cast<const char*>(value.data()), value.size()};
    }
    struct State
    {
        CNet*                             net = nullptr;
        experimental_client::Release      release = nullptr;
        CNetHTTPDownloadManagerInterface* http = nullptr;
        std::string                       url, destination, error;
        unsigned                          calls = 0, forbidden_calls = 0;
        bool                              shutdown_in_callback = false, succeeded = false;
    };
    State* state = nullptr;
    void   forbidden_callback(const SHttpDownloadResult&)
    {
        ++state->forbidden_calls;
    }
    void callback(const SHttpDownloadResult& result)
    {
        auto& current = *static_cast<State*>(result.pObj);
        ++current.calls;
        current.succeeded = result.bSuccess && result.iErrorCode == 200 && result.uiAttemptNumber == 1;
        if (!current.succeeded)
            current.error = current.http->GetError();
        if (!current.shutdown_in_callback)
            return;
        // Do not throw assertions into a backend which deliberately catches user
        // callback exceptions. Record failures for the caller after the pulse.
        try
        {
            current.net->Shutdown();
            current.release();  // Must defer deletion while the actual slot is pulsing.
            if (current.net->IsReady() || current.net->IsConnected())
                current.error = "Shutdown retained transport readiness";
            auto* closed = current.net->GetHTTPDownloadManager(mode);
            if (!closed || closed != current.http)
            {
                current.error = "closed HTTP facade is missing/unstable";
                return;
            }
            SHttpRequestOptionsTx options;
            options.bCheckContents = true;
            if (closed->QueueFile(current.url.c_str(), current.destination.c_str(), &current, forbidden_callback, options))
                current.error = "closed facade accepted a new request";
            if (!closed->ProcessQueuedFiles())
                current.error = "closed facade retained pending work";
        }
        catch (const std::exception& error)
        {
            current.error = error.what();
        }
        catch (...)
        {
            current.error = "exception during callback Shutdown/Release";
        }
    }
    void download(State& current)
    {
        current.calls = 0;
        current.succeeded = false;
        current.error.clear();
        SHttpRequestOptionsTx options;
        options.bCheckContents = true;
        options.bIsLocal = true;
        options.uiConnectionAttempts = 1;
        options.uiConnectTimeoutMs = 2000;
        require(current.http->QueueFile(current.url.c_str(), current.destination.c_str(), &current, callback, options), "real CNet QueueFile rejected fixture");
        require(!current.calls, "QueueFile invoked callback synchronously");
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        while (!current.http->ProcessQueuedFiles())
        {
            require(Clock::now() < deadline, "configured CNet HTTP deadline");
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!current.error.empty())
            throw std::runtime_error(current.error);
        require(current.calls == 1 && current.succeeded && !current.forbidden_calls, "actual checked download/lifecycle failed");
    }
    struct Module
    {
        HMODULE                      library = nullptr;
        CNet*                        net = nullptr;
        experimental_client::Release release = nullptr;
        ~Module()
        {
            if (net)
            {
                net->Shutdown();
                if (release)
                    release();
            }
            if (library)
                FreeLibrary(library);
        }
    };
}

int main(int argc, char** argv)
{
    static_assert(sizeof(void*) == 4, "This is an I386 SDK DLL boundary test");
    try
    {
        require(argc == 5, "expected DLL URL destination-relative expected-source-asset");
        const auto expected = read(std::filesystem::u8path(argv[4]));
        require(!expected.empty() && expected.size() <= 1024 * 1024, "configured fixture must be small/nonempty");
        const DWORD length = GetEnvironmentVariableW(L"MTA_EXPERIMENTAL_CLIENT_CACHE_ROOT", nullptr, 0);
        require(length > 1 && length < 32768, "isolated cache environment required");
        std::wstring cache(length, L'\0');
        const auto   copied = GetEnvironmentVariableW(L"MTA_EXPERIMENTAL_CLIENT_CACHE_ROOT", cache.data(), length);
        require(copied && copied < length, "cache root environment read failed");
        cache.resize(copied);
        const auto root = std::filesystem::canonical(cache);
        const auto relative = std::filesystem::u8path(argv[3]);
        require(!relative.is_absolute() && !relative.has_root_path(), "expected relative destination");
        for (const auto& part : relative)
            require(part != ".." && part != ".", "destination traversal");
        const auto target = root / relative;
        require(!std::filesystem::exists(target), "cache was precopied before HTTP");
        State current;
        state = &current;
        Module module;
        module.library = LoadLibraryW(std::filesystem::u8path(argv[1]).c_str());
        require(module.library != nullptr, "LoadLibrary real configured CNet DLL failed");
        const auto init = symbol<experimental_client::Initialize>(module.library, "InitNetInterface");
        const auto compatible = symbol<experimental_client::Compatibility>(module.library, "CheckCompatibility");
        module.release = symbol<experimental_client::Release>(module.library, "ReleaseNetInterface");
        unsigned long version = 0;
        require(compatible(MTA_DM_CLIENT_NET_MODULE_VERSION, &version) && version == MTA_DM_CLIENT_NET_MODULE_VERSION, "actual client SDK compatibility");
        // This owned module leaves Core opaque. Null is not a fake Core object;
        // neither Core, CClientGame, player nor client Lua is started by this test.
        module.net = init(nullptr);
        require(module.net && module.net->IsReady() && !module.net->IsConnected(), "configured module initialization");
        current.net = module.net;
        current.release = module.release;
        current.url = argv[2];
        current.destination = utf8(target);
        current.http = module.net->GetHTTPDownloadManager(mode);
        require(current.http != nullptr, "actual SDK HTTP facade is null");
        download(current);
        require(read(target) == expected, "actual downloaded bytes mismatch");
        {
            std::unique_ptr<CBinaryFileInterface> retained(module.net->AllocateBinaryFile());
            require(retained && retained->FOpen(current.destination.c_str(), "rb", true), "actual validated SDK binary file open");
            std::vector<unsigned char> bytes(expected.size());
            require(retained->FRead(bytes.data(), static_cast<uint>(bytes.size())) == static_cast<int>(bytes.size()) && bytes == expected,
                    "actual SDK file read differs from HTTP bytes");
            auto mutation = expected;
            mutation[mutation.size() / 2] ^= 1;
            std::ofstream changed(target, std::ios::binary | std::ios::trunc);
            changed.write(reinterpret_cast<const char*>(mutation.data()), static_cast<std::streamsize>(mutation.size()));
            changed.close();
            require(changed.good(), "test disk mutation failed");
            retained->FSeek(0, SEEK_SET);
            require(retained->FRead(bytes.data(), static_cast<uint>(bytes.size())) == static_cast<int>(bytes.size()) && bytes == expected,
                    "validated reader reopened changed disk instead of retaining snapshot");
            std::unique_ptr<CBinaryFileInterface> reopen(module.net->AllocateBinaryFile());
            require(reopen && !reopen->FOpen(current.destination.c_str(), "rb", true), "changed disk bypassed trusted manifest on reopen");
        }  // All SDK file objects die before test-only Release/Unload.
        current.shutdown_in_callback = true;
        download(current);  // Real network restoration; never precopy the approved asset.
        require(read(target) == expected, "last real download did not restore approved bytes");
        require(!module.net->IsReady() && module.net->GetHTTPDownloadManager(mode) == current.http, "Shutdown facade lifetime mismatch");
        module.release();
        module.net = nullptr;
        std::cout << "PASS configured real CNet DLL HTTP/file/snapshot/Shutdown/Release I38632; Core/player/Lua/G4 NOT RUN\n";
        state = nullptr;
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
