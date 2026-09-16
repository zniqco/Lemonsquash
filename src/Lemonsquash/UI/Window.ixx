module;

#include "Platform.h"
#include <dwmapi.h>

export module lemonsquash.window;

export namespace Lemonsquash {
    class Window {
    protected:
        HINSTANCE instance;
        HWND handle = nullptr;

        explicit Window(HINSTANCE module) : instance(module) {}

        ~Window() = default;

        virtual void OnClosed() {}

        static int Pixels(int value, UINT dpi) {
            return MulDiv(value, static_cast<int>(dpi), 96);
        }

        void SetBounds(const RECT& rect) const {
            SetWindowPos(handle, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }

    public:
        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;

        void SetDarkTitleBar(bool dark) const {
            if (handle) {
                BOOL enabled = dark;
                DwmSetWindowAttribute(handle, DWMWA_USE_IMMERSIVE_DARK_MODE, &enabled, sizeof(enabled));
            }
        }

        bool Destroy() {
            return handle && DestroyWindow(handle) != FALSE;
        }

        void Close() {
            if (Destroy())
                OnClosed();
        }

        HWND Handle() const {
            return handle;
        }

        bool IsOpen() const {
            return handle != nullptr;
        }

        bool Activate() const {
            if (!handle)
                return false;

            SetForegroundWindow(handle);
            return true;
        }
    };
}
