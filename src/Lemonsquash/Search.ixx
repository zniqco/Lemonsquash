module;

#include "Platform.h"
#include <array>

export module lemonsquash.search;

export import lemonsquash.common;
import lemonsquash.settings;
import lemonsquash.calculator;

namespace Lemonsquash {
    namespace {
        constexpr size_t MaxQueryLength = 512;
        constexpr size_t MaxSearchTextLength = 4096;
        constexpr size_t MaxAppResults = 10;
        constexpr size_t MaxGoogleResults = 5;

        int SeparatorBonus(wchar_t character) {
            if (character == L'/' || character == L'\\')
                return 5;

            return std::wstring_view(L"_-. '\":").find(character) != std::wstring_view::npos ? 4 : 0;
        }
    }
}

export namespace Lemonsquash {
    enum class Action {
        Shortcut,
        Appx,
        ControlPanel,
        Calculator,
        Google,
        Shell,
        Preferences,
        About,
        Restart
    };

    struct AppEntry {
        std::wstring id;
        std::wstring caption;
        std::wstring tag;
        std::wstring target;
        std::wstring iconPath;
        std::wstring lowerCaption;
        std::wstring lowerTag;
        Action action = Action::Shortcut;
        bool hasArguments = false;
        std::wstring resolvedTarget;
    };

    struct Result {
        std::wstring id;
        std::wstring caption;
        std::wstring description;
        std::wstring target;
        std::wstring iconPath;
        Action action = Action::Shell;
        bool rewrite = false;
        std::wstring resolvedTarget;
        bool operator==(const Result&) const = default;
    };

    int ComputeScore(std::wstring_view query, std::wstring_view lowerQuery, std::wstring_view text,
        std::wstring_view lowerText) {
        if (query.empty() || text.size() < query.size() || query.size() != lowerQuery.size() ||
            text.size() != lowerText.size() || query.size() > MaxQueryLength || text.size() > MaxSearchTextLength)
            return 0;

        thread_local std::vector<int> buffer;
        buffer.resize(text.size() * 4);

        int* previousScores = buffer.data();
        int* currentScores = previousScores + text.size();
        int* previousMatchLengths = currentScores + text.size();
        int* currentMatchLengths = previousMatchLengths + text.size();

        for (size_t queryIndex = 0; queryIndex < query.size(); ++queryIndex) {
            for (size_t textIndex = 0; textIndex < text.size(); ++textIndex) {
                int skippedScore = textIndex ? currentScores[textIndex - 1] : 0;
                bool hasPreviousCharacter = queryIndex > 0 && textIndex > 0;
                int precedingScore = hasPreviousCharacter ? previousScores[textIndex - 1] : 0;
                int consecutiveMatches = hasPreviousCharacter ? previousMatchLengths[textIndex - 1] : 0;
                int characterScore = 0;

                if ((precedingScore != 0 || queryIndex == 0) && lowerQuery[queryIndex] == lowerText[textIndex]) {
                    characterScore = 1 + consecutiveMatches * 5 + (query[queryIndex] == text[textIndex] ? 1 : 0);

                    if (textIndex == 0) {
                        characterScore += 8;
                    } else {
                        auto boundaryBonus = SeparatorBonus(text[textIndex - 1]);
                        if (boundaryBonus > 0)
                            characterScore += boundaryBonus;
                        else if (text[textIndex] != lowerText[textIndex] && consecutiveMatches == 0)
                            characterScore += 2;
                    }
                }

                bool useMatch = characterScore != 0 && precedingScore + characterScore >= skippedScore;
                currentScores[textIndex] = useMatch ? precedingScore + characterScore : skippedScore;
                currentMatchLengths[textIndex] = useMatch ? consecutiveMatches + 1 : 0;
            }
            std::swap(previousScores, currentScores);
            std::swap(previousMatchLengths, currentMatchLengths);
        }

        return previousScores[text.size() - 1] - static_cast<int>(text.size());
    }
}

namespace Lemonsquash {
    namespace {
        struct AppMatch {
            int score;
            bool matchesOnlyTagWithArguments;
            const AppEntry* app;
        };

        std::vector<Result> FindAppResults(const std::wstring& query, const std::vector<AppEntry>& apps) {
            auto lowerQuery = Lower(query);
            std::vector<AppMatch> matches;

            for (const auto& app : apps) {
                int score = ComputeScore(query, lowerQuery, app.caption, app.lowerCaption);
                bool matchesOnlyTagWithArguments = app.hasArguments && score <= 0;

                if (!app.tag.empty())
                    score = std::max(score, ComputeScore(query, lowerQuery, app.tag, app.lowerTag) * 2 / 3);

                score -= app.hasArguments ? static_cast<int>(app.tag.size()) : 0;

                if (score > 0)
                    matches.push_back({score, matchesOnlyTagWithArguments, &app});
            }

            std::stable_sort(matches.begin(), matches.end(), [](const AppMatch& left, const AppMatch& right) {
                if (left.matchesOnlyTagWithArguments != right.matchesOnlyTagWithArguments)
                    return !left.matchesOnlyTagWithArguments;

                return left.score > right.score;
            });

            std::vector<Result> results;
            for (size_t index = 0; index < std::min(MaxAppResults, matches.size()); ++index) {
                const auto& app = *matches[index].app;
                results.push_back({app.id, app.caption, {}, app.target, app.iconPath, app.action, false, app.resolvedTarget});
            }

            return results;
        }

        std::vector<Result> FindCalculatorResults(const std::wstring& input) {
            bool explicitlyRequested = input.front() == L'=';
            auto expression = Trim(explicitlyRequested ? input.substr(1) : input);

            if (expression.size() < 3 && !explicitlyRequested)
                return {};

            auto calculation = Calculate(expression);

            if (!explicitlyRequested && (!calculation.success || calculation.text == expression))
                return {};

            return {{L"calculator",
                calculation.text,
                calculation.success ? L"Copy value" : L"Invalid expression",
                calculation.text,
                {},
                Action::Calculator}};
        }

        std::vector<Result> FindGoogleResults(const std::wstring& query, const std::vector<std::wstring>& suggestions) {
            if (Trim(query).empty())
                return {};

            std::vector<Result> results{
                {L"google:" + query, query, L"Search in Google", query, {}, Action::Google, true}};

            for (const auto& suggestion : suggestions) {
                if (suggestion != query && results.size() < MaxGoogleResults)
                    results.push_back(
                        {L"google:" + suggestion, suggestion, L"Search in Google", suggestion, {}, Action::Google, true});
            }

            return results;
        }

        std::vector<Result> FindCommandResults(const std::wstring& input) {
            if (input.front() != L'/')
                return {};

            const Result commands[] = {
                {L"preferences", L"/preferences", L"Open preferences", {}, {}, Action::Preferences, true},
                {L"about", L"/about", L"About Lemonsquash", {}, {}, Action::About, true},
                {L"restart", L"/restart", L"Restart Lemonsquash", {}, {}, Action::Restart, true}};

            auto lowerInput = Lower(input);
            std::vector<Result> results;
            for (const auto& command : commands)
                if (command.caption.starts_with(lowerInput))
                    results.push_back(command);

            return results;
        }

        std::vector<Result> FindShellResults(const std::wstring& command) {
            if (Trim(command).empty())
                return {};

            return {{L"shell:" + command, command, {}, command, {}, Action::Shell}};
        }

        struct ResultGroup {
            wchar_t prefix;
            std::vector<Result> results;
        };
    }
}

export namespace Lemonsquash {
    std::vector<Result> SearchApps(const std::wstring& input, const std::vector<AppEntry>& apps, const Settings& settings) {
        if (input.empty() || !settings.shortcutEnabled)
            return {};

        return FindAppResults(input.front() == L'#' ? input.substr(1) : input, apps);
    }

    std::vector<Result> Search(const std::wstring& input, const std::vector<Result>& appResults, const Settings& settings,
        const std::vector<std::wstring>& suggestions = {}) {
        if (input.empty())
            return {};

        wchar_t prefix = input.front();
        if (prefix == L'?' && settings.googleEnabled && !settings.googleSuggestions)
            return FindGoogleResults(input.substr(1), {});

        std::array<ResultGroup, 5> groups{{
            {L'#', appResults},
            {L'=', settings.calculatorEnabled ? FindCalculatorResults(input) : std::vector<Result>{}},
            {L'?', settings.googleEnabled ? FindGoogleResults(prefix == L'?' ? input.substr(1) : input,
                settings.googleSuggestions ? suggestions : std::vector<std::wstring>{}) : std::vector<Result>{}},
            {L'/', FindCommandResults(input)},
            {L'>', settings.executeEnabled ? FindShellResults(prefix == L'>' ? input.substr(1) : input) : std::vector<Result>{}},
        }};

        std::vector<Result> results;
        auto appendGroup = [&](const ResultGroup& group) {
            results.insert(results.end(), group.results.begin(), group.results.end());
        };

        for (const auto& group : groups)
            if (group.prefix == prefix)
                appendGroup(group);

        for (const auto& group : groups)
            if (group.prefix != prefix)
                appendGroup(group);

        return results;
    }
}
