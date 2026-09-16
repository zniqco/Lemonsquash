module;

#include "Platform.h"
#include "Resource.h"
#include <commctrl.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <imm.h>
#include <initguid.h>
#include <uxtheme.h>
#include <wincodec.h>
#include <windowsx.h>

export module lemonsquash.main_dialog;

export import lemonsquash.common;
export import lemonsquash.window;
import lemonsquash.backend;
import lemonsquash.bitmap;
import lemonsquash.monitor;
import lemonsquash.search;
import lemonsquash.settings;
import lemonsquash.theme;

namespace Lemonsquash {
    namespace {
        constexpr UINT_PTR FadeTimerId = 1;
        constexpr ULONGLONG FadeDuration = 120;
        constexpr int InputHeight = 56, InputPadding = 10, RowHeight = 52, BottomPadding = 6;
        constexpr int InputTextOffsetY = 3, ResultsTopPadding = 6, ResultTextOffsetY = 0;
        constexpr int ResultsSidePadding = 8, RowSideInset = 1;
        constexpr D2D1_RECT_F ResultIconRect{10, 10, 42, 42};
        const std::wstring CalculatorIconId = L"builtin:calculator";
        const std::wstring GoogleIconId = L"builtin:google";
        const std::wstring SettingsIconId = L"builtin:settings";
        const std::wstring TerminalIconId = L"builtin:terminal";

        D2D1_COLOR_F Color(COLORREF c) {
            return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f);
        }

        void Label(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush, const std::wstring& value,
            IDWriteTextFormat* font, D2D1_RECT_F rect, D2D1_COLOR_F color) {
            brush->SetColor(color);
            target->DrawTextW(value.c_str(), static_cast<UINT32>(value.size()), font, rect, brush,
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        const std::wstring& ResultIconId(const Result& result) {
            switch (result.action) {
                case Action::Calculator: {
                    return CalculatorIconId;
                }

                case Action::Google: {
                    return GoogleIconId;
                }

                case Action::Preferences:
                case Action::About:
                case Action::Restart: {
                    return SettingsIconId;
                }

                case Action::Shell: {
                    return TerminalIconId;
                }

                default: {
                    return result.id;
                }
            }
        }
    }
}

export namespace Lemonsquash {
    struct MainDialogCallbacks {
        std::function<void(const std::wstring&)> suggest;
        std::function<void(const std::wstring&, const std::wstring&)> requestIcon;
        std::function<void(const Result&, bool)> execute;
        std::function<void()> showPreferences;
        std::function<std::optional<LRESULT>(HWND, UINT, WPARAM, LPARAM)> message;
    };

    class MainDialog : public Window {
        HWND input = nullptr;
        HFONT inputFont = nullptr;
        HBRUSH backgroundBrush = nullptr;

        UINT dpi = 96, fontDpi = 0;
        float scale = 1.0f;
        bool suppressQuery = false, composing = false, clearOnShow = true;
        bool ignoreMouse = false, hiding = false, bufferedPaint = false;
        bool fading = false;
        ULONGLONG fadeStarted = 0;
        BYTE opacity = 255, fadeFrom = 0, fadeTo = 255;
        int selectedIndex = 0, topIndex = 0, visibleRows = 0, wheelDelta = 0;
        POINT lastMouse{};

        ThemeState theme;
        std::wstring query;
        const Settings& settings;
        MainDialogCallbacks callbacks;

        std::shared_ptr<const std::vector<AppEntry>> apps = std::make_shared<const std::vector<AppEntry>>();
        std::vector<Result> results, appResults;
        std::vector<std::wstring> suggestions;
        std::unordered_map<std::wstring, std::shared_ptr<Bitmap>> icons;
        std::unordered_map<std::wstring, winrt::com_ptr<ID2D1Bitmap>> drawnIcons;

        winrt::com_ptr<ID2D1Factory> graphics;
        winrt::com_ptr<IDWriteFactory> text;
        winrt::com_ptr<ID2D1DCRenderTarget> rowTarget;
        winrt::com_ptr<ID2D1SolidColorBrush> rowBrush;
        winrt::com_ptr<IDWriteTextFormat> titleFont, centeredTitleFont, detailFont;

        void CreateControls() {
            bufferedPaint = SUCCEEDED(BufferedPaintInit());
            dpi = GetDpiForWindow(handle);
            scale = dpi / 96.0f;

            input = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, handle,
                reinterpret_cast<HMENU>(10), instance, nullptr);

            if (!input)
                throw winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError()));

            SendMessageW(input, EM_SETLIMITTEXT, 512, 0);
            SendMessageW(input, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
            winrt::check_bool(SetWindowSubclass(input, InputProc, 1, reinterpret_cast<DWORD_PTR>(this)));

            winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, graphics.put()));
            icons.emplace(CalculatorIconId,
                std::make_shared<Bitmap>(Bitmap::LoadFromResource(instance, IDR_CALCULATOR)));

            icons.emplace(GoogleIconId,
                std::make_shared<Bitmap>(Bitmap::LoadFromResource(instance, IDR_GOOGLE)));

            icons.emplace(SettingsIconId,
                std::make_shared<Bitmap>(Bitmap::LoadFromResource(instance, IDR_SETTINGS)));

            icons.emplace(TerminalIconId,
                std::make_shared<Bitmap>(Bitmap::LoadFromResource(instance, IDR_TERMINAL)));

            winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(text.put())));

            auto format = [&](float size, DWRITE_FONT_WEIGHT weight, winrt::com_ptr<IDWriteTextFormat>& font) {
                winrt::check_hresult(text->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL, size, L"", font.put()));

                font->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                winrt::com_ptr<IDWriteInlineObject> ellipsis;
                text->CreateEllipsisTrimmingSign(font.get(), ellipsis.put());
                font->SetTrimming(&trimming, ellipsis.get());
            };

            format(17.0f, DWRITE_FONT_WEIGHT_NORMAL, titleFont);
            format(17.0f, DWRITE_FONT_WEIGHT_NORMAL, centeredTitleFont);
            centeredTitleFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            format(11.0f, DWRITE_FONT_WEIGHT_NORMAL, detailFont);

            EnsureRowResources();
            Theme();
            Layout();
        }

        void Theme() {
            theme = ResolveTheme(settings.theme);

            if (backgroundBrush)
                DeleteObject(backgroundBrush);

            backgroundBrush = CreateSolidBrush(theme.colors.background);

            SetDarkTitleBar(theme.mode == ThemeMode::Dark);
            BOOL noTransitions = TRUE;
            DwmSetWindowAttribute(handle, DWMWA_TRANSITIONS_FORCEDISABLED, &noTransitions, sizeof(noTransitions));
            int corner = 2;
            DwmSetWindowAttribute(handle, 33, &corner, sizeof(corner));
            MARGINS margins{1, 1, 1, 1};
            DwmExtendFrameIntoClientArea(handle, &margins);

            if (input)
                InvalidateRect(input, nullptr, TRUE);

            InvalidateRect(handle, nullptr, FALSE);
        }

        Monitor PlacementMonitor() const {
            if (settings.position == 1)
                return Monitor::FromCursor();

            if (settings.position == 2)
                return Monitor::Primary();

            return Monitor::FromWindow(GetForegroundWindow());
        }

        void Layout(bool reposition = false) {
            auto px = [&](int n) {
                return Pixels(n, dpi);
            };
            int rows = static_cast<int>(std::min(size_t(6), results.size()));
            int width = px(640), x = 0, y = 0;
            UINT flags = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOMOVE;
            auto monitor = reposition ? PlacementMonitor() : Monitor::FromWindow(handle);
            auto workRect = monitor.WorkArea();
            int workWidth = static_cast<int>(workRect.right - workRect.left);
            int workHeight = static_cast<int>(workRect.bottom - workRect.top);
            int available = workHeight * 4 / 5 - px(12);
            rows =
                std::min(rows, std::max(1, (available - px(InputHeight + ResultsTopPadding + BottomPadding)) / px(RowHeight)));

            int height = InputHeight + (rows ? ResultsTopPadding + rows * RowHeight + BottomPadding : 0);

            if (reposition) {
                width = std::min(width, workWidth - px(24));
                x = workRect.left + (workWidth - width) / 2;
                y = workRect.top + workHeight / 5;
                flags &= ~SWP_NOMOVE;
            } else {
                RECT rect{};
                GetClientRect(handle, &rect);

                if (rect.right > 0)
                    width = rect.right;
            }

            RECT current{};
            GetWindowRect(handle, &current);
            bool resized = current.right - current.left != width || current.bottom - current.top != px(height);

            if (!resized)
                flags |= SWP_NOSIZE;

            if (reposition || resized)
                SetWindowPos(handle, nullptr, x, y, width, px(height), flags);

            auto place = [&](HWND control, int left, int top, int cx, int cy) {
                RECT old{};
                GetWindowRect(control, &old);
                MapWindowPoints(nullptr, handle, reinterpret_cast<POINT*>(&old), 2);

                if (old.left == left && old.top == top && old.right - old.left == cx && old.bottom - old.top == cy)
                    return;

                SetWindowPos(control, nullptr, left, top, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE);
            };

            place(input, px(InputPadding), px(InputPadding + InputTextOffsetY), width - px(InputPadding * 2),
                px(InputHeight - InputPadding * 2));

            visibleRows = rows;
            topIndex = std::clamp(topIndex, 0, std::max(0, static_cast<int>(results.size()) - visibleRows));

            if (!inputFont || fontDpi != dpi) {
                auto font = CreateFontW(-px(22), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

                if (font) {
                    SendMessageW(input, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

                    if (inputFont)
                        DeleteObject(inputFont);

                    inputFont = font;
                    fontDpi = dpi;
                }
            }

            if (resized)
                InvalidateRect(handle, nullptr, FALSE);
        }

        RECT ResultsRect() const {
            RECT client{};
            GetClientRect(handle, &client);

            int side = Pixels(ResultsSidePadding, dpi), top = Pixels(InputHeight + ResultsTopPadding, dpi);

            return {side, top, client.right - side, top + visibleRows * Pixels(RowHeight, dpi)};
        }

        int ResultFromPoint(LPARAM l) const {
            POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            auto area = ResultsRect();

            if (!PtInRect(&area, point))
                return -1;

            int index = topIndex + (point.y - area.top) / Pixels(RowHeight, dpi);

            return index < static_cast<int>(results.size()) ? index : -1;
        }

        void Paint() {
            PAINTSTRUCT paint{};
            auto dc = BeginPaint(handle, &paint);
            RECT rect{};
            GetClientRect(handle, &rect);

            HDC memory = nullptr;
            auto buffer = bufferedPaint ? BeginBufferedPaint(dc, &rect, BPBF_COMPATIBLEBITMAP, nullptr, &memory) : nullptr;
            auto target = buffer ? memory : dc;

            FillRect(target, &rect, backgroundBrush);

            if (!results.empty()) {
                auto previous = SelectObject(target, GetStockObject(DC_PEN));
                SetDCPenColor(target, theme.colors.separator);

                int inset = Pixels(9, dpi);
                int y = Pixels(InputHeight - 1, dpi);

                MoveToEx(target, inset, y, nullptr);
                LineTo(target, rect.right - inset, y);
                SelectObject(target, previous);
                PaintResults(target, ResultsRect());
            }

            if (buffer)
                EndBufferedPaint(buffer, TRUE);

            EndPaint(handle, &paint);
        }

        bool EnsureRowResources() {
            if (!rowTarget) {
                auto p = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                    static_cast<float>(dpi), static_cast<float>(dpi));

                if (FAILED(graphics->CreateDCRenderTarget(&p, rowTarget.put())))
                    return false;
            }

            if (!rowBrush && FAILED(rowTarget->CreateSolidColorBrush(Color(theme.colors.background), rowBrush.put())))
                return false;

            return true;
        }

        void DiscardRowResources() {
            drawnIcons.clear();
            rowBrush = nullptr;
            rowTarget = nullptr;
        }

        void DrawResult(const Result& item, float width, bool selected) {
            const auto& colors = theme.colors;
            auto ink = Color(selected ? colors.selectionText : colors.text);
            auto muted = Color(selected ? colors.selectionMutedText : colors.mutedText);

            if (selected) {
                rowBrush->SetColor(Color(colors.selection));
                rowTarget->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(RowSideInset, 2, width - RowSideInset, RowHeight - 2), 6, 6), rowBrush.get());
            }

            const auto& iconId = ResultIconId(item);
            auto found = drawnIcons.find(iconId);

            if (found == drawnIcons.end()) {
                auto raw = icons.find(iconId);

                if (raw != icons.end() && raw->second) {
                    winrt::com_ptr<ID2D1Bitmap> bitmap;
                    auto& data = *raw->second;

                    if (SUCCEEDED(
                        rowTarget->CreateBitmap(D2D1::SizeU(data.Width(), data.Height()), data.Pixels().data(), data.Width() * 4,
                            D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                D2D1_ALPHA_MODE_PREMULTIPLIED)),
                            bitmap.put())))
                        found = drawnIcons.emplace(iconId, std::move(bitmap)).first;
                }
            }

            if (found != drawnIcons.end()) {
                rowTarget->DrawBitmap(found->second.get(), ResultIconRect);
            } else {
                rowBrush->SetColor(theme.mode == ThemeMode::HighContrast ? ink : Color(colors.placeholder));
                rowTarget->FillRoundedRectangle(D2D1::RoundedRect(ResultIconRect, 7, 7), rowBrush.get());
            }

            bool canElevate = item.action == Action::Shell || item.action == Action::Shortcut || item.action == Action::Appx;
            bool elevate = canElevate && (GetKeyState(VK_CONTROL) & 0x8000);
            auto description = item.description;

            if (canElevate) {
                description = elevate ? L"Run as administrator" : L"Run";

                if (item.action == Action::Shortcut && !item.resolvedTarget.empty())
                    description += L" · " + item.resolvedTarget;
            }

            if (description.empty()) {
                Label(rowTarget.get(), rowBrush.get(), item.caption, centeredTitleFont.get(),
                    D2D1::RectF(52, 0, width - 12, RowHeight), ink);
            } else {
                Label(rowTarget.get(), rowBrush.get(), item.caption, titleFont.get(),
                    D2D1::RectF(52, 6 + ResultTextOffsetY, width - 12, 30 + ResultTextOffsetY), ink);
                Label(rowTarget.get(), rowBrush.get(), description, detailFont.get(),
                    D2D1::RectF(52, 28 + ResultTextOffsetY, width - 12, 48 + ResultTextOffsetY), muted);
            }
        }

        void PaintResults(HDC dc, const RECT& area) {
            if (IsRectEmpty(&area) || !EnsureRowResources() || FAILED(rowTarget->BindDC(dc, &area)))
                return;

            float width = (area.right - area.left) / scale, rowHeight = Pixels(RowHeight, dpi) / scale;
            int last = std::min(topIndex + visibleRows, static_cast<int>(results.size()));

            rowTarget->BeginDraw();
            rowTarget->Clear(Color(theme.colors.background));

            for (int index = topIndex; index < last; ++index) {
                rowTarget->SetTransform(D2D1::Matrix3x2F::Translation(0, (index - topIndex) * rowHeight));
                rowTarget->PushAxisAlignedClip(D2D1::RectF(0, 0, width, rowHeight), D2D1_ANTIALIAS_MODE_ALIASED);
                DrawResult(results[index], width, index == selectedIndex);
                rowTarget->PopAxisAlignedClip();
            }

            rowTarget->SetTransform(D2D1::Matrix3x2F::Identity());

            if (rowTarget->EndDraw() == D2DERR_RECREATE_TARGET) {
                DiscardRowResources();
                InvalidateRect(handle, nullptr, FALSE);
            }
        }

        std::wstring GoogleQuery() const {
            return query.starts_with(L'?') ? query.substr(1) : query;
        }

        std::wstring SuggestionQuery() const {
            return settings.googleEnabled && settings.googleSuggestions ? GoogleQuery() : L"";
        }

        void QueryChanged() {
            if (suppressQuery)
                return;

            auto nextQuery = WindowText(input);

            if (nextQuery == query)
                return;

            query = std::move(nextQuery);
            suggestions.clear();
            appResults = SearchApps(query, *apps, settings);

            if (callbacks.suggest)
                callbacks.suggest(SuggestionQuery());

            RefreshResults(true);
        }

        void RefreshResults(bool reset) {
            auto next = Search(query, appResults, settings, suggestions);

            if (next == results && !reset)
                return;

            std::wstring previous = reset || results.empty() ? L"" : results[selectedIndex].id;
            results = std::move(next);
            selectedIndex = 0;

            for (size_t i = 0; i < results.size(); ++i) {
                if (results[i].id == previous)
                    selectedIndex = static_cast<int>(i);

                if (callbacks.requestIcon)
                    callbacks.requestIcon(results[i].id, results[i].iconPath);
            }

            if (reset)
                topIndex = 0;

            Layout();
            InvalidateRect(handle, nullptr, FALSE);
        }

        void Select(int selected, bool rewrite) {
            if (results.empty())
                return;

            selected = std::clamp(selected, 0, static_cast<int>(results.size()) - 1);

            if (selected != selectedIndex) {
                selectedIndex = selected;

                if (selectedIndex < topIndex)
                    topIndex = selectedIndex;
                else if (selectedIndex >= topIndex + visibleRows)
                    topIndex = selectedIndex - visibleRows + 1;

                InvalidateRect(handle, nullptr, FALSE);
            }

            if (rewrite && results[selected].rewrite) {
                const auto& caption = results[selected].caption;
                suppressQuery = true;

                bool caretHidden = HideCaret(input) != FALSE;
                SendMessageW(input, WM_SETREDRAW, FALSE, 0);
                SetWindowTextW(input, caption.c_str());

                if (results[selected].action == Action::Google) {
                    auto typed = GoogleQuery();
                    auto start = caption.starts_with(typed) ? typed.size() : 0;
                    SendMessageW(input, EM_SETSEL, caption.size(), static_cast<LPARAM>(start));
                    SendMessageW(input, EM_SCROLLCARET, 0, 0);
                } else {
                    SendMessageW(input, EM_SETSEL, static_cast<WPARAM>(-1), -1);
                }

                SendMessageW(input, WM_SETREDRAW, TRUE, 0);
                RedrawWindow(input, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);

                if (caretHidden)
                    ShowCaret(input);

                suppressQuery = false;
            }
        }

        void Execute() {
            if (hiding || selectedIndex >= static_cast<int>(results.size()))
                return;

            auto item = results[selectedIndex];
            bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            Hide(true);

            if (callbacks.execute)
                callbacks.execute(item, control);
        }

        bool AnimationsEnabled() const {
            BOOL animations = FALSE;
            return theme.mode != ThemeMode::HighContrast &&
                SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0) && animations;
        }

        void SetOpacity(BYTE value) {
            opacity = value;
            SetLayeredWindowAttributes(handle, 0, opacity, LWA_ALPHA);
        }

        void FinishFade() {
            KillTimer(handle, FadeTimerId);
            fading = false;
            SetOpacity(fadeTo);

            if (hiding) {
                ShowWindow(handle, SW_HIDE);
                hiding = false;
                SetOpacity(255);
            }
        }

        void StartFade(BYTE target) {
            fadeFrom = opacity;
            fadeTo = target;
            fadeStarted = GetTickCount64();
            fading = fadeFrom != fadeTo && SetTimer(handle, FadeTimerId, 15, nullptr) != 0;

            if (!fading)
                FinishFade();
        }

        LRESULT Message(UINT message, WPARAM w, LPARAM l) {
            switch (message) {
                case WM_CREATE: {
                    CreateControls();

                    return 0;
                }

                case WM_NCCALCSIZE: {
                    if (w)
                        return 0;

                    break;
                }

                case WM_NCHITTEST: {
                    return HTCLIENT;
                }

                case WM_ERASEBKGND: {
                    return 1;
                }

                case WM_TIMER: {
                    if (w == FadeTimerId) {
                        if (fading) {
                            auto elapsed = GetTickCount64() - fadeStarted;

                            if (elapsed >= FadeDuration) {
                                FinishFade();
                            } else {
                                float progress = static_cast<float>(elapsed) / static_cast<float>(FadeDuration);
                                float eased = progress * (2.0f - progress);
                                SetOpacity(static_cast<BYTE>(fadeFrom + (static_cast<float>(fadeTo) - fadeFrom) * eased));
                            }
                        }
                        return 0;
                    }

                    break;
                }

                case WM_PAINT: {
                    Paint();

                    return 0;
                }

                case WM_CTLCOLOREDIT: {
                    SetTextColor(reinterpret_cast<HDC>(w), theme.colors.text);
                    SetBkColor(reinterpret_cast<HDC>(w), theme.colors.background);

                    return reinterpret_cast<LRESULT>(backgroundBrush);
                }

                case WM_DPICHANGED: {
                    dpi = HIWORD(w);
                    scale = dpi / 96.0f;
                    DiscardRowResources();
                    SetBounds(*reinterpret_cast<RECT*>(l));
                    Layout();

                    return 0;
                }

                case WM_SETTINGCHANGE: {
                    if (IsThemeSettingChange(w, l))
                        Theme();

                    return 0;
                }

                case WM_THEMECHANGED:
                case WM_SYSCOLORCHANGE: {
                    Theme();

                    return 0;
                }

                case WM_ACTIVATE: {
                    if (LOWORD(w) == WA_INACTIVE && !hiding)
                        Hide(false);

                    return 0;
                }

                case WM_SETFOCUS: {
                    SetFocus(input);

                    return 0;
                }

                case WM_MOUSEMOVE: {
                    POINT point{};
                    GetCursorPos(&point);

                    if (ignoreMouse && abs(point.x - lastMouse.x) + abs(point.y - lastMouse.y) < 4)
                        return 0;

                    ignoreMouse = false;

                    if (auto index = ResultFromPoint(l); index >= 0)
                        Select(index, false);

                    return 0;
                }

                case WM_LBUTTONUP: {
                    if (auto index = ResultFromPoint(l); index >= 0) {
                        Select(index, false);
                        Execute();
                    } else {
                        SetFocus(input);
                    }

                    return 0;
                }

                case WM_MOUSEWHEEL: {
                    wheelDelta += GET_WHEEL_DELTA_WPARAM(w);
                    int steps = wheelDelta / WHEEL_DELTA;
                    wheelDelta %= WHEEL_DELTA;

                    UINT lines = 3;
                    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);

                    int amount = lines == WHEEL_PAGESCROLL ? visibleRows : static_cast<int>(std::min(lines, 100u));
                    int last = std::max(0, static_cast<int>(results.size()) - visibleRows);
                    int next = std::clamp(topIndex - steps * amount, 0, last);

                    if (next != topIndex) {
                        topIndex = next;
                        InvalidateRect(handle, nullptr, FALSE);
                        GetCursorPos(&lastMouse);
                        ignoreMouse = true;
                    }

                    return 0;
                }

                case WM_COMMAND: {
                    if (LOWORD(w) == 10 && HIWORD(w) == EN_CHANGE)
                        QueryChanged();

                    return 0;
                }

                case WM_CLOSE: {
                    Hide(false);

                    return 0;
                }

                case WM_DESTROY: {
                    KillTimer(handle, FadeTimerId);
                    fading = false;

                    break;
                }
            }

            if (callbacks.message) {
                if (auto result = callbacks.message(handle, message, w, l))
                    return *result;
            }

            return DefWindowProcW(handle, message, w, l);
        }

        static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
            auto self = reinterpret_cast<MainDialog*>(GetWindowLongPtrW(window, GWLP_USERDATA));

            if (message == WM_NCCREATE) {
                self = static_cast<MainDialog*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
                self->handle = window;
                SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            }

            if (!self)
                return DefWindowProcW(window, message, w, l);

            if (message == WM_NCDESTROY) {
                auto result = DefWindowProcW(window, message, w, l);
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
                self->handle = nullptr;
                self->input = nullptr;

                return result;
            }

            try {
                return self->Message(message, w, l);
            } catch (...) {
                ShowException(window, L"Lemonsquash");
            }

            if (message == WM_CREATE)
                return -1;

            return DefWindowProcW(window, message, w, l);
        }

        static LRESULT CALLBACK InputProc(HWND control, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
            auto self = reinterpret_cast<MainDialog*>(data);

            if (self->hiding && ((message >= WM_KEYFIRST && message <= WM_KEYLAST) ||
                (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST)))
                return 0;

            if (message == WM_SETFOCUS ||
                ((message == WM_KEYDOWN || message == WM_KEYUP || message == WM_SYSKEYDOWN || message == WM_SYSKEYUP) &&
                w == VK_CONTROL))
                InvalidateRect(self->handle, nullptr, FALSE);

            if (message == WM_MOUSEWHEEL)
                return SendMessageW(self->handle, message, w, l);

            if ((message == WM_KEYDOWN && w == VK_TAB) || (message == WM_CHAR && w == L'\t'))
                return 0;

            if (message == WM_IME_STARTCOMPOSITION)
                self->composing = true;

            if (message == WM_IME_ENDCOMPOSITION)
                self->composing = false;

            if (message == WM_KEYDOWN && !self->composing) {
                try {
                    int index = self->selectedIndex;
                    int count = static_cast<int>(self->results.size());
                    bool repeat = (l & (1LL << 30)) != 0;

                    if (w == VK_UP || w == VK_DOWN || w == VK_PRIOR || w == VK_NEXT) {
                        if (count) {
                            int step = w == VK_UP ? -1 : w == VK_DOWN ? 1
                                : w == VK_PRIOR  ? -6
                                : 6;

                            int next = index + step;

                            if (!repeat && step == -1 && next < 0)
                                next = count - 1;

                            if (!repeat && step == 1 && next >= count)
                                next = 0;

                            self->Select(next, true);
                            GetCursorPos(&self->lastMouse);
                            self->ignoreMouse = true;
                        }

                        return 0;
                    }

                    if (w == VK_RETURN) {
                        if (!repeat)
                            self->Execute();

                        return 0;
                    }

                    if (w == VK_ESCAPE) {
                        self->Hide(false);

                        return 0;
                    }

                    if (w == VK_F1) {
                        if (self->callbacks.showPreferences)
                            self->callbacks.showPreferences();

                        return 0;
                    }
                } catch (...) {
                    ShowException(self->handle, L"Could not open");
                }
            }

            if (message == WM_CHAR && !self->composing && (w == VK_RETURN || w == VK_ESCAPE))
                return 0;

            return DefSubclassProc(control, message, w, l);
        }

    public:
        MainDialog(HINSTANCE module, const Settings& options, MainDialogCallbacks handlers)
            : Window(module), settings(options), callbacks(std::move(handlers)) {}

        ~MainDialog() {
            Destroy();

            if (inputFont)
                DeleteObject(inputFont);

            if (backgroundBrush)
                DeleteObject(backgroundBrush);

            if (bufferedPaint)
                BufferedPaintUnInit();
        }

        bool Create() {
            WNDCLASSEXW type{sizeof(type)};
            type.lpfnWndProc = WindowProc;
            type.hInstance = instance;
            type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            type.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
            type.hIconSm = type.hIcon;
            type.lpszClassName = L"Lemonsquash";

            winrt::check_bool(RegisterClassExW(&type));

            HWND created = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, type.lpszClassName, L"Lemonsquash",
                WS_POPUP | WS_THICKFRAME | WS_CAPTION | WS_CLIPCHILDREN, CW_USEDEFAULT,
                CW_USEDEFAULT, 640, 240, nullptr, nullptr, instance, this);

            if (!created)
                return false;

            winrt::check_bool(SetLayeredWindowAttributes(handle, 0, 255, LWA_ALPHA));

            return true;
        }

        bool IsVisible() const {
            return IsWindowVisible(handle) && !hiding;
        }

        void RefreshTheme() {
            Theme();
        }

        void RefreshSettings() {
            suggestions.clear();
            appResults = SearchApps(query, *apps, settings);

            if (callbacks.suggest)
                callbacks.suggest(SuggestionQuery());

            RefreshResults(true);
        }

        void Show() {
            bool visible = IsWindowVisible(handle) != FALSE;
            bool animate = AnimationsEnabled();

            KillTimer(handle, FadeTimerId);
            fading = false;
            hiding = false;
            Layout(true);

            if (clearOnShow)
                SetWindowTextW(input, L"");
            else
                SendMessageW(input, EM_SETSEL, 0, -1);

            if (!visible || !animate)
                SetOpacity(animate ? 0 : 255);

            ShowWindow(handle, SW_SHOW);
            Activate();
            SetFocus(input);
            GetCursorPos(&lastMouse);
            ignoreMouse = true;

            if (animate && !hiding && IsWindowVisible(handle) && opacity < 255) {
                RedrawWindow(handle, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
                StartFade(255);
            }
        }

        void Hide(bool clear) {
            if (hiding) {
                clearOnShow = clearOnShow || clear;
                return;
            }

            clearOnShow = clear;
            hiding = true;

            if (IsWindowVisible(handle) && AnimationsEnabled()) {
                StartFade(0);
            } else {
                fadeTo = 0;
                FinishFade();
            }
        }

        void Receive(BackendUpdate update) {
            bool refresh = false;

            if (update.apps) {
                apps = update.apps;
                appResults = SearchApps(query, *apps, settings);
                refresh = true;
            }

            if (update.suggestions) {
                if (settings.googleEnabled && settings.googleSuggestions && update.suggestions->first == GoogleQuery()) {
                    suggestions = std::move(update.suggestions->second);
                    refresh = true;
                }
            }

            for (auto& [id, image] : update.icons) {
                icons[id] = std::move(image);
                drawnIcons.erase(id);
            }

            if (refresh)
                RefreshResults(false);

            if (!update.icons.empty())
                InvalidateRect(handle, nullptr, FALSE);
        }
    };
}
