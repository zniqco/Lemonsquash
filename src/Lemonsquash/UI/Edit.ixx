module;

#include "Platform.h"
#include <commctrl.h>
#include <uxtheme.h>

export module lemonsquash.edit;

export import lemonsquash.control;

export namespace Lemonsquash {
    class Edit : public Control {
        std::optional<int> textInset;

        void PaintFrame() {
            auto dc = GetWindowDC(handle);

            if (!dc)
                return;

            RECT bounds{}, client{};

            GetWindowRect(handle, &bounds);
            GetClientRect(handle, &client);
            MapWindowPoints(handle, nullptr, reinterpret_cast<POINT*>(&client), 2);
            OffsetRect(&client, -bounds.left, -bounds.top);
            ExcludeClipRect(dc, client.left, client.top, client.right, client.bottom);

            for (LONG object : {OBJID_VSCROLL, OBJID_HSCROLL}) {
                SCROLLBARINFO scroll{sizeof(scroll)};

                if (GetScrollBarInfo(handle, object, &scroll) && !(scroll.rgstate[0] & STATE_SYSTEM_INVISIBLE)) {
                    OffsetRect(&scroll.rcScrollBar, -bounds.left, -bounds.top);
                    ExcludeClipRect(dc, scroll.rcScrollBar.left, scroll.rcScrollBar.top, scroll.rcScrollBar.right,
                        scroll.rcScrollBar.bottom);
                }
            }

            OffsetRect(&bounds, -bounds.left, -bounds.top);
            Fill(dc, bounds, theme.colors.field);

            bool active = IsWindowEnabled(handle) && (GetFocus() == handle || hovered);

            Border(dc, bounds, active ? theme.colors.accent : theme.colors.border);
            ReleaseDC(handle, dc);
        }

    protected:
        COLORREF BackgroundColor() const override {
            return theme.colors.field;
        }

        void ApplyNativeTheme() override {
            bool themedScrollbar = theme.mode != ThemeMode::HighContrast &&
                (GetWindowLongPtrW(handle, GWL_STYLE) & (WS_VSCROLL | WS_HSCROLL));

            if (themedScrollbar)
                SetWindowTheme(handle, theme.mode == ThemeMode::Dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
            else
                Control::ApplyNativeTheme();
        }

        std::optional<LRESULT> Message(UINT message, WPARAM w, LPARAM l) override {
            if (theme.mode == ThemeMode::HighContrast)
                return {};

            if (message == WM_NCCALCSIZE) {
                auto result = DefSubclassProc(handle, message, w, l);

                if (!(GetWindowLongPtrW(handle, GWL_STYLE) & ES_MULTILINE)) {
                    auto rect = w ? &reinterpret_cast<NCCALCSIZE_PARAMS*>(l)->rgrc[0] : reinterpret_cast<RECT*>(l);

                    if (auto dc = GetDC(handle)) {
                        auto font = reinterpret_cast<HFONT>(SendMessageW(handle, WM_GETFONT, 0, 0));
                        auto old = font ? SelectObject(dc, font) : nullptr;
                        TEXTMETRICW metrics{};

                        if (GetTextMetricsW(dc, &metrics) && rect->bottom - rect->top > metrics.tmHeight) {
                            rect->top += (rect->bottom - rect->top - metrics.tmHeight) / 2;
                            rect->bottom = rect->top + metrics.tmHeight;
                        }

                        if (old)
                            SelectObject(dc, old);
                        ReleaseDC(handle, dc);
                    }
                }

                return result;
            }

            if (message == WM_NCPAINT) {
                auto result = DefSubclassProc(handle, message, w, l);

                PaintFrame();

                return result;
            }

            bool hoverChanged = !(GetWindowLongPtrW(handle, GWL_STYLE) & ES_MULTILINE) && UpdateHover(message);
            auto result = DefSubclassProc(handle, message, w, l);

            if (handle) {
                if (message == WM_SETFONT || message == WM_DPICHANGED_AFTERPARENT)
                    SetWindowPos(handle, nullptr, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

                if (message == WM_SIZE || message == WM_SETFONT || message == WM_DPICHANGED_AFTERPARENT)
                    UpdateMetrics();

                if (hoverChanged || message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE)
                    PaintFrame();
            }

            return result;
        }

    public:
        using Control::ApplyTheme;

        virtual ~Edit() {
            Destroy();
        }

        void Create(HWND owner, int controlId, DWORD style = ES_AUTOHSCROLL) {
            Control::Create(owner, controlId, L"Edit", L"", style | WS_BORDER | WS_TABSTOP, WS_EX_CLIENTEDGE);
        }

        void SetTextInset(int pixels) {
            textInset = std::max(0, pixels);
            UpdateMetrics();
        }

        void UpdateMetrics() override {
            if (!handle)
                return;

            auto dpi = Dpi();
            int padding = Pixels(6, dpi);

            if (GetWindowLongPtrW(handle, GWL_STYLE) & ES_MULTILINE) {
                RECT rect{};

                GetClientRect(handle, &rect);
                InflateRect(&rect, -padding, -padding);
                SendMessageW(handle, EM_SETRECTNP, 0, reinterpret_cast<LPARAM>(&rect));
            } else {
                if (textInset) {
                    RECT bounds{};
                    POINT clientOrigin{};

                    GetWindowRect(handle, &bounds);
                    ClientToScreen(handle, &clientOrigin);
                    padding = std::max(0, *textInset - static_cast<int>(clientOrigin.x - bounds.left));
                }

                SendMessageW(handle, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(padding, padding));
            }
        }

        void ApplyTheme(const ThemeState& value) override {
            Control::ApplyTheme(value);

            if (!handle)
                return;

            SetWindowLongPtrW(handle, GWL_EXSTYLE,
                GetWindowLongPtrW(handle, GWL_EXSTYLE) & ~(WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
            SetWindowLongPtrW(handle, GWL_STYLE, GetWindowLongPtrW(handle, GWL_STYLE) | WS_BORDER);
            SetWindowPos(handle, nullptr, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            UpdateMetrics();
        }
    };
}
