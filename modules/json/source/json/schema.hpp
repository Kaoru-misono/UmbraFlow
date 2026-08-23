#pragma once

#include "value.hpp"

#include <core/error/result.hpp>

#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace uf::json
{
    // A JSON Schema Draft 2020-12 evaluator, scoped to the keywords this
    // repository's schemas and its one consuming project's schemas actually
    // use, and refusing every other keyword outright.
    //
    // Three properties make it usable as a schema authority rather than as a
    // convenience.
    //
    // compile refuses any keyword it does not implement, so a schema cannot
    // carry a constraint this evaluator would silently ignore. A misspelled
    // keyword is a refusal when the schema is compiled -- at startup, where the
    // deployment builds its authorities -- rather than a hole that opens years
    // later. Because the refusal is enumerated rather than inferred, the set of
    // keywords is a fact this type can state: see implementedKeywords.
    //
    // compile also refuses a keyword whose VALUE has the wrong shape. That is
    // the same defect one level down and it is easy to miss: an evaluator that
    // reads `"required": "id"` as an empty name list, or `"minLength": "3"` as
    // the bound zero, has a check that cannot fail. Every keyword's value is
    // shape-checked before any instance is judged.
    //
    // A schema is compiled from exact bytes and resolves $ref only within the
    // closed set of documents handed to compile, so those bytes determine the
    // whole of the validation. A schema that could reach outside that set would
    // put a pinned schema hash back to being a convention.
    class Schema final
    {
        struct State;

        std::shared_ptr<State const> m_state;

        explicit Schema(std::shared_ptr<State const> p_state) noexcept;

    public:
        // Exact bytes plus the name they are known by. label appears in every
        // refusal the compiled schema produces, so a red suite says which
        // document refused; it is normally the schema's own path.
        struct Document final
        {
            std::string_view label{};
            std::string_view exactBytes{};
        };

        Schema(Schema const&) noexcept = default;
        Schema(Schema&&) noexcept = default;
        auto operator=(Schema const&) noexcept -> Schema& = default;
        auto operator=(Schema&&) noexcept -> Schema& = default;
        ~Schema() = default;

        // referencedDocuments is the closed world a cross-document $ref may
        // name. Each must declare an absolute $id, and a reference resolving to
        // an identity outside the set is refused rather than fetched. Passing
        // none restricts the schema to same-document references.
        //
        // The three ways a reference fails to resolve are reported apart,
        // because each has a different repair: a document of an origin the set
        // publishes but did not supply is widened in, a document of an origin
        // the set never publishes is removed, and a pointer that misses inside
        // a document the set does carry is corrected.
        [[nodiscard]]
        static auto compile(
            Document const& document,
            std::span<Document const> referencedDocuments = {}
        ) -> Result<Schema>;

        [[nodiscard]] auto validate(Value const& instance) const -> Status;

        // The whole of one refusal, in the terms the author has to act on.
        //
        // THIS NEVER ACCEPTS ANYTHING. validate above stays the sole authority
        // on accept and refuse; this runs only after it has already refused,
        // and only explains that refusal. A report that finds nothing to say
        // while validate refuses is a bug in the report and never permission to
        // install the document.
        //
        // It exists because validate short-circuits, which is right for a
        // verdict and wrong for a work list: a declaration with six problems
        // reports one, and the author edits six times. So this reads the one
        // instance path the refusal names and does pure set arithmetic there --
        // `required` minus the instance's member set is what is missing, and
        // for a closed object the instance's member set minus the `properties`
        // keys is what is not declared. Each missing member is printed with its
        // own `$comment`, because that is where the explanation an author needs
        // is already written.
        //
        // It is deliberately not a second evaluator, and must not grow into
        // one. It resolves `$ref` only along the path it walks, implements no
        // format, pattern, numeric or combinator semantics, and says nothing
        // about any location other than the one the refusal named.
        //
        // The answer is EMPTY when there is nothing to add -- a refusal about a
        // value rather than a member set, or a location this cannot read. That
        // is "I have nothing further to say about the refusal above", never
        // "the document is fine", and a caller must print the refusal either
        // way. A paragraph announcing that it had nothing to say would be noise
        // on the majority of refusals and would make two readers of one schema
        // disagree by wording alone.
        [[nodiscard]]
        auto explainRefusal(Value const& instance, Error const& refusal) const
            -> std::string;

        // Validates against `#/$defs/<name>`. One document holding one
        // subschema per case is how a tool-precondition schema is written: the
        // arguments of a tool are judged by the subschema its own catalog entry
        // names, and the caller hands the name in beside the bytes.
        [[nodiscard]]
        auto validateDefinition(std::string_view name, Value const& instance) const
            -> Status;

        [[nodiscard]] auto hasDefinition(std::string_view name) const -> bool;

        // Every keyword this evaluator implements, in ASCII order. The boundary
        // is data rather than prose so that a test can pin it and a widening
        // cannot pass unnoticed. The returned view names static storage.
        [[nodiscard]]
        static auto implementedKeywords() -> std::span<std::string_view const>;
    };
}
