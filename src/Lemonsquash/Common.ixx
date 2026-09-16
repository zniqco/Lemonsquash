module;

#include "Platform.h"
#include "Version.h"
#include <cwctype>
#include <type_traits>

export module lemonsquash.common;

export namespace Lemonsquash {
    namespace fs = std::filesystem;
    inline constexpr const wchar_t* DisplayVersion = LEMONSQUASH_VERSION_POSTFIX[0]
        ? LEMONSQUASH_VERSION_WSTRING L"-" LEMONSQUASH_VERSION_POSTFIX
        : LEMONSQUASH_VERSION_WSTRING;
    inline constexpr wchar_t ServiceUrl[] = L"https://lemonsquash.zniq.co";
    inline constexpr UINT ResultsMessage = WM_APP + 1;
    inline constexpr UINT UpdateMessage = WM_APP + 2;
    inline constexpr UINT TrayMessage = WM_APP + 3;
    inline constexpr UINT ExitMessage = WM_APP + 4;

    struct Handle {
        HANDLE value = nullptr;
        Handle() = default;

        explicit Handle(HANDLE handle)
            : value(handle) {}

        ~Handle() {
            if (value && value != INVALID_HANDLE_VALUE)
                CloseHandle(value);
        }

        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;

        operator HANDLE() const {
            return value;
        }
    };

    std::wstring UTF16(std::string_view utf8Text) {
        if (utf8Text.empty())
            return {};

        int characterCount = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Text.data(),
            static_cast<int>(utf8Text.size()), nullptr, 0);

        if (!characterCount)
            throw std::runtime_error("Invalid UTF-8");

        std::wstring utf16Text(characterCount, L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Text.data(), static_cast<int>(utf8Text.size()),
            utf16Text.data(), characterCount);

        return utf16Text;
    }

    std::string UTF8(std::wstring_view utf16Text) {
        if (utf16Text.empty())
            return {};

        int byteCount = WideCharToMultiByte(CP_UTF8, 0, utf16Text.data(), static_cast<int>(utf16Text.size()),
            nullptr, 0, nullptr, nullptr);
        std::string utf8Text(byteCount, '\0');
        WideCharToMultiByte(CP_UTF8, 0, utf16Text.data(), static_cast<int>(utf16Text.size()), utf8Text.data(), byteCount,
            nullptr, nullptr);

        return utf8Text;
    }

    std::wstring Lower(std::wstring_view text) {
        if (text.empty())
            return {};

        int characterCount = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(),
            static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr, 0);

        if (!characterCount)
            return std::wstring(text);

        std::wstring lowercaseText(characterCount, L'\0');

        if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(), static_cast<int>(text.size()),
            lowercaseText.data(), characterCount, nullptr, nullptr, 0))
            return std::wstring(text);

        return lowercaseText;
    }

    std::wstring Trim(std::wstring_view text) {
        while (!text.empty() && iswspace(text.front()))
            text.remove_prefix(1);

        while (!text.empty() && iswspace(text.back()))
            text.remove_suffix(1);

        return std::wstring(text);
    }

    std::wstring QuoteArgument(std::wstring_view argument) {
        std::wstring quoted = L"\"";
        size_t backslashCount = 0;

        for (auto character : argument) {
            if (character == L'\\') {
                ++backslashCount;
                continue;
            }

            quoted.append(backslashCount * (character == L'"' ? 2 : 1), L'\\');
            backslashCount = 0;

            if (character == L'"')
                quoted += L'\\';

            quoted += character;
        }

        quoted.append(backslashCount * 2, L'\\');

        return quoted + L'"';
    }

    std::wstring EncodeUrl(std::wstring_view value) {
        static constexpr wchar_t hexDigits[] = L"0123456789ABCDEF";
        std::wstring encoded;

        for (unsigned char byte : UTF8(value)) {
            bool isUnreserved = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' || byte == '~';

            if (isUnreserved) {
                encoded += byte;
            } else {
                encoded += L'%';
                encoded += hexDigits[byte >> 4];
                encoded += hexDigits[byte & 15];
            }
        }

        return encoded;
    }

    std::vector<std::wstring> ParseArguments(const std::wstring& value) {
        auto trimmed = Trim(value);

        if (trimmed.empty())
            return {};

        int argumentCount = 0;
        auto parsedArguments = CommandLineToArgvW(trimmed.c_str(), &argumentCount);

        if (!parsedArguments)
            return {};

        std::vector<std::wstring> arguments(parsedArguments, parsedArguments + argumentCount);
        LocalFree(parsedArguments);

        return arguments;
    }

    fs::path ExecutablePath() {
        std::wstring path(32768, L'\0');
        DWORD characterCount = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));

        if (!characterCount || characterCount >= path.size())
            throw std::runtime_error("Cannot locate executable");

        path.resize(characterCount);

        return path;
    }

    fs::path AppDataPath() {
        PWSTR path = nullptr;
        winrt::check_hresult(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path));
        fs::path result = fs::path(path) / L"Lemonsquash";
        CoTaskMemFree(path);
        fs::create_directories(result);

        return result;
    }

    std::wstring WindowText(HWND window) {
        std::wstring text(GetWindowTextLengthW(window) + 1, L'\0');
        auto count = GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
        text.resize(count);

        return text;
    }

    std::wstring ErrorText(DWORD error) {
        wchar_t* buffer = nullptr;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
            error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);

        std::wstring result = buffer ? Trim(buffer) : L"Windows error " + std::to_wstring(error);
        LocalFree(buffer);

        return result;
    }

    void OpenShellItem(HWND owner, const std::wstring& path) {
        PIDLIST_ABSOLUTE value = nullptr;
        auto result = SHParseDisplayName(path.c_str(), nullptr, &value, 0, nullptr);
        std::unique_ptr<std::remove_pointer_t<PIDLIST_ABSOLUTE>, decltype(&CoTaskMemFree)> item(value, CoTaskMemFree);
        winrt::check_hresult(result);

        SHELLEXECUTEINFOW info{sizeof(info)};
        info.fMask = SEE_MASK_INVOKEIDLIST | SEE_MASK_FLAG_NO_UI;
        info.hwnd = owner;
        info.lpIDList = item.get();
        info.nShow = SW_SHOWNORMAL;

        if (!ShellExecuteExW(&info)) {
            DWORD error = GetLastError();

            if (error != ERROR_CANCELLED)
                MessageBoxW(owner, ErrorText(error).c_str(), L"Could not open", MB_OK | MB_ICONERROR);
        }
    }

    void OpenTarget(HWND owner, const std::wstring& target, const std::wstring& arguments = {}, bool elevate = false) {
        SHELLEXECUTEINFOW info{sizeof(info)};
        info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_ASYNCOK;
        info.hwnd = owner;
        info.lpVerb = elevate ? L"runas" : L"open";
        info.lpFile = target.c_str();
        info.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
        info.nShow = SW_SHOWNORMAL;

        if (!ShellExecuteExW(&info)) {
            DWORD error = GetLastError();

            if (error != ERROR_CANCELLED)
                MessageBoxW(owner, ErrorText(error).c_str(), L"Could not open", MB_OK | MB_ICONERROR);
        }
    }

    bool CopyText(HWND owner, const std::wstring& text) {
        if (!OpenClipboard(owner))
            return false;

        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
        bool copied = false;

        if (memory) {
            if (void* data = GlobalLock(memory)) {
                memcpy(data, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
                GlobalUnlock(memory);

                if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory))
                    copied = true;
            }

            if (!copied)
                GlobalFree(memory);
        }

        CloseClipboard();

        return copied;
    }

    void Log(const std::wstring& text) noexcept {
        OutputDebugStringW(L"Lemonsquash: ");
        OutputDebugStringW(text.c_str());
        OutputDebugStringW(L"\n");
    }

    std::wstring ExceptionText() {
        try {
            throw;
        } catch (const winrt::hresult_error& error) {
            return std::wstring(error.message());
        } catch (const std::exception& error) {
            return std::wstring(winrt::to_hstring(std::string_view(error.what())));
        } catch (...) {
            return L"Unknown error";
        }
    }

    void ShowException(HWND owner, const wchar_t* title) noexcept {
        try {
            auto text = ExceptionText();

            Log(text);
            MessageBoxW(owner, text.c_str(), title, MB_OK | MB_ICONERROR);
        } catch (...) {
        }
    }
}
