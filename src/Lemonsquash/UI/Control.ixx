module;

#include "Platform.h"
#include <commctrl.h>
#include <uxtheme.h>

export module lemonsquash.control;

export import lemonsquash.window;
export import lemonsquash.theme;
export import lemonsquash.lazy_resource;
import lemonsquash.common;

export namespace Lemonsquash {
    class Control : public Window {
        bool pageBackground = true;
        LazyResource<HFONT> textFont, symbolFont;
        UINT symbolDpi = 0;

        static bool IsColorMessage(UINT message) {
            return message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN || message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX;
        }

        LRESULT ApplyColors(HDC dc) const {
            auto background = BackgroundColor();
            SetTextColor(dc, IsWindowEnabled(handle) ? theme.colors.text : theme.colors.disabledText);
            SetBkColor(dc, background);
            SetDCBrushColor(dc, background);

            return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }

        static LRESULT CALLBACK ControlProc(HWND control, UINT message, WPARAM w, LPARAM l, UINT_PTR subclassId, DWORD_PTR data) {
            auto self = reinterpret_cast<Control*>(data);

            if (message == WM_NCDESTROY) {
                RemoveWindowSubclass(control, ControlProc, subclassId);

                auto result = DefSubclassProc(control, message, w, l);
                self->handle = nullptr;
                self->hovered = false;
                self->textFont.Reset();
                self->symbolFont.Reset();
                self->symbolDpi = 0;
                self->OnDestroyed();

                return result;
            }

            try {
                if (message == WM_DPICHANGED_AFTERPARENT)
                    self->UpdateFont();

                if (IsColorMessage(message) && self->OwnsColorTarget(reinterpret_cast<HWND>(l)))
                    return self->ApplyColors(reinterpret_cast<HDC>(w));

                if (auto result = self->Message(message, w, l))
                    return *result;
            } catch (...) {
                Log(L"Could not handle control message");
            }

            return DefSubclassProc(control, message, w, l);
        }

    protected:
        ThemeState theme;
        bool hovered = false;

        static void Fill(HDC dc, const RECT& rect, COLORREF color) {
            SetDCBrushColor(dc, color);
            FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        }

        static void Border(HDC dc, const RECT& rect, COLORREF color) {
            SetDCBrushColor(dc, color);
            FrameRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        }

        static void FocusRect(HDC dc, const RECT& rect) {
            auto previousText = SetTextColor(dc, RGB(0, 0, 0));
            auto previousBackground = SetBkColor(dc, RGB(255, 255, 255));

            DrawFocusRect(dc, &rect);
            SetBkColor(dc, previousBackground);
            SetTextColor(dc, previousText);
        }

        static void FocusLabel(HDC dc, const std::wstring& caption, const RECT& label, UINT flags, int padding) {
            RECT bounds{0, 0, label.right - label.left, label.bottom - label.top};

            DrawTextW(dc, caption.c_str(), -1, &bounds, flags | DT_SINGLELINE | DT_CALCRECT);

            int width = std::min(bounds.right, label.right - label.left), height = bounds.bottom;
            int left = label.left + ((flags & DT_CENTER) ? (label.right - label.left - width) / 2 : 0);
            int top = label.top + (label.bottom - label.top - height) / 2;
            RECT focus{left - padding, top - padding, left + width + padding, top + height + padding};

            IntersectRect(&focus, &focus, &label);
            FocusRect(dc, focus);
        }

        void DrawSymbol(HDC dc, RECT rect, wchar_t symbol, COLORREF color) {
            auto dpi = Dpi();

            if (!symbolFont || symbolDpi != dpi) {
                symbolFont = SymbolFont(dpi);
                symbolDpi = dpi;
            }

            auto font = symbolFont.Get();

            if (!font)
                return;

            int saved = SaveDC(dc);

            SelectObject(dc, font);
            SetTextColor(dc, color);
            SetBkMode(dc, TRANSPARENT);
            DrawTextW(dc, &symbol, 1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            RestoreDC(dc, saved);
        }

        bool UpdateHover(UINT message) {
            if (message == WM_MOUSEMOVE && !hovered) {
                hovered = true;

                TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, handle, 0};

                TrackMouseEvent(&tracking);

                return true;
            }

            if (message == WM_MOUSELEAVE) {
                bool changed = hovered;
                hovered = false;

                return changed;
            }

            return false;
        }

        std::optional<LRESULT> PaintMessage(UINT message, WPARAM w) {
            if (message == WM_ERASEBKGND)
                return 1;

            if (message != WM_PAINT && message != WM_PRINTCLIENT)
                return {};

            PAINTSTRUCT paint{};
            HDC dc = message == WM_PAINT ? BeginPaint(handle, &paint) : reinterpret_cast<HDC>(w);

            if (dc) {
                int saved = SaveDC(dc);

                try {
                    Paint(dc);
                } catch (...) {
                }

                RestoreDC(dc, saved);
            }

            if (message == WM_PAINT)
                EndPaint(handle, &paint);

            return 0;
        }

        Control() : Window(nullptr) {}

        ~Control() {
            Destroy();
        }

        void Create(HWND owner, int controlId, const wchar_t* className, const wchar_t* text, DWORD style, DWORD extendedStyle = 0) {
            if (handle)
                throw std::logic_error("Control is already created");

            instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
            winrt::check_bool(instance != nullptr);

            handle = CreateWindowExW(WS_EX_NOPARENTNOTIFY | extendedStyle, className, text,
                WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, owner,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)), instance, nullptr);

            if (!handle || !SetWindowSubclass(handle, ControlProc, 1, reinterpret_cast<DWORD_PTR>(this))) {
                auto error = GetLastError();

                if (handle)
                    DestroyWindow(handle);

                handle = nullptr;

                throw winrt::hresult_error(HRESULT_FROM_WIN32(error));
            }
            try {
                UpdateFont();
            } catch (...) {
                Destroy();
                throw;
            }
        }

        UINT Dpi() const {
            return GetDpiForWindow(handle);
        }

        virtual COLORREF BackgroundColor() const {
            return pageBackground ? theme.colors.surface : theme.colors.background;
        }

        virtual bool OwnsColorTarget(HWND target) const {
            return handle && target == handle;
        }

        virtual void ApplyNativeTheme() {
            bool custom = theme.mode != ThemeMode::HighContrast;
            SetWindowTheme(handle, custom ? L"" : nullptr, custom ? L"" : nullptr);
        }

        virtual void Paint(HDC) {}
        virtual std::optional<LRESULT> Message(UINT, WPARAM, LPARAM) { return {}; }
        virtual std::optional<LRESULT> ParentMessage(UINT, WPARAM, LPARAM) { return {}; }
        virtual void OnDestroyed() noexcept {}

    public:
        static LazyResource<HFONT> TextFont(UINT dpi) {
            static LazyResourceCache<UINT, HFONT> fonts;

            return fonts.Acquire(dpi, [dpi] {
                return CreateFontW(-MulDiv(9, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    DEFAULT_PITCH, L"Segoe UI");
            }, [](HFONT font) noexcept { DeleteObject(font); });
        }

        static LazyResource<HFONT> SymbolFont(UINT dpi) {
            static LazyResourceCache<UINT, HFONT> fonts;

            return fonts.Acquire(dpi, [dpi] {
                return CreateFontW(-MulDiv(12, static_cast<int>(dpi), 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                    DEFAULT_PITCH, L"Segoe MDL2 Assets");
            }, [](HFONT font) noexcept { DeleteObject(font); });
        }

        static Control* FromHandle(HWND control) {
            DWORD_PTR data = 0;

            return GetWindowSubclass(control, ControlProc, 1, &data) ? reinterpret_cast<Control*>(data) : nullptr;
        }

        static std::optional<LRESULT> Reflect(HWND owner, UINT message, WPARAM w, LPARAM l) {
            HWND source = nullptr;

            if (IsColorMessage(message) || message == WM_COMMAND)
                source = reinterpret_cast<HWND>(l);
            else if (message == WM_NOTIFY && l)
                source = reinterpret_cast<NMHDR*>(l)->hwndFrom;
            else if (message == WM_DRAWITEM && l && reinterpret_cast<DRAWITEMSTRUCT*>(l)->CtlType != ODT_MENU)
                source = reinterpret_cast<DRAWITEMSTRUCT*>(l)->hwndItem;

            auto control = source ? FromHandle(source) : nullptr;

            if (!control)
                return {};

            try {
                if (IsColorMessage(message))
                    return control->ApplyColors(reinterpret_cast<HDC>(w));

                return control->ParentMessage(message, w, l);
            } catch (...) {
                ShowException(owner, L"Lemonsquash");
            }

            return 0;
        }

        virtual void ApplyTheme(const ThemeState& value) {
            theme = value;

            if (handle) {
                ApplyNativeTheme();
                InvalidateRect(handle, nullptr, FALSE);
            }
        }

        void ApplyTheme(const ThemeState& value, bool isPage) {
            pageBackground = isPage;

            ApplyTheme(value);
        }

        virtual void UpdateMetrics() {}

        virtual void UpdateFont() {
            if (!handle)
                return;

            auto font = TextFont(Dpi());

            if (font.Get())
                SetFont(font);
        }

        void SetFont(HFONT font, bool redraw = true) {
            if (handle) {
                auto previousFont = textFont;

                if (textFont.Get() != font)
                    textFont.Reset();

                SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(font), redraw);
            }
        }

        void SetFont(const LazyResource<HFONT>& font, bool redraw = true) {
            if (handle) {
                auto fontHandle = font.Get();
                auto previousFont = std::move(textFont);

                textFont = font;

                SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(fontHandle), redraw);
            }
        }

        void SetText(const wchar_t* text) const {
            SetWindowTextW(handle, text);
        }

        void SetBounds(int x, int y, int width, int height) const {
            MoveWindow(handle, x, y, width, height, TRUE);
        }

        void Show(bool visible = true) const {
            ShowWindow(handle, visible ? SW_SHOW : SW_HIDE);
        }

        void Enable(bool enabled = true) const {
            EnableWindow(handle, enabled);
        }
    };
}
