#pragma once

#include <windows.h>
#include <string_view>

namespace experimental_client_net
{
    enum class Selection
    {
        Stock,
        Owned,
        Invalid,
    };

    // Both pre-Core bootstrap and Core must reject an arbitrary DLL path. The
    // explicit empty variable is an error, rather than a silent stock fallback.
    inline Selection ReadSelection()
    {
        wchar_t value[32] = {};
        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetEnvironmentVariableW(L"MTA_EXPERIMENTAL_CLIENT_NET_MODULE", value, 32);
        if (!length && GetLastError() == ERROR_ENVVAR_NOT_FOUND)
            return Selection::Stock;
        if (length && length < 32 && std::wstring_view(value, length) == L"netc_experimental")
            return Selection::Owned;
        return Selection::Invalid;
    }

    inline const char* ModuleStem(Selection selection)
    {
        return selection == Selection::Owned ? "netc_experimental" : selection == Selection::Stock ? "netc" : nullptr;
    }

    inline const wchar_t* ModuleFilename(Selection selection)
    {
#ifdef MTA_DEBUG
        return selection == Selection::Owned ? L"netc_experimental_d.dll" : selection == Selection::Stock ? L"netc_d.dll" : nullptr;
#else
        return selection == Selection::Owned ? L"netc_experimental.dll" : selection == Selection::Stock ? L"netc.dll" : nullptr;
#endif
    }

    using CompatibilityCheck = unsigned long (*)(unsigned long, unsigned long*);
    using ServiceCheck = bool (*)(unsigned int);

    enum class BootstrapResult
    {
        Accepted,
        InvalidSelection,
        MissingStockService,
        IncompatibleOwnedModule,
    };

    inline BootstrapResult Bootstrap(Selection selection, CompatibilityCheck compatibility, ServiceCheck service, unsigned long expectedVersion)
    {
        if (selection == Selection::Owned)
        {
            // The owned transport does not implement the private post-create
            // service. Validate its public ABI instead; never fake service 7.
            return compatibility && compatibility(expectedVersion, nullptr) ? BootstrapResult::Accepted : BootstrapResult::IncompatibleOwnedModule;
        }
        if (selection != Selection::Stock)
            return BootstrapResult::InvalidSelection;
        if (!service)
            return BootstrapResult::MissingStockService;
        service(7);  // Preserve the stock caller's existing ignored return value.
        return BootstrapResult::Accepted;
    }
}
