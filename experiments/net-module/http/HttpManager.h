#pragma once
#include <net/CNetHTTPDownloadManagerInterface.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>

namespace experimental_http
{
    struct Limits
    {
        size_t requests = 64;
        size_t memory_body = 8 * 1024 * 1024;
        size_t file_body = 64 * 1024 * 1024;
        size_t memory_total = 32 * 1024 * 1024;
        size_t option_bytes = 1024 * 1024;
        bool   loopback_only = true;
    };
    // bCheckContents is a private net-module policy, not an HTTP or checksum
    // guarantee. An explicit validator is required; absence fails closed.
    struct FileValidationContext
    {
        std::filesystem::path temporary;
        std::filesystem::path destination;
        std::string_view      url;
        std::string_view      effective_url;
        long                  http_status = 0;
        size_t                bytes_received = 0;
    };
    // Context string views are borrowed for this synchronous call only.
    using FileValidator = std::function<bool(const FileValidationContext&, std::string&)>;

    class HttpManager final : public CNetHTTPDownloadManagerInterface
    {
    public:
        explicit HttpManager(Limits limits = {}, FileValidator validator = {});
        ~HttpManager();
        HttpManager(const HttpManager&) = delete;
        HttpManager& operator=(const HttpManager&) = delete;
        uint         GetDownloadSizeNow() override;
        void         ResetDownloadSize() override;
        const char*  GetError() override;
        bool         ProcessQueuedFiles() override;
        bool         QueueFile(const char*, const char*, void*, PFN_DOWNLOAD_FINISHED_CALLBACK, const SHttpRequestOptionsTx&) override;
        void         SetMaxConnections(int) override;
        void         Reset() override;
        bool         CancelDownload(void*, PFN_DOWNLOAD_FINISHED_CALLBACK) override;
        bool         GetDownloadStatus(void*, PFN_DOWNLOAD_FINISHED_CALLBACK, SDownloadStatus&) override;

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;
    };
}
