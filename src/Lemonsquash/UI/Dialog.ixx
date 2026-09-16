module;

#include "Platform.h"
#include <exception>
#include <windowsx.h>

export module lemonsquash.dialog;

export import lemonsquash.window;
export import lemonsquash.theme;
import lemonsquash.control;
import lemonsquash.common;
import lemonsquash.monitor;

export namespace Lemonsquash {
    class Dialog : public Window {
        ThemePreference preference = ThemePreference::System;
        ThemeState theme;
        LazyResource<HBRUSH> backgroundBrush, surfaceBrush;
        LazyResource<HFONT> textFont;
        std::exception_ptr creationError;
        bool applyingTheme = false;

        static LazyResource<HBRUSH> Brush(COLORREF color) {
            return {[color] { return CreateSolidBrush(color); }, [](HBRUSH brush) noexcept { DeleteObject(brush); }};
        }

        template <typename Callback>
        void ForEachChild(Callback callback) {
            if (!handle)
                return;

            struct Enumeration {
                Dialog& owner;
                HWND window;
                Callback& callback;
                std::exception_ptr error;
            } enumeration{*this, handle, callback, {}};

            EnumChildWindows(handle, [](HWND child, LPARAM value) -> BOOL {
                auto& enumeration = *reinterpret_cast<Enumeration*>(value);

                if (enumeration.owner.handle != enumeration.window)
                    return FALSE;

                try {
                    enumeration.callback(child);
                    return TRUE;
                } catch (...) {
                    enumeration.error = std::current_exception();
                    return FALSE;
                }
            }, reinterpret_cast<LPARAM>(&enumeration));

            if (enumeration.error)
                std::rethrow_exception(enumeration.error);
        }

        HWND CreateNativeDialog(HWND owner, bool page) {
            struct alignas(DWORD) EmptyDialogTemplate {
                DLGTEMPLATE dialog{};
                WORD menu = 0, windowClass = 0, title = 0;
            } definition;

            definition.dialog.style = DS_3DLOOK | WS_CLIPCHILDREN |
                (page ? DS_CONTROL | WS_CHILD | WS_CLIPSIBLINGS
                : DS_MODALFRAME | WS_POPUP | WS_CAPTION | WS_SYSMENU);
            definition.dialog.dwExtendedStyle = page ? WS_EX_CONTROLPARENT : 0;

            return CreateDialogIndirectParamW(instance, &definition.dialog, owner, page ? PageProc : DialogProc,
                reinterpret_cast<LPARAM>(this));
        }

        std::optional<INT_PTR> BackgroundMessage(HWND dialog, bool page, UINT message, WPARAM w) {
            if (message != WM_ERASEBKGND && message != WM_CTLCOLORDLG)
                return {};

            auto brush = (page ? surfaceBrush : backgroundBrush).Get();

            if (!brush)
                return {};

            if (message == WM_CTLCOLORDLG)
                return reinterpret_cast<INT_PTR>(brush);

            RECT rect{};

            GetClientRect(dialog, &rect);
            FillRect(reinterpret_cast<HDC>(w), &rect, brush);

            return TRUE;
        }

        static INT_PTR CALLBACK DialogProc(HWND dialog, UINT message, WPARAM w, LPARAM l) {
            auto self = reinterpret_cast<Dialog*>(GetWindowLongPtrW(dialog, DWLP_USER));

            if (message == WM_INITDIALOG) {
                self = reinterpret_cast<Dialog*>(l);
                SetWindowLongPtrW(dialog, DWLP_USER, l);
                self->handle = dialog;
            }

            if (!self)
                return FALSE;

            if (message == WM_NCDESTROY) {
                SetWindowLongPtrW(dialog, DWLP_USER, 0);
                self->handle = nullptr;
                self->textFont.Reset();
                self->backgroundBrush.Reset();
                self->surfaceBrush.Reset();
                self->OnDestroyed();

                return FALSE;
            }

            try {
                if (message == WM_INITDIALOG) {
                    SetDialogDpiChangeBehavior(dialog, DDC_DISABLE_ALL, DDC_DISABLE_ALL);
                    return self->OnInitialize();
                }

                if (auto result = Control::Reflect(dialog, message, w, l))
                    return SetDlgMsgResult(dialog, message, *result);

                if (auto result = self->BackgroundMessage(dialog, false, message, w))
                    return SetDlgMsgResult(dialog, message, *result);

                if (auto result = self->Message(message, w, l))
                    return SetDlgMsgResult(dialog, message, *result);

                switch (message) {
                    case WM_COMMAND: {
                        if (LOWORD(w) != IDOK && LOWORD(w) != IDCANCEL)
                            break;

                        self->Close();

                        return TRUE;
                    }

                    case WM_CLOSE: {
                        self->Close();

                        return TRUE;
                    }

                    case WM_DPICHANGED: {
                        self->SetBounds(*reinterpret_cast<RECT*>(l));
                        self->UpdateMetrics();
                        self->OnDpiChanged();

                        return TRUE;
                    }

                    case WM_SETTINGCHANGE: {
                        if (IsThemeSettingChange(w, l))
                            self->ApplyTheme(self->preference);

                        break;
                    }

                    case WM_THEMECHANGED:
                    case WM_SYSCOLORCHANGE: {
                        self->ApplyTheme(self->preference);

                        break;
                    }
                }
            } catch (...) {
                if (message == WM_INITDIALOG) {
                    self->creationError = std::current_exception();
                    DestroyWindow(dialog);
                } else {
                    ShowException(dialog, L"Lemonsquash");
                }
            }

            return FALSE;
        }

        static INT_PTR CALLBACK PageProc(HWND dialog, UINT message, WPARAM w, LPARAM l) {
            auto self = reinterpret_cast<Dialog*>(GetWindowLongPtrW(dialog, DWLP_USER));

            if (message == WM_INITDIALOG) {
                SetWindowLongPtrW(dialog, DWLP_USER, l);
                SetDialogDpiChangeBehavior(dialog, DDC_DISABLE_ALL, DDC_DISABLE_ALL);
                return TRUE;
            }

            if (message == WM_NCDESTROY) {
                SetWindowLongPtrW(dialog, DWLP_USER, 0);
                return FALSE;
            }

            if (!self)
                return FALSE;

            try {
                if (auto result = Control::Reflect(dialog, message, w, l))
                    return SetDlgMsgResult(dialog, message, *result);

                if (auto result = self->BackgroundMessage(dialog, true, message, w))
                    return SetDlgMsgResult(dialog, message, *result);

                if (message == WM_COMMAND) {
                    auto result = SendMessageW(GetParent(dialog), message, w, l);

                    return SetDlgMsgResult(dialog, message, result);
                }
            } catch (...) {
                Log(L"Could not handle dialog page message");
            }

            return FALSE;
        }

    protected:
        virtual bool OnInitialize() { return true; }
        virtual void OnDpiChanged() {}
        virtual void OnThemeChanged(ThemeMode) {}
        virtual void OnDestroyed() noexcept {}

        virtual std::optional<INT_PTR> Message(UINT, WPARAM, LPARAM) { return {}; }

        void CenterOn(HWND window) {
            RECT rect{};
            GetWindowRect(handle, &rect);

            auto workRect = Monitor::FromWindow(window).WorkArea();
            int x = workRect.left + (workRect.right - workRect.left - (rect.right - rect.left)) / 2;
            int y = workRect.top + (workRect.bottom - workRect.top - (rect.bottom - rect.top)) / 2;

            SetWindowPos(handle, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }

        HWND CreatePage() {
            if (!handle)
                throw std::logic_error("A dialog must exist before creating its pages");

            auto page = CreateNativeDialog(handle, true);

            if (!page)
                throw std::runtime_error("Cannot create dialog page");

            if (auto font = textFont.Get())
                SendMessageW(page, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);

            return page;
        }

    public:
        explicit Dialog(HINSTANCE module) : Window(module) {}

        virtual ~Dialog() {
            Destroy();
        }

        void Create(HWND owner = nullptr, ThemePreference value = ThemePreference::System) {
            if (handle)
                throw std::logic_error("Dialog is already created");

            preference = value;
            creationError = nullptr;

            auto created = CreateNativeDialog(owner, false);

            if (creationError)
                std::rethrow_exception(std::exchange(creationError, nullptr));

            if (!created || handle != created || !IsWindow(created))
                throw std::runtime_error("Cannot create dialog");

            try {
                ApplyTheme(preference);
            } catch (...) {
                Destroy();
                throw;
            }
        }

        virtual bool ProcessMessage(MSG& message) {
            return handle && IsDialogMessageW(handle, &message) != FALSE;
        }

        const ThemeState& State() const {
            return theme;
        }

        void UpdateMetrics() {
            if (!handle)
                return;

            auto previousFont = textFont;
            auto font = Control::TextFont(GetDpiForWindow(handle));

            if (font.Get())
                textFont = std::move(font);

            if (auto fontHandle = textFont.Get())
                SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(fontHandle), FALSE);

            ForEachChild([&](HWND child) {
                if (!Control::FromHandle(child)) {
                    if (auto fontHandle = textFont.Get())
                        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(fontHandle), TRUE);
                }
            });

            ForEachChild([](HWND child) {
                if (auto control = Control::FromHandle(child))
                    control->UpdateFont();
            });

            ForEachChild([](HWND child) {
                if (auto control = Control::FromHandle(child))
                    control->UpdateMetrics();
            });
        }

        void ApplyTheme(ThemePreference value) {
            if (applyingTheme)
                return;

            preference = value;

            if (!handle)
                return;

            struct Application {
                Dialog& dialog;

                ~Application() { dialog.applyingTheme = false; }
            } application{*this};

            applyingTheme = true;

            auto previousMode = theme.mode;
            auto next = ResolveTheme(preference);

            if (!backgroundBrush || theme.colors.background != next.colors.background)
                backgroundBrush = Brush(next.colors.background);

            if (!surfaceBrush || theme.colors.surface != next.colors.surface)
                surfaceBrush = Brush(next.colors.surface);

            theme = next;

            SetDarkTitleBar(theme.mode == ThemeMode::Dark);

            ForEachChild([&](HWND child) {
                if (auto control = Control::FromHandle(child))
                    control->ApplyTheme(theme, GetParent(child) != handle);
            });

            UpdateMetrics();

            if (!handle)
                return;

            OnThemeChanged(previousMode);

            if (handle)
                RedrawWindow(handle, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
        }
    };
}
