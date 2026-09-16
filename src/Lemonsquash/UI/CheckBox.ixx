module;

#include "Platform.h"

export module lemonsquash.check_box;

export import lemonsquash.button;

export namespace Lemonsquash {
    class CheckBox : public Button {
    public:
        using Button::Button;

        void Create(HWND owner, int controlId, const wchar_t* text, bool rightAligned = false) {
            Button::Create(owner, controlId, text, BS_AUTOCHECKBOX | (rightAligned ? BS_RIGHTBUTTON : 0));
        }

        bool Checked() const {
            return SendMessageW(handle, BM_GETCHECK, 0, 0) == BST_CHECKED;
        }

        void Check(bool checked) {
            SendMessageW(handle, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
        }
    };
}
