#pragma once
#include "../http/HttpManager.h"
#include <mutex>
#include <stdexcept>
#include <string>

namespace experimental_client
{
    // The SDK exposes a borrowed pointer and has no virtual destructor. Keep the
    // facade stable while destroying the concrete curl owner before DLL unload.
    class HttpManagerSlot final : public CNetHTTPDownloadManagerInterface
    {
    public:
        explicit HttpManagerSlot(std::unique_ptr<experimental_http::HttpManager> backend = std::make_unique<experimental_http::HttpManager>())
            : backend_(std::move(backend))
        {
            if (!backend_)
                throw std::invalid_argument("HTTP slot requires a concrete manager");
        }
        uint GetDownloadSizeNow() override
        {
            std::lock_guard lock(mutex_);
            return open() ? backend_->GetDownloadSizeNow() : 0;
        }
        void ResetDownloadSize() override
        {
            std::lock_guard lock(mutex_);
            if (open())
                backend_->ResetDownloadSize();
        }
        const char* GetError() override
        {
            std::lock_guard lock(mutex_);
            // Keep borrowed diagnostics alive when Close destroys the backend.
            if (!open())
                return "HTTP manager is shut down";
            error_snapshot_ = backend_->GetError();
            return error_snapshot_.c_str();
        }
        bool ProcessQueuedFiles() override
        {
            std::lock_guard lock(mutex_);
            if (!open())
                return true;
            if (processing_)
                return false;
            processing_ = true;
            struct Scope
            {
                HttpManagerSlot& slot;
                ~Scope()
                {
                    slot.processing_ = false;
                    if (slot.closed_)
                        slot.backend_.reset();
                }
            } scope{*this};
            return backend_->ProcessQueuedFiles();
        }
        bool QueueFile(const char* url, const char* destination, void* object, PFN_DOWNLOAD_FINISHED_CALLBACK callback,
                       const SHttpRequestOptionsTx& options) override
        {
            std::lock_guard lock(mutex_);
            return open() && backend_->QueueFile(url, destination, object, callback, options);
        }
        void SetMaxConnections(int count) override
        {
            std::lock_guard lock(mutex_);
            if (open())
                backend_->SetMaxConnections(count);
        }
        void Reset() override
        {
            std::lock_guard lock(mutex_);
            if (open())
                backend_->Reset();
        }
        bool CancelDownload(void* object, PFN_DOWNLOAD_FINISHED_CALLBACK callback) override
        {
            std::lock_guard lock(mutex_);
            return open() && backend_->CancelDownload(object, callback);
        }
        bool GetDownloadStatus(void* object, PFN_DOWNLOAD_FINISHED_CALLBACK callback, SDownloadStatus& status) override
        {
            std::lock_guard lock(mutex_);
            status = {};
            return open() && backend_->GetDownloadStatus(object, callback, status);
        }
        void Close()
        {
            std::lock_guard lock(mutex_);
            if (closed_)
                return;
            closed_ = true;
            backend_->Reset();
            // Callback-triggered Shutdown may cancel immediately, but cannot
            // destroy curl state while its ProcessQueuedFiles frame is active.
            if (!processing_)
                backend_.reset();
        }
        bool CanRelease()
        {
            std::lock_guard lock(mutex_);
            return !processing_;
        }

    private:
        bool                                            open() const { return !closed_; }
        std::recursive_mutex                            mutex_;
        std::unique_ptr<experimental_http::HttpManager> backend_;
        std::string                                     error_snapshot_;
        bool                                            closed_ = false, processing_ = false;
    };
}
