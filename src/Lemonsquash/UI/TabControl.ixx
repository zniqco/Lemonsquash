module;

#include "Platform.h"
#include <commctrl.h>
#include <windowsx.h>

export module lemonsquash.tab_control;

export import lemonsquash.control;

namespace Lemonsquash {
    namespace {
        void Line(HDC dc, COLORREF color, const POINT* points, int count) {
            SelectObject(dc, GetStockObject(DC_PEN));
            SetDCPenColor(dc, color);
            Polyline(dc, points, count);
        }
    }
}

export namespace Lemonsquash {
    class TabControl : public Control {
        int hoveredTab = 0;
        std::function<void(int)> selectionChanged;

        void Paint(HDC dc) override {
            auto control = handle;
            const auto& colors = theme.colors;
            RECT rect{};

            GetClientRect(control, &rect);

            auto px = [&](int value) {
                return MulDiv(value, static_cast<int>(GetDpiForWindow(control)), 96);
            };

            bool enabled = IsWindowEnabled(control) != FALSE, focused = GetFocus() == control;
            POINT cursor{};

            GetCursorPos(&cursor);
            ScreenToClient(control, &cursor);

            bool hot = enabled && PtInRect(&rect, cursor);

            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, enabled ? colors.text : colors.disabledText);

            auto font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));

            if (font)
                SelectObject(dc, font);

            UINT focusState = static_cast<UINT>(SendMessageW(control, WM_QUERYUISTATE, 0, 0));

            Fill(dc, rect, colors.background);

            RECT body = rect;

            TabCtrl_AdjustRect(control, FALSE, &body);
            InflateRect(&body, px(2), px(2));
            Fill(dc, body, colors.surface);
            Border(dc, body, colors.border);

            int selected = TabCtrl_GetCurSel(control);

            auto paintTab = [&](int index) {
                RECT tab{};

                TabCtrl_GetItemRect(control, index, &tab);

                RECT labelRect = tab;
                bool active = selected == index;

                if (active) {
                    InflateRect(&tab, px(2), px(2));
                    tab.left = std::max(tab.left, body.left);
                    tab.right = std::min(tab.right, body.right);
                }

                tab.bottom = body.top + 1;
                Fill(dc, tab, active ? colors.surface : (PtInRect(&tab, cursor) && hot) ? colors.hover
                    : colors.background);

                if (active) {
                    POINT outline[]{{tab.left, tab.bottom - 1}, {tab.left, tab.top},
                        {tab.right - 1, tab.top}, {tab.right - 1, tab.bottom - 1}};

                    Line(dc, colors.border, outline, 4);
                } else {
                    Border(dc, tab, colors.border);
                }

                wchar_t label[128]{};
                TCITEMW item{};

                item.mask = TCIF_TEXT;
                item.pszText = label;
                item.cchTextMax = 128;

                TabCtrl_GetItem(control, index, &item);

                UINT flags = DT_CENTER | (focusState & UISF_HIDEACCEL ? DT_HIDEPREFIX : 0);

                DrawTextW(dc, label, -1, &labelRect, DT_SINGLELINE | DT_VCENTER | flags);

                if (focused && active && !(focusState & UISF_HIDEFOCUS))
                    FocusLabel(dc, label, labelRect, flags, px(1));
            };

            int count = TabCtrl_GetItemCount(control);

            for (int index = 0; index < count; ++index)
                if (index != selected)
                    paintTab(index);

            if (selected >= 0 && selected < count)
                paintTab(selected);
        }

    protected:
        std::optional<LRESULT> Message(UINT message, WPARAM w, LPARAM l) override {
            if (theme.mode == ThemeMode::HighContrast)
                return {};

            if (auto result = PaintMessage(message, w))
                return result;

            bool hoverChanged = false;

            if (message == WM_MOUSEMOVE) {
                TCHITTESTINFO hit{{GET_X_LPARAM(l), GET_Y_LPARAM(l)}, 0};
                int hot = TabCtrl_HitTest(handle, &hit) + 2;

                if (hoveredTab != hot) {
                    hoveredTab = hot;
                    hoverChanged = true;

                    TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, handle, 0};

                    TrackMouseEvent(&tracking);
                }
            } else if (message == WM_MOUSELEAVE) {
                hoverChanged = hoveredTab != 0;
                hoveredTab = 0;
            }

            auto result = DefSubclassProc(handle, message, w, l);

            if (handle && (hoverChanged || message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE ||
                message == TCM_SETCURSEL || message == WM_UPDATEUISTATE || message == WM_LBUTTONDOWN ||
                message == WM_KEYDOWN || message == WM_SETFONT || message == WM_DPICHANGED_AFTERPARENT))
                InvalidateRect(handle, nullptr, FALSE);

            return result;
        }

        std::optional<LRESULT> ParentMessage(UINT message, WPARAM, LPARAM l) override {
            if (message == WM_NOTIFY && reinterpret_cast<NMHDR*>(l)->code == TCN_SELCHANGE && selectionChanged) {
                selectionChanged(SelectedIndex());
                return 0;
            }

            return {};
        }

        void OnDestroyed() noexcept override {
            hoveredTab = 0;
        }

    public:
        explicit TabControl(std::function<void(int)> onSelectionChanged = {})
            : selectionChanged(std::move(onSelectionChanged)) {}

        ~TabControl() {
            Destroy();
        }

        void Create(HWND owner, int controlId) {
            Control::Create(owner, controlId, WC_TABCONTROLW, L"", WS_TABSTOP | WS_CLIPSIBLINGS);
        }

        void AddItem(const wchar_t* text) {
            TCITEMW item{};

            item.mask = TCIF_TEXT;
            item.pszText = const_cast<wchar_t*>(text);

            if (TabCtrl_InsertItem(handle, TabCtrl_GetItemCount(handle), &item) == -1)
                throw std::runtime_error("Cannot add tab item");
        }

        int SelectedIndex() const {
            return handle ? TabCtrl_GetCurSel(handle) : -1;
        }

        void Select(int index) {
            TabCtrl_SetCurSel(handle, index);
        }

        RECT PageBounds(HWND relativeTo) const {
            RECT rect{};

            GetClientRect(handle, &rect);
            TabCtrl_AdjustRect(handle, FALSE, &rect);
            MapWindowPoints(handle, relativeTo, reinterpret_cast<POINT*>(&rect), 2);

            return rect;
        }

    };
}
