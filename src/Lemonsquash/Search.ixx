module;

#include "Platform.h"
#include <array>
#include <cwctype>
#include <limits>

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

        int BoundaryBonus(std::wstring_view text, size_t index) {
            if (index == 0)
                return 8;

            auto previous = text[index - 1];
            if (previous == L'/' || previous == L'\\')
                return 5;
            if (iswspace(previous) || iswpunct(previous))
                return 4;

            // Preserve the display name's casing: fooBar and XMLParser have word starts.
            bool camelStart = iswlower(previous) && iswupper(text[index]);
            bool acronymEnd = iswupper(previous) && iswupper(text[index]) &&
                index + 1 < text.size() && iswlower(text[index + 1]);
            return camelStart || acronymEnd ? 4 : 0;
        }

        std::wstring NormalizeAppQuery(std::wstring_view input) {
            std::wstring query;
            bool pendingSpace = false;
            for (auto character : Trim(input)) {
                if (iswspace(character)) {
                    pendingSpace = !query.empty();
                } else {
                    if (pendingSpace)
                        query += L' ';
                    query += character;
                    pendingSpace = false;
                }
            }
            return query;
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

    struct SearchAlias {
        std::wstring text;
        std::wstring lowerText;
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
        std::vector<SearchAlias> aliases;
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

    // A valid match may have a negative rank. Length tunes ordering, never eligibility.
    std::optional<int> ComputeScore(std::wstring_view query, std::wstring_view lowerQuery, std::wstring_view text,
        std::wstring_view lowerText) {
        if (query.empty() || text.size() < query.size() || query.size() != lowerQuery.size() ||
            text.size() != lowerText.size() || query.size() > MaxQueryLength || text.size() > MaxSearchTextLength)
            return {};

        constexpr int NoMatch = std::numeric_limits<int>::min() / 4;
        constexpr int PrefixBonus = 32, ExactBonus = 64;
        thread_local std::vector<int> buffer;
        buffer.resize(text.size() * 4);
        int* previousScores = buffer.data();
        int* currentScores = previousScores + text.size();
        int* previousMatchLengths = currentScores + text.size();
        int* currentMatchLengths = previousMatchLengths + text.size();

        for (size_t queryIndex = 0; queryIndex < query.size(); ++queryIndex) {
            std::fill_n(currentScores, text.size(), NoMatch);
            std::fill_n(currentMatchLengths, text.size(), 0);
            int bestPreceding = NoMatch;
            for (size_t textIndex = 0; textIndex < text.size(); ++textIndex) {
                if (queryIndex > 0 && textIndex > 0)
                    bestPreceding = std::max(bestPreceding, previousScores[textIndex - 1]);
                if (lowerQuery[queryIndex] != lowerText[textIndex])
                    continue;

                int characterScore = 2 + BoundaryBonus(text, textIndex);
                int score = queryIndex == 0 ? characterScore
                    : bestPreceding == NoMatch ? NoMatch : bestPreceding + characterScore;
                int consecutive = 1;
                if (queryIndex > 0 && textIndex > 0 && previousScores[textIndex - 1] != NoMatch) {
                    int adjacent = previousScores[textIndex - 1] + characterScore +
                        previousMatchLengths[textIndex - 1] * 5;
                    if (adjacent >= score) {
                        score = adjacent;
                        consecutive = previousMatchLengths[textIndex - 1] + 1;
                    }
                }
                currentScores[textIndex] = score;
                currentMatchLengths[textIndex] = score == NoMatch ? 0 : consecutive;
            }
            std::swap(previousScores, currentScores);
            std::swap(previousMatchLengths, currentMatchLengths);
        }

        int score = *std::max_element(previousScores, previousScores + text.size());
        if (score == NoMatch)
            return {};
        if (lowerText.starts_with(lowerQuery))
            score += PrefixBonus;
        if (lowerText == lowerQuery)
            score += ExactBonus;
        // Retain the original one-point-per-character preference for shorter names.
        return score - static_cast<int>(text.size());
    }
}

namespace Lemonsquash {
    namespace {
        bool HasConfidentMatch(std::wstring_view lowerQuery, std::wstring_view text,
            std::wstring_view lowerText, bool packageMetadata) {
            if (lowerText.find(lowerQuery) != std::wstring_view::npos)
                return true;

            constexpr unsigned char InitialsOnly = 1, OneInterior = 2, TwoOrMoreInteriors = 4;
            bool wordQuery = lowerQuery.find(L' ') != std::wstring_view::npos;
            thread_local std::vector<unsigned char> buffer;
            buffer.assign(text.size() * 2, 0);
            auto previous = buffer.data();
            auto current = previous + text.size();
            for (size_t qi = 0; qi < lowerQuery.size(); ++qi) {
                std::fill_n(current, text.size(), unsigned char{});
                unsigned char preceding = 0;
                for (size_t ti = 0; ti < text.size(); ++ti) {
                    if (qi > 0 && ti > 0)
                        preceding |= previous[ti - 1];
                    if (lowerQuery[qi] != lowerText[ti])
                        continue;
                    bool boundary = BoundaryBonus(text, ti) > 0 ||
                        (iswdigit(text[ti]) && (ti == 0 || !iswdigit(text[ti - 1])));
                    bool separator = iswspace(text[ti]) || iswpunct(text[ti]);
                    if (qi == 0) {
                        current[ti] = boundary ? InitialsOnly : 0;
                        continue;
                    }
                    if (boundary || separator)
                        current[ti] = preceding;
                    if (ti > 0) {
                        auto adjacent = previous[ti - 1];
                        if (boundary || separator || (iswdigit(text[ti - 1]) && iswdigit(text[ti]))) {
                            current[ti] |= adjacent;
                        } else {
                            if (adjacent & InitialsOnly) current[ti] |= OneInterior;
                            if (adjacent & (OneInterior | TwoOrMoreInteriors)) current[ti] |= TwoOrMoreInteriors;
                        }
                    }
                }
                std::swap(previous, current);
            }
            unsigned char admitted = !packageMetadata || wordQuery ?
                InitialsOnly | OneInterior | TwoOrMoreInteriors : InitialsOnly | TwoOrMoreInteriors;
            return std::any_of(previous, previous + text.size(),
                [=](auto evidence) { return (evidence & admitted) != 0; });
        }

        struct AppMatch {
            int score;
            bool matchesOnlyTagWithArguments;
            const AppEntry* app;
        };

        std::vector<Result> FindAppResults(const std::wstring& query, const std::vector<AppEntry>& apps) {
            auto lowerQuery = Lower(query);
            std::vector<AppMatch> matches;

            for (const auto& app : apps) {
                auto captionScore = ComputeScore(query, lowerQuery, app.caption, app.lowerCaption);
                if (captionScore && !HasConfidentMatch(lowerQuery, app.caption, app.lowerCaption, false))
                    captionScore.reset();
                auto bestScore = captionScore;
                auto considerAlias = [&](std::wstring_view text, std::wstring_view lowerText, bool packageMetadata = false) {
                    if (auto score = ComputeScore(query, lowerQuery, text, lowerText);
                        score && HasConfidentMatch(lowerQuery, text, lowerText, packageMetadata)) {
                        int weighted = *score * 2 / 3;
                        if (!bestScore || weighted > *bestScore)
                            bestScore = weighted;
                    }
                };
                if (!app.tag.empty())
                    considerAlias(app.tag, app.lowerTag, app.action == Action::Appx || app.action == Action::ControlPanel);
                for (const auto& alias : app.aliases)
                    considerAlias(alias.text, alias.lowerText);

                if (bestScore) {
                    int score = *bestScore - (app.hasArguments ? static_cast<int>(app.tag.size()) : 0);
                    bool matchesOnlyTagWithArguments = app.hasArguments && !captionScore;
                    matches.push_back({score, matchesOnlyTagWithArguments, &app});
                }
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

        auto query = NormalizeAppQuery(input);
        if (!query.empty() && query.front() == L'#')
            query = NormalizeAppQuery(std::wstring_view(query).substr(1));
        return query.empty() ? std::vector<Result>{} : FindAppResults(query, apps);
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
