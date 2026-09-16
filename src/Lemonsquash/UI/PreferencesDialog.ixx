module;

#include "Platform.h"
#include "Resource.h"
#include <commctrl.h>

export module lemonsquash.preferences_dialog;

export import lemonsquash.settings;
export import lemonsquash.dialog;
import lemonsquash.combo_box;
import lemonsquash.button;
import lemonsquash.check_box;
import lemonsquash.tab_control;
import lemonsquash.hotkey_edit;
import lemonsquash.label;

export namespace Lemonsquash {
    class PreferencesDialog : public Dialog {
        std::function<bool(const Settings&, bool)> applySettings;
        std::function<void()> closed;

        HWND generalPage = nullptr, functionsPage = nullptr, placementWindow = nullptr;
        ComboBox position, appearance;
        Button ok, cancel;
        CheckBox startup, presentation, shortcut, calculator, google, execute, googleSuggestions;
        TabControl tabs;
        HotkeyEdit hotkey;
        Label keyLabel, positionLabel, appearanceLabel;

        Settings editingSettings;

        void CreateControls() {
            SetWindowTextW(handle, L"Preferences");
            tabs.Create(handle, IDC_TABS);
            ok.Create(handle, IDOK, L"OK", BS_DEFPUSHBUTTON);
            cancel.Create(handle, IDCANCEL, L"Cancel");
            SendMessageW(handle, DM_SETDEFID, IDOK, 0);

            generalPage = CreatePage();
            functionsPage = CreatePage();

            keyLabel.Create(generalPage, IDC_KEY_LABEL, L"Toggle key:");
            hotkey.Create(generalPage, IDC_HOTKEY);
            positionLabel.Create(generalPage, IDC_POSITION_LABEL, L"Show Position:");
            position.Create(generalPage, IDC_POSITION);
            appearanceLabel.Create(generalPage, IDC_THEME_LABEL, L"Appearance:");
            appearance.Create(generalPage, IDC_THEME);
            startup.Create(generalPage, IDC_STARTUP, L"Launch application at startup:", true);
            presentation.Create(generalPage, IDC_PRESENTATION, L"Ignore when in presentation mode:", true);

            shortcut.Create(functionsPage, IDC_FUNCTION_SHORTCUT, L"Shortcut");
            calculator.Create(functionsPage, IDC_FUNCTION_CALCULATOR, L"Calculator");
            google.Create(functionsPage, IDC_FUNCTION_GOOGLE, L"Google");
            execute.Create(functionsPage, IDC_FUNCTION_EXECUTE, L"Execute");
            googleSuggestions.Create(functionsPage, IDC_GOOGLE_SUGGESTIONS, L"Show Google suggestions");
        }

        void LayoutSettingsRows(HWND page) {
            auto px = [dpi = GetDpiForWindow(page)](int value) {
                return Pixels(value, dpi);
            };

            RECT client{}, positionBounds{}, appearanceBounds{};

            GetClientRect(page, &client);
            GetWindowRect(position.Handle(), &positionBounds);
            GetWindowRect(appearance.Handle(), &appearanceBounds);

            int rowHeight = std::max<LONG>({px(20), positionBounds.bottom - positionBounds.top,
                appearanceBounds.bottom - appearanceBounds.top});
            int left = px(11), fieldLeft = px(213), right = client.right - px(13);
            constexpr int rowGap = 6;
            auto rowY = [&](int index) {
                return px(13) + index * (rowHeight + px(rowGap));
            };

            keyLabel.SetBounds(left, rowY(0), px(200), rowHeight);
            hotkey.SetBounds(fieldLeft, rowY(0), right - fieldLeft, rowHeight);
            positionLabel.SetBounds(left, rowY(1), px(200), rowHeight);
            position.SetBounds(fieldLeft, rowY(1) + (rowHeight - (positionBounds.bottom - positionBounds.top)) / 2,
                right - fieldLeft, px(160));
            appearanceLabel.SetBounds(left, rowY(2), px(200), rowHeight);
            appearance.SetBounds(fieldLeft, rowY(2) + (rowHeight - (appearanceBounds.bottom - appearanceBounds.top)) / 2,
                right - fieldLeft, px(160));
            hotkey.SetTextInset(position.TextInset() - px(1));
            startup.SetBounds(left, rowY(3), right - left, rowHeight);
            presentation.SetBounds(left, rowY(4), right - left, rowHeight);
        }

        void LayoutFunctionRows() {
            if (!functionsPage)
                return;

            auto px = [dpi = GetDpiForWindow(functionsPage)](int value) {
                return Pixels(value, dpi);
            };

            RECT client{};
            GetClientRect(functionsPage, &client);

            int left = px(11), right = client.right - px(13), y = px(13);
            int rowHeight = px(24), rowGap = px(6);

            auto row = [&](CheckBox& box, int indent) {
                box.SetBounds(left + indent, y, right - left - indent, rowHeight);
                y += rowHeight + rowGap;
            };

            row(shortcut, 0);
            row(calculator, 0);
            row(google, 0);
            row(googleSuggestions, px(26));
            row(execute, 0);
        }

        void Layout() {
            auto dialogDpi = GetDpiForWindow(handle);
            auto px = [&](int value) {
                return Pixels(value, dialogDpi);
            };

            constexpr int dialogHeight = 350;
            RECT frame{0, 0, px(524), px(dialogHeight)};

            AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(GetWindowLongPtrW(handle, GWL_STYLE)), FALSE,
                static_cast<DWORD>(GetWindowLongPtrW(handle, GWL_EXSTYLE)), dialogDpi);
            SetWindowPos(handle, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top,
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            tabs.SetBounds(px(9), px(9), px(506), px(dialogHeight - 57));
            ok.SetBounds(px(331), px(dialogHeight - 40), px(88), px(28));
            cancel.SetBounds(px(425), px(dialogHeight - 40), px(88), px(28));

            auto page = tabs.PageBounds(handle);

            for (auto child : {generalPage, functionsPage})
                SetWindowPos(child, HWND_TOP, page.left, page.top, page.right - page.left, page.bottom - page.top,
                    SWP_NOACTIVATE);

            LayoutSettingsRows(generalPage);
            LayoutFunctionRows();
            InvalidateRect(handle, nullptr, TRUE);
        }

        void Theme() {
            if (!handle || !generalPage)
                return;

            auto selected = appearance.SelectedIndex();

            if (selected >= 0 && selected <= 2)
                editingSettings.theme = static_cast<ThemePreference>(selected);

            ApplyTheme(editingSettings.theme);
        }

        void SelectTab(int page) {
            page = page == 1 ? 1 : 0;

            auto tab = tabs.Handle();
            auto focus = GetFocus();

            if ((generalPage && IsChild(generalPage, focus)) ||
                (functionsPage && IsChild(functionsPage, focus)))
                SetFocus(tab);

            tabs.Select(page);
            ShowWindow(generalPage, page == 0 ? SW_SHOW : SW_HIDE);
            ShowWindow(functionsPage, page == 1 ? SW_SHOW : SW_HIDE);
        }

        void Save() {
            auto value = editingSettings;

            value.key = hotkey.Key();
            value.modifiers = hotkey.Modifiers();
            value.presentation = presentation.Checked();
            value.position = position.SelectedIndex();
            value.shortcutEnabled = shortcut.Checked();
            value.calculatorEnabled = calculator.Checked();
            value.googleEnabled = google.Checked();
            value.executeEnabled = execute.Checked();
            value.googleSuggestions = googleSuggestions.Checked();

            auto selectedTheme = appearance.SelectedIndex();

            if (selectedTheme >= 0 && selectedTheme <= 2)
                value.theme = static_cast<ThemePreference>(selectedTheme);

            if (!applySettings(value, startup.Checked())) {
                MessageBoxW(handle,
                    L"Choose a shortcut that is not already used by Windows or "
                    L"another application.",
                    L"Shortcut unavailable", MB_OK | MB_ICONINFORMATION);

                return;
            }

            Close();
        }

    protected:
        bool OnInitialize() override {
            CreateControls();

            tabs.AddItem(L"General");
            tabs.AddItem(L"Functions");
            hotkey.SetHotkey(editingSettings.key, editingSettings.modifiers);

            for (auto label : {L"Foreground Window", L"Cursor Position", L"Primary Screen"})
                position.AddItem(label);

            for (auto label : {L"System Default", L"Light", L"Dark"})
                appearance.AddItem(label);

            position.Select(editingSettings.position);
            appearance.Select(static_cast<int>(editingSettings.theme));
            presentation.Check(editingSettings.presentation);
            shortcut.Check(editingSettings.shortcutEnabled);
            calculator.Check(editingSettings.calculatorEnabled);
            google.Check(editingSettings.googleEnabled);
            execute.Check(editingSettings.executeEnabled);
            googleSuggestions.Check(editingSettings.googleSuggestions);
            googleSuggestions.Enable(editingSettings.googleEnabled);
            startup.Check(StartupEnabled());

            UpdateMetrics();
            Layout();
            SelectTab(0);
            CenterOn(placementWindow);

            SetFocus(GetNextDlgTabItem(handle, nullptr, FALSE));

            return false;
        }

        void OnDpiChanged() override {
            Layout();
        }

        void OnThemeChanged(ThemeMode previousMode) override {
            if (!generalPage)
                return;

            if (previousMode != State().mode)
                Layout();
            else {
                LayoutSettingsRows(generalPage);
                LayoutFunctionRows();
            }
        }

        void OnDestroyed() noexcept override {
            generalPage = functionsPage = nullptr;
        }

        std::optional<INT_PTR> Message(UINT message, WPARAM w, LPARAM) override {
            if (message == WM_COMMAND && LOWORD(w) == IDOK) {
                Save();

                return TRUE;
            }

            if (message == WM_SYSCOMMAND && (w & 0xfff0) == SC_KEYMENU)
                return TRUE;

            return {};
        }

        void OnClosed() override {
            if (closed)
                closed();
        }

    public:
        PreferencesDialog(HINSTANCE module, std::function<bool(const Settings&, bool)> apply,
            std::function<void()> onClosed)
            : Dialog(module), applySettings(std::move(apply)), closed(std::move(onClosed)),
            appearance([this] { Theme(); }),
            ok([this] { Save(); }), cancel([this] { Close(); }),
            google([this] { googleSuggestions.Enable(google.Checked()); }),
            tabs([this](int page) { SelectTab(page); }) {}

        ~PreferencesDialog() {
            Destroy();
        }

        void Show(HWND placement, const Settings& settings) {
            if (handle) {
                Activate();

                return;
            }

            placementWindow = placement;
            editingSettings = settings;
            Create(nullptr, editingSettings.theme);

            ShowWindow(handle, SW_SHOW);
            Activate();
        }

        bool ProcessMessage(MSG& message) override {
            if (!handle)
                return false;

            if ((message.hwnd == handle || IsChild(handle, message.hwnd)) && message.message == WM_KEYDOWN &&
                message.wParam == VK_TAB && (GetKeyState(VK_CONTROL) & 0x8000)) {
                SelectTab(1 - tabs.SelectedIndex());
                return true;
            }

            return Dialog::ProcessMessage(message);
        }
    };
}
