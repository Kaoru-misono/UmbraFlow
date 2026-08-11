#pragma once

#include "error.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uf::json
{
    class Value;

    // Declared before Value because Value stores a sequence of these, so it
    // cannot follow the definition the way a vocabulary alias normally does.
    // It earns its name: std::pair<std::string, Value> is equally the shape of
    // a lookup result or an unordered couple, and this one is neither -- it is
    // an object member, in the order the document spelled it.
    using Member = std::pair<std::string, Value>;

    enum class ValueKind : uint8
    {
        Null,
        Boolean,
        Number,
        String,
        Array,
        Object,
    };

    // An immutable JSON value.
    //
    // Numbers are IEEE-754 doubles and this is a specification consequence
    // rather than a convenience: RFC 8785 section 3.2.2.3 adopts ES6
    // Number::toString whole, so a canonical JSON document's numbers ARE
    // doubles, and a wider model here would accept literals that no canonical
    // document can spell. Callers whose numbers exceed 2^53 do not have a
    // number problem, they have a canonical-form problem -- see
    // requireExactCanonical.
    //
    // Object members keep the order the document spelled them in, because a
    // document whose members are not already in JCS order is not canonical and
    // this type has to be able to say so.
    class Value final
    {
        // std::monostate is null. The alternatives are in ValueKind order, so
        // index() and the enumerator agree by construction.
        std::variant<
            std::monostate,
            bool,
            double,
            std::string,
            std::vector<Value>,
            std::vector<Member>>
            m_storage{};

    public:
        Value() = default;

        [[nodiscard]] static auto ofBoolean(bool value) -> Value;
        [[nodiscard]] static auto ofNumber(double value) -> Value;
        [[nodiscard]] static auto ofString(std::string value) -> Value;
        [[nodiscard]] static auto ofArray(std::vector<Value> items) -> Value;
        [[nodiscard]] static auto ofObject(std::vector<Member> members) -> Value;

        [[nodiscard]] auto kind() const noexcept -> ValueKind;

        // Each observer answers for its own kind and returns the neutral value
        // of that kind otherwise, so a caller that forgot to check kind() reads
        // false, 0.0, or an empty sequence rather than anything indeterminate.
        [[nodiscard]] auto boolean() const noexcept -> bool;
        [[nodiscard]] auto number() const noexcept -> double;

        [[nodiscard]]
        auto string() const noexcept UF_LIFETIME_BOUND -> std::string_view;

        [[nodiscard]]
        auto items() const noexcept UF_LIFETIME_BOUND -> std::span<Value const>;

        [[nodiscard]]
        auto members() const noexcept UF_LIFETIME_BOUND -> std::span<Member const>;

        // Whether this number is an exact integer, which is the only way a JSON
        // number satisfies `"type": "integer"`.
        [[nodiscard]] auto isInteger() const noexcept -> bool;

        // The member named, or nullptr. A non-owning observation of one value
        // inside this one; it dies with the value it came out of.
        [[nodiscard]]
        auto find(std::string_view name) const UF_LIFETIME_BOUND -> Value const*;

        // Value equality, which is what const, enum and uniqueItems compare.
        // Member order is deliberately not part of it: two spellings of the
        // same object are the same value, and whether one of them is canonical
        // is requireExactCanonical's question.
        [[nodiscard]] auto operator==(Value const& other) const -> bool;
    };

    // Parses one complete JSON document, by RFC 8259 with three deliberate
    // narrowings, each of which a canonical document satisfies already:
    // duplicate member names are refused rather than resolved, a number outside
    // the finite double range is refused rather than folded to infinity, and a
    // string that is not valid UTF-8 once its escapes are resolved is refused.
    [[nodiscard]] auto parse(std::string_view text) -> Result<Value>;

    // The RFC 8785 form of this value. Total: every Value has one, because
    // parse admits no value that lacks one.
    [[nodiscard]] auto canonicalBytes(Value const& value) -> std::string;

    // Whether these exact bytes already are what canonicalBytes would produce.
    // This is the whole of the RFC 8785 obligation a CanonicalJsonValidator
    // carries (modules/operator/source/operator/project-plugin.hpp:90-95), and
    // it is answered by parsing and re-serializing rather than by recognizing
    // shapes, so a document nobody anticipated is judged too.
    [[nodiscard]] auto requireExactCanonical(std::string_view text) -> Status;
}
