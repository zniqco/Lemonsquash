#include "Platform.h"
#include <commctrl.h>

import lemonsquash.common;
import lemonsquash.app;
import lemonsquash.updater;

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int) {
    using namespace Lemonsquash;

    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        auto args = ParseArguments(GetCommandLineW());
        Handle mutex(CreateMutexW(nullptr, TRUE, L"Lemonsquash"));

        if (!mutex.value)
            winrt::throw_last_error();

        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            bool restarting = args.size() == 2 && args[1] == L"--restart";
            auto wait = restarting ? WaitForSingleObject(mutex, 30000) : WAIT_TIMEOUT;

            if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
                return 0;
        }

        CleanupUpdate();

        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};

        InitCommonControlsEx(&controls);

        App app(instance);
        int result = app.Run();

        ReleaseMutex(mutex);
        return result;
    } catch (...) {
        ShowException(nullptr, L"Lemonsquash could not start");
    }

    return 1;
}
