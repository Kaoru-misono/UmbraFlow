#pragma once

#include <core/error/result.hpp>
#include <core/types/enum-reflection.hpp>
#include <core/types/integer.hpp>

#include <optional>
#include <source_location>
#include <string>

namespace uf::json
{
    // Four kinds because a caller reacts differently to each. uf-chaos's
    // evaluator reported all four as one AutomationErrorKind::InvalidResource,
    // which cannot distinguish "the schema you pinned is not one this evaluator
    // implements" from "the document you handed me is invalid" -- the first is
    // a deployment defect that no document can fix, the second is the answer
    // the caller asked for.
    enum class ErrorKind : uint8
    {
        // The bytes are not a JSON document at all.
        Syntax,
        // The bytes parse, but are not their own RFC 8785 form.
        NotCanonical,
        // The schema is not one this evaluator can apply: an unimplemented
        // keyword, a keyword whose value has the wrong shape, a $ref that
        // resolves to nothing, or an assertion this evaluator's number model
        // cannot represent. Never a statement about any instance.
        SchemaUnsupported,
        // An instance failed a schema this evaluator did apply.
        DocumentRejected,
    };

    [[nodiscard]]
    auto errorKind(Error const& error) noexcept -> std::optional<ErrorKind>;

    [[nodiscard]]
    auto fail(
        ErrorKind kind,
        std::string message,
        std::source_location location = std::source_location::current()
    ) -> std::unexpected<Error>;
}

UF_REFLECT_ENUM(
    uf::json::ErrorKind,
    uf::json::ErrorKind::Syntax,
    uf::json::ErrorKind::NotCanonical,
    uf::json::ErrorKind::SchemaUnsupported,
    uf::json::ErrorKind::DocumentRejected
);
