#pragma once
#include "../http/FixtureContentPolicy.h"
#include <windows.h>
#include <memory>
#include <stdexcept>
#include <string>
#ifdef MTA_APPROVED_FIXTURE_CONFIG
    #include <ApprovedFixtureConfig.h>
#endif

namespace experimental_client
{
    inline std::shared_ptr<const experimental_http::FixtureContentPolicy> LoadClientResourcePolicy()
    {
#ifdef MTA_APPROVED_FIXTURE_CONFIG
        // Freeze the selected lab root for this CNet instance. Do not consult a
        // mutable process environment again during a later file-open callback.
        const DWORD required = GetEnvironmentVariableW(L"MTA_EXPERIMENTAL_CLIENT_CACHE_ROOT", nullptr, 0);
        if (!required || required > 32768)
            throw std::runtime_error("Approved resources require MTA_EXPERIMENTAL_CLIENT_CACHE_ROOT");
        std::wstring value(required, L'\0');
        const DWORD  copied = GetEnvironmentVariableW(L"MTA_EXPERIMENTAL_CLIENT_CACHE_ROOT", value.data(), required);
        if (!copied || copied >= required)
            throw std::runtime_error("Approved resource cache root changed or is empty");
        value.resize(copied);
        const std::filesystem::path root(value);
        if (!root.is_absolute())
            throw std::runtime_error("Approved resource cache root must be absolute");
        return std::make_shared<const experimental_http::FixtureContentPolicy>(root, approved_fixture::Entries());
#else
        return {};
#endif
    }

    inline std::unique_ptr<experimental_http::HttpManager> MakeClientHttpManager(const std::shared_ptr<const experimental_http::FixtureContentPolicy>& policy)
    {
        experimental_http::FileValidator validator;
        if (policy)
            validator = [policy](const auto& context, std::string& error) { return (*policy)(context, error); };
        return std::make_unique<experimental_http::HttpManager>(experimental_http::Limits{}, std::move(validator));
    }
}
