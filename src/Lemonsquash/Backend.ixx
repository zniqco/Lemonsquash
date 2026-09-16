module;

#include "Platform.h"
#include <propkey.h>
#include <type_traits>
#include <unordered_set>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Management.Deployment.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>

export module lemonsquash.backend;

export import lemonsquash.common;
import lemonsquash.search;
import lemonsquash.bitmap;
import lemonsquash.http;

namespace Lemonsquash {
    namespace {
        constexpr DWORD ShortcutSettleDelay = 500;
        constexpr auto PackagePollInterval = std::chrono::minutes(5);
        constexpr size_t ShortcutBufferSize = 32768;

        struct Apartment {
            Apartment() {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
            }

            ~Apartment() {
                winrt::uninit_apartment();
            }
        };

        struct Watchers {
            std::vector<HANDLE> handles;

            ~Watchers() {
                for (auto handle : handles)
                    FindCloseChangeNotification(handle);
            }
        };

        struct CatalogSignal {
            std::atomic<bool> packagesChanged{true};
            Handle wake;

            CatalogSignal()
                : wake(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
                if (!wake.value)
                    winrt::throw_last_error();
            }

            void Wake() const noexcept {
                SetEvent(wake);
            }

            void PackagesChanged() noexcept {
                packagesChanged.store(true);
                Wake();
            }
        };

        struct PackageWatchers {
            winrt::Windows::ApplicationModel::PackageCatalog catalog;
            winrt::Windows::ApplicationModel::PackageCatalog::PackageInstalling_revoker installing;
            winrt::Windows::ApplicationModel::PackageCatalog::PackageUninstalling_revoker uninstalling;
            winrt::Windows::ApplicationModel::PackageCatalog::PackageUpdating_revoker updating;

            explicit PackageWatchers(const std::shared_ptr<CatalogSignal>& signal)
                : catalog(winrt::Windows::ApplicationModel::PackageCatalog::OpenForCurrentUser()) {
                auto completed = [signal](const auto&, const auto& args) {
                    if (args.IsComplete())
                        signal->PackagesChanged();
                };

                installing = catalog.PackageInstalling(winrt::auto_revoke, completed);
                uninstalling = catalog.PackageUninstalling(winrt::auto_revoke, completed);
                updating = catalog.PackageUpdating(winrt::auto_revoke, completed);
            }
        };

        std::vector<fs::path> StartFolders() {
            std::vector<fs::path> paths;

            for (auto id : {FOLDERID_StartMenu, FOLDERID_CommonStartMenu}) {
                PWSTR value = nullptr;

                if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &value))) {
                    paths.emplace_back(value);
                    CoTaskMemFree(value);
                }
            }

            return paths;
        }

        std::wstring ShellItemName(IShellItem* item, SIGDN format) {
            PWSTR value = nullptr;
            auto result = item->GetDisplayName(format, &value);
            std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> name(value, CoTaskMemFree);
            winrt::check_hresult(result);

            return name ? name.get() : L"";
        }

        std::vector<AppEntry> EnumerateControlPanel(std::stop_token stop) {
            std::vector<AppEntry> apps;

            if (stop.stop_requested())
                return apps;

            try {
                PIDLIST_ABSOLUTE folderValue = nullptr;
                auto folderResult = SHGetKnownFolderIDList(FOLDERID_ControlPanelFolder, KF_FLAG_DEFAULT, nullptr, &folderValue);
                std::unique_ptr<std::remove_pointer_t<PIDLIST_ABSOLUTE>, decltype(&CoTaskMemFree)> folderId(folderValue, CoTaskMemFree);
                winrt::check_hresult(folderResult);
                winrt::com_ptr<IShellItem> folder;
                winrt::check_hresult(SHCreateItemFromIDList(folderId.get(), IID_PPV_ARGS(folder.put())));
                winrt::com_ptr<IEnumShellItems> items;
                winrt::check_hresult(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(items.put())));

                while (!stop.stop_requested()) {
                    winrt::com_ptr<IShellItem> item;
                    auto next = items->Next(1, item.put(), nullptr);
                    winrt::check_hresult(next);

                    if (next != S_OK)
                        break;

                    try {
                        SFGAOF attributes = 0;
                        if (SUCCEEDED(item->GetAttributes(SFGAO_HIDDEN, &attributes)) && (attributes & SFGAO_HIDDEN))
                            continue;

                        auto caption = ShellItemName(item.get(), SIGDN_NORMALDISPLAY);
                        auto path = ShellItemName(item.get(), SIGDN_DESKTOPABSOLUTEPARSING);

                        if (caption.empty() || path.empty() || caption == path)
                            continue;

                        std::wstring tag;
                        if (auto properties = item.try_as<IShellItem2>()) {
                            PWSTR value = nullptr;
                            auto result = properties->GetString(PKEY_ApplicationName, &value);
                            std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> name(value, CoTaskMemFree);

                            if (SUCCEEDED(result) && name)
                                tag = name.get();
                        }

                        winrt::com_ptr<IShellItem> parsed;
                        if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(parsed.put())))) {
                            // Some items (such as Fonts) return a localized path that cannot be parsed again.
                            if (tag.empty())
                                continue;

                            auto panel = winrt::create_instance<IOpenControlPanel>(CLSID_OpenControlPanel);
                            path.assign(32768, L'\0');
                            winrt::check_hresult(panel->GetPath(tag.c_str(), path.data(), static_cast<UINT>(path.size())));
                            path.resize(wcslen(path.c_str()));
                            winrt::check_hresult(
                                SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(parsed.put())));
                        }

                        auto id = L"controlpanel:" + Lower(tag.empty() ? path : tag);
                        apps.push_back({id, caption, tag, path, path, {}, {}, Action::ControlPanel});
                    } catch (...) {
                        Log(L"Skipped unreadable Control Panel item");
                    }
                }
            } catch (...) {
                Log(L"Control Panel enumeration unavailable; applications remain usable");
            }

            return apps;
        }

        std::vector<AppEntry> EnumerateShortcuts(std::stop_token stop) {
            std::vector<AppEntry> apps;
            std::vector<wchar_t> targetBuffer(ShortcutBufferSize), argumentsBuffer(ShortcutBufferSize);

            for (const auto& folder : StartFolders()) {
                std::error_code ec;
                fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;

                for (; it != end && !stop.stop_requested(); it.increment(ec)) {
                    if (ec) {
                        ec.clear();
                        continue;
                    }

                    const auto path = it->path();

                    if (Lower(path.extension().wstring()) != L".lnk")
                        continue;

                    try {
                        auto link = winrt::create_instance<IShellLinkW>(CLSID_ShellLink);
                        winrt::check_hresult(link.as<IPersistFile>()->Load(path.c_str(), STGM_READ));
                        WIN32_FIND_DATAW data{};
                        targetBuffer[0] = argumentsBuffer[0] = L'\0';
                        link->GetPath(targetBuffer.data(), static_cast<int>(targetBuffer.size()), &data, SLGP_UNCPRIORITY);
                        link->GetArguments(argumentsBuffer.data(), static_cast<int>(argumentsBuffer.size()));
                        targetBuffer.back() = argumentsBuffer.back() = L'\0';
                        std::wstring target(targetBuffer.data());
                        bool hasArguments = argumentsBuffer[0] != L'\0';
                        auto caption = path.stem().wstring();
                        winrt::com_ptr<IShellItem> item;

                        if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(item.put())))) {
                            PWSTR name = nullptr;

                            if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name))) {
                                caption = name;
                                CoTaskMemFree(name);
                            }
                        }

                        auto tag = target.empty() ? path.stem().wstring() : fs::path(target).stem().wstring();
                        AppEntry app{L"lnk:" + Lower(path.wstring()), caption, tag, path.wstring(), path.wstring()};
                        app.hasArguments = hasArguments;
                        app.resolvedTarget = std::move(target);
                        apps.push_back(std::move(app));
                    } catch (...) {
                        Log(L"Skipped unreadable shortcut: " + path.wstring());
                    }
                }
            }

            return apps;
        }

        std::vector<AppEntry> EnumeratePackages(std::stop_token stop) {
            std::vector<AppEntry> apps;

            if (!stop.stop_requested()) {
                try {
                    winrt::Windows::Management::Deployment::PackageManager manager;

                    for (const auto& package : manager.FindPackagesForUser(L"")) {
                        if (stop.stop_requested())
                            break;

                        try {
                            if (package.IsFramework() || package.IsResourcePackage())
                                continue;

                            auto operation = package.GetAppListEntriesAsync();
                            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

                            while (operation.wait_for(std::chrono::milliseconds(100)) ==
                                winrt::Windows::Foundation::AsyncStatus::Started &&
                                !stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
                            }

                            if (stop.stop_requested() ||
                                operation.Status() == winrt::Windows::Foundation::AsyncStatus::Started) {
                                operation.Cancel();
                                continue;
                            }
                            for (const auto& entry : operation.get()) {
                                std::wstring id(entry.AppUserModelId()), caption(entry.DisplayInfo().DisplayName());

                                if (caption.empty())
                                    continue;

                                apps.push_back({L"appx:" + id,
                                    caption,
                                    std::wstring(package.Id().Name()),
                                    id,
                                    L"shell:AppsFolder\\" + id,
                                    {},
                                    {},
                                    Action::Appx});
                            }
                        } catch (...) {
                            Log(L"Skipped unavailable package");
                        }
                    }
                } catch (...) {
                    Log(L"Windows app enumeration unavailable; shortcuts remain usable");
                }
            }

            return apps;
        }

        std::vector<AppEntry> MergeApps(const std::vector<AppEntry>& shortcuts, const std::vector<AppEntry>& packages,
            const std::vector<AppEntry>& controlPanel) {
            std::vector<AppEntry> apps;
            apps.reserve(shortcuts.size() + packages.size() + controlPanel.size());

            for (auto source : {&shortcuts, &packages, &controlPanel})
                apps.insert(apps.end(), source->begin(), source->end());

            std::stable_sort(apps.begin(), apps.end(), [](const auto& a, const auto& b) {
                return CompareStringEx(LOCALE_NAME_USER_DEFAULT, SORT_STRINGSORT, a.caption.c_str(), -1, b.caption.c_str(), -1,
                    nullptr, nullptr, 0) == CSTR_LESS_THAN;
            });
            std::unordered_set<std::wstring> seen;
            std::erase_if(apps, [&](const AppEntry& item) {
                return !seen.insert(item.id).second;
            });

            return apps;
        }

        std::shared_ptr<Bitmap> ReadIcon(const std::wstring& path) {
            winrt::com_ptr<IShellItemImageFactory> shell;

            if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(shell.put()))))
                return {};

            HBITMAP image = nullptr;

            if (FAILED(shell->GetImage({64, 64}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &image)))
                return {};

            std::shared_ptr<Bitmap> bitmap;
            try {
                bitmap = std::make_shared<Bitmap>(Bitmap::FromGdiBitmap(image));
            } catch (...) {
                DeleteObject(image);
                throw;
            }
            DeleteObject(image);

            return bitmap;
        }
    }
}

export namespace Lemonsquash {
    struct BackendUpdate {
        std::shared_ptr<const std::vector<AppEntry>> apps;
        std::optional<std::pair<std::wstring, std::vector<std::wstring>>> suggestions;
        std::vector<std::pair<std::wstring, std::shared_ptr<Bitmap>>> icons;
    };

    class Backend {
        HWND window;
        std::mutex mutex;
        std::condition_variable_any changed;
        BackendUpdate pending;
        std::deque<std::pair<std::wstring, std::wstring>> iconRequests;
        std::unordered_set<std::wstring> requestedIcons;
        std::wstring query;
        uint64_t generation = 0;
        std::stop_source suggestionStop;
        std::jthread catalogThread, iconThread, networkThread;

        void CatalogLoop(std::stop_token stop) {
            try {
                Apartment apartment;
                Watchers watchers;
                auto signal = std::make_shared<CatalogSignal>();
                std::stop_callback wakeOnStop(stop, [signal] { signal->Wake(); });

                for (const auto& path : StartFolders()) {
                    auto h = FindFirstChangeNotificationW(path.c_str(), TRUE,
                        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                        FILE_NOTIFY_CHANGE_LAST_WRITE);

                    if (h != INVALID_HANDLE_VALUE)
                        watchers.handles.push_back(h);
                }

                std::vector<AppEntry> shortcuts, packages, controlPanel;
                auto assign = [](std::vector<AppEntry>& source, std::vector<AppEntry> entries) {
                    for (auto& app : entries) {
                        app.lowerCaption = Lower(app.caption);
                        app.lowerTag = Lower(app.tag);
                    }
                    source = std::move(entries);
                };
                auto publish = [&] {
                    if (stop.stop_requested())
                        return;

                    auto apps = std::make_shared<const std::vector<AppEntry>>(MergeApps(shortcuts, packages, controlPanel));

                    {
                        std::lock_guard lock(mutex);
                        pending.apps = std::move(apps);
                    }
                    Notify();
                };

                assign(shortcuts, EnumerateShortcuts(stop));
                publish();

                if (stop.stop_requested())
                    return;

                std::optional<PackageWatchers> packageWatchers;

                try {
                    packageWatchers.emplace(signal);
                } catch (...) {
                    Log(L"Package change notifications unavailable; packages will be checked periodically");
                }

                auto nextPackageScan = std::chrono::steady_clock::now();
                std::vector<HANDLE> waits;

                while (!stop.stop_requested()) {
                    bool scanPackages = signal->packagesChanged.exchange(false);
                    if (!packageWatchers && std::chrono::steady_clock::now() >= nextPackageScan)
                        scanPackages = true;

                    bool scanShortcuts = false;
                    for (auto watcher = watchers.handles.begin(); watcher != watchers.handles.end();) {
                        if (WaitForSingleObject(*watcher, 0) != WAIT_OBJECT_0) {
                            ++watcher;
                            continue;
                        }

                        scanShortcuts = true;

                        if (FindNextChangeNotification(*watcher)) {
                            ++watcher;
                            continue;
                        }

                        FindCloseChangeNotification(*watcher);
                        watcher = watchers.handles.erase(watcher);
                    }

                    if (scanShortcuts || scanPackages) {
                        if (scanShortcuts)
                            assign(shortcuts, EnumerateShortcuts(stop));

                        assign(controlPanel, EnumerateControlPanel(stop));
                        publish();
                    }

                    if (scanPackages) {
                        assign(packages, EnumeratePackages(stop));
                        publish();
                        nextPackageScan = std::chrono::steady_clock::now() + PackagePollInterval;
                    }

                    DWORD timeout = INFINITE;

                    if (!packageWatchers) {
                        auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
                            nextPackageScan - std::chrono::steady_clock::now()).count();
                        timeout = static_cast<DWORD>(std::max<long long>(remaining, 0));
                    }

                    waits.assign(1, signal->wake.value);
                    waits.insert(waits.end(), watchers.handles.begin(), watchers.handles.end());

                    auto result = WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), FALSE, timeout);

                    if (result == WAIT_FAILED)
                        winrt::throw_last_error();

                    if (result > WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + waits.size())
                        WaitForSingleObject(signal->wake, ShortcutSettleDelay);
                }
            } catch (...) {
                Log(L"Could not read application list");
            }
        }

        void IconLoop(std::stop_token stop) {
            try {
                Apartment apartment;

                while (!stop.stop_requested()) {
                    std::pair<std::wstring, std::wstring> request;
                    {
                        std::unique_lock lock(mutex);
                        changed.wait(lock, stop, [&] {
                            return !iconRequests.empty();
                        });

                        if (stop.stop_requested())
                            break;

                        request = std::move(iconRequests.front());
                        iconRequests.pop_front();
                    }
                    std::shared_ptr<Bitmap> image;

                    try {
                        image = ReadIcon(request.second);
                    } catch (...) {
                    }
                    {
                        std::lock_guard lock(mutex);
                        pending.icons.emplace_back(request.first, std::move(image));
                    }
                    Notify();
                }
            } catch (...) {
                Log(L"Icon worker unavailable");
            }
        }

        void NetworkLoop(std::stop_token stop) {
            try {
                Apartment apartment;
                uint64_t handled = 0;

                while (!stop.stop_requested()) {
                    std::wstring request;
                    uint64_t requestId;
                    std::stop_source requestStop(std::nostopstate);

                    {
                        std::unique_lock lock(mutex);
                        changed.wait(lock, stop, [&] {
                            return generation != handled;
                        });

                        if (stop.stop_requested())
                            break;

                        requestId = generation;

                        if (changed.wait_for(lock, stop, std::chrono::milliseconds(80), [&] {
                                return generation != requestId;
                            }))
                            continue;

                        if (stop.stop_requested())
                            break;

                        handled = requestId;
                        request = query;
                        requestStop = suggestionStop;
                    }

                    if (Trim(request).empty())
                        continue;

                    std::stop_callback cancelOnShutdown(stop, [&] { requestStop.request_stop(); });
                    std::vector<std::wstring> suggestions;

                    try {
                        auto body =
                            HttpGet(L"https://suggestqueries.google.com/complete/search?client=firefox&q=" + EncodeUrl(request),
                                256 * 1024, requestStop.get_token());

                        if (requestStop.stop_requested())
                            continue;

                        auto json = winrt::Windows::Data::Json::JsonArray::Parse(
                            UTF16(std::string_view(reinterpret_cast<char*>(body.data()), body.size())));

                        if (json.Size() >= 2) {
                            auto values = json.GetArrayAt(1);

                            for (uint32_t i = 0; i < std::min(4u, values.Size()); ++i)
                                suggestions.emplace_back(values.GetStringAt(i));
                        }
                    } catch (...) {
                    }

                    {
                        std::lock_guard lock(mutex);

                        if (generation != requestId || stop.stop_requested())
                            continue;

                        pending.suggestions = std::make_pair(request, std::move(suggestions));
                    }
                    Notify();
                }
            } catch (...) {
                Log(L"Suggestion worker unavailable");
            }
        }

        void Notify() {
            PostMessageW(window, ResultsMessage, 0, 0);
        }

    public:
        explicit Backend(HWND target)
            : window(target) {
            catalogThread = std::jthread([this](auto stop) {
                CatalogLoop(stop);
            });
            iconThread = std::jthread([this](auto stop) {
                IconLoop(stop);
            });
            networkThread = std::jthread([this](auto stop) {
                NetworkLoop(stop);
            });
        }

        ~Backend() {
            Stop();
        }

        void Stop() {
            catalogThread.request_stop();
            iconThread.request_stop();
            networkThread.request_stop();
            changed.notify_all();

            if (catalogThread.joinable())
                catalogThread.join();

            if (iconThread.joinable())
                iconThread.join();

            if (networkThread.joinable())
                networkThread.join();
        }

        void Suggest(const std::wstring& text) {
            std::stop_source previous;
            {
                std::lock_guard lock(mutex);
                query = text;
                std::swap(previous, suggestionStop);
                ++generation;
                pending.suggestions.reset();
            }
            previous.request_stop();
            changed.notify_all();
        }

        void RequestIcon(const std::wstring& id, const std::wstring& path) {
            if (path.empty())
                return;

            {
                std::lock_guard lock(mutex);

                if (!requestedIcons.insert(id).second)
                    return;

                iconRequests.emplace_back(id, path);
            }

            changed.notify_all();
        }

        BackendUpdate Poll() {
            std::lock_guard lock(mutex);
            BackendUpdate update = std::move(pending);
            pending = {};

            return update;
        }
    };
}
