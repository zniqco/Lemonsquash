module;

#include "Platform.h"
#include <commctrl.h>

export module lemonsquash.button;

export import lemonsquash.control;
import lemonsquash.common;

export namespace Lemonsquash {
    class Button : public Control {
        std::function<void()> clicked;

        void Paint(HDC dc) override {
            const auto& colors = theme.colors;
            RECT rect{};

            GetClientRect(handle, &rect);

            auto px = [&](int value) { return Pixels(value, Dpi()); };
            bool enabled = IsWindowEnabled(handle) != FALSE, focused = GetFocus() == handle;
            POINT cursor{};

            GetCursorPos(&cursor);
            ScreenToClient(handle, &cursor);

            bool hot = enabled && PtInRect(&rect, cursor);

            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, enabled ? colors.text : colors.disabledText);

            auto font = reinterpret_cast<HFONT>(SendMessageW(handle, WM_GETFONT, 0, 0));

            if (font)
                SelectObject(dc, font);

            UINT focusState = static_cast<UINT>(SendMessageW(handle, WM_QUERYUISTATE, 0, 0));
            auto style = GetWindowLongPtrW(handle, GWL_STYLE);
            bool checkbox = (style & BS_TYPEMASK) == BS_AUTOCHECKBOX || (style & BS_TYPEMASK) == BS_CHECKBOX;
            bool pressed = (SendMessageW(handle, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0;

            Fill(dc, rect, checkbox ? colors.surface : pressed ? colors.background : hot ? colors.hover : colors.field);

            RECT label = rect;

            if (checkbox) {
                int side = px(14), top = (rect.bottom - side) / 2;
                RECT box{rect.right - side - px(1), top, rect.right - px(1), top + side};

                if (!(style & BS_RIGHTBUTTON)) {
                    box.left = px(1);
                    box.right = box.left + side;
                    label.left = box.right + px(6);
                } else {
                    label.right = box.left - px(6);
                }

                bool checked = SendMessageW(handle, BM_GETCHECK, 0, 0) == BST_CHECKED;

                Fill(dc, box, checked && enabled ? colors.accent : colors.field);
                Border(dc, box, enabled && (hot || focused) ? colors.accent : colors.border);

                if (checked)
                    DrawSymbol(dc, box, L'\uE73E', enabled ? colors.accentText : colors.disabledText);
            } else {
                Border(dc, rect, enabled && (focused || (style & BS_TYPEMASK) == BS_DEFPUSHBUTTON)
                    ? colors.accent : colors.border);
                InflateRect(&label, -px(6), 0);
            }

            auto caption = WindowText(handle);

            DrawTextW(dc, caption.c_str(), -1, &label,
                DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | (checkbox ? DT_LEFT : DT_CENTER) |
                (focusState & UISF_HIDEACCEL ? DT_HIDEPREFIX : 0));

            if (focused && !(focusState & UISF_HIDEFOCUS)) {
                if (checkbox) {
                    FocusLabel(dc, caption, label, focusState & UISF_HIDEACCEL ? DT_HIDEPREFIX : 0, px(1));
                } else {
                    RECT focus = rect;
                    InflateRect(&focus, -px(3), -px(3));
                    FocusRect(dc, focus);
                }
            }
        }

    protected:
        std::optional<LRESULT> Message(UINT message, WPARAM w, LPARAM l) override {
            if (theme.mode == ThemeMode::HighContrast)
                return {};

            if (auto result = PaintMessage(message, w))
                return result;

            bool hoverChanged = UpdateHover(message);
            auto result = DefSubclassProc(handle, message, w, l);
            if (handle && (hoverChanged || message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE ||
                message == BM_SETCHECK || message == BM_SETSTATE || message == BM_SETSTYLE ||
                message == WM_SETTEXT || message == WM_SETFONT || message == WM_UPDATEUISTATE ||
                message == WM_DPICHANGED_AFTERPARENT))
            {
                InvalidateRect(handle, nullptr, FALSE);
            }

            return result;
        }

        std::optional<LRESULT> ParentMessage(UINT message, WPARAM w, LPARAM) override {
            if (message == WM_COMMAND && HIWORD(w) == BN_CLICKED && clicked) {
                clicked();
                return TRUE;
            }

            return {};
        }

    public:
        explicit Button(std::function<void()> onClicked = {})
            : clicked(std::move(onClicked)) {}

        virtual ~Button() {
            Destroy();
        }

        void Create(HWND owner, int controlId, const wchar_t* text, DWORD style = BS_PUSHBUTTON) {
            Control::Create(owner, controlId, L"Button", text, style | WS_TABSTOP);
        }
    };
}
