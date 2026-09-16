module;

#include "Platform.h"
#include "Resource.h"

export module lemonsquash.app;

export import lemonsquash.common;
import lemonsquash.backend;
import lemonsquash.main_dialog;
import lemonsquash.notify_icon;
import lemonsquash.search;
import lemonsquash.settings;
import lemonsquash.preferences_dialog;
import lemonsquash.about_dialog;
import lemonsquash.updater;

namespace Lemonsquash {
    namespace {
        constexpr UINT HotkeyId = 1;
        constexpr UINT MenuPreferences = 2002, MenuExit = 2003, MenuAbout = 2004;
    }
}

export namespace Lemonsquash {
    class App {
        HINSTANCE instance;
        Settings settings;
        MainDialog mainDialog;
        NotifyIcon notifyIcon;
        HMENU trayMenu = nullptr;
        std::unique_ptr<Backend> backend;
        std::unique_ptr<Updater> updater;
        std::unique_ptr<PreferencesDialog> preferences;
        std::unique_ptr<AboutDialog> about;
        bool registered = false, stopping = false;

        void Initialize() {
            auto window = mainDialog.Handle();
            trayMenu = CreatePopupMenu();
            winrt::check_bool(trayMenu != nullptr);
            winrt::check_bool(AppendMenuW(trayMenu, MF_STRING, MenuPreferences, L"Preferences"));
            winrt::check_bool(AppendMenuW(trayMenu, MF_STRING, MenuAbout, L"About"));
            winrt::check_bool(AppendMenuW(trayMenu, MF_SEPARATOR, 0, nullptr));
            winrt::check_bool(AppendMenuW(trayMenu, MF_STRING, MenuExit, L"Exit"));
            notifyIcon.Create(window, TrayMessage, LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP)), L"Lemonsquash");

            if (!RegisterShortcut(settings))
                MessageBoxW(window,
                    L"The shortcut is already in use. Open Preferences from the tray to choose another.",
                    L"Lemonsquash", MB_OK | MB_ICONINFORMATION);

            if (settings.tutorial) {
                auto message = L"Press " + HotkeyText(settings.key, settings.modifiers) + L" to search apps, calculate, or run a command.";
                notifyIcon.ShowBalloon(L"Lemonsquash is ready", message);
                settings.tutorial = false;

                try {
                    settings.Save();
                } catch (...) {
                    Log(L"Could not save tutorial state");
                }
            }

            backend = std::make_unique<Backend>(window);
            updater = std::make_unique<Updater>([window] { PostMessageW(window, UpdateMessage, 0, 0); });
            preferences = std::make_unique<PreferencesDialog>(
                instance, [this](const Settings& value, bool startup) { return ApplySettings(value, startup); },
                [this] { RegisterShortcut(settings); });
            about = std::make_unique<AboutDialog>(instance, *updater);
        }

        void Execute(const Result& item, bool control) {
            auto window = mainDialog.Handle();

            switch (item.action) {
                case Action::Shortcut: {
                    OpenTarget(window, item.target, {}, control);

                    break;
                }

                case Action::Appx: {
                    if (control) {
                        OpenTarget(window, L"shell:AppsFolder\\" + item.target, {}, true);
                    } else {
                        auto manager = winrt::create_instance<IApplicationActivationManager>(CLSID_ApplicationActivationManager);
                        DWORD pid;
                        winrt::check_hresult(manager->ActivateApplication(item.target.c_str(), nullptr, AO_NONE, &pid));
                    }

                    break;
                }

                case Action::ControlPanel: {
                    OpenShellItem(window, item.target);

                    break;
                }

                case Action::Calculator: {
                    if (!CopyText(window, item.target))
                        MessageBoxW(window, L"The clipboard is busy. Please try again.", L"Lemonsquash", MB_OK);

                    break;
                }

                case Action::Google: {
                    OpenTarget(window, L"https://www.google.com/search?q=" + EncodeUrl(item.target));

                    break;
                }

                case Action::Shell: {
                    auto command = Trim(item.target);
                    bool quoted = command.starts_with(L'"');
                    auto end = quoted ? command.find(L'"', 1) : command.find_first_of(L" \t");
                    auto program = quoted ? command.substr(1, end - 1) : command.substr(0, end);
                    auto parameters = end == std::wstring::npos ? L"" : Trim(std::wstring_view(command).substr(end + 1));

                    if (!program.empty())
                        OpenTarget(window, program, parameters, control);

                    break;
                }

                case Action::Preferences: {
                    ShowPreferences();

                    break;
                }

                case Action::About: {
                    ShowAbout();

                    break;
                }

                case Action::Restart: {
                    StartRestart(window);

                    break;
                }
            }
        }

        void Receive() {
            mainDialog.Receive(backend->Poll());
        }

        void Show() {
            if (preferences && preferences->Activate())
                return;
            if (about && about->Activate())
                return;

            mainDialog.Show();
        }

        void ShowPreferences() {
            mainDialog.Hide(false);

            if (!preferences->IsOpen())
                UnregisterShortcut();

            try {
                preferences->Show(mainDialog.Handle(), settings);
            } catch (...) {
                RegisterShortcut(settings);
                throw;
            }
        }

        void ShowAbout() {
            mainDialog.Hide(false);
            about->Show(mainDialog.Handle(), settings.theme);
        }

        bool ApplySettings(const Settings& value, bool startup) {
            if (!value.key || !RegisterShortcut(value)) {
                UnregisterShortcut();
                return false;
            }

            try {
                value.Save();

                try {
                    SetStartup(startup);
                } catch (...) {
                    settings.Save();
                    throw;
                }
            } catch (...) {
                UnregisterShortcut();
                throw;
            }

            settings = value;
            mainDialog.RefreshTheme();
            mainDialog.RefreshSettings();
            about->RefreshTheme(settings.theme);
            return true;
        }

        void UnregisterShortcut() {
            if (registered) {
                UnregisterHotKey(mainDialog.Handle(), HotkeyId);
                registered = false;
            }
        }

        bool RegisterShortcut(const Settings& value) {
            if (registered)
                UnregisterHotKey(mainDialog.Handle(), HotkeyId);

            registered = RegisterHotKey(mainDialog.Handle(), HotkeyId, value.modifiers | MOD_NOREPEAT, value.key) != FALSE;

            return registered;
        }

        void Stop() {
            if (stopping)
                return;

            stopping = true;
            preferences.reset();
            about.reset();
            backend.reset();
            updater.reset();
            UnregisterShortcut();

            notifyIcon.Remove();

            if (trayMenu) {
                DestroyMenu(trayMenu);
                trayMenu = nullptr;
            }
        }

        std::optional<LRESULT> Message(UINT message, LPARAM l) {
            if (stopping)
                return {};

            if (auto command = notifyIcon.Message(message, l, trayMenu, MenuPreferences)) {
                if (*command == MenuPreferences)
                    ShowPreferences();
                else if (*command == MenuAbout)
                    ShowAbout();
                else if (*command == MenuExit)
                    mainDialog.Close();

                return 0;
            }

            switch (message) {
                case ResultsMessage: {
                    if (backend)
                        Receive();

                    return 0;
                }

                case UpdateMessage: {
                    if (about)
                        about->ReceiveUpdate();

                    return 0;
                }

                case ExitMessage: {
                    mainDialog.Close();

                    return 0;
                }

                case WM_HOTKEY: {
                    if (mainDialog.IsVisible()) {
                        mainDialog.Hide(false);

                        return 0;
                    }

                    QUERY_USER_NOTIFICATION_STATE state{};

                    if (settings.presentation && SUCCEEDED(SHQueryUserNotificationState(&state)) &&
                        state != QUNS_ACCEPTS_NOTIFICATIONS && state != QUNS_QUIET_TIME)
                        return 0;

                    Show();

                    return 0;
                }

                case WM_DESTROY: {
                    Stop();
                    PostQuitMessage(0);

                    return 0;
                }
            }

            return {};
        }

    public:
        explicit App(HINSTANCE module)
            : instance(module), settings(Settings::Load()),
            mainDialog(module, settings,
                {[this](const std::wstring& query) {
                    if (backend)
                        backend->Suggest(query);
                },
                [this](const std::wstring& id, const std::wstring& path) {
                    if (backend)
                        backend->RequestIcon(id, path);
                },
                [this](const Result& item, bool control) { Execute(item, control); },
                [this] { ShowPreferences(); },
                [this](HWND, UINT message, WPARAM, LPARAM l) { return Message(message, l); }}) {}

        ~App() {
            Stop();
            mainDialog.Destroy();
        }

        App(const App&) = delete;
        App& operator=(const App&) = delete;

        int Run() {
            if (!mainDialog.Create())
                return 1;

            Initialize();

            MSG message{};
            BOOL result;

            while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
                if (preferences && preferences->ProcessMessage(message))
                    continue;
                if (about && about->ProcessMessage(message))
                    continue;

                TranslateMessage(&message);
                DispatchMessageW(&message);
            }

            return result == -1 ? 1 : static_cast<int>(message.wParam);
        }
    };
}
