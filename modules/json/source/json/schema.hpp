#pragma once

#include "value.hpp"

#include <core/error/result.hpp>

#include <memory>
#include <span>
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
