#pragma once

#include <core/types/integer.hpp>

#include <span>
#include <string>
#include <string_view>

namespace uf::authoring
{
    // A minimal JSON emitter, because the caller of this tool is a program and
    // a machine-readable answer is the whole point of it existing.
    //
    // Every function returns one complete encoded JSON value, and an object or
    // array is composed from values the leaves already encoded. Encoding
    // therefore happens exactly once, at the leaf, and a string can neither be
    // quoted twice nor be left unquoted -- which matters here because a Windows
    // project root is full of backslashes and reaches stdout on every command.

    // One member of a JSON object. `value` is an already-encoded JSON value from
    // one of the functions below, never raw text.
    struct JsonMember final
    {
        std::string key{};
        std::string value{};
    };

    [[nodiscard]] auto jsonString(std::string_view value) -> std::string;
    [[nodiscard]] auto jsonUnsigned(uint64 value) -> std::string;
    [[nodiscard]] auto jsonBoolean(bool value) -> std::string;
    [[nodiscard]] auto jsonNull() -> std::string;

    // A finite value at three decimals, and null for anything else. JSON has no
    // spelling for an infinity or a NaN, so emitting one would produce a
    // document the caller's parser rejects.
    [[nodiscard]] auto jsonNumber(float value) -> std::string;

    [[nodiscard]]
    auto jsonObject(std::span<JsonMember const> members) -> std::string;

    [[nodiscard]]
    auto jsonArray(std::span<std::string const> values) -> std::string;
}
