module;

#include "Platform.h"
#include <oleauto.h>
#include <cmath>
#include <cwctype>
#include <numbers>
#include <utility>

export module lemonsquash.calculator;

export import lemonsquash.common;

namespace Lemonsquash {
    namespace {
        constexpr unsigned MaxParseDepth = 128;
        constexpr size_t MaxExpressionLength = 2048;

        struct CalcError {
            std::wstring text;
        };

        template<typename T>
        struct [[nodiscard]] CalcResult {
            bool success = false;
            T value{};
            CalcError error;

            CalcResult(T value)
                : success(true), value(std::move(value)) {}

            CalcResult(CalcError error)
                : error(std::move(error)) {}

            explicit operator bool() const {
                return success;
            }
        };

        template<typename T>
        CalcResult<T> DecimalResult(HRESULT status, T value) {
            if (FAILED(status))
                return CalcError{status == DISP_E_DIVBYZERO ? L"Divide by 0" : L"Overflow"};

            return value;
        }

        CalcResult<DECIMAL> DecimalFromInteger(int64_t value) {
            DECIMAL result{};
            auto status = VarDecFromI8(value, &result);

            return DecimalResult(status, result);
        }

        CalcResult<DECIMAL> DecimalFromDouble(double value) {
            DECIMAL result{};

            if (!std::isfinite(value))
                return CalcError{L"Overflow"};

            auto status = VarDecFromR8(value, &result);

            return DecimalResult(status, result);
        }

        CalcResult<double> DecimalToDouble(const DECIMAL& value) {
            double result = 0;
            auto status = VarR8FromDec(&value, &result);

            return DecimalResult(status, result);
        }

        bool IsZero(const DECIMAL& value) {
            return value.Hi32 == 0 && value.Lo64 == 0;
        }

        CalcResult<DECIMAL> Negate(DECIMAL value) {
            DECIMAL result{};
            auto status = VarDecNeg(&value, &result);

            return DecimalResult(status, result);
        }

        CalcResult<DECIMAL> EvaluateFunction(const std::wstring& name, DECIMAL argument) {
            DECIMAL result{};

            if (name == L"abs") {
                auto status = VarDecAbs(&argument, &result);
                return DecimalResult(status, result);
            }

            if (name == L"round") {
                auto status = VarDecRound(&argument, 0, &result);
                return DecimalResult(status, result);
            }

            if (name == L"floor") {
                auto status = VarDecInt(&argument, &result);
                return DecimalResult(status, result);
            }

            if (name == L"ceil") {
                auto negated = Negate(argument);

                if (!negated)
                    return negated;

                auto status = VarDecInt(&negated.value, &result);
                auto integer = DecimalResult(status, result);

                if (!integer)
                    return integer;

                return Negate(result);
            }

            if (name == L"sign") {
                if (IsZero(argument))
                    return DecimalFromInteger(0);

                return DecimalFromInteger(argument.sign & DECIMAL_NEG ? -1 : 1);
            }

            auto converted = DecimalToDouble(argument);
            if (!converted)
                return converted.error;

            double value = converted.value;
            double functionResult;

            if (name == L"sin")
                functionResult = std::sin(value);
            else if (name == L"cos")
                functionResult = std::cos(value);
            else if (name == L"tan")
                functionResult = std::tan(value);
            else if (name == L"asin" || name == L"arcsin")
                functionResult = std::asin(value);
            else if (name == L"acos" || name == L"arccos")
                functionResult = std::acos(value);
            else if (name == L"atan" || name == L"arctan")
                functionResult = std::atan(value);
            else if (name == L"sqrt")
                functionResult = std::sqrt(value);
            else if (name == L"log")
                functionResult = std::log(value);
            else
                return CalcError{L"Unknown function '" + name + L"'"};

            auto convertedResult = DecimalFromDouble(functionResult);

            if (!convertedResult)
                return CalcError{L"Function '" + name + L"': Overflow"};

            return convertedResult;
        }

        class Parser {
            std::wstring text;
            size_t position = 0;
            unsigned parseDepth = 0;

            struct ParseDepthGuard {
                unsigned& depth;

                explicit ParseDepthGuard(unsigned& parseDepth)
                    : depth(parseDepth) {
                    ++depth;
                }

                ~ParseDepthGuard() {
                    --depth;
                }
            };

            wchar_t Peek() {
                while (position < text.size() && iswspace(text[position]))
                    ++position;

                return position == text.size() ? L'\0' : text[position];
            }

            CalcResult<bool> Expect(wchar_t character) {
                if (Peek() != character)
                    return CalcError{L"Expected '" + std::wstring(1, character) + L"' at position " + std::to_wstring(position)};

                ++position;

                return true;
            }

            static bool IsLetter(wchar_t character) {
                return character >= L'a' && character <= L'z';
            }

            CalcResult<DECIMAL> ParseSum() {
                auto value = ParseProduct();

                if (!value)
                    return value;

                while (Peek() == L'+' || Peek() == L'-') {
                    auto operation = text[position++];
                    auto right = ParseProduct();

                    if (!right)
                        return right;

                    DECIMAL result{};
                    auto status = operation == L'+' ? VarDecAdd(&value.value, &right.value, &result)
                        : VarDecSub(&value.value, &right.value, &result);

                    value = DecimalResult(status, result);

                    if (!value)
                        return value;
                }

                return value;
            }

            CalcResult<DECIMAL> ParseProduct() {
                auto value = ParsePower();

                if (!value)
                    return value;

                while (Peek() == L'*' || Peek() == L'/') {
                    auto operation = text[position++];
                    auto right = ParsePower();

                    if (!right)
                        return right;

                    DECIMAL result{};

                    if (operation == L'/' && IsZero(right.value))
                        return CalcError{L"Divide by 0"};

                    auto status = operation == L'*' ? VarDecMul(&value.value, &right.value, &result)
                        : VarDecDiv(&value.value, &right.value, &result);

                    value = DecimalResult(status, result);

                    if (!value)
                        return value;
                }

                return value;
            }

            CalcResult<DECIMAL> ParsePower() {
                ParseDepthGuard guard(parseDepth);

                if (parseDepth > MaxParseDepth)
                    return CalcError{L"Expression is too deeply nested"};

                auto value = ParsePrimary();

                if (!value)
                    return value;

                if (Peek() == L'^') {
                    ++position;

                    auto exponent = ParsePower();

                    if (!exponent)
                        return exponent;

                    auto base = DecimalToDouble(value.value);

                    if (!base)
                        return base.error;

                    auto power = DecimalToDouble(exponent.value);

                    if (!power)
                        return power.error;

                    return DecimalFromDouble(std::pow(base.value, power.value));
                }

                return value;
            }

            CalcResult<DECIMAL> ParsePrimary() {
                ParseDepthGuard guard(parseDepth);

                if (parseDepth > MaxParseDepth)
                    return CalcError{L"Expression is too deeply nested"};

                auto character = Peek();

                if (character == L'+' || character == L'-') {
                    ++position;

                    auto value = ParsePrimary();

                    if (!value)
                        return value;

                    return character == L'-' ? Negate(value.value) : value;
                }

                if (character == L'(') {
                    ++position;

                    auto value = ParseSum();

                    if (!value)
                        return value;

                    if (auto closing = Expect(L')'); !closing)
                        return closing.error;

                    return value;
                }

                if (character == L'0' && position + 1 < text.size() && text[position + 1] == L'x')
                    return ParseHexLiteral();

                if (IsLetter(character))
                    return ParseNamedValue();

                return ParseDecimalLiteral();
            }

            CalcResult<DECIMAL> ParseHexLiteral() {
                position += 2;
                size_t start = position;
                uint64_t value = 0;

                while (position < text.size() && iswxdigit(text[position])) {
                    auto character = text[position++];
                    unsigned digit = character <= L'9' ? character - L'0' : character - L'a' + 10;

                    if (value > (UINT64_MAX - digit) / 16)
                        return CalcError{L"Invalid hex literal"};

                    value = value * 16 + digit;
                }

                if (start == position || (position < text.size() && IsLetter(text[position])))
                    return CalcError{L"Invalid hex literal"};

                int64_t signedValue;
                memcpy(&signedValue, &value, sizeof(value));

                return DecimalFromInteger(signedValue);
            }

            CalcResult<DECIMAL> ParseNamedValue() {
                size_t start = position;

                while (position < text.size() && IsLetter(text[position]))
                    ++position;

                auto name = text.substr(start, position - start);

                if (name == L"pi")
                    return DecimalFromDouble(std::numbers::pi);

                if (name == L"e")
                    return DecimalFromDouble(std::numbers::e);

                if (auto opening = Expect(L'('); !opening)
                    return opening.error;

                auto argument = ParseSum();

                if (!argument)
                    return argument;

                if (auto closing = Expect(L')'); !closing)
                    return closing.error;

                return EvaluateFunction(name, argument.value);
            }

            CalcResult<DECIMAL> ParseDecimalLiteral() {
                size_t start = position;

                while (position < text.size() && ((text[position] >= L'0' && text[position] <= L'9') || text[position] == L'.'))
                    ++position;

                auto number = text.substr(start, position - start);
                DECIMAL result{};

                if (number.empty() || std::count(number.begin(), number.end(), L'.') > 1 || number == L"." ||
                    FAILED(VarDecFromStr(number.data(), LOCALE_INVARIANT, 0, &result)))
                    return CalcError{L"Invalid number '" + number + L"'"};

                return result;
            }

        public:
            explicit Parser(std::wstring_view expression)
                : text(Lower(expression)) {}

            CalcResult<DECIMAL> Parse() {
                if (!Peek())
                    return DecimalFromInteger(0);

                auto value = ParseSum();

                if (!value)
                    return value;

                if (Peek())
                    return CalcError{L"Unexpected character '" + std::wstring(1, text[position]) + L"' at position " +
                        std::to_wstring(position)};

                return value;
            }
        };
    }
}

export namespace Lemonsquash {
    struct Calculation {
        bool success = false;
        std::wstring text;
    };

    Calculation Calculate(std::wstring_view expression) {
        if (expression.size() > MaxExpressionLength)
            return {false, L"Expression is too long"};

        auto value = Parser(expression).Parse();

        if (!value)
            return {false, value.error.text};

        BSTR formattedValue = nullptr;
        auto status = VarBstrFromDec(&value.value, LOCALE_INVARIANT, 0, &formattedValue);
        auto formatted = DecimalResult(status, formattedValue);

        if (!formatted)
            return {false, formatted.error.text};

        std::wstring text(formattedValue, SysStringLen(formattedValue));
        SysFreeString(formattedValue);

        return {true, text};
    }
}
