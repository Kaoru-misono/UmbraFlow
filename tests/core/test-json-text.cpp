#include <core/text/json-text.hpp>

#include <core/types/integer.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf
{
    namespace
    {
        // The two RFC 8785 rules C++ implements are held to the same rows as
        // tools/annotate/jcs.py and modules/task/runtime/jcs.luau. The vector
        // file's own header states the format and why it exists.
        constexpr auto k_vectorPath = std::string_view{
            "tests/vectors/jcs-vectors.txt"
        };

        enum class Outcome : uint8
        {
            Bytes,
            Reject,
            Absent
        };

        struct Expectation final
        {
            Outcome     outcome{Outcome::Bytes};
            std::string bytes{};
        };

        struct VectorRow final
        {
            std::string                             kind{};
            std::vector<std::optional<std::string>> fields{};
            std::optional<Expectation>              cpp{};
            std::string                             label{};
        };

        [[nodiscard]]
        auto repositoryRoot() -> std::filesystem::path
        {
            auto source = std::filesystem::path{__FILE__};
            if (source.is_relative())
            {
                source = std::filesystem::absolute(source);
            }
            auto candidate = source.parent_path().parent_path().parent_path();
            if (std::filesystem::is_regular_file(candidate / k_vectorPath))
            {
                return candidate;
            }

            candidate = std::filesystem::current_path();
            while (!candidate.empty())
            {
                if (std::filesystem::is_regular_file(candidate / k_vectorPath))
                {
                    return candidate;
                }
                auto const parent = candidate.parent_path();
                if (parent == candidate)
                {
                    break;
                }
                candidate = parent;
            }

            FAIL("the shared JCS vector file was not found");
            return {};
        }

        [[nodiscard]]
        auto vectorFileLines() -> std::vector<std::string>
        {
            auto stream = std::ifstream{
                repositoryRoot() / k_vectorPath,
                std::ios::binary,
            };
            REQUIRE(stream.good());
            auto const content = std::string{
                std::istreambuf_iterator<char>{stream},
                std::istreambuf_iterator<char>{},
            };

            auto lines = std::vector<std::string>{};
            auto line  = std::string{};
            for (auto const character : content)
            {
                if (character == '\n')
                {
                    lines.push_back(line);
                    line.clear();
                    continue;
                }
                if (character != '\r')
                {
                    line.push_back(character);
                }
            }
            if (!line.empty())
            {
                lines.push_back(line);
            }
            return lines;
        }

        [[nodiscard]]
        auto splitTokens(std::string_view line) -> std::vector<std::string>
        {
            auto tokens = std::vector<std::string>{};
            auto token  = std::string{};
            for (auto const character : line)
            {
                if (character != ' ')
                {
                    token.push_back(character);
                    continue;
                }
                if (!token.empty())
                {
                    tokens.push_back(token);
                    token.clear();
                }
            }
            if (!token.empty())
            {
                tokens.push_back(token);
            }
            return tokens;
        }

        [[nodiscard]]
        auto hexDigit(char character) -> uint32
        {
            if (character >= '0' && character <= '9')
            {
                return static_cast<uint32>(character - '0');
            }
            if (character >= 'a' && character <= 'f')
            {
                return static_cast<uint32>(character - 'a') + 10U;
            }

            FAIL("a vector field carries a digit that is not lowercase hex");
            return 0U;
        }

        // A field is `-` when it does not apply, or `x` followed by an even
        // number of lowercase hex digits, so that the empty byte string is a
        // bare `x` rather than an empty token.
        [[nodiscard]]
        auto decodeField(std::string_view token) -> std::optional<std::string>
        {
            if (token == "-")
            {
                return std::nullopt;
            }
            REQUIRE(token.starts_with('x'));
            auto const digits = token.substr(1U);
            REQUIRE(digits.size() % 2U == 0U);

            auto bytes = std::string{};
            bytes.reserve(digits.size() / 2U);
            for (auto index = std::size_t{0}; index < digits.size(); index += 2U)
            {
                auto const high = hexDigit(digits[index]);
                auto const low  = hexDigit(digits[index + 1U]);
                bytes.push_back(static_cast<char>((high << 4U) | low));
            }
            return bytes;
        }

        [[nodiscard]]
        auto parseOutcome(std::string_view value) -> Expectation
        {
            if (value == "reject")
            {
                return Expectation{.outcome = Outcome::Reject};
            }
            if (value == "absent")
            {
                return Expectation{.outcome = Outcome::Absent};
            }

            auto const bytes = decodeField(value);
            REQUIRE(bytes.has_value());
            return Expectation{.outcome = Outcome::Bytes, .bytes = *bytes};
        }

        // A row kind no consumer recognizes must fail rather than be skipped:
        // a silently ignored row is a vector that asserts nothing.
        constexpr auto k_rowKinds = std::array{
            std::string_view{"string"},
            std::string_view{"order"},
            std::string_view{"integer"},
            std::string_view{"double"},
            std::string_view{"empty-object"},
            std::string_view{"empty-array"},
        };

        // The rules C++ has no entry point for at all. Trace and the Operator
        // canonicalizer format their numbers inline with std::format and their
        // containers as literal braces, so nothing here can be called.
        constexpr auto k_rowKindsWithoutCpp = std::array{
            std::string_view{"integer"},
            std::string_view{"double"},
            std::string_view{"empty-object"},
            std::string_view{"empty-array"},
        };

        [[nodiscard]]
        auto parseRow(std::vector<std::string> const& tokens) -> VectorRow
        {
            REQUIRE(std::ranges::contains(k_rowKinds, tokens[0]));
            auto row      = VectorRow{.kind = tokens[0]};
            auto labelled = false;
            for (auto index = std::size_t{1}; index < tokens.size(); ++index)
            {
                auto const& token = tokens[index];
                if (labelled)
                {
                    row.label += row.label.empty() ? "" : " ";
                    row.label += token;
                    continue;
                }
                if (token == "#")
                {
                    labelled = true;
                    continue;
                }

                auto const assignment = token.find('=');
                if (assignment == std::string::npos)
                {
                    row.fields.push_back(decodeField(token));
                    continue;
                }

                auto const name = token.substr(0U, assignment);
                // An unknown implementation name must fail rather than be
                // ignored: a typo would otherwise silently drop the only
                // expectation the row carries for one of the three.
                REQUIRE((name == "cpp" || name == "py" || name == "luau"));
                if (name == "cpp")
                {
                    row.cpp = parseOutcome(token.substr(assignment + 1U));
                }
            }
            return row;
        }

        [[nodiscard]]
        auto parseVectorFile() -> std::vector<VectorRow>
        {
            auto rows          = std::vector<VectorRow>{};
            auto declaredCount = std::optional<std::size_t>{};
            for (auto const& line : vectorFileLines())
            {
                if (line.empty() || line.starts_with('#'))
                {
                    continue;
                }

                auto const tokens = splitTokens(line);
                REQUIRE(!tokens.empty());
                if (tokens[0] == "count")
                {
                    REQUIRE(!declaredCount.has_value());
                    REQUIRE(tokens.size() == 2U);
                    declaredCount = std::stoull(tokens[1]);
                    continue;
                }

                auto row = parseRow(tokens);
                REQUIRE(!row.fields.empty());
                rows.push_back(std::move(row));
            }

            // A vector file that failed to load, or loaded short, would make
            // every assertion below pass by having nothing to assert.
            REQUIRE(declaredCount.has_value());
            REQUIRE(rows.size() == *declaredCount);
            return rows;
        }

        [[nodiscard]]
        auto escaped(std::string_view value) -> std::string
        {
            auto output = std::string{};
            appendJsonString(output, value);
            return output;
        }
    }

    TEST_CASE("JSON string escaping matches the shared RFC 8785 vectors")
    {
        auto checked = std::size_t{0};
        for (auto const& row : parseVectorFile())
        {
            if (row.kind != "string")
            {
                continue;
            }
            CAPTURE(row.label);

            // C++ has no rejection path here at all -- appendJsonString copies
            // every byte it does not escape straight through -- so a row
            // claiming one would describe an implementation that does not
            // exist.
            auto const outcome = row.cpp.value_or(Expectation{}).outcome;
            REQUIRE(outcome != Outcome::Reject);
            if (outcome == Outcome::Absent)
            {
                continue;
            }

            REQUIRE(row.fields.size() == 2U);
            REQUIRE(row.fields[0].has_value());
            auto const expected = row.cpp.has_value()
                ? std::optional<std::string>{row.cpp->bytes}
                : row.fields[1];
            REQUIRE(expected.has_value());
            CHECK(escaped(*row.fields[0]) == *expected);
            ++checked;
        }
        CHECK(checked > 0U);
    }

    TEST_CASE("JSON member ordering matches the shared RFC 8785 vectors")
    {
        auto checked = std::size_t{0};
        for (auto const& row : parseVectorFile())
        {
            if (row.kind != "order")
            {
                continue;
            }
            CAPTURE(row.label);
            auto const outcome = row.cpp.value_or(Expectation{}).outcome;
            REQUIRE(outcome != Outcome::Reject);
            if (outcome == Outcome::Absent)
            {
                continue;
            }

            // Field 0 is the document Python and Luau must emit. C++ has no
            // value-tree encoder, so what is under test here is the name
            // sequence and that document is deliberately unused.
            auto names = std::vector<std::string>{};
            for (auto const& field : row.fields | std::views::drop(1))
            {
                REQUIRE(field.has_value());
                names.push_back(*field);
            }
            REQUIRE(names.size() >= 2U);

            for (auto index = std::size_t{1}; index < names.size(); ++index)
            {
                CHECK(jsonMemberNameLess(names[index - 1U], names[index]));
                CHECK(!jsonMemberNameLess(names[index], names[index - 1U]));
            }

            auto sorted = names;
            std::ranges::reverse(sorted);
            std::ranges::stable_sort(sorted, jsonMemberNameLess);
            CHECK(sorted == names);
            ++checked;
        }
        CHECK(checked > 0U);
    }

    TEST_CASE("the vectors record every JCS rule C++ does not implement")
    {
        // What does not exist is recorded as deliberately as what does: no C++
        // entry point formats a number or frames a container, and
        // modules/cli's explore-protocol.cpp `{:.17g}` is not this format. A row
        // claiming otherwise would describe an entry point nothing could call.
        auto checked = std::size_t{0};
        for (auto const& row : parseVectorFile())
        {
            if (!std::ranges::contains(k_rowKindsWithoutCpp, row.kind))
            {
                continue;
            }
            CAPTURE(row.label);
            REQUIRE(row.cpp.has_value());
            CHECK(row.cpp->outcome == Outcome::Absent);
            ++checked;
        }
        CHECK(checked > 0U);
    }

    TEST_CASE("JSON string escaping appends rather than replaces")
    {
        auto output = std::string{"{\"k\":"};
        appendJsonString(output, "v");
        output += '}';
        CHECK(output == "{\"k\":\"v\"}");
    }
}
