module;

#include "Platform.h"
#include <span>
#include <sstream>
#include <utility>

export module lemonsquash.settings;

export import lemonsquash.common;
export import lemonsquash.theme;

export namespace Lemonsquash {
    struct Settings {
        UINT modifiers = MOD_ALT, key = VK_SPACE;
        int position = 0;
        bool presentation = true, tutorial = true;
        bool shortcutEnabled = true, calculatorEnabled = true, googleEnabled = true, executeEnabled = true;
        bool googleSuggestions = true;
        ThemePreference theme = ThemePreference::System;

        static std::span<const std::pair<const wchar_t*, UINT>> ModifierKeys() {
            static constexpr std::pair<const wchar_t*, UINT> keys[]{
                {L"HotKeyControl", MOD_CONTROL},
                {L"HotKeyAlt", MOD_ALT},
                {L"HotKeyShift", MOD_SHIFT},
                {L"HotKeyWindowsKey", MOD_WIN},
            };

            return keys;
        }

        static std::span<const std::pair<const wchar_t*, bool Settings::*>> FlagKeys() {
            static constexpr std::pair<const wchar_t*, bool Settings::*> keys[]{
                {L"DetectPresentationMode", &Settings::presentation},
                {L"ShowTutorial", &Settings::tutorial},
                {L"FunctionShortcut", &Settings::shortcutEnabled},
                {L"FunctionCalculator", &Settings::calculatorEnabled},
                {L"FunctionGoogle", &Settings::googleEnabled},
                {L"FunctionGoogleSuggestions", &Settings::googleSuggestions},
                {L"FunctionExecute", &Settings::executeEnabled},
            };

            return keys;
        }

        static Settings Parse(std::wstring_view text) {
            Settings s;
            std::wistringstream stream{std::wstring(text)};
            std::wstring line;

            while (std::getline(stream, line)) {
                if (!line.empty() && line.front() == 0xfeff)
                    line.erase(line.begin());

                line = Trim(line);

                if (line.empty() || line.front() == L'#')
                    continue;

                auto equals = line.find(L'=');

                if (equals == std::wstring::npos)
                    continue;

                auto key = Trim(line.substr(0, equals)), value = Trim(line.substr(equals + 1));
                auto boolean = Lower(value);

                if (boolean == L"true" || boolean == L"false") {
                    bool enabled = boolean == L"true";

                    for (const auto& [name, modifier] : ModifierKeys()) {
                        if (key == name)
                            s.modifiers = enabled ? s.modifiers | modifier : s.modifiers & ~modifier;
                    }

                    for (const auto& [name, field] : FlagKeys()) {
                        if (key == name)
                            s.*field = enabled;
                    }

                    continue;
                }

                try {
                    size_t used;
                    int n = std::stoi(value, &used);

                    if (used != value.size())
                        continue;

                    if (key == L"HotKeyCode" && n >= 1 && n <= 254)
                        s.key = static_cast<UINT>(n);
                    else if (key == L"ShowPosition" && n >= 0 && n <= 2)
                        s.position = n;
                    else if (key == L"Theme" && n >= 0 && n <= 2)
                        s.theme = static_cast<ThemePreference>(n);
                } catch (...) {
                }
            }

            return s;
        }

        std::wstring Serialize() const {
            std::wostringstream out;
            auto boolean = [&](const wchar_t* name, bool value) {
                out << name << L'=' << (value ? L"True" : L"False") << L"\r\n";
            };

            for (const auto& [name, modifier] : ModifierKeys())
                boolean(name, (modifiers & modifier) != 0);

            out << L"HotKeyCode=" << key << L"\r\nShowPosition=" << position << L"\r\n";

            for (const auto& [name, field] : FlagKeys())
                boolean(name, this->*field);

            out << L"Theme=" << static_cast<int>(theme) << L"\r\n";

            return out.str();
        }

        static Settings Load() {
            std::ifstream in(AppDataPath() / L"lemonsquash.properties", std::ios::binary);

            if (!in)
                return {};

            std::string text((std::istreambuf_iterator<char>(in)), {});

            try {
                return Parse(UTF16(text));
            } catch (...) {
                return {};
            }
        }

        void Save() const {
            auto target = AppDataPath() / L"lemonsquash.properties";
            auto temporary = target;
            temporary += L".tmp";
            {
                std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                out << UTF8(Serialize());
                out.flush();

                if (!out)
                    throw std::runtime_error("Cannot save preferences");
            }

            winrt::check_bool(
                MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
        }
    };

    bool StartupEnabled() {
        std::vector<wchar_t> value(32768);
        DWORD bytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));

        if (RegGetValueW(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", L"Lemonsquash",
            RRF_RT_REG_SZ, nullptr, value.data(), &bytes) != ERROR_SUCCESS)
            return false;

        return value.data() == QuoteArgument(ExecutablePath().wstring());
    }

    void SetStartup(bool enabled) {
        HKEY key = nullptr;
        auto error = RegCreateKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0,
            KEY_SET_VALUE, nullptr, &key, nullptr);

        if (error != ERROR_SUCCESS)
            winrt::throw_hresult(HRESULT_FROM_WIN32(error));

        if (enabled) {
            auto value = QuoteArgument(ExecutablePath().wstring());
            error = RegSetValueExW(key, L"Lemonsquash", 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        } else {
            error = RegDeleteValueW(key, L"Lemonsquash");
        }

        RegCloseKey(key);

        if (error != ERROR_SUCCESS && error != ERROR_FILE_NOT_FOUND)
            winrt::throw_hresult(HRESULT_FROM_WIN32(error));
    }

    std::wstring HotkeyText(UINT key, UINT modifiers) {
        wchar_t name[80]{};
        UINT scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC);

        if (key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN || key == VK_INSERT ||
            key == VK_DELETE || key == VK_HOME || key == VK_END || key == VK_PRIOR || key == VK_NEXT)
            scan |= 0x100;

        GetKeyNameTextW(static_cast<LONG>(scan << 16), name, 80);

        return std::wstring(modifiers & MOD_CONTROL ? L"Ctrl+" : L"") + (modifiers & MOD_ALT ? L"Alt+" : L"") +
            (modifiers & MOD_SHIFT ? L"Shift+" : L"") + (modifiers & MOD_WIN ? L"Win+" : L"") +
            (name[0] ? std::wstring(name) : std::to_wstring(key));
    }
}
