module;

#include "Platform.h"

export module lemonsquash.notify_icon;

export namespace Lemonsquash {
    class NotifyIcon {
        NOTIFYICONDATAW icon{sizeof(icon)};
        UINT taskbarCreated = 0;

        void Add() {
            auto data = icon;
            Shell_NotifyIconW(NIM_ADD, &data);
            data.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &data);
        }

    public:
        NotifyIcon() = default;

        ~NotifyIcon() {
            Remove();
        }

        NotifyIcon(const NotifyIcon&) = delete;
        NotifyIcon& operator=(const NotifyIcon&) = delete;

        void Create(HWND window, UINT callbackMessage, HICON image, const std::wstring& tooltip) {
            Remove();

            taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

            icon.hWnd = window;
            icon.uID = 1;
            icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
            icon.uCallbackMessage = callbackMessage;
            icon.hIcon = image;

            wcsncpy_s(icon.szTip, tooltip.c_str(), _TRUNCATE);

            Add();
        }

        void Remove() {
            if (!icon.hWnd)
                return;

            Shell_NotifyIconW(NIM_DELETE, &icon);
            icon.hWnd = nullptr;
        }

        void ShowBalloon(const std::wstring& title, const std::wstring& message) const {
            if (!icon.hWnd)
                return;

            NOTIFYICONDATAW notice{sizeof(notice)};
            notice.hWnd = icon.hWnd;
            notice.uID = icon.uID;
            notice.uFlags = NIF_INFO;

            wcsncpy_s(notice.szInfoTitle, title.c_str(), _TRUNCATE);
            wcsncpy_s(notice.szInfo, message.c_str(), _TRUNCATE);

            notice.dwInfoFlags = NIIF_INFO;

            Shell_NotifyIconW(NIM_MODIFY, &notice);
        }

        std::optional<UINT> Message(UINT message, LPARAM lparam, HMENU menu = nullptr, UINT doubleClickCommand = 0) {
            if (!icon.hWnd)
                return {};

            if (message == taskbarCreated && taskbarCreated) {
                Add();

                return 0;
            }

            if (message != icon.uCallbackMessage)
                return {};

            auto event = LOWORD(lparam);

            if (event == WM_LBUTTONDBLCLK)
                return doubleClickCommand;

            if (event == WM_CONTEXTMENU && menu) {
                POINT position{};
                GetCursorPos(&position);
                SetForegroundWindow(icon.hWnd);

                UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, position.x, position.y, 0, icon.hWnd, nullptr);

                if (icon.hWnd)
                    PostMessageW(icon.hWnd, WM_NULL, 0, 0);

                return command;
            }

            return 0;
        }
    };
}
