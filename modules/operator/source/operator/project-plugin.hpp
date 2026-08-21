#pragma once

#include "manifest.hpp"

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

        friend class ProjectPluginHandle;

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

        [[nodiscard]]
        static auto create(
            VerifiedProjectRegistration const& registration,
            ProjectDocumentSchemaBytes const& exactSchemas,
            CanonicalJsonValidator validateCanonicalJson,
            ProjectDocumentValidator validateDocument
        ) -> Result<ProjectSchemaOwner>;

        [[nodiscard]]
        auto canonicalize(std::string exactJcs) const -> Result<CanonicalJson>;

        [[nodiscard]] auto projectRegistrationHash() const -> ContentHash;
    };

    // Immutable handle to precompiled plugin bytecode and verified artifact
    // blobs. Every data call creates a fresh pure VM; no VM, callback registry,
    // native handle, Host, Receipt, database, clock, RNG, loader, or path is
    // exposed.
    class ProjectPluginHandle final
    {
        class State;

        friend class ProjectPluginRegistrar;

        std::shared_ptr<State const> m_state;

        explicit ProjectPluginHandle(std::shared_ptr<State const> p_state) noexcept;

        [[nodiscard]]
        auto invoke(
            ProjectPluginFunction function,
            CanonicalJson const& input
        ) const
            -> Result<ValidatedDocument>;

    public:
        ProjectPluginHandle(ProjectPluginHandle const&) noexcept = default;
        ProjectPluginHandle(ProjectPluginHandle&&) noexcept = default;
        auto operator=(ProjectPluginHandle const&) noexcept -> ProjectPluginHandle& = default;
        auto operator=(ProjectPluginHandle&&) noexcept -> ProjectPluginHandle& = default;
        ~ProjectPluginHandle() = default;

        [[nodiscard]] auto pluginId() const -> std::string;
        [[nodiscard]] auto projectRegistrationHash() const -> ContentHash;
        [[nodiscard]] auto pluginModuleManifestHash() const -> ContentHash;

        // The resource identities this registration pinned, in the
        // manifest's own order -- resource names sorted by UTF-8 bytes, so
        // the sequence is determined and a JCS array of it is too. Trusted
        // Operator code needs them because it assembles the derive envelope
        // itself rather than accepting one; they are already public in the
        // registration this handle was built from.
        [[nodiscard]]
        auto projectResourceHashes() const -> std::vector<ContentHash>;

        // The observation schema this registration pinned. The Operator records
        // it beside every derived reading so a stored observation names the
        // schema that judged it; project_registrations carries no column for
        // it, and the handle is already checked against that row.
        [[nodiscard]] auto projectObservationSchemaHash() const -> ContentHash;

        // Mints canonical bytes through this plugin's pinned schema owner.
        // Trusted Operator code needs it because it assembles the reduce
        // envelope itself rather than accepting one from a caller; it grants no
        // authority beyond proving the bytes are exact JCS.
        [[nodiscard]]
        auto canonicalize(std::string exactJcs) const -> Result<CanonicalJson>;

        [[nodiscard]]
        auto derive(CanonicalJson const& input) const -> Result<ValidatedDocument>;

        [[nodiscard]]
        auto plan(CanonicalJson const& input) const -> Result<ValidatedDocument>;

        [[nodiscard]]
        auto nextStep(CanonicalJson const& input) const -> Result<ValidatedDocument>;

        [[nodiscard]]
        auto reconcile(CanonicalJson const& input) const -> Result<ValidatedDocument>;

        [[nodiscard]]
        auto reduce(CanonicalJson const& input) const -> Result<ValidatedDocument>;
    };

    // Startup-only exact registry. Registration accepts only an already verified
    // registration, the exact module and resource closures pinned by it, and a
    // schema owner bound to the same root.
    class ProjectPluginRegistrar final
    {
    public:
        struct ModuleBlob final
        {
            std::string name{};
            std::string source{};
        };

        struct ResourceBlob final
        {
            ProjectResourceKind kind{ProjectResourceKind::Json};
            std::string         name{};
            std::string         bytes{};
        };

    private:
        std::map<std::pair<std::string, ContentHash>, ProjectPluginHandle> m_plugins{};

    public:
        [[nodiscard]]
        auto registerPlugin(
            VerifiedProjectRegistration const& registration,
            std::string entryModule,
            std::vector<ModuleBlob> exactModules,
            std::vector<ResourceBlob> exactResources,
            ProjectSchemaOwner schemaOwner
        ) -> Result<ProjectPluginHandle>;

        [[nodiscard]]
        auto findExact(
            std::string const& pluginId,
            ContentHash projectRegistrationHash
        ) const
            -> Result<ProjectPluginHandle>;
    };

    [[nodiscard]]
    auto derivePluginModuleManifestHash(
        std::string_view entryModule,
        std::span<ProjectPluginRegistrar::ModuleBlob const> modules
    ) -> Result<ContentHash>;

    [[nodiscard]]
    auto validateProjectResourceClosure(
        std::span<ProjectPluginRegistrar::ResourceBlob const> resources
    ) -> Status;

    [[nodiscard]]
    auto currentProjectPluginEnvironmentMaterial() -> Result<std::string>;

    [[nodiscard]] auto currentProjectPluginEnvironmentHash() -> Result<ContentHash>;
} // namespace uf::operator_runtime
