#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #include <io.h>
    #include <fcntl.h>
#else
    #include <arpa/inet.h>
    #include <fcntl.h>
    #include <unistd.h>
#endif
#include "HttpManager.h"
#include <curl/curl.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <list>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <array>
#include <cstdint>
#include <cctype>
#include <cstdlib>

namespace experimental_http
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        constexpr int unsupported = 10001, invalid_options = 10002, bound_error = 10003, file_error = 1007;
        constexpr int curl_error_base = 20000, multi_error_base = 21000;
        struct CurlRuntime
        {
            std::mutex mutex;
            unsigned   users = 0;
        };
        CurlRuntime& runtime()
        {
            static CurlRuntime value;
            return value;
        }
        void acquire()
        {
            auto&           value = runtime();
            std::lock_guard lock(value.mutex);
            if (!value.users && curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
                throw std::runtime_error("curl_global_init failed");
            ++value.users;
        }
        void release()
        {
            auto&           value = runtime();
            std::lock_guard lock(value.mutex);
            if (!--value.users)
                curl_global_cleanup();
        }
        uint narrow(size_t value)
        {
            return static_cast<uint>(std::min(value, size_t{0xffffffffu}));
        }
        void add(uint& total, size_t value)
        {
            total += static_cast<uint>(std::min(value, size_t{0xffffffffu - total}));
        }
        bool token(const std::string& value)
        {
            return !value.empty() &&
                   std::all_of(value.begin(), value.end(), [](unsigned char c)
                               { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || std::strchr("!#$%&'*+-.^_`|~", c); });
        }
        bool contains_nul(const std::string& value)
        {
            return value.find('\0') != std::string::npos;
        }
        std::filesystem::path output_path(const std::string& value)
        {
            return std::filesystem::absolute(std::filesystem::u8path(value)).lexically_normal();
        }
        FILE* open_read(const std::filesystem::path& path)
        {
#ifdef _WIN32
            return _wfopen(path.c_str(), L"rb");
#else
            return std::fopen(path.c_str(), "rb");
#endif
        }
        FILE* create_exclusive(const std::filesystem::path& path)
        {
#ifdef _WIN32
            HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
                return nullptr;
            int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_BINARY | _O_RDWR);
            if (descriptor < 0)
            {
                CloseHandle(handle);
                return nullptr;
            }
            FILE* file = _fdopen(descriptor, "wb+");
            if (!file)
                _close(descriptor);
            return file;
#else
            int descriptor = open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
            if (descriptor < 0)
                return nullptr;
            FILE* file = fdopen(descriptor, "wb+");
            if (!file)
                close(descriptor);
            return file;
#endif
        }
        bool publish(const std::filesystem::path& temporary, const std::filesystem::path& destination)
        {
#ifdef _WIN32
            return MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
            std::error_code error;
            std::filesystem::rename(temporary, destination, error);
            return !error;
#endif
        }
    }

    class HttpManager::Impl
    {
        friend class HttpManager;
        struct Options
        {
            bool                                             legacy = false, local = false, check = false, resume = false, binary = false;
            uint                                             attempts = 10, timeout = 10000, redirects = 8;
            std::string                                      post, method, username, password;
            std::vector<std::pair<std::string, std::string>> fields, headers;
        };
        struct Request
        {
            Impl&                             owner;
            void*                             object = nullptr;
            PFN_DOWNLOAD_FINISHED_CALLBACK    callback = nullptr;
            std::string                       url, destination, body, headers, detail;
            Options                           options;
            CURL*                             easy = nullptr;
            curl_slist*                       header_list = nullptr;
            curl_mime*                        mime = nullptr;
            FILE*                             file = nullptr;
            std::filesystem::path             temporary;
            std::array<char, CURL_ERROR_SIZE> curl_error{};
            Clock::time_point                 created = Clock::now();
            SDownloadStatus                   status{};
            size_t                            received = 0, prefix = 0, header_bytes = 0, range_total = 0;
            long                              response = 0;
            int                               error = 0;
            bool                              cancelled = false, ready = false, range_valid = false, success = false;
            explicit Request(Impl& owner) : owner(owner) {}
            ~Request() { cleanup(); }
            void cleanup_easy()
            {
                if (easy)
                {
                    curl_easy_cleanup(easy);
                    easy = nullptr;
                }
                if (mime)
                {
                    curl_mime_free(mime);
                    mime = nullptr;
                }
                if (header_list)
                {
                    curl_slist_free_all(header_list);
                    header_list = nullptr;
                }
            }
            void cleanup()
            {
                cleanup_easy();
                if (file)
                {
                    std::fclose(file);
                    file = nullptr;
                }
                if (!temporary.empty())
                {
                    std::error_code ignored;
                    std::filesystem::remove(temporary, ignored);
                    temporary.clear();
                }
            }
            size_t limit() const { return destination.empty() ? owner.limits.memory_body : owner.limits.file_body; }
        };

    public:
        explicit Impl(Limits limits, FileValidator validator) : limits(limits), validator(std::move(validator))
        {
            if (!limits.requests || limits.requests > 1024 || !limits.memory_body || limits.memory_body > 64 * 1024 * 1024 || !limits.file_body ||
                limits.file_body > 256 * 1024 * 1024 || limits.memory_total < limits.memory_body || limits.memory_total > 256 * 1024 * 1024 ||
                !limits.option_bytes || limits.option_bytes > 4 * 1024 * 1024)
                throw std::invalid_argument("HTTP limits outside supported bounds");
            acquire();
            multi = curl_multi_init();
            if (!multi)
            {
                release();
                throw std::runtime_error("curl_multi_init failed");
            }
        }
        ~Impl()
        {
            reset();
            curl_multi_cleanup(multi);
            release();
        }
        std::recursive_mutex                mutex;
        Limits                              limits;
        FileValidator                       validator;
        CURLM*                              multi = nullptr;
        std::list<std::shared_ptr<Request>> requests;
        std::string                         last_error;
        uint                                downloaded = 0;
        unsigned                            max_connections = 4, active = 0;
        bool                                processing = false;
        std::uint64_t                       epoch = 0;

        void detach(Request& request)
        {
            if (request.easy)
            {
                curl_multi_remove_handle(multi, request.easy);
                --active;
                request.cleanup_easy();
            }
        }
        void reset()
        {
            ++epoch;
            for (auto& request : requests)
            {
                request->cancelled = true;
                detach(*request);
                request->cleanup();
            }
            requests.clear();
            downloaded = 0;
        }
        std::string text(const SStringContent& value, size_t& used, size_t maximum = 65536)
        {
            if (value.length > maximum || value.length > limits.option_bytes - used || (value.length && !value.pData))
                throw std::invalid_argument("HTTP option bytes exceed bounds or pointer is null");
            used += value.length;
            return value.length ? std::string(value.pData, value.length) : std::string{};
        }
        std::vector<std::pair<std::string, std::string>> map(const SStringMapContent& value, size_t& used)
        {
            if (value.numItems > 128 || value.numItems % 2 || (value.numItems && !value.pItems))
                throw std::invalid_argument("Malformed/big HTTP option map");
            std::vector<std::pair<std::string, std::string>> result;
            for (size_t i = 0; i < value.numItems; i += 2)
                result.emplace_back(text(value.pItems[i], used), text(value.pItems[i + 1], used));
            return result;
        }
        Options copy(const SHttpRequestOptionsTx& value)
        {
            size_t  used = 0;
            Options result;
            result.legacy = value.bIsLegacy;
            result.local = value.bIsLocal;
            result.check = value.bCheckContents;
            result.resume = value.bResumeFile;
            result.binary = value.bPostBinary;
            result.attempts = value.uiConnectionAttempts;
            result.timeout = value.uiConnectTimeoutMs;
            result.redirects = value.uiMaxRedirects;
            result.post = text(value.strPostData, used, limits.option_bytes);
            result.method = text(value.strRequestMethod, used, 32);
            result.username = text(value.strUsername, used, 4096);
            result.password = text(value.strPassword, used, 4096);
            result.fields = map(value.formFields, used);
            result.headers = map(value.requestHeaders, used);
            if (!result.attempts || result.attempts > 10 || result.timeout < 100 || result.timeout > 120000 || result.redirects > 16 ||
                (!result.method.empty() && !token(result.method)) || contains_nul(result.username) || contains_nul(result.password))
                throw std::invalid_argument("Unsupported HTTP option range/method/credentials");
            for (const auto& [name, body] : result.headers)
                if (!token(name) || contains_nul(body) || body.find_first_of("\r\n") != std::string::npos)
                    throw std::invalid_argument("Invalid HTTP header name/value");
            for (const auto& [name, body] : result.fields)
                if (contains_nul(name) || name.empty())
                    throw std::invalid_argument("Invalid form field name");
            if (!result.binary)
                result.post.resize(std::strlen(result.post.c_str()));
            return result;
        }
        static size_t write(char* data, size_t size, size_t count, void* context) noexcept
        {
            auto& request = *static_cast<Request*>(context);
            try
            {
                if (count && size > SIZE_MAX / count)
                    return 0;
                const size_t bytes = size * count;
                if (request.prefix && !request.range_valid)
                {
                    request.error = invalid_options;
                    request.detail = "Server did not confirm the requested resume range";
                    return 0;
                }
                if (bytes > request.limit() - request.received)
                {
                    request.error = bound_error;
                    request.detail = "Response body exceeds configured bound";
                    return 0;
                }
                if (request.file)
                {
                    if (std::fwrite(data, 1, bytes, request.file) != bytes)
                    {
                        request.error = file_error;
                        request.detail = "HTTP temporary file write failed";
                        return 0;
                    }
                }
                else
                {
                    size_t buffered = 0;
                    for (const auto& item : request.owner.requests)
                        buffered += item->body.size();
                    if (bytes > request.owner.limits.memory_total - buffered)
                    {
                        request.error = bound_error;
                        request.detail = "Aggregate response memory bound";
                        return 0;
                    }
                    request.body.append(data, bytes);
                }
                request.received += bytes;
                request.status.uiBytesReceived = narrow(request.received);
                add(request.owner.downloaded, bytes);
                return bytes;
            }
            catch (...)
            {
                request.error = bound_error;
                return 0;
            }
        }
        static size_t header(char* data, size_t size, size_t count, void* context) noexcept
        {
            auto& request = *static_cast<Request*>(context);
            try
            {
                if (count && size > SIZE_MAX / count)
                    return 0;
                const size_t bytes = size * count;
                if (bytes > 256 * 1024 - request.header_bytes)
                {
                    request.error = bound_error;
                    request.detail = "Response header bound";
                    return 0;
                }
                request.header_bytes += bytes;
                std::string line(data, bytes);
                if (line.starts_with("HTTP/"))
                {
                    // Redirect/authentication response bodies must not be
                    // concatenated with the final representation.
                    if (request.received != request.prefix)
                    {
                        if (request.file)
                        {
                            if (std::fflush(request.file))
                                return 0;
#ifdef _WIN32
                            if (_chsize_s(_fileno(request.file), request.prefix))
                                return 0;
#else
                            if (ftruncate(fileno(request.file), request.prefix))
                                return 0;
#endif
                            if (std::fseek(request.file, static_cast<long>(request.prefix), SEEK_SET))
                                return 0;
                        }
                        request.body.clear();
                        request.received = request.prefix;
                    }
                    request.headers.clear();
                    request.range_valid = false;
                    request.range_total = 0;
                    request.status.uiBytesReceived = narrow(request.prefix);
                    const auto space = line.find(' ');
                    request.response = space == std::string::npos ? 0 : std::strtol(line.c_str() + space + 1, nullptr, 10);
                }
                if (request.headers.size() + bytes > 64 * 1024)
                {
                    request.error = bound_error;
                    return 0;
                }
                request.headers += line;
                std::transform(line.begin(), line.end(), line.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (line.starts_with("content-range: bytes "))
                {
                    unsigned long long begin = 0, end = 0, total = 0;
                    char               extra = 0;
                    const int          parsed = std::sscanf(line.c_str() + 21, "%llu-%llu/%llu %c", &begin, &end, &total, &extra);
                    request.range_valid = parsed == 3 && request.response == 206 && begin == request.prefix && end >= begin && total && end == total - 1 &&
                                          total <= request.limit();
                    if (request.range_valid)
                        request.range_total = static_cast<size_t>(total);
                }
                return bytes;
            }
            catch (...)
            {
                request.error = bound_error;
                return 0;
            }
        }
        static int progress(void* context, curl_off_t total, curl_off_t, curl_off_t, curl_off_t) noexcept
        {
            auto& request = *static_cast<Request*>(context);
            request.status.uiContentLength = total > 0 ? narrow(static_cast<size_t>(total) + request.prefix) : 0;
            return Clock::now() - request.created >= std::chrono::minutes(5) ? 1 : 0;
        }
        static curl_socket_t open_socket(void* context, curlsocktype, struct curl_sockaddr* address) noexcept
        {
            auto& request = *static_cast<Request*>(context);
            if (request.owner.limits.loopback_only)
            {
                bool allowed = false;
                if (address->family == AF_INET)
                    allowed = (ntohl(reinterpret_cast<sockaddr_in*>(&address->addr)->sin_addr.s_addr) >> 24) == 127;
                else if (address->family == AF_INET6)
                {
                    const auto*         value = reinterpret_cast<sockaddr_in6*>(&address->addr)->sin6_addr.s6_addr;
                    const unsigned char loopback[16]{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
                    allowed = std::memcmp(value, loopback, 16) == 0;
                }
                if (!allowed)
                {
                    request.error = unsupported;
                    return CURL_SOCKET_BAD;
                }
            }
            return socket(address->family, address->socktype, address->protocol);
        }
        template <typename T>
        void set(Request& request, CURLoption key, T value)
        {
            const auto code = curl_easy_setopt(request.easy, key, value);
            if (code != CURLE_OK)
                throw std::runtime_error(curl_easy_strerror(code));
        }
        void prepare_file(Request& request)
        {
            if (request.destination.empty())
                return;
            const auto destination = output_path(request.destination);
            std::filesystem::create_directories(destination.parent_path());
            static std::atomic<std::uint64_t> serial{0};
            for (unsigned attempt = 0; attempt < 16 && !request.file; ++attempt)
            {
                request.temporary = destination;
#ifdef _WIN32
                const auto pid = GetCurrentProcessId();
#else
                const auto pid = getpid();
#endif
                request.temporary += ".mta-http-" + std::to_string(pid) + "-" + std::to_string(++serial) + ".part";
                request.file = create_exclusive(request.temporary);
                if (!request.file)
                    request.temporary.clear();
            }
            if (!request.file)
                throw std::runtime_error("Unable to create exclusive HTTP temporary file");
            if (request.options.resume && std::filesystem::exists(destination))
            {
                const size_t bytes = static_cast<size_t>(std::filesystem::file_size(destination));
                if (bytes > request.limit())
                    throw std::runtime_error("Existing resume prefix exceeds file bound");
                FILE* input = open_read(destination);
                if (!input)
                    throw std::runtime_error("Cannot read existing resume prefix");
                auto                                    closer = [](FILE* value) { std::fclose(value); };
                std::unique_ptr<FILE, decltype(closer)> owned(input, closer);
                std::array<char, 16384>                 buffer;
                size_t                                  copied = 0;
                while (copied < bytes)
                {
                    const auto count = std::fread(buffer.data(), 1, std::min(buffer.size(), bytes - copied), input);
                    if (!count || std::fwrite(buffer.data(), 1, count, request.file) != count)
                        throw std::runtime_error("Resume prefix copy failed");
                    copied += count;
                }
                request.prefix = request.received = bytes;
                request.status.uiBytesReceived = narrow(bytes);
            }
        }
        void start(Request& request)
        {
            request.cleanup();
            request.body.clear();
            request.headers.clear();
            request.curl_error = {};
            request.received = request.prefix = request.header_bytes = request.range_total = 0;
            request.response = 0;
            request.range_valid = false;
            request.status.uiBytesReceived = request.status.uiContentLength = 0;
            if (Clock::now() - request.created >= std::chrono::minutes(5))
            {
                request.ready = true;
                request.error = curl_error_base + CURLE_OPERATION_TIMEDOUT;
                request.detail = "HTTP request lifetime exceeded";
                return;
            }
            ++request.status.uiAttemptNumber;
            try
            {
                prepare_file(request);
            }
            catch (const std::exception& error)
            {
                request.error = file_error;
                request.detail = error.what();
                request.ready = true;
                return;
            }
            request.easy = curl_easy_init();
            if (!request.easy)
                throw std::runtime_error("curl_easy_init failed");
            auto& options = request.options;
            set(request, CURLOPT_URL, request.url.c_str());
            set(request, CURLOPT_PROTOCOLS_STR, "http,https");
            set(request, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
            set(request, CURLOPT_PROXY, "");
            set(request, CURLOPT_NOSIGNAL, 1L);
            set(request, CURLOPT_FOLLOWLOCATION, options.redirects ? 1L : 0L);
            set(request, CURLOPT_MAXREDIRS, static_cast<long>(options.redirects));
            set(request, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(options.timeout));
            set(request, CURLOPT_TIMEOUT_MS, 120000L);
            if (!options.local)
            {
                set(request, CURLOPT_LOW_SPEED_LIMIT, 10L);
                set(request, CURLOPT_LOW_SPEED_TIME, static_cast<long>((options.timeout + 999) / 1000));
            }
            set(request, CURLOPT_SSL_VERIFYPEER, 1L);
            set(request, CURLOPT_SSL_VERIFYHOST, 2L);
            set(request, CURLOPT_FAILONERROR, options.legacy ? 1L : 0L);
            set(request, CURLOPT_ACCEPT_ENCODING, "");
            set(request, CURLOPT_ERRORBUFFER, request.curl_error.data());
            set(request, CURLOPT_WRITEFUNCTION, &write);
            set(request, CURLOPT_WRITEDATA, &request);
            set(request, CURLOPT_HEADERFUNCTION, &header);
            set(request, CURLOPT_HEADERDATA, &request);
            set(request, CURLOPT_XFERINFOFUNCTION, &progress);
            set(request, CURLOPT_XFERINFODATA, &request);
            set(request, CURLOPT_NOPROGRESS, 0L);
            set(request, CURLOPT_OPENSOCKETFUNCTION, &open_socket);
            set(request, CURLOPT_OPENSOCKETDATA, &request);
            if (!options.username.empty() || !options.password.empty())
            {
                set(request, CURLOPT_USERNAME, options.username.c_str());
                set(request, CURLOPT_PASSWORD, options.password.c_str());
                set(request, CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_ANY));
            }
            for (const auto& [name, value] : options.headers)
            {
                auto* appended = curl_slist_append(request.header_list, (name + ": " + value).c_str());
                if (!appended)
                    throw std::bad_alloc();
                request.header_list = appended;
            }
            if (request.header_list)
                set(request, CURLOPT_HTTPHEADER, request.header_list);
            if (!options.fields.empty())
            {
                request.mime = curl_mime_init(request.easy);
                if (!request.mime)
                    throw std::bad_alloc();
                for (const auto& [name, value] : options.fields)
                {
                    auto* part = curl_mime_addpart(request.mime);
                    if (!part || curl_mime_name(part, name.c_str()) != CURLE_OK || curl_mime_data(part, value.data(), value.size()) != CURLE_OK)
                        throw std::runtime_error("MIME field setup failed");
                }
                set(request, CURLOPT_MIMEPOST, request.mime);
            }
            else if (!options.post.empty() || options.method == "POST")
            {
                set(request, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(options.post.size()));
                set(request, CURLOPT_POSTFIELDS, options.post.data());
            }
            if (!options.method.empty())
            {
                if (options.method == "HEAD")
                    set(request, CURLOPT_NOBODY, 1L);
                set(request, CURLOPT_CUSTOMREQUEST, options.method.c_str());
            }
            if (request.prefix)
                set(request, CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(request.prefix));
            if (curl_multi_add_handle(multi, request.easy) != CURLM_OK)
                throw std::runtime_error("curl_multi_add_handle failed");
            ++active;
        }
        static bool retry(CURLcode code)
        {
            return code == CURLE_COULDNT_RESOLVE_HOST || code == CURLE_COULDNT_CONNECT || code == CURLE_OPERATION_TIMEDOUT || code == CURLE_RECV_ERROR ||
                   code == CURLE_SEND_ERROR || code == CURLE_PARTIAL_FILE || code == CURLE_GOT_NOTHING;
        }
        void finished(const std::shared_ptr<Request>& request, CURLcode code)
        {
            curl_easy_getinfo(request->easy, CURLINFO_RESPONSE_CODE, &request->response);
            detach(*request);
            if (code == CURLE_OK && request->prefix && (!request->range_valid || request->received != request->range_total))
            {
                request->error = invalid_options;
                request->detail = "Incomplete or unconfirmed resume representation";
            }
            const auto& options = request->options;
            const bool  safe_method =
                options.post.empty() && options.fields.empty() && (options.method.empty() || options.method == "GET" || options.method == "HEAD");
            const bool safe_retry = safe_method || code == CURLE_COULDNT_CONNECT || code == CURLE_COULDNT_RESOLVE_HOST;
            if (code != CURLE_OK && !request->error && safe_retry && retry(code) && request->status.uiAttemptNumber < request->options.attempts &&
                Clock::now() - request->created < std::chrono::minutes(5))
                return;
            request->ready = true;
            request->success = code == CURLE_OK && !request->error && request->response >= 100 && request->response <= 599;
            if (!request->error)
                request->error = request->success || (code == CURLE_HTTP_RETURNED_ERROR && request->response >= 400 && request->response <= 599)
                                     ? static_cast<int>(request->response)
                                     : curl_error_base + static_cast<int>(code);
            if (!request->success && request->detail.empty())
                request->detail = request->curl_error[0] ? request->curl_error.data() : curl_easy_strerror(code);
            if (request->options.legacy && !request->success)
                request->body.clear();
        }
        void dispatch(const std::shared_ptr<Request>& request)
        {
            if (request->success && request->file)
            {
                const int flushed = std::fflush(request->file);
                const int closed = std::fclose(request->file);
                request->file = nullptr;
                if (flushed || closed)
                {
                    request->success = false;
                    request->error = file_error;
                    request->detail = "Temporary file flush/close failed";
                }
                if (request->success && request->options.check)
                {
                    try
                    {
                        request->success = validator(request->temporary, request->detail);
                    }
                    catch (...)
                    {
                        request->success = false;
                        request->detail = "Content validator threw";
                    }
                    if (!request->success)
                    {
                        request->error = unsupported;
                        if (request->detail.empty())
                            request->detail = "Content validator rejected downloaded file";
                    }
                }
                if (request->cancelled)
                    return;
                if (request->success && !publish(request->temporary, output_path(request->destination)))
                {
                    request->success = false;
                    request->error = file_error;
                    request->detail = "HTTP temporary file publish failed";
                }
                else if (request->success)
                    request->temporary.clear();
            }
            if (request->cancelled)
                return;
            requests.remove(request);
            last_error = request->success ? "" : request->detail;
            if (request->callback)
            {
                const SHttpDownloadResult result{request->body.c_str(),
                                                 request->body.size(),
                                                 request->object,
                                                 request->success,
                                                 request->error,
                                                 request->headers.c_str(),
                                                 request->status.uiAttemptNumber,
                                                 request->status.uiContentLength};
                try
                {
                    request->callback(result);
                }
                catch (...)
                {
                    last_error = "HTTP completion callback threw";
                }
            }
            request->cleanup();
        }
        bool pulse()
        {
            if (processing)
                return requests.empty();
            processing = true;
            struct Scope
            {
                bool& flag;
                ~Scope() { flag = false; }
            } scope{processing};
            const auto began_epoch = epoch;
            for (const auto& request : requests)
                if (!request->ready && !request->easy && active < max_connections)
                {
                    try
                    {
                        start(*request);
                    }
                    catch (const std::exception& error)
                    {
                        // Setup failure may own an easy handle that was not added.
                        request->cleanup();
                        request->ready = true;
                        request->error = invalid_options;
                        request->detail = error.what();
                    }
                }
            int        running = 0;
            const auto result = curl_multi_perform(multi, &running);
            if (result != CURLM_OK)
            {
                for (auto& request : requests)
                {
                    detach(*request);
                    request->ready = true;
                    request->error = multi_error_base + result;
                    request->detail = curl_multi_strerror(result);
                }
            }
            int remaining = 0;
            while (auto* message = curl_multi_info_read(multi, &remaining))
                if (message->msg == CURLMSG_DONE)
                {
                    auto found = std::find_if(requests.begin(), requests.end(), [&](const auto& value) { return value->easy == message->easy_handle; });
                    if (found != requests.end())
                        finished(*found, message->data.result);
                }
            // Select one item at a time: a callback can cancel/reset the rest,
            // queue more work, or recursively pulse without retaining stale owners.
            for (size_t count = 0; count < limits.requests && epoch == began_epoch; ++count)
            {
                auto found = std::find_if(requests.begin(), requests.end(), [](const auto& value) { return value->ready; });
                if (found == requests.end())
                    break;
                auto request = *found;
                dispatch(request);
            }
            return requests.empty();
        }
    };

    HttpManager::HttpManager(Limits limits, FileValidator validator) : impl_(std::make_unique<Impl>(limits, std::move(validator)))
    {
    }
    HttpManager::~HttpManager() = default;
    uint HttpManager::GetDownloadSizeNow()
    {
        std::lock_guard lock(impl_->mutex);
        return impl_->downloaded;
    }
    void HttpManager::ResetDownloadSize()
    {
        std::lock_guard lock(impl_->mutex);
        impl_->downloaded = 0;
    }
    const char* HttpManager::GetError()
    {
        std::lock_guard lock(impl_->mutex);
        return impl_->last_error.c_str();
    }
    void HttpManager::SetMaxConnections(int count)
    {
        std::lock_guard lock(impl_->mutex);
        if (count < 1 || count > 16)
        {
            impl_->last_error = "HTTP concurrency must be 1..16";
            return;
        }
        impl_->max_connections = static_cast<unsigned>(count);
    }
    void HttpManager::Reset()
    {
        std::lock_guard lock(impl_->mutex);
        impl_->reset();
    }
    bool HttpManager::ProcessQueuedFiles()
    {
        std::lock_guard lock(impl_->mutex);
        return impl_->pulse();
    }
    bool HttpManager::QueueFile(const char* url, const char* destination, void* object, PFN_DOWNLOAD_FINISHED_CALLBACK callback,
                                const SHttpRequestOptionsTx& options)
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->requests.size() >= impl_->limits.requests)
        {
            impl_->last_error = "HTTP request queue bound";
            return false;
        }
        try
        {
            auto request = std::make_shared<Impl::Request>(*impl_);
            request->object = object;
            request->callback = callback;
            try
            {
                if (!url || strnlen(url, 8193) > 8192 || (destination && strnlen(destination, 4097) > 4096))
                    throw std::invalid_argument("Invalid HTTP URL/path length");
                request->url = url;
                request->destination = destination ? destination : "";
                for (const auto& item : impl_->requests)
                    if (item->object == object && item->callback == callback && item->url == request->url && item->destination == request->destination)
                        return false;
                request->options = impl_->copy(options);
                if (request->options.check && (!impl_->validator || request->destination.empty()))
                {
                    request->ready = true;
                    request->error = unsupported;
                    request->detail = "bCheckContents requires an explicit file validator";
                }
                if (request->options.resume && (!request->options.post.empty() || !request->options.fields.empty() ||
                                                (!request->options.method.empty() && request->options.method != "GET")))
                    throw std::invalid_argument("File resume supports GET only");
            }
            catch (const std::exception& error)
            {
                request->ready = true;
                request->error = invalid_options;
                request->detail = error.what();
            }
            impl_->requests.push_back(std::move(request));
            return true;
        }
        catch (const std::exception& error)
        {
            impl_->last_error = error.what();
            return false;
        }
    }
    bool HttpManager::CancelDownload(void* object, PFN_DOWNLOAD_FINISHED_CALLBACK callback)
    {
        std::lock_guard lock(impl_->mutex);
        bool            cancelled = false;
        for (auto it = impl_->requests.begin(); it != impl_->requests.end();)
            if ((*it)->object == object && (*it)->callback == callback)
            {
                auto request = *it;
                request->cancelled = true;
                impl_->detach(*request);
                request->cleanup();
                it = impl_->requests.erase(it);
                cancelled = true;
            }
            else
                ++it;
        return cancelled;
    }
    bool HttpManager::GetDownloadStatus(void* object, PFN_DOWNLOAD_FINISHED_CALLBACK callback, SDownloadStatus& status)
    {
        std::lock_guard lock(impl_->mutex);
        for (const auto& request : impl_->requests)
            if (request->object == object && request->callback == callback)
            {
                status = request->status;
                return true;
            }
        status = {};
        return false;
    }
}
