#pragma once

#include <core/types/integer.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace uf::trace
{
    // The read counterpart of json-text.hpp, and deliberately not a JSON parser:
    // it walks the exact subset this module WRITES, which is one flat object of
    // string, number, boolean, array and object members on a single line. It
    // recognises structure well enough to find a top-level member and to skip
    // past one it was not asked for, and it decodes nothing.
    //
    // It exists as its own header because a trace read back is checked against
    // the model it was recorded from, and a second scanner would be a second
    // opinion about the format this module owns. Both readers -- the golden-line
    // strip in event.cpp and the replay projection next door -- go through here.

    // One past-the-end index pair naming a half-open range of a line. The range
    // covers the member WHOLE: its quoted key, the colon, and its value.
    struct MemberSpan final
    {
        std::size_t begin{};
        std::size_t end{};
    };

    // Advances past the JSON string starting at `line[start]`, which must be its
    // opening quote, and returns the index just past its closing quote. Nullopt
    // when the string is unterminated.
    [[nodiscard]]
    auto skipString(
        std::string_view line,
        std::size_t      start
    ) noexcept -> std::optional<std::size_t>;

    // Advances past one JSON value starting at `line[start]` and returns the
    // index just past it. Nullopt when the value is malformed or runs off the
    // end.
    [[nodiscard]]
    auto skipValue(
        std::string_view line,
        std::size_t      start
    ) noexcept -> std::optional<std::size_t>;

    // The span of the top-level member named `name`, or nullopt when `line` is
    // not a flat JSON object or holds no such member. Nested members are never
    // returned: a `meta` inside an object is not the line's `meta`.
    [[nodiscard]]
    auto findTopLevelMember(
        std::string_view line,
        std::string_view name
    ) noexcept -> std::optional<MemberSpan>;

    // The value of the top-level string member named `name`, unescaped, or
    // nullopt when there is no such member or its value is not a string.
    //
    // Only the seven named escapes and \u00XX are undone, which is the whole of
    // what `escapeJsonString` can produce. A \u escape naming anything else is a
    // string this module did not write, and is refused rather than guessed at.
    [[nodiscard]]
    auto memberString(
        std::string_view line,
        std::string_view name
    ) -> std::optional<std::string>;

    // The value of the top-level number member named `name` as an unsigned
    // integer, or nullopt when there is no such member, its value is not a bare
    // integer, or it does not fit.
    [[nodiscard]]
    auto memberUnsigned(
        std::string_view line,
        std::string_view name
    ) noexcept -> std::optional<uint64>;
}
