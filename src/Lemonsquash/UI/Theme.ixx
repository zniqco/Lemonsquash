module;

#include "Platform.h"

export module lemonsquash.theme;

namespace Lemonsquash {
    namespace {
        bool IsDarkTheme() {
            DWORD appsUseLightTheme = 1;
            DWORD valueSize = sizeof(appsUseLightTheme);

            RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &appsUseLightTheme, &valueSize);

            return !appsUseLightTheme;
        }

        bool IsHighContrast() {
            HIGHCONTRASTW value{sizeof(value)};

            return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(value), &value, 0) && (value.dwFlags & HCF_HIGHCONTRASTON);
        }
    }
}

export namespace Lemonsquash {
    enum class ThemePreference {
        System = 0,
        Light = 1,
        Dark = 2
    };

    enum class ThemeMode {
        Light,
        Dark,
        HighContrast
    };

    struct ThemePalette {
        COLORREF background = 0, surface = 0, field = 0;
        COLORREF text = 0, mutedText = 0, disabledText = 0;
        COLORREF border = 0, separator = 0, hover = 0;
        COLORREF selection = 0, selectionText = 0, selectionMutedText = 0;
        COLORREF accent = 0, accentText = 0, placeholder = 0;
    };

    struct ThemeState {
        ThemeMode mode = ThemeMode::Light;
        ThemePalette colors;
    };

    bool IsThemeSettingChange(WPARAM w, LPARAM l) {
        if (w == SPI_SETHIGHCONTRAST)
            return true;

        auto area = reinterpret_cast<const wchar_t*>(l);

        return area && CompareStringOrdinal(area, -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL;
    }

    ThemeState ResolveTheme(ThemePreference preference) {
        ThemeState state;

        if (IsHighContrast())
            state.mode = ThemeMode::HighContrast;
        else if (preference == ThemePreference::Dark)
            state.mode = ThemeMode::Dark;
        else if (preference == ThemePreference::System && IsDarkTheme())
            state.mode = ThemeMode::Dark;

        if (state.mode == ThemeMode::HighContrast) {
            state.colors = {
                .background = GetSysColor(COLOR_WINDOW),
                .surface = GetSysColor(COLOR_WINDOW),
                .field = GetSysColor(COLOR_WINDOW),
                .text = GetSysColor(COLOR_WINDOWTEXT),
                .mutedText = GetSysColor(COLOR_WINDOWTEXT),
                .disabledText = GetSysColor(COLOR_GRAYTEXT),
                .border = GetSysColor(COLOR_WINDOWTEXT),
                .separator = GetSysColor(COLOR_WINDOWTEXT),
                .hover = GetSysColor(COLOR_HIGHLIGHT),
                .selection = GetSysColor(COLOR_HIGHLIGHT),
                .selectionText = GetSysColor(COLOR_HIGHLIGHTTEXT),
                .selectionMutedText = GetSysColor(COLOR_HIGHLIGHTTEXT),
                .accent = GetSysColor(COLOR_HIGHLIGHT),
                .accentText = GetSysColor(COLOR_HIGHLIGHTTEXT),
                .placeholder = GetSysColor(COLOR_WINDOWTEXT),
            };
        } else if (state.mode == ThemeMode::Dark) {
            state.colors = {
                .background = RGB(31, 31, 31),
                .surface = RGB(32, 32, 32),
                .field = RGB(43, 43, 43),
                .text = RGB(241, 241, 241),
                .mutedText = RGB(170, 170, 170),
                .disabledText = RGB(128, 128, 128),
                .border = RGB(90, 90, 90),
                .separator = RGB(60, 60, 60),
                .hover = RGB(54, 54, 54),
                .selection = RGB(54, 54, 54),
                .selectionText = RGB(241, 241, 241),
                .selectionMutedText = RGB(170, 170, 170),
                .accent = RGB(160, 160, 160),
                .accentText = RGB(31, 31, 31),
                .placeholder = RGB(74, 74, 74),
            };
        } else {
            state.colors = {
                .background = RGB(248, 248, 248),
                .surface = RGB(255, 255, 255),
                .field = RGB(255, 255, 255),
                .text = RGB(34, 34, 34),
                .mutedText = RGB(100, 100, 100),
                .disabledText = RGB(128, 128, 128),
                .border = RGB(160, 160, 160),
                .separator = RGB(225, 225, 225),
                .hover = RGB(238, 238, 238),
                .selection = RGB(224, 224, 224),
                .selectionText = RGB(34, 34, 34),
                .selectionMutedText = RGB(100, 100, 100),
                .accent = RGB(96, 96, 96),
                .accentText = RGB(255, 255, 255),
                .placeholder = RGB(220, 220, 220),
            };
        }

        return state;
    }
}
