#include "HttpManager.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

using experimental_http::HttpManager;
using Clock = std::chrono::steady_clock;
void require(bool value, const char* reason)
{
    if (!value)
        throw std::runtime_error(reason);
}
struct Result
{
    unsigned     calls = 0;
    bool         success = false;
    int          code = 0;
    uint         attempt = 0, length = 0;
    std::string  body, headers, error;
    HttpManager* manager = nullptr;
    Result*      cancel = nullptr;
    Result*      next = nullptr;
    std::string  next_url;
    bool         reset = false;
    bool         reentry_ok = true;
};
void callback(const SHttpDownloadResult& data);
void queue(HttpManager& manager, const std::string& url, Result& result, const SHttpRequestOptionsTx& options = {}, const std::string& file = {})
{
    result.manager = &manager;
    require(manager.QueueFile(url.c_str(), file.empty() ? nullptr : file.c_str(), &result, callback, options), "QueueFile rejected a unique request");
}
void callback(const SHttpDownloadResult& data)
{
    auto& result = *static_cast<Result*>(data.pObj);
    ++result.calls;
    result.success = data.bSuccess;
    result.code = data.iErrorCode;
    result.attempt = data.uiAttemptNumber;
    result.length = data.uiContentLength;
    require(data.pData && data.szHeaders, "callbacks need valid empty strings even on failure");
    result.body.assign(data.pData, data.dataSize);
    result.headers = data.szHeaders;
    result.error = result.manager->GetError();
    if (result.cancel)
        result.reentry_ok &= result.manager->CancelDownload(result.cancel, callback);
    if (result.reset)
        result.manager->Reset();
    if (result.next)
        queue(*result.manager, result.next_url, *result.next);
    // Reentry must not dispatch another callback from this stack frame.
    if (result.next)
    {
        result.manager->ProcessQueuedFiles();
        result.reentry_ok &= result.next->calls == 0;
    }
}
void settle(HttpManager& manager)
{
    const auto deadline = Clock::now() + std::chrono::seconds(12);
    while (!manager.ProcessQueuedFiles())
    {
        require(Clock::now() < deadline, "HTTP integration deadline");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
void text(SStringContent& output, const std::string& input)
{
    output.length = input.size();
    output.pData = input.data();
}
std::string read(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
}
void write(const std::filesystem::path& path, const std::string& value)
{
    std::ofstream file(path, std::ios::binary);
    file << value;
    require(file.good(), "test file setup");
}
std::string narrow_path(const std::filesystem::path& path)
{
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

int main(int argc, char** argv)
{
    try
    {
        require(argc == 3, "expected localhost base URL and isolated temporary directory");
        const std::string base = argv[1];
        const auto        directory = std::filesystem::u8path(argv[2]);
        const std::string expected("hello\0world", 11);
        HttpManager       manager;
        {
            Result invalid_url, invalid_map;
            invalid_url.manager = &manager;
            require(manager.QueueFile(nullptr, nullptr, &invalid_url, callback, {}), "bad URL must settle asynchronously");
            SHttpRequestOptionsTx malformed;
            malformed.formFields.numItems = 1;
            malformed.formFields.pItems = new SStringContent[1];
            queue(manager, base + "/get", invalid_map, malformed);
            require(!invalid_url.calls && !invalid_map.calls, "validation callbacks are deferred");
            settle(manager);
            require(invalid_url.calls == 1 && !invalid_url.success && invalid_url.attempt == 0 && invalid_map.calls == 1 && !invalid_map.success &&
                        invalid_map.attempt == 0,
                    "invalid input cannot hang ignored Queue return callers");
        }
        {
            Result get, empty, modern, legacy, redirect;
            queue(manager, base + "/get", get);
            require(!get.calls, "QueueFile must not invoke callback synchronously");
            queue(manager, base + "/empty", empty);
            queue(manager, base + "/missing", modern);
            SHttpRequestOptionsTx options;
            options.bIsLegacy = true;
            queue(manager, base + "/missing", legacy, options);
            queue(manager, base + "/redirect", redirect);
            settle(manager);
            require(get.calls == 1 && get.success && get.code == 200 && get.body == expected && get.attempt == 1 && get.length == 11,
                    "real binary GET/status/length/attempt");
            require(get.headers.find("X-Fixture: actual-server") != std::string::npos, "actual response headers");
            require(empty.success && empty.body.empty(), "empty body still succeeds");
            require(modern.success && modern.code == 404 && modern.body == "not-found", "modern HTTP status/body semantics");
            require(!legacy.success && legacy.code == 404 && legacy.body.empty() && !legacy.error.empty(), "legacy HTTP error semantics");
            require(redirect.success && redirect.body == expected, "redirect final representation only");
        }
        {
            Result                post, form, header, auth, custom;
            std::string           body("a\0b", 3), method = "POST", user = "user", password = "pass", name = "X-Probe", value = "copied-header";
            SHttpRequestOptionsTx options;
            options.bPostBinary = true;
            text(options.strPostData, body);
            text(options.strRequestMethod, method);
            queue(manager, base + "/echo", post, options);
            options.requestHeaders.numItems = 2;
            options.requestHeaders.pItems = new SStringContent[2];
            text(options.requestHeaders.pItems[0], name);
            text(options.requestHeaders.pItems[1], value);
            options.strPostData = {};
            options.strRequestMethod = {};
            queue(manager, base + "/header", header, options);
            options.requestHeaders.numItems = 0;
            text(options.strUsername, user);
            text(options.strPassword, password);
            queue(manager, base + "/auth", auth, options);
            options.strUsername = {};
            options.strPassword = {};
            std::string field = "field", field_value("v\0x", 3);
            options.formFields.numItems = 2;
            options.formFields.pItems = new SStringContent[2];
            text(options.formFields.pItems[0], field);
            text(options.formFields.pItems[1], field_value);
            queue(manager, base + "/form", form, options);
            options.formFields.numItems = 0;
            method = "PUT";
            text(options.strRequestMethod, method);
            text(options.strPostData, body);
            queue(manager, base + "/echo", custom, options);
            // All borrowed Tx data may die or change immediately after QueueFile.
            body.assign(100, 'z');
            value.assign(100, 'z');
            user.clear();
            password.clear();
            field.clear();
            field_value.clear();
            method.clear();
            settle(manager);
            require(post.success && post.body == std::string("a\0b", 3), "copied binary POST");
            require(custom.success && custom.body == std::string("a\0b", 3), "custom method/body");
            require(header.success && header.body == "copied-header", "copied header Tx map");
            require(auth.success && auth.body == "authenticated", "real HTTP authentication");
            require(form.success && form.body.find("name=\"field\"") != std::string::npos && form.body.find(std::string("v\0x", 3)) != std::string::npos,
                    "real MIME form fields/copy/binary value");
        }
        {
            Result                ordinary_post, retry, unsafe_retry, loop, timeout;
            SHttpRequestOptionsTx options;
            std::string           body("a\0ignored", 9);
            text(options.strPostData, body);
            queue(manager, base + "/echo", ordinary_post, options);
            options.strPostData = {};
            options.uiConnectionAttempts = 2;
            queue(manager, base + "/flaky", retry, options);
            std::string method = "POST";
            text(options.strRequestMethod, method);
            text(options.strPostData, body);
            queue(manager, base + "/flaky-post", unsafe_retry, options);
            options.strRequestMethod = {};
            options.strPostData = {};
            options.uiMaxRedirects = 2;
            queue(manager, base + "/loop", loop, options);
            options.uiConnectionAttempts = 1;
            options.uiConnectTimeoutMs = 100;
            queue(manager, base + "/slow", timeout, options);
            settle(manager);
            require(ordinary_post.success && ordinary_post.body == "a", "nonbinary POST truncates at NUL");
            require(retry.success && retry.body == "retried" && retry.attempt == 2, "bounded real connection retry");
            require(!unsafe_retry.success && unsafe_retry.attempt == 1, "do not repeat a possibly processed POST");
            require(!loop.success && loop.calls == 1 && !loop.error.empty(), "real redirect limit error");
            require(!timeout.success && timeout.calls == 1 && !timeout.error.empty(), "real low-speed timeout");
        }
        {
            const auto file = directory / "result.dat";
            write(file, "original");
            Result result;
            queue(manager, base + "/file", result, {}, narrow_path(file));
            require(read(file) == "original", "QueueFile cannot overwrite destination");
            settle(manager);
            require(result.success && read(file) == "on-disk", "real temporary file atomically published");
            SHttpRequestOptionsTx options;
            options.bResumeFile = true;
            write(file, "hello");
            Result resume;
            queue(manager, base + "/range", resume, options, narrow_path(file));
            settle(manager);
            require(resume.success && resume.code == 206 && read(file) == "hello-range", "actual confirmed range resume");
            write(file, "hello");
            Result ignored;
            queue(manager, base + "/ignore-range", ignored, options, narrow_path(file));
            settle(manager);
            require(!ignored.success && read(file) == "hello", "ignored range never corrupts destination");
            options = SHttpRequestOptionsTx{};  // Maps are empty in this local options object.
            options.bCheckContents = true;
            Result validation;
            queue(manager, base + "/validated", validation, options, narrow_path(file));
            settle(manager);
            require(!validation.success && validation.attempt == 0 && read(file) == "hello", "missing content validator fails before HTTP/publish");
            HttpManager checked({}, [](const auto& context, std::string&) { return read(context.temporary) == "on-disk"; });
            Result      accepted;
            queue(checked, base + "/file", accepted, options, narrow_path(file));
            settle(checked);
            require(accepted.success && read(file) == "on-disk", "explicit real fixture validation on actual downloaded bytes");
            HttpManager denied({},
                               [](const auto&, std::string& error)
                               {
                                   error = "fixture rejects actual bytes";
                                   return false;
                               });
            Result      rejected;
            queue(denied, base + "/file", rejected, options, narrow_path(file));
            settle(denied);
            require(!rejected.success && rejected.error == "fixture rejects actual bytes" && read(file) == "on-disk",
                    "validator failure preserves destination");
            HttpManager* reentrant_validator = nullptr;
            write(file, "preserved-on-reset");
            HttpManager validation_reset({},
                                         [&](const auto&, std::string&)
                                         {
                                             reentrant_validator->Reset();
                                             return true;
                                         });
            reentrant_validator = &validation_reset;
            Result suppressed;
            queue(validation_reset, base + "/file", suppressed, options, narrow_path(file));
            settle(validation_reset);
            require(!suppressed.calls && read(file) == "preserved-on-reset", "validator reset cannot publish or call a cancelled owner");
            write(directory / "not-directory", "x");
            Result local_error;
            queue(manager, base + "/file", local_error, {}, narrow_path(directory / "not-directory/out"));
            settle(manager);
            require(!local_error.success && local_error.code == 1007, "real file-open error contract");
        }
        {
            experimental_http::Limits limits;
            limits.memory_body = 1024;
            limits.memory_total = 2048;
            HttpManager bounded(limits);
            Result      large, zip;
            queue(bounded, base + "/large", large);
            queue(bounded, base + "/gzip", zip);
            settle(bounded);
            require(!large.success && !zip.success, "raw and decompressed bodies obey bounds");
        }
        {
            Result slow, trigger;
            trigger.cancel = &slow;
            queue(manager, base + "/slow", slow);
            queue(manager, base + "/get", trigger);
            settle(manager);
            require(!slow.calls && trigger.calls == 1 && trigger.reentry_ok, "callback-time cancellation suppresses target callback");
            Result reset_slow, reset;
            reset.reset = true;
            queue(manager, base + "/slow", reset_slow);
            queue(manager, base + "/get", reset);
            settle(manager);
            require(!reset_slow.calls && reset.calls == 1, "callback reset suppresses other live owners");
            Result first, second;
            first.next = &second;
            first.next_url = base + "/get";
            queue(manager, base + "/get", first);
            settle(manager);
            require(first.calls == 1 && second.calls == 1 && first.reentry_ok, "callback enqueue/reentry/lifetime");
            Result duplicate;
            queue(manager, base + "/slow", duplicate);
            require(!manager.QueueFile((base + "/slow").c_str(), nullptr, &duplicate, callback, {}), "duplicate detection");
            SDownloadStatus status;
            require(manager.GetDownloadStatus(&duplicate, callback, status) && status.uiAttemptNumber == 0, "queued status");
            require(manager.CancelDownload(&duplicate, callback) && !duplicate.calls, "queued cancel without callback");
        }
        for (const auto& item : std::filesystem::directory_iterator(directory))
            require(item.path().extension() != ".part", "owned HTTP temporary file leaked");
        manager.ResetDownloadSize();
        require(manager.GetDownloadSizeNow() == 0, "download counter reset");
        std::cout << "PASS real curl HTTP methods/options/status/files/retry/callback lifecycle; pointer_bits=" << sizeof(void*) * 8
                  << "; MTA/game/client Lua/G4 NOT RUN\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
