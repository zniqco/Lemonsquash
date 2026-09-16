module;

#include "Platform.h"

export module lemonsquash.label;

export import lemonsquash.control;

export namespace Lemonsquash {
    class Label : public Control {
    public:
        void Create(HWND owner, int controlId, const wchar_t* text, DWORD style = SS_LEFT | SS_CENTERIMAGE) {
            Control::Create(owner, controlId, L"Static", text, style | WS_GROUP);
        }
    };
}
