module;

#include "Platform.h"

export module lemonsquash.monitor;

export namespace Lemonsquash {
    class Monitor {
        RECT rect{};
        RECT workArea{};

        explicit Monitor(HMONITOR handle) {
            MONITORINFO info{sizeof(info)};
            winrt::check_bool(GetMonitorInfoW(handle, &info));

            rect = info.rcMonitor;
            workArea = info.rcWork;
        }

    public:
        static Monitor Primary() {
            return FromPoint({0, 0});
        }

        static Monitor FromPoint(POINT point) {
            return Monitor(MonitorFromPoint(point, MONITOR_DEFAULTTOPRIMARY));
        }

        static Monitor FromCursor() {
            POINT cursor{};
            GetCursorPos(&cursor);

            return FromPoint(cursor);
        }

        static Monitor FromWindow(HWND window) {
            return Monitor(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY));
        }

        RECT Rect() const {
            return rect;
        }

        RECT WorkArea() const {
            return workArea;
        }
    };
}
