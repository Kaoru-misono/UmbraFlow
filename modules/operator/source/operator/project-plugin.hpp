#pragma once

#include "manifest.hpp"

#include <script/pure-data-program.hpp>

#include <json/value.hpp>

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::operator_runtime
{
    enum class ProjectPluginFunction : uint8
    {
        Derive,
        Plan,
        NextStep,
        Reconcile,
        Reduce,
    };

    enum class ProjectDocumentDirection : uint8
    {
        Input,
        Output,
    };

    class ProjectSchemaOwner;

    // Immutable exact JCS with no schema authority. It proves canonical bytes
    // and their content hash only; it does not claim that any field is valid for
    // a ProjectPlugin function.
    //
    // It carries the value those bytes denote beside them. That cached value is
    // useful only to the owner that minted it: a different ProjectSchemaOwner
    // re-parses the bytes at its call boundary, and the value produced by that
    // validation is the only value a ProjectPlugin may execute against.
    class CanonicalJson final
    {
        friend class ProjectSchemaOwner;

        ContentHash m_contentHash;
        std::string m_bytes;
        json::Value m_value;

        CanonicalJson(
            ContentHash contentHash,
            std::string bytes,
            json::Value value
        );

    public:
        [[nodiscard]]
        static auto parseExact(std::string exactJcs) -> Result<CanonicalJson>;

        [[nodiscard]] auto contentHash() const -> ContentHash;

        [[nodiscard]]
        auto bytes() const noexcept UF_LIFETIME_BOUND -> std::string const&;

        [[nodiscard]]
        auto value() const noexcept UF_LIFETIME_BOUND -> json::Value const&;

        auto operator==(CanonicalJson const&) const -> bool = default;
    };

    // A document accepted by the schema owner for one exact registration,
    // function, and direction. No schema hash label is exposed or accepted.
    //
    // The value the accepted bytes denote is exposed beside them: the owner
    // already parsed once to validate, and the Operator consumes the proposal
    // shape it stamped instead of parsing the same bytes a second time.
    class ValidatedDocument final
    {
        friend class ProjectSchemaOwner;

        ContentHash              m_projectRegistrationHash;
        ProjectPluginFunction    m_function;
        ProjectDocumentDirection m_direction;
        CanonicalJson            m_canonicalJson;

        ValidatedDocument(
            ContentHash projectRegistrationHash,
            ProjectPluginFunction function,
            ProjectDocumentDirection direction,
            CanonicalJson canonicalJson
        );

    public:
        [[nodiscard]] auto projectRegistrationHash() const -> ContentHash;
        [[nodiscard]] auto function() const noexcept -> ProjectPluginFunction;

        [[nodiscard]]
        auto direction() const noexcept -> ProjectDocumentDirection;

        [[nodiscard]] auto contentHash() const -> ContentHash;

        [[nodiscard]]
        auto bytes() const noexcept UF_LIFETIME_BOUND -> std::string const&;

        [[nodiscard]]
        auto value() const noexcept UF_LIFETIME_BOUND -> json::Value const&;

        auto operator==(ValidatedDocument const&) const -> bool = default;
    };

    // These validators are trusted deployment code. The canonical validator
    // must reject anything other than exact RFC 8785 JCS, and returns the value
    // those exact bytes denote -- it had to build one to answer, and returning
    // it is what keeps the ProjectPlugin boundary from parsing the same
    // document again. The document validator must validate the complete
    // function-specific JSON Schema, including every project-owned nested
    // payload. Neither callable is passed to plugin code or published in a
    // business VM.
    using CanonicalJsonValidator = std::function<Result<json::Value>(std::string_view exactJcs)>;
    using ProjectDocumentValidator = std::function<
        Status(
            ProjectPluginFunction function,
            ProjectDocumentDirection direction,
            std::string_view exactJcs
        )
    >;

    // The exact bytes of the three schemas this owner answers for. They are
    // required for the same reason the Journal, Tool Catalog and reconcile
    // owners require theirs: an owner that merely names a hash is a
    // convention, and every ValidatedDocument it stamps is downstream proof
    // that the pinned schema was applied.
    struct ProjectDocumentSchemaBytes final
    {
        std::string_view projectState{};
        std::string_view projectObservation{};
        std::string_view toolPrecondition{};
    };

    class ProjectSchemaOwner final
    {
        class State;

        friend class ProjectGenerationHandle;

        std::shared_ptr<State const> m_state;

        explicit ProjectSchemaOwner(std::shared_ptr<State const> p_state) noexcept;

        [[nodiscard]]
        auto validate(
            ProjectPluginFunction function,
            ProjectDocumentDirection direction,
            CanonicalJson const& document
        ) const -> Result<json::Value>;

        // Mints canonical bytes from a value rather than judging bytes a caller
        // spelled. A plugin returns a value, so its output has no serialization
        // of its own to be refused: RFC 8785 is applied here, once, and the
        // canonical form is a fact about the mint rather than a claim about the
        // plugin.
        [[nodiscard]]
        auto canonicalizeValue(json::Value value) const -> Result<CanonicalJson>;

        [[nodiscard]]
        auto validateOutput(
            ProjectPluginFunction function,
            CanonicalJson document
        ) const
            -> Result<ValidatedDocument>;

    public:
        ProjectSchemaOwner(ProjectSchemaOwner const&) noexcept = default;
        ProjectSchemaOwner(ProjectSchemaOwner&&) noexcept = default;
        auto operator=(ProjectSchemaOwner const&) noexcept -> ProjectSchemaOwner& = default;
        auto operator=(ProjectSchemaOwner&&) noexcept -> ProjectSchemaOwner& = default;
        ~ProjectSchemaOwner() = default;

        // `project` is the registration identity: the root this owner stamps
        // its documents with, and the three schema digests the exact bytes
        // must satisfy. Both generations of the registration document state
        // all four, so one owner judges the documents of either.
        [[nodiscard]]
        static auto create(
            ProjectIdentity const& project,
            ProjectDocumentSchemaBytes const& exactSchemas,
            CanonicalJsonValidator validateCanonicalJson,
            ProjectDocumentValidator validateDocument
        ) -> Result<ProjectSchemaOwner>;

        [[nodiscard]]
        auto canonicalize(std::string exactJcs) const -> Result<CanonicalJson>;

        [[nodiscard]] auto projectRegistrationHash() const -> ContentHash;
    };

    // One authored module of a Project closure, and one blob of the resource
    // closure a registration pinned, as they are handed to a loader.
    //
    // They live at namespace scope rather than inside the registrar because
    // they are the vocabulary of the loader layer and not of one loader: the
    // generation registrar, the manifest derivation and the deployment that
    // reads the bytes off disk all name them. Nesting them inside the registrar
    // would make every other consumer spell a loader's name to say "a module".
    struct ProjectModuleBlob final
    {
        std::string name{};
        std::string source{};
    };

    struct ProjectResourceBlob final
    {
        ProjectResourceKind kind{ProjectResourceKind::Json};
        std::string         name{};
        std::string         bytes{};
    };

    [[nodiscard]]
    auto derivePluginModuleManifestHash(
        std::string_view entryModule,
        std::span<ProjectModuleBlob const> modules
    ) -> Result<ContentHash>;

    [[nodiscard]]
    auto validateProjectResourceClosure(
        std::span<ProjectResourceBlob const> resources
    ) -> Status;

    // The exact Project resource closure a registration pinned, held to that
    // registration blob by blob -- name, kind, size and sha256 -- and returned
    // in the registration's own order, ready for a program to be compiled over.
    //
    // It is one function rather than one per loader because a registration
    // pins ONE resource closure and both program types a registration loads
    // read it. Two copies of this check would be two answers to "which bytes
    // did this project register", and only one of them would be inside
    // project_registration_hash.
    //
    // It takes the pinned rows rather than a registration so that the check
    // names the rows it judges and not the document that happens to carry
    // them. `pinnedResources` is a call-scoped borrow of the document's own
    // rows; nothing here retains it.
    [[nodiscard]]
    auto verifyProjectResourceClosure(
        std::span<ProjectResource const> pinnedResources,
        std::vector<ProjectResourceBlob> exactResources
    ) -> Result<std::vector<script::PureDataProgram::Resource>>;

    [[nodiscard]]
    auto currentProjectPluginEnvironmentMaterial() -> Result<std::string>;

    [[nodiscard]] auto currentProjectPluginEnvironmentHash() -> Result<ContentHash>;
} // namespace uf::operator_runtime
