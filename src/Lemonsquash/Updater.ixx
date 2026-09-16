module;

#include "Platform.h"
#include "Version.h"
#include <array>
#include <initializer_list>
#include <span>

export module lemonsquash.updater;

export import lemonsquash.common;
import lemonsquash.http;
import lemonsquash.signature;

namespace Lemonsquash {
    namespace {
        constexpr size_t MaxPackageSize = 64 * 1024 * 1024, MaxExecutableSize = 128 * 1024 * 1024;
        // The executable on disk may already belong to a failed update.
        constexpr std::array<unsigned, 4> CurrentVersion{LEMONSQUASH_VERSION_NUMBERS};

        std::optional<std::array<unsigned, 4>> ReadExecutableVersion(const fs::path& path) {
            DWORD unused = 0, size = GetFileVersionInfoSizeW(path.c_str(), &unused);

            if (!size || size > 1024 * 1024)
                return {};

            std::vector<uint8_t> data(size);

            if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
                return {};

            VS_FIXEDFILEINFO* version = nullptr;
            UINT length = 0;

            if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&version), &length) || length < sizeof(*version))
                return {};

            return std::array<unsigned, 4>{HIWORD(version->dwFileVersionMS), LOWORD(version->dwFileVersionMS),
                HIWORD(version->dwFileVersionLS), LOWORD(version->dwFileVersionLS)};
        }

        HANDLE Launch(const fs::path& executable, const std::wstring& arguments,
            std::initializer_list<HANDLE> inherited = {}, HANDLE output = nullptr, HANDLE nullDevice = nullptr) {
            auto command = QuoteArgument(executable.wstring()) + L" " + arguments;
            STARTUPINFOEXW startup{};

            startup.StartupInfo.cb = sizeof(STARTUPINFOW);

            std::vector<uint8_t> attributes;
            auto deleteAttributes = [](LPPROC_THREAD_ATTRIBUTE_LIST value) {
                if (value)
                    DeleteProcThreadAttributeList(value);
            };

            std::unique_ptr<_PROC_THREAD_ATTRIBUTE_LIST, decltype(deleteAttributes)> attributeList(nullptr, deleteAttributes);
            DWORD flags = CREATE_NO_WINDOW;

            if (inherited.size()) {
                SIZE_T size = 0;

                InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
                attributes.resize(size);

                auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());

                winrt::check_bool(InitializeProcThreadAttributeList(list, 1, 0, &size));
                attributeList.reset(list);

                winrt::check_bool(UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                    const_cast<HANDLE*>(inherited.begin()), inherited.size() * sizeof(HANDLE), nullptr, nullptr));

                startup.lpAttributeList = list;
                startup.StartupInfo.cb = sizeof(startup);
                flags |= EXTENDED_STARTUPINFO_PRESENT;
            }

            if (output) {
                startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
                startup.StartupInfo.hStdOutput = output;
                startup.StartupInfo.hStdError = startup.StartupInfo.hStdInput = nullDevice;
            }

            PROCESS_INFORMATION process{};

            winrt::check_bool(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
                inherited.size() ? TRUE : FALSE, flags, nullptr, executable.parent_path().c_str(),
                &startup.StartupInfo, &process));
            CloseHandle(process.hThread);

            return process.hProcess;
        }

        void Spawn(const fs::path& executable, const std::wstring& arguments) {
            Handle process(Launch(executable, arguments));
        }

        fs::path UpdateCachePath() {
            PWSTR value = nullptr;

            winrt::check_hresult(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &value));

            std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(value, CoTaskMemFree);

            return fs::path(value) / L"Lemonsquash" / L"Updates";
        }

        void CheckCancelled(std::stop_token stop) {
            if (stop.stop_requested())
                throw std::runtime_error("Update cancelled");
        }

        void ExtractExecutable(const fs::path& archive, const fs::path& destination, std::stop_token stop) {
            wchar_t systemDirectory[MAX_PATH]{};
            auto length = GetSystemDirectoryW(systemDirectory, MAX_PATH);

            if (!length || length >= MAX_PATH)
                winrt::throw_last_error();

            auto extractor = fs::path(systemDirectory) / L"tar.exe";
            SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
            Handle output(CreateFileW(destination.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL, nullptr));

            if (output.value == INVALID_HANDLE_VALUE)
                winrt::throw_last_error();

            Handle nullDevice(CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));

            if (nullDevice.value == INVALID_HANDLE_VALUE)
                winrt::throw_last_error();

            Handle process(Launch(extractor, L"-xOf " + QuoteArgument(archive.wstring()) + L" -- Lemonsquash.exe",
                {output, nullDevice}, output, nullDevice));
            auto deadline = GetTickCount64() + 60000;

            try {
                for (;;) {
                    auto wait = WaitForSingleObject(process, 25);

                    if (wait == WAIT_FAILED)
                        winrt::throw_last_error();

                    CheckCancelled(stop);

                    LARGE_INTEGER size{};

                    winrt::check_bool(GetFileSizeEx(output, &size));

                    if (size.QuadPart > MaxExecutableSize || GetTickCount64() >= deadline)
                        throw std::runtime_error("The update archive is too large or took too long to extract");

                    if (wait == WAIT_OBJECT_0)
                        break;
                }

                DWORD result = 0;

                winrt::check_bool(GetExitCodeProcess(process, &result));

                if (result)
                    throw std::runtime_error("The update ZIP is invalid or does not contain Lemonsquash.exe");

                winrt::check_bool(FlushFileBuffers(output));
            } catch (...) {
                TerminateProcess(process, 1);
                WaitForSingleObject(process, 5000);
                throw;
            }
        }

        void ValidateExecutable(const fs::path& executable, const std::array<unsigned, 4>& expected) {
            auto size = fs::file_size(executable);
            std::ifstream input(executable, std::ios::binary);
            IMAGE_DOS_HEADER dos{};
            IMAGE_NT_HEADERS64 nt{};

            input.read(reinterpret_cast<char*>(&dos), sizeof(dos));

            if (!input || size > MaxExecutableSize || dos.e_magic != IMAGE_DOS_SIGNATURE ||
                dos.e_lfanew < sizeof(dos) || static_cast<uint64_t>(dos.e_lfanew) + sizeof(nt) > size)
                throw std::runtime_error("The update does not contain a valid Windows executable");

            input.seekg(dos.e_lfanew);
            input.read(reinterpret_cast<char*>(&nt), sizeof(nt));

#if defined(_M_ARM64)
            constexpr WORD machine = IMAGE_FILE_MACHINE_ARM64;
#else
            constexpr WORD machine = IMAGE_FILE_MACHINE_AMD64;
#endif

            if (!input || nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != machine ||
                nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
                !(nt.FileHeader.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) ||
                (nt.FileHeader.Characteristics & IMAGE_FILE_DLL))
                throw std::runtime_error("The update executable is not compatible with this application");

            if (ReadExecutableVersion(executable) != expected)
                throw std::runtime_error("The update executable version does not match latest.txt");
        }

        void MoveWithRetry(const fs::path& source, const fs::path& destination) {
            for (unsigned attempt = 0;; ++attempt) {
                if (MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                    return;

                auto error = GetLastError();

                if (attempt >= 49 || (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED))
                    winrt::throw_hresult(HRESULT_FROM_WIN32(error));

                Sleep(100);
            }
        }

        void ReplaceExecutable(const fs::path& source, const fs::path& target) {
            auto replacement = target, previous = target;
            replacement += L".new";
            previous += L".old";

            try {
                winrt::check_bool(CopyFileW(source.c_str(), replacement.c_str(), FALSE));
                MoveWithRetry(target, previous);

                try {
                    MoveWithRetry(replacement, target);
                } catch (...) {
                    MoveFileExW(previous.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
                    throw;
                }
            } catch (...) {
                std::error_code ignored;
                fs::remove(replacement, ignored);
                throw;
            }
        }
    }
}

export namespace Lemonsquash {
    std::optional<std::array<unsigned, 4>> ParseVersion(std::wstring_view input) {
        auto value = Trim(input);

        if (!value.empty() && value.front() == 0xfeff)
            value = Trim(std::wstring_view(value).substr(1));

        std::array<unsigned, 4> result{};
        unsigned index = 0, digits = 0;

        for (wchar_t c : value) {
            if (c == L'.') {
                if (!digits || ++index >= result.size())
                    return {};

                digits = 0;
            } else if (c >= L'0' && c <= L'9') {
                result[index] = result[index] * 10 + c - L'0';

                if (++digits > 5 || result[index] > 65535)
                    return {};
            } else {
                return {};
            }
        }

        if (!digits || index < 2)
            return {};

        return result;
    }

    struct ReleaseInfo {
        std::wstring version, displayVersion;
        std::array<unsigned, 4> comparisonVersion;
    };

    ReleaseInfo ParseReleaseInfo(std::wstring_view input) {
        auto text = Trim(input);

        if (!text.empty() && text.front() == 0xfeff)
            text = Trim(std::wstring_view(text).substr(1));

        auto separator = text.find(L'|');

        if (separator == std::wstring::npos || text.find(L'|', separator + 1) != std::wstring::npos)
            throw std::runtime_error("latest.txt must contain comparisonVersion|displayVersion");

        auto version = Trim(std::wstring_view(text).substr(0, separator));
        auto displayVersion = Trim(std::wstring_view(text).substr(separator + 1));
        auto comparisonVersion = ParseVersion(version);

        if (!comparisonVersion)
            throw std::runtime_error("Invalid comparison version");

        return {std::move(version), std::move(displayVersion), *comparisonVersion};
    }

    void CleanupUpdate() noexcept {
        try {
            std::error_code ignored;
            fs::remove_all(UpdateCachePath(), ignored);

            auto previous = ExecutablePath();
            previous += L".old";

            for (int attempt = 0; attempt < 50 && fs::exists(previous, ignored); ++attempt) {
                if (fs::remove(previous, ignored))
                    break;

                Sleep(100);
            }
        } catch (...) {
            Log(L"Could not remove previous update files");
        }
    }

    void StartRestart(HWND window) {
        Spawn(ExecutablePath(), L"--restart");
        PostMessageW(window, ExitMessage, 0, 0);
    }

    enum class UpdateState { Idle, Checking, Available, Downloading, Installing, Restarting, UpToDate, Failed };

    class Updater {
        std::function<void()> onChanged;
        std::jthread worker;
        mutable std::mutex mutex;
        UpdateState state = UpdateState::Idle;
        std::optional<ReleaseInfo> availableRelease;
        std::wstring lastError;
        std::optional<std::chrono::steady_clock::time_point> lastCheck;

        void Notify(UpdateState value) {
            {
                std::lock_guard lock(mutex);
                state = value;
            }

            if (onChanged)
                onChanged();
        }

        void Fail(const std::wstring& error) {
            Log(L"Update failed: " + error);

            {
                std::lock_guard lock(mutex);
                lastError = error;
            }

            Notify(UpdateState::Failed);
        }

    public:
        static bool Busy(UpdateState value) {
            return value == UpdateState::Checking || value == UpdateState::Downloading ||
                value == UpdateState::Installing || value == UpdateState::Restarting;
        }

        explicit Updater(std::function<void()> notify)
            : onChanged(std::move(notify)) {}

        ~Updater() {
            worker.request_stop();

            if (worker.joinable())
                worker.join();
        }

        UpdateState State() const {
            std::lock_guard lock(mutex);

            return state;
        }

        std::wstring AvailableDisplayVersion() const {
            std::lock_guard lock(mutex);

            return availableRelease ? availableRelease->displayVersion : L"";
        }

        std::wstring Error() const {
            std::lock_guard lock(mutex);

            return lastError;
        }

        bool Check(bool automatic = false) {
            {
                std::lock_guard lock(mutex);

                if (Busy(state))
                    return false;

                auto now = std::chrono::steady_clock::now();

                if (automatic && lastCheck && now - *lastCheck < std::chrono::hours(6))
                    return false;

                state = UpdateState::Checking;
                lastCheck = now;
                availableRelease.reset();
                lastError.clear();
            }

            try {
                worker = std::jthread([this](std::stop_token stop) {
                    try {
                        auto bytes = HttpGet(std::wstring(ServiceUrl) + L"/updates/latest.txt", 4096, stop);

                        if (stop.stop_requested())
                            return;

                        auto release = ParseReleaseInfo(UTF16(
                            std::string_view(reinterpret_cast<char*>(bytes.data()), bytes.size())));
                        bool newer = release.comparisonVersion > CurrentVersion;

                        {
                            std::lock_guard lock(mutex);
                            if (newer)
                                availableRelease = std::move(release);
                        }

                        Notify(newer ? UpdateState::Available : UpdateState::UpToDate);
                    } catch (...) {
                        if (!stop.stop_requested())
                            Fail(ExceptionText());
                    }
                });
            } catch (...) {
                Notify(UpdateState::Failed);
                throw;
            }

            return true;
        }

        void Install(HWND applicationWindow) {
            std::wstring version;

            {
                std::lock_guard lock(mutex);

                if (state != UpdateState::Available || !availableRelease)
                    return;

                version = availableRelease->version;
                lastError.clear();
                state = UpdateState::Downloading;
            }

            try {
                worker = std::jthread([this, version, applicationWindow](std::stop_token stop) {
                    try {
                        auto expected = ParseVersion(version);

                        if (!expected)
                            throw std::runtime_error("Invalid update version");

                        auto target = ExecutablePath();
                        auto staging = UpdateCachePath();
                        std::error_code ignored;

                        fs::remove_all(staging, ignored);
                        fs::create_directories(staging);

                        auto archive = staging / L"package.zip";
                        auto payload = staging / L"Lemonsquash.exe";

                        {
                            std::ofstream output(archive, std::ios::binary);

                            HttpDownload(std::wstring(ServiceUrl) + L"/updates/" + version + L".zip", MaxPackageSize, stop,
                                [&](std::span<const uint8_t> chunk) {
                                    output.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
                                });

                            CheckCancelled(stop);
                            output.close();

                            if (!output)
                                throw std::runtime_error("Cannot save the update ZIP; check free space and folder permissions");
                        }

                        Notify(UpdateState::Installing);
                        ExtractExecutable(archive, payload, stop);
                        ValidateExecutable(payload, *expected);
                        VerifyExecutableSignature(payload);
                        CheckCancelled(stop);

                        ReplaceExecutable(payload, target);
                        Handle restarted;

                        try {
                            CheckCancelled(stop);
                            restarted.value = Launch(target, L"--restart");
                            CheckCancelled(stop);

                            if (!PostMessageW(applicationWindow, ExitMessage, 0, 0))
                                throw std::runtime_error("The application could not restart for the update");
                        } catch (...) {
                            if (restarted.value) {
                                if (!TerminateProcess(restarted, 1)) {
                                    auto error = GetLastError();

                                    if (WaitForSingleObject(restarted, 0) != WAIT_OBJECT_0)
                                        winrt::throw_hresult(HRESULT_FROM_WIN32(error));
                                }

                                if (WaitForSingleObject(restarted, 5000) != WAIT_OBJECT_0)
                                    throw std::runtime_error("The update process could not be stopped to restore the previous version");
                            }

                            auto previous = target;
                            previous += L".old";
                            MoveWithRetry(previous, target);
                            throw;
                        }

                        Notify(UpdateState::Restarting);
                    } catch (...) {
                        if (!stop.stop_requested())
                            Fail(ExceptionText());
                    }
                });
            } catch (...) {
                Notify(UpdateState::Failed);
                throw;
            }
        }
    };
}
