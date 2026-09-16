module;

#include "Platform.h"
#include <commctrl.h>

export module lemonsquash.combo_box;

export import lemonsquash.control;

export namespace Lemonsquash {
    class ComboBox : public Control {
        static constexpr int ItemHeight = 18;
        std::function<void()> selectionChanged;

        void PaintItem(const DRAWITEMSTRUCT& item) const {
            const auto& colors = theme.colors;
            int saved = SaveDC(item.hDC);
            bool selectionField = (item.itemState & ODS_COMBOBOXEDIT) != 0;
            bool selected = (item.itemState & ODS_SELECTED) && !selectionField;
            RECT rect = item.rcItem;

            if (selectionField) {
                COMBOBOXINFO info{sizeof(info)};

                if (GetComboBoxInfo(item.hwndItem, &info))
                    rect = info.rcItem;
            }

            Fill(item.hDC, rect, selected ? colors.selection : colors.field);
            SetTextColor(item.hDC, item.itemState & ODS_DISABLED ? colors.disabledText
                : selected ? colors.selectionText
                : colors.text);
            SetBkMode(item.hDC, TRANSPARENT);

            auto font = reinterpret_cast<HFONT>(SendMessageW(item.hwndItem, WM_GETFONT, 0, 0));

            if (font)
                SelectObject(item.hDC, font);

            RECT label = rect;

            InflateRect(&label, -Pixels(TextPadding, GetDpiForWindow(item.hwndItem)), 0);

            try {
                auto caption = ItemText(static_cast<int>(item.itemID));

                DrawTextW(item.hDC, caption.c_str(), -1, &label, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            } catch (...) {
            }

            if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT)) {
                InflateRect(&rect, -1, -1);
                FocusRect(item.hDC, rect);
            }

            RestoreDC(item.hDC, saved);
        }

        void Paint(HDC dc) override {
            COMBOBOXINFO info{sizeof(info)};

            if (!GetComboBoxInfo(handle, &info))
                return;

            const auto& colors = theme.colors;
            RECT rect{};

            GetClientRect(handle, &rect);

            bool enabled = IsWindowEnabled(handle) != FALSE, focused = GetFocus() == handle;
            POINT cursor{};

            GetCursorPos(&cursor);
            ScreenToClient(handle, &cursor);

            bool hot = enabled && PtInRect(&rect, cursor);

            Fill(dc, rect, colors.field);
            Border(dc, rect, enabled && (focused || hot) ? colors.accent : colors.border);

            DRAWITEMSTRUCT item{};
            item.CtlType = ODT_COMBOBOX;
            item.itemID = static_cast<UINT>(SelectedIndex());
            item.itemState = ODS_COMBOBOXEDIT | (enabled ? 0 : ODS_DISABLED);

            if (focused && !SendMessageW(handle, CB_GETDROPPEDSTATE, 0, 0))
                item.itemState |= ODS_FOCUS;

            if (SendMessageW(handle, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)
                item.itemState |= ODS_NOFOCUSRECT;

            item.hwndItem = handle;
            item.hDC = dc;
            item.rcItem = info.rcItem;

            PaintItem(item);
            DrawSymbol(dc, info.rcButton, L'\uE70D', enabled ? colors.text : colors.disabledText);
        }

    protected:
        COLORREF BackgroundColor() const override {
            return theme.colors.field;
        }

        bool OwnsColorTarget(HWND target) const override {
            if (Control::OwnsColorTarget(target))
                return true;

            COMBOBOXINFO info{sizeof(info)};

            return handle && target && GetComboBoxInfo(handle, &info) && (target == info.hwndList || target == info.hwndItem);
        }

        std::optional<LRESULT> Message(UINT message, WPARAM w, LPARAM l) override {
            if (message == WM_SETFONT || message == WM_DPICHANGED_AFTERPARENT) {
                auto result = DefSubclassProc(handle, message, w, l);

                UpdateMetrics();

                return result;
            }

            if (theme.mode == ThemeMode::HighContrast)
                return {};

            if (auto result = PaintMessage(message, w))
                return result;

            bool hoverChanged = UpdateHover(message);
            auto result = DefSubclassProc(handle, message, w, l);

            if (handle && (hoverChanged || message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE ||
                message == CB_SETCURSEL || message == WM_UPDATEUISTATE))
                InvalidateRect(handle, nullptr, FALSE);

            return result;
        }

        std::optional<LRESULT> ParentMessage(UINT message, WPARAM w, LPARAM l) override {
            if (message == WM_DRAWITEM) {
                PaintItem(*reinterpret_cast<DRAWITEMSTRUCT*>(l));
                return TRUE;
            } else if (message == WM_COMMAND && HIWORD(w) == CBN_SELCHANGE) {
                if (selectionChanged)
                    selectionChanged();

                return TRUE;
            }

            return {};
        }

    public:
        static constexpr int TextPadding = 3;

        int TextInset() const {
            COMBOBOXINFO info{sizeof(info)};

            return handle && GetComboBoxInfo(handle, &info)
                ? static_cast<int>(info.rcItem.left) + Pixels(TextPadding, Dpi())
                : Pixels(6, Dpi());
        }

        void UpdateMetrics() override {
            if (!handle)
                return;

            int height = Pixels(ItemHeight, Dpi());

            SendMessageW(handle, CB_SETITEMHEIGHT, 0, height);
            SendMessageW(handle, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), height);
        }

        explicit ComboBox(std::function<void()> onSelectionChanged = {})
            : selectionChanged(std::move(onSelectionChanged)) {}

        ~ComboBox() {
            Destroy();
        }

        void Create(HWND owner, int controlId) {
            Control::Create(owner, controlId, L"ComboBox", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS |
                WS_VSCROLL | WS_TABSTOP);
            UpdateMetrics();
        }

        void AddItem(const wchar_t* text) {
            auto result = SendMessageW(handle, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));

            if (result == CB_ERR || result == CB_ERRSPACE)
                throw std::runtime_error("Cannot add combo box item");
        }

        int SelectedIndex() const {
            return handle ? static_cast<int>(SendMessageW(handle, CB_GETCURSEL, 0, 0)) : -1;
        }

        void Select(int index) {
            SendMessageW(handle, CB_SETCURSEL, index, 0);
        }

        std::wstring ItemText(int index) const {
            if (index < 0)
                return {};

            auto length = SendMessageW(handle, CB_GETLBTEXTLEN, index, 0);

            if (length == CB_ERR || length > 32767)
                return {};

            std::wstring value(static_cast<size_t>(length) + 1, L'\0');
            auto copied = SendMessageW(handle, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(value.data()));

            value.resize(copied == CB_ERR ? 0 : static_cast<size_t>(copied));

            return value;
        }

    };
}
