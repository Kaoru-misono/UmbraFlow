#pragma once

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace uf::operator_runtime
{
    // The generation of the ProjectRegistration document contract this
    // framework derives and reads. It is a compatibility statement: a
    // registration declares the number and this binary decides whether it
    // understands what that number describes.
    //
    // It is a generation rather than the digest of
    // schema/umbraflow-project-registration-v2.schema.json, because the digest
    // made every cosmetic edit to that file move every registration root -- and
    // a registration root is a project's identity, pinned by consumers. The
    // digest also compared a value with itself: the loader derived it once and
    // handed the same local to both the document and the schema owner that
    // judged the document.
    inline constexpr auto k_projectRegistrationFormat = uint64{4U};

    enum class ProjectResourceKind : uint8
    {
        Json,
        Utf8,
        Bytes,
    };

    struct ProjectResource final
    {
        ProjectResourceKind kind{ProjectResourceKind::Json};
        std::string         name{};
        ContentHash         hash;
        uint64              size{};
    };

    // One row of the Project Tool binding table: the catalog name of a Tool,
    // and the entry point of that project's closure that implements it.
    //
    // It is a registration member, beside the Tool Catalog rather than inside
    // it. The registration already splits identity into contract
    // (tool_catalog_hash) and code (plugin_environment_hash), and a binding is
    // the JOIN of the two -- it names a contract name and a code entry and
    // references both sides -- so it belongs beside both and inside neither.
    // Three things break if it moves into the catalog:
    //
    // 1. Exact-bytes verifiability dies. The catalog's exact bytes are hashed,
    //    and a caller may legitimately need existence and schema without being
    //    entitled to know what implements a Tool. With the binding inside,
    //    describing the catalog must either leak it or serve a redacted
    //    projection whose bytes no longer hash to tool_catalog_hash -- so the
    //    caller can no longer verify the served catalog against the pinned
    //    digest, which is that digest's entire job.
    // 2. Contract churn on refactor. Renaming a handler entry is a pure
    //    implementation change; with the binding in the catalog it would move
    //    tool_catalog_hash, invalidating every pin that cares only about the
    //    contract -- child admission intersects descriptors, and approvals and
    //    policy reference catalog authority. Implementation motion would force
    //    contract re-approval.
    // 3. Audience mismatch. Every catalog field is consumed by callers and by
    //    admission. A binding is consumed by exactly one party at one moment:
    //    the dispatcher, at dispatch.
    //
    // It is nonetheless INSIDE the registration root, and that is not
    // negotiable either. Replay compares execution identity field by field. If
    // the binding sat outside the root digest, a re-registration could rebind a
    // Tool to different code mid-history with no detectable identity change,
    // and "the exact pinned environment" would stop pinning which code answers
    // a call. Inside the root, registration refusal on digest inequality covers
    // rebinding for free, and a mid-run rebind surfaces as a named-field
    // mismatch that stops the run.
    struct ProjectToolBinding final
    {
        std::string toolName{};
        std::string entryPoint{};

        auto operator==(ProjectToolBinding const&) const -> bool = default;
    };

    // Values extracted only after the schema owner has accepted the exact
    // ProjectRegistration JCS bytes. This is not a construction spec: callers
    // cannot pass it to the registrar or mint a registration from it.
    struct ProjectRegistrationClaims final
    {
        uint64                       projectRegistrationFormat{};
        std::string                  pluginId{};
        ContentHash                  pluginModuleManifestHash;
        ContentHash                  pluginEnvironmentHash;
        ContentHash                  toolCatalogHash;
        ContentHash                  projectStateSchemaHash;
        ContentHash                  projectObservationSchemaHash;
        ContentHash                  projectToolPreconditionSchemaHash;
        ContentHash                  reconcilePayloadSchemaManifestHash;
        ContentHash                  journalEventSchemaManifestHash;
        std::string                  baselineEventType{};
        std::vector<ProjectResource> projectResources{};

        // The closed set of observed-instance identity schema documents this
        // registration owns, as the sha256 of each exact document, sorted and
        // unique. The deployment that supplied the documents derives it; the
        // claims check refuses any other order.
        std::vector<ContentHash> observedInstanceIdentitySchemaHashes{};

        // The Project Tool binding table, sorted by tool name and unique. An
        // empty table is the whole statement "this registration binds no Tool
        // to an entry", which is what a project that ships no handler declares;
        // there is deliberately no second spelling of that and no absent form.
        std::vector<ProjectToolBinding> projectToolBindings{};
    };

    // The implementation must parse the complete registration, validate it
    // against the owner's exact JSON Schema, and reject bytes that are not the
    // exact RFC 8785 JCS serialization. Returning claims without doing all
    // three is a schema-owner bug, never an extension point for project code.
    using ProjectRegistrationExactValidator = std::function<
        Result<ProjectRegistrationClaims>(std::string_view exactJcs)
    >;

    class ProjectRegistrationSchemaOwner final
    {
        ProjectRegistrationExactValidator m_validate;

        explicit ProjectRegistrationSchemaOwner(
            ProjectRegistrationExactValidator validate
        );

        friend class ProjectRegistration;

        [[nodiscard]]
        auto validate(std::string_view exactJcs) const
            -> Result<ProjectRegistrationClaims>;

    public:
        [[nodiscard]]
        static auto create(
            ProjectRegistrationExactValidator validate
        ) -> Result<ProjectRegistrationSchemaOwner>;
    };

    // Authority-bearing registration identity. Its constructor is unreachable
    // except from ProjectRegistration::verifyExact after exact JCS validation,
    // exact schema validation, and root verification have all succeeded.
    class VerifiedProjectRegistration final
    {
        ProjectRegistrationClaims m_claims;
        std::string               m_canonicalJcs;
        ContentHash               m_rootHash;

        VerifiedProjectRegistration(
            ProjectRegistrationClaims claims,
            std::string canonicalJcs,
            ContentHash rootHash
        );

        friend class ProjectRegistration;

    public:
        [[nodiscard]]
        auto canonicalJcs() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;

        [[nodiscard]] auto hash() const -> ContentHash;
        [[nodiscard]] auto pluginId() const -> std::string;
        [[nodiscard]] auto pluginModuleManifestHash() const -> ContentHash;
        [[nodiscard]] auto pluginEnvironmentHash() const -> ContentHash;
        [[nodiscard]] auto projectStateSchemaHash() const -> ContentHash;
        [[nodiscard]] auto toolCatalogHash() const -> ContentHash;

        // Each schema-owning authority is bound to the exact bytes the
        // registration names here, so that an owner cannot answer for a schema
        // this registration never pinned.
        [[nodiscard]] auto projectObservationSchemaHash() const -> ContentHash;
        [[nodiscard]] auto projectToolPreconditionSchemaHash() const -> ContentHash;
        [[nodiscard]] auto reconcilePayloadSchemaManifestHash() const -> ContentHash;
        [[nodiscard]] auto journalEventSchemaManifestHash() const -> ContentHash;

        [[nodiscard]] auto baselineEventType() const -> std::string;

        [[nodiscard]]
        auto projectResources() const noexcept UF_LIFETIME_BOUND
            -> std::vector<ProjectResource> const&;

        [[nodiscard]]
        auto observedInstanceIdentitySchemaHashes() const noexcept
            UF_LIFETIME_BOUND -> std::vector<ContentHash> const&;

        // The binding table this registration pinned. See ProjectToolBinding
        // for why these bytes are inside this root and outside the catalog.
        [[nodiscard]]
        auto projectToolBindings() const noexcept UF_LIFETIME_BOUND
            -> std::vector<ProjectToolBinding> const&;
    };

    // The sole mint for VerifiedProjectRegistration. There is deliberately no
    // loose field spec and no API that canonicalizes caller-provided fields.
    class ProjectRegistration final
    {
    public:
        [[nodiscard]]
        static auto verifyExact(
            std::string canonicalJcs,
            ContentHash expectedRootHash,
            ProjectRegistrationSchemaOwner const& schemaOwner
        ) -> Result<VerifiedProjectRegistration>;
    };

    struct SessionManifestSpec final
    {
        ContentHash runtimeModelArtifactRootHash;
        ContentHash operatorProtocolSchemaHash;
        ContentHash projectRegistrationHash;
        ContentHash policyArtifactHash;
        ContentHash agentProfileHash;
    };

    class SessionManifest final
    {
        SessionManifestSpec m_spec;
        std::string         m_canonicalBytes;
        ContentHash         m_hash;

        SessionManifest(
            SessionManifestSpec spec,
            std::string canonicalBytes,
            ContentHash hash
        );

    public:
        [[nodiscard]]
        static auto create(
            SessionManifestSpec const& spec
        ) -> Result<SessionManifest>;

        [[nodiscard]] auto canonicalBytes() const -> std::string;
        [[nodiscard]] auto hash() const -> ContentHash;
        [[nodiscard]] auto projectRegistrationHash() const -> ContentHash;
        [[nodiscard]] auto runtimeModelArtifactRootHash() const -> ContentHash;

        // The operator protocol schema this session is pinned to. It is
        // exposed because OperatorPlanAuthority must satisfy it with exact
        // bytes: an authority that merely named a hash would be a convention.
        [[nodiscard]] auto operatorProtocolSchemaHash() const -> ContentHash;

        // The PolicyArtifact this session is pinned to, exposed for the same
        // reason: VerifiedPolicyArtifact::verifyExact must satisfy it with
        // exact bytes, so the rules a plan is judged under are the ones this
        // manifest attests to and not ones a caller handed in as a hash.
        [[nodiscard]] auto policyArtifactHash() const -> ContentHash;

        // The AgentProfile this session is pinned to. Exposed for the same
        // reason: AgentProfile::verifyExact must satisfy it with exact bytes,
        // so that the ceilings an Agent binding runs under are the ones this
        // manifest attests to and not ones a caller stated.
        [[nodiscard]] auto agentProfileHash() const -> ContentHash;

    };
}
