module;

#include "Platform.h"
#include <span>
#include <utility>
#include <winhttp.h>

export module lemonsquash.http;

namespace Lemonsquash {
    namespace {
        constexpr int RequestTimeoutMs = 30000;

        struct InternetHandle {
            HINTERNET value;

            explicit InternetHandle(HINTERNET h)
                : value(h) {
                if (!h)
                    winrt::throw_last_error();
            }

            ~InternetHandle() {
                Close();
            }

            InternetHandle(const InternetHandle&) = delete;
            InternetHandle& operator=(const InternetHandle&) = delete;

            void Close() noexcept {
                if (value)
                    WinHttpCloseHandle(std::exchange(value, nullptr));
            }

            operator HINTERNET() const {
                return value;
            }
        };

        class AsyncRequest {
            InternetHandle handle;
            std::mutex mutex;
            std::condition_variable_any changed;
            DWORD completed = 0, error = 0;
            bool closed = false;

            static void CALLBACK Status(HINTERNET, DWORD_PTR context, DWORD status, void* information, DWORD length) {
                if (!context || status == WINHTTP_CALLBACK_STATUS_HANDLE_CREATED)
                    return;

                auto& self = *reinterpret_cast<AsyncRequest*>(context);
                std::lock_guard lock(self.mutex);

                if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
                    self.closed = true;
                } else if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
                    self.error = static_cast<WINHTTP_ASYNC_RESULT*>(information)->dwError;
                } else {
                    self.completed = status;

                    if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE)
                        self.bytesRead = length;
                }

                self.changed.notify_all();
            }

        public:
            std::vector<uint8_t> buffer = std::vector<uint8_t>(16384);
            DWORD bytesRead = 0;

            explicit AsyncRequest(HINTERNET request) : handle(request) {
                DWORD_PTR context = Context();
                winrt::check_bool(WinHttpSetOption(handle, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)));

                constexpr DWORD notifications = WINHTTP_CALLBACK_FLAG_SENDREQUEST_COMPLETE |
                    WINHTTP_CALLBACK_FLAG_HEADERS_AVAILABLE | WINHTTP_CALLBACK_FLAG_READ_COMPLETE |
                    WINHTTP_CALLBACK_FLAG_REQUEST_ERROR | WINHTTP_CALLBACK_FLAG_HANDLES;
                if (WinHttpSetStatusCallback(handle, Status, notifications, 0) == WINHTTP_INVALID_STATUS_CALLBACK)
                    winrt::throw_last_error();
            }

            ~AsyncRequest() {
                handle.Close();

                std::unique_lock lock(mutex);
                changed.wait(lock, [&] { return closed; });
            }

            operator HINTERNET() const {
                return handle;
            }

            DWORD_PTR Context() const {
                return reinterpret_cast<DWORD_PTR>(this);
            }

            template <typename Start>
            bool Run(DWORD completion, std::stop_token stop, Start start) {
                if (stop.stop_requested())
                    return false;

                {
                    std::lock_guard lock(mutex);
                    completed = error = 0;
                }

                winrt::check_bool(start());

                std::unique_lock lock(mutex);
                bool ready = changed.wait_for(lock, stop, std::chrono::milliseconds(RequestTimeoutMs),
                    [&] { return completed == completion || error; });

                if (stop.stop_requested())
                    return false;

                if (!ready)
                    winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_WINHTTP_TIMEOUT));

                if (error)
                    winrt::throw_hresult(HRESULT_FROM_WIN32(error));

                return true;
            }
        };
    }
}

export namespace Lemonsquash {
    bool HttpDownload(const std::wstring& url, size_t limit, std::stop_token stop,
        const std::function<void(std::span<const uint8_t>)>& receive) {
        if (stop.stop_requested())
            return false;

        URL_COMPONENTSW parts{sizeof(parts)};
        parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);

        winrt::check_bool(WinHttpCrackUrl(url.c_str(), 0, 0, &parts));

        if (parts.nScheme != INTERNET_SCHEME_HTTPS)
            throw std::runtime_error("HTTPS is required");

        std::wstring host(parts.lpszHostName, parts.dwHostNameLength), path(parts.lpszUrlPath, parts.dwUrlPathLength);

        if (parts.dwExtraInfoLength)
            path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

        InternetHandle session(WinHttpOpen(L"Lemonsquash/2.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));

        winrt::check_bool(WinHttpSetTimeouts(session, 5000, 5000, RequestTimeoutMs, RequestTimeoutMs));

        DWORD responseTimeout = RequestTimeoutMs;

        winrt::check_bool(WinHttpSetOption(session, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &responseTimeout, sizeof(responseTimeout)));

        InternetHandle connection(WinHttpConnect(session, host.c_str(), parts.nPort, 0));
        AsyncRequest request(WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));

        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;

        winrt::check_bool(WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy)));

        constexpr std::wstring_view cacheControlHeader = L"Cache-Control: no-cache\r\n";

        if (!request.Run(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, stop, [&] {
                return WinHttpSendRequest(request, cacheControlHeader.data(), static_cast<DWORD>(cacheControlHeader.size()),
                    WINHTTP_NO_REQUEST_DATA, 0, 0, request.Context());
            }))
            return false;

        if (!request.Run(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, stop, [&] {
                return WinHttpReceiveResponse(request, nullptr);
            }))
            return false;

        DWORD status = 0, size = sizeof(status);

        winrt::check_bool(WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX));

        if (status != 200)
            throw std::runtime_error("Server returned HTTP " + std::to_string(status));

        size_t received = 0;

        while (request.Run(WINHTTP_CALLBACK_STATUS_READ_COMPLETE, stop, [&] {
            return WinHttpReadData(request, request.buffer.data(), static_cast<DWORD>(request.buffer.size()), nullptr);
        })) {
            auto read = request.bytesRead;

            if (!read)
                return true;

            if (read > limit - received)
                throw std::runtime_error("Response is too large");

            received += read;
            receive(std::span<const uint8_t>(request.buffer.data(), read));
        }

        return false;
    }

    std::vector<uint8_t> HttpGet(const std::wstring& url, size_t limit, std::stop_token stop = {}) {
        std::vector<uint8_t> output;

        if (!HttpDownload(url, limit, stop, [&](std::span<const uint8_t> chunk) {
                output.insert(output.end(), chunk.begin(), chunk.end());
            }))
            return {};

        return output;
    }
}
