module;

#include "Platform.h"
#include <commctrl.h>

export module lemonsquash.link_image;

export import lemonsquash.control;
import lemonsquash.bitmap;

export namespace Lemonsquash {
    class LinkImage : public Control {
        LazyResource<HBITMAP> bitmap;
        std::wstring fallback;
        std::function<void()> clicked;

        void Draw(const DRAWITEMSTRUCT& draw) const {
            HBITMAP image = nullptr;

            try {
                image = bitmap.Get();
            } catch (...) {
            }

            int saved = SaveDC(draw.hDC);

            SetDCBrushColor(draw.hDC, BackgroundColor());
            FillRect(draw.hDC, &draw.rcItem, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));

            BITMAP size{};

            if (image && GetObjectW(image, sizeof(size), &size)) {
                auto dc = CreateCompatibleDC(draw.hDC);

                if (dc) {
                    auto previous = SelectObject(dc, image);
                    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};

                    AlphaBlend(draw.hDC, draw.rcItem.left, draw.rcItem.top, draw.rcItem.right - draw.rcItem.left,
                        draw.rcItem.bottom - draw.rcItem.top, dc, 0, 0, size.bmWidth, size.bmHeight, blend);
                    SelectObject(dc, previous);
                    DeleteDC(dc);
                }
            } else {
                auto rect = draw.rcItem;

                SetTextColor(draw.hDC, theme.colors.text);
                SetBkMode(draw.hDC, TRANSPARENT);
                DrawTextW(draw.hDC, fallback.c_str(), -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }

            RestoreDC(draw.hDC, saved);
        }

    protected:
        std::optional<LRESULT> Message(UINT message, WPARAM, LPARAM l) override {
            if (message == WM_SETCURSOR && LOWORD(l) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }

            return {};
        }

        std::optional<LRESULT> ParentMessage(UINT message, WPARAM w, LPARAM l) override {
            if (message == WM_DRAWITEM) {
                Draw(*reinterpret_cast<DRAWITEMSTRUCT*>(l));
                return TRUE;
            }

            if (message == WM_COMMAND && HIWORD(w) == STN_CLICKED) {
                if (clicked)
                    clicked();

                return TRUE;
            }

            return {};
        }

        void OnDestroyed() noexcept override {
            bitmap.Reset();
        }

    public:
        explicit LinkImage(std::function<void()> onClicked = {})
            : clicked(std::move(onClicked)) {}

        ~LinkImage() {
            Destroy();
        }

        void Create(HWND owner, int controlId, const wchar_t* text, const wchar_t* fallbackText) {
            fallback = fallbackText;

            Control::Create(owner, controlId, L"Static", text, SS_OWNERDRAW | SS_NOTIFY);
        }

        void SetImageFromResource(int resourceId) {
            bitmap = LazyResource<HBITMAP>([module = instance, resourceId] {
                return Bitmap::LoadFromResource(module, resourceId).ToGdiBitmap();
            }, [](HBITMAP image) noexcept { DeleteObject(image); });

            if (handle)
                InvalidateRect(handle, nullptr, FALSE);
        }
    };
}
