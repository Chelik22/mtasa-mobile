#include "ExperimentalClientNetSelection.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace experimental_client_net;

namespace
{
    unsigned int  serviceCalls = 0;
    unsigned int  compatibilityCalls = 0;
    unsigned long expected = 0x40b5;

    void Require(bool condition, const char* message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    bool StockService(unsigned int request)
    {
        Require(request == 7, "wrong stock service request");
        ++serviceCalls;
        return false;  // The real stock caller historically ignores this result.
    }

    unsigned long Compatibility(unsigned long version, unsigned long* output)
    {
        ++compatibilityCalls;
        Require(!output, "unexpected version output");
        return version == expected;
    }

    template <class T>
    T Procedure(HMODULE module, const char* name)
    {
        const auto address = GetProcAddress(module, name);
        T          result = nullptr;
        static_assert(sizeof(result) == sizeof(address));
        std::memcpy(&result, &address, sizeof(result));
        return result;
    }
}

int main(int argc, char** argv)
{
    try
    {
        Require(sizeof(void*) == 4, "this receipt requires actual Win32");
        Require(SetEnvironmentVariableW(L"MTA_EXPERIMENTAL_CLIENT_NET_MODULE", nullptr) != 0, "unset failed");
        Require(ReadSelection() == Selection::Stock, "absent variable must select stock");
        Require(SetEnvironmentVariableW(L"MTA_EXPERIMENTAL_CLIENT_NET_MODULE", L"") != 0, "set empty failed");
        Require(ReadSelection() == Selection::Invalid, "empty variable must fail closed");
        for (const auto* value :
             {L"netc_experimental.dll", L"NETC_EXPERIMENTAL", L"../netc_experimental", L"C:\\tmp\\netc_experimental", L" netc_experimental"})
        {
            Require(SetEnvironmentVariableW(L"MTA_EXPERIMENTAL_CLIENT_NET_MODULE", value) != 0, "set invalid failed");
            Require(ReadSelection() == Selection::Invalid, "arbitrary name or path accepted");
        }
        const std::wstring oversized(100, L'x');
        Require(SetEnvironmentVariableW(L"MTA_EXPERIMENTAL_CLIENT_NET_MODULE", oversized.c_str()) != 0, "set oversized failed");
        Require(ReadSelection() == Selection::Invalid, "oversized variable accepted");
        Require(SetEnvironmentVariableW(L"MTA_EXPERIMENTAL_CLIENT_NET_MODULE", L"netc_experimental") != 0, "set owned failed");
        Require(ReadSelection() == Selection::Owned, "owned variable rejected");
        Require(std::string(ModuleStem(Selection::Owned)) == "netc_experimental", "wrong Core stem");
        Require(std::string(ModuleStem(Selection::Stock)) == "netc", "wrong stock stem");
        Require(!ModuleStem(Selection::Invalid) && !ModuleFilename(Selection::Invalid), "invalid selection has a path");
#ifdef MTA_DEBUG
        Require(std::wstring(ModuleFilename(Selection::Owned)) == L"netc_experimental_d.dll", "wrong debug filename");
        Require(std::wstring(ModuleFilename(Selection::Stock)) == L"netc_d.dll", "wrong stock debug filename");
#else
        Require(std::wstring(ModuleFilename(Selection::Owned)) == L"netc_experimental.dll", "wrong release filename");
        Require(std::wstring(ModuleFilename(Selection::Stock)) == L"netc.dll", "wrong stock release filename");
#endif
        Require(Bootstrap(Selection::Invalid, Compatibility, StockService, expected) == BootstrapResult::InvalidSelection, "invalid bootstrap accepted");
        Require(serviceCalls == 0 && compatibilityCalls == 0, "invalid selection invoked a service");
        Require(Bootstrap(Selection::Stock, Compatibility, nullptr, expected) == BootstrapResult::MissingStockService, "missing stock export accepted");
        Require(Bootstrap(Selection::Stock, Compatibility, StockService, expected) == BootstrapResult::Accepted, "stock behavior changed");
        Require(serviceCalls == 1 && compatibilityCalls == 0, "stock dispatch changed");
        Require(Bootstrap(Selection::Owned, nullptr, StockService, expected) == BootstrapResult::IncompatibleOwnedModule, "missing owned ABI accepted");
        Require(Bootstrap(Selection::Owned, Compatibility, StockService, expected + 1) == BootstrapResult::IncompatibleOwnedModule, "wrong owned ABI accepted");
        Require(Bootstrap(Selection::Owned, Compatibility, StockService, expected) == BootstrapResult::Accepted, "owned ABI rejected");
        Require(serviceCalls == 1 && compatibilityCalls == 2, "owned bootstrap called private stock service");

        Require(argc == 2 && argv[1][0], "provide the previously verified owned DLL; actual ABI gate must run");
        const auto module = LoadLibraryA(argv[1]);
        Require(module != nullptr, "actual owned DLL failed to load");
        const auto actualCompatibility = Procedure<CompatibilityCheck>(module, "CheckCompatibility");
        const auto actualService = Procedure<ServiceCheck>(module, "CheckService");
        Require(!actualService, "owned DLL unexpectedly claims the private service");
        Require(Bootstrap(Selection::Owned, actualCompatibility, actualService, expected) == BootstrapResult::Accepted, "actual owned ABI rejected");
        Require(Bootstrap(Selection::Owned, actualCompatibility, actualService, expected + 1) == BootstrapResult::IncompatibleOwnedModule,
                "actual wrong ABI accepted");
        FreeLibrary(module);
        std::cout << "pointer_bits=32 environment_contract=passed stock_service_calls=1 owned_private_service_calls=0 actual_owned_abi=passed game=not_run\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
