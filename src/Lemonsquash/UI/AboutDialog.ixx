module;

#include "Platform.h"
#include "Resource.h"

export module lemonsquash.about_dialog;

export import lemonsquash.dialog;
import lemonsquash.updater;
import lemonsquash.common;
import lemonsquash.link_image;
import lemonsquash.link_label;
import lemonsquash.button;
import lemonsquash.label;

export namespace Lemonsquash {
    class AboutDialog : public Dialog {
        Updater& updater;
        HWND placementWindow = nullptr;
        std::wstring reportedUpdateError;
        bool reportUpdateErrors = false;

        LinkImage logo;
        LinkLabel attribution;
        Label version;
        Button updateButton;

        void FocusDefault() {
            SetFocus(IsWindowEnabled(updateButton.Handle()) ? updateButton.Handle() : handle);
        }

        void Layout() {
            auto dpi = GetDpiForWindow(handle);
            auto px = [&](int value) { return Pixels(value, dpi); };

            RECT frame{0, 0, px(360), px(260)};

            AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(GetWindowLongPtrW(handle, GWL_STYLE)), FALSE,
                static_cast<DWORD>(GetWindowLongPtrW(handle, GWL_EXSTYLE)), dpi);
            SetWindowPos(handle, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top,
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

            RECT client{};
            GetClientRect(handle, &client);

            constexpr int contentHeight = 214;
            int top = (client.bottom - px(contentHeight)) / 2;
            auto centerX = [&](int width) { return (client.right - px(width)) / 2; };

            logo.SetBounds(centerX(240), top, px(240), px(125));
            attribution.SetBounds(centerX(300), top + px(130), px(300), px(16));
            updateButton.SetBounds(centerX(216), top + px(168), px(216), px(27));
            version.SetBounds(centerX(200), top + px(202), px(200), px(16));

            InvalidateRect(handle, nullptr, TRUE);
        }

        void RefreshUpdate() {
            if (!handle)
                return;

            auto state = updater.State();
            std::wstring text = L"Check for updates";

            switch (state) {
                case UpdateState::Idle: {
                    break;
                }

                case UpdateState::Checking: {
                    text = L"Checking for updates...";
                    break;
                }

                case UpdateState::Available: {
                    text = L"Update to v" + updater.AvailableDisplayVersion();
                    break;
                }

                case UpdateState::Downloading: {
                    text = L"Downloading update...";
                    break;
                }

                case UpdateState::Installing: {
                    text = L"Preparing update...";
                    break;
                }

                case UpdateState::Restarting: {
                    text = L"Restarting...";
                    break;
                }

                case UpdateState::UpToDate: {
                    text = L"You're using the latest version!";
                    break;
                }

                case UpdateState::Failed: {
                    text = L"Update failed - Retry";
                    break;
                }
            }

            bool busy = Updater::Busy(state);

            updateButton.Enable(!busy);
            updateButton.SetText(text.c_str());

            if (!busy && !GetFocus() && GetActiveWindow() == handle)
                SetFocus(updateButton.Handle());
        }

        void UpdateClicked() {
            auto state = updater.State();

            if (Updater::Busy(state))
                return;

            reportedUpdateError.clear();
            reportUpdateErrors = true;

            if (state == UpdateState::Available)
                updater.Install(placementWindow);
            else
                updater.Check();

            RefreshUpdate();
        }

    protected:
        bool OnInitialize() override {
            SetWindowTextW(handle, L"About");
            logo.Create(handle, IDC_LOGO, L"Lemonsquash website", L"Lemonsquash");
            version.Create(handle, IDC_VERSION, L"", SS_CENTER);
            updateButton.Create(handle, IDC_UPDATE, L"Check for updates");
            attribution.Create(handle, IDC_ATTRIBUTION,
                L"by zniq / <a href=\"https://github.com/zniqco/Lemonsquash\">GitHub</a>", true);
            version.SetText((L"v" + std::wstring(DisplayVersion)).c_str());

            try {
                logo.SetImageFromResource(IDR_LOGO);
            } catch (...) {
                Log(L"Could not load About logo");
            }

            RefreshUpdate();
            UpdateMetrics();
            Layout();
            CenterOn(placementWindow);

            return true;
        }

        void OnDpiChanged() override {
            Layout();
        }

        void OnThemeChanged(ThemeMode previousMode) override {
            if (previousMode != State().mode)
                Layout();
        }

    public:
        AboutDialog(HINSTANCE module, Updater& updateService)
            : Dialog(module), updater(updateService),
            logo([this] { OpenTarget(handle, ServiceUrl); }),
            attribution([this](const std::wstring& target) { OpenTarget(handle, target); }),
            updateButton([this] { UpdateClicked(); }) {}

        ~AboutDialog() {
            Destroy();
        }

        void Show(HWND placement, ThemePreference selectedTheme) {
            if (handle) {
                Activate();
                FocusDefault();

                return;
            }

            placementWindow = placement;
            reportedUpdateError.clear();
            reportUpdateErrors = false;

            Create(nullptr, selectedTheme);

            if (updater.Check(true))
                RefreshUpdate();

            ShowWindow(handle, SW_SHOW);
            Activate();
            FocusDefault();
        }

        void RefreshTheme(ThemePreference selectedTheme) {
            ApplyTheme(selectedTheme);
        }

        bool ProcessMessage(MSG& message) override {
            if (handle && message.hwnd == handle && message.message == WM_SYSKEYDOWN &&
                message.wParam == VK_ESCAPE && !(message.lParam & (1L << 29))) {
                Close();
                return true;
            }

            return Dialog::ProcessMessage(message);
        }

        void ReceiveUpdate() {
            RefreshUpdate();

            if (handle && reportUpdateErrors && updater.State() == UpdateState::Failed) {
                auto error = updater.Error();

                if (!error.empty() && error != reportedUpdateError) {
                    reportedUpdateError = error;
                    MessageBoxW(handle, error.c_str(), L"Lemonsquash update failed", MB_OK | MB_ICONERROR);
                }
            }
        }
    };
}
