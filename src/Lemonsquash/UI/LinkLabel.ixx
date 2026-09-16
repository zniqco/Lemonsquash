module;

#include "Platform.h"
#include <utility>
#include <windowsx.h>

export module lemonsquash.link_label;

export import lemonsquash.control;

export namespace Lemonsquash {
    class LinkLabel : public Control {
        struct Span {
            std::wstring text;
            std::wstring target;
            RECT bounds{};
        };

        std::vector<Span> spans;
        std::wstring plainText;
        std::function<void(const std::wstring&)> clicked;
        LazyResource<HFONT> regularFont, linkFont;
        int pressedSpan = -1;
        bool centered = false;

        void Parse(std::wstring_view markup) {
            spans.clear();
            plainText.clear();

            constexpr std::wstring_view opening = L"<a href=\"";
            constexpr std::wstring_view closing = L"</a>";
            size_t position = 0;

            while (position < markup.size()) {
                auto begin = markup.find(opening, position);

                if (begin == std::wstring_view::npos) {
                    spans.push_back({std::wstring(markup.substr(position)), {}});

                    break;
                }

                auto targetEnd = markup.find(L"\">", begin + opening.size());
                auto end = targetEnd == std::wstring_view::npos
                    ? std::wstring_view::npos : markup.find(closing, targetEnd + 2);

                if (end == std::wstring_view::npos) {
                    spans.push_back({std::wstring(markup.substr(position)), {}});

                    break;
                }

                if (begin > position)
                    spans.push_back({std::wstring(markup.substr(position, begin - position)), {}});

                spans.push_back({
                    std::wstring(markup.substr(targetEnd + 2, end - targetEnd - 2)),
                    std::wstring(markup.substr(begin + opening.size(), targetEnd - begin - opening.size()))});

                position = end + closing.size();
            }

            for (const auto& span : spans)
                plainText += span.text;
        }

        void Measure(HDC dc, RECT area) {
            std::vector<SIZE> sizes;
            sizes.reserve(spans.size());
            int width = 0;
            int height = 0;

            for (const auto& span : spans) {
                auto font = span.target.empty() ? regularFont.Get() : linkFont.Get();
                auto previous = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
                SIZE size{};
                GetTextExtentPoint32W(dc, span.text.c_str(), static_cast<int>(span.text.size()), &size);

                SelectObject(dc, previous);

                sizes.push_back(size);

                width += size.cx;
                height = std::max(height, static_cast<int>(size.cy));
            }

            int x = area.left + (centered ? (area.right - area.left - width) / 2 : 0);
            int y = area.top + (area.bottom - area.top - height) / 2;

            for (size_t i = 0; i < spans.size(); ++i) {
                spans[i].bounds = {x, y, x + sizes[i].cx, y + height};
                x += sizes[i].cx;
            }
        }

        int HitTest(POINT point) {
            RECT area{};
            GetClientRect(handle, &area);

            if (!PtInRect(&area, point))
                return -1;

            auto dc = GetDC(handle);

            if (!dc)
                return -1;

            Measure(dc, area);
            ReleaseDC(handle, dc);

            for (size_t i = 0; i < spans.size(); ++i) {
                if (!spans[i].target.empty() && PtInRect(&spans[i].bounds, point))
                    return static_cast<int>(i);
            }

            return -1;
        }

        void Draw(const DRAWITEMSTRUCT& draw) {
            int saved = SaveDC(draw.hDC);
            auto brush = reinterpret_cast<HBRUSH>(SendMessageW(GetParent(handle), WM_CTLCOLORSTATIC,
                reinterpret_cast<WPARAM>(draw.hDC), reinterpret_cast<LPARAM>(handle)));

            if (brush)
                FillRect(draw.hDC, &draw.rcItem, brush);

            SetBkMode(draw.hDC, TRANSPARENT);
            Measure(draw.hDC, draw.rcItem);

            for (const auto& span : spans) {
                auto font = span.target.empty() ? regularFont.Get() : linkFont.Get();
                auto previous = SelectObject(draw.hDC, font ? font : GetStockObject(DEFAULT_GUI_FONT));

                TextOutW(draw.hDC, span.bounds.left, span.bounds.top, span.text.c_str(), static_cast<int>(span.text.size()));
                SelectObject(draw.hDC, previous);
            }

            RestoreDC(draw.hDC, saved);
        }

    protected:
        std::optional<LRESULT> Message(UINT message, WPARAM, LPARAM l) override {
            if (message == WM_SETCURSOR && LOWORD(l) == HTCLIENT) {
                POINT point{};
                GetCursorPos(&point);
                ScreenToClient(handle, &point);

                if (HitTest(point) >= 0) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
            }

            if (message == WM_LBUTTONDOWN) {
                POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                pressedSpan = HitTest(point);

                if (pressedSpan >= 0) {
                    SetCapture(handle);
                    return 0;
                }
            }

            if (message == WM_LBUTTONUP && pressedSpan >= 0) {
                POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                int pressed = std::exchange(pressedSpan, -1);

                if (GetCapture() == handle)
                    ReleaseCapture();

                if (HitTest(point) == pressed && clicked)
                    clicked(spans[pressed].target);

                return 0;
            }

            if (message == WM_CAPTURECHANGED)
                pressedSpan = -1;

            return {};
        }

        std::optional<LRESULT> ParentMessage(UINT message, WPARAM, LPARAM l) override {
            if (message == WM_DRAWITEM) {
                Draw(*reinterpret_cast<DRAWITEMSTRUCT*>(l));

                return TRUE;
            }

            return {};
        }

    public:
        explicit LinkLabel(std::function<void(const std::wstring&)> onClicked = {})
            : clicked(std::move(onClicked)) {}

        ~LinkLabel() {
            Destroy();
        }

        void Create(HWND owner, int controlId, const wchar_t* markup, bool center = false) {
            centered = center;

            Parse(markup);
            Control::Create(owner, controlId, L"Static", plainText.c_str(), SS_OWNERDRAW | SS_NOTIFY);
        }

        void SetText(const wchar_t* markup) {
            pressedSpan = -1;

            if (GetCapture() == handle)
                ReleaseCapture();

            Parse(markup);

            if (handle) {
                Control::SetText(plainText.c_str());
                InvalidateRect(handle, nullptr, TRUE);
            }
        }

        void UpdateFont() override {
            if (!handle)
                return;

            regularFont = TextFont(Dpi());
            LOGFONTW description{};

            if (auto font = regularFont.Get(); font &&
                GetObjectW(font, sizeof(description), &description) == sizeof(description)) {
                description.lfUnderline = TRUE;
                linkFont = LazyResource<HFONT>(
                    [description] { return CreateFontIndirectW(&description); },
                    [](HFONT font) noexcept { DeleteObject(font); });
            }

            SetFont(regularFont);
        }
    };
}
