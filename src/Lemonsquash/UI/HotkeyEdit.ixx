module;

#include "Platform.h"

export module lemonsquash.hotkey_edit;

export import lemonsquash.edit;
import lemonsquash.settings;

export namespace Lemonsquash {
    class HotkeyEdit : public Edit {
        UINT key = VK_SPACE, modifiers = MOD_ALT;

        void RefreshText() {
            SetText(HotkeyText(key, modifiers).c_str());
        }

    protected:
        std::optional<LRESULT> Message(UINT message, WPARAM w, LPARAM l) override {
            if (message == WM_GETDLGCODE && w != VK_TAB && w != VK_ESCAPE)
                return DLGC_WANTALLKEYS | DLGC_WANTCHARS;

            if (message == WM_CHAR || message == WM_SYSCHAR)
                return 0;

            if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && w != VK_TAB && w != VK_ESCAPE) {
                if (w == VK_CONTROL || w == VK_SHIFT || w == VK_MENU || w == VK_LWIN || w == VK_RWIN)
                    return 0;

                UINT flags = (GetKeyState(VK_CONTROL) & 0x8000 ? MOD_CONTROL : 0) |
                    (GetKeyState(VK_MENU) & 0x8000 ? MOD_ALT : 0) |
                    (GetKeyState(VK_SHIFT) & 0x8000 ? MOD_SHIFT : 0) |
                    ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000 ? MOD_WIN : 0);

                SetHotkey(static_cast<UINT>(w), flags);
                return 0;
            }

            return Edit::Message(message, w, l);
        }

    public:
        void Create(HWND owner, int controlId) {
            Edit::Create(owner, controlId, ES_READONLY | ES_AUTOHSCROLL);
            RefreshText();
        }

        void SetHotkey(UINT virtualKey, UINT modifierKeys) {
            key = virtualKey;
            modifiers = modifierKeys;

            if (handle)
                RefreshText();
        }

        UINT Key() const { return key; }
        UINT Modifiers() const { return modifiers; }
    };
}
