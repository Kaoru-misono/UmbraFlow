#pragma once

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>

#include <domain/content-hash.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace uf::operator_runtime
{
    struct NamedArtifactRoot final
    {
        std::string name{};
        ContentHash rootHash;
    };

    // Values extracted only after the schema owner has accepted the exact
    // ProjectRegistration JCS bytes. This is not a construction spec: callers
    // cannot pass it to the registrar or mint a registration from it.
    struct ProjectRegistrationClaims final
    {
        ContentHash                    manifestSchemaHash;
        std::string                    pluginId{};
        ContentHash                    pluginHash;
        ContentHash                    toolCatalogHash;
        ContentHash                    projectStateSchemaHash;
        ContentHash                    projectObservationSchemaHash;
        ContentHash                    projectToolPreconditionSchemaHash;
        ContentHash                    reconcilePayloadSchemaManifestHash;
        ContentHash                    journalEventSchemaManifestHash;
        std::string                    baselineEventType{};
        std::vector<NamedArtifactRoot> projectArtifactRoots{};
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
        ContentHash                       m_schemaHash;
        ProjectRegistrationExactValidator m_validate;

        ProjectRegistrationSchemaOwner(
            ContentHash schemaHash,
            ProjectRegistrationExactValidator validate
        );

        friend class ProjectRegistration;

        [[nodiscard]]
        auto validate(std::string_view exactJcs) const
            -> Result<ProjectRegistrationClaims>;

    public:
        [[nodiscard]]
        static auto create(
            ContentHash schemaHash,
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
        [[nodiscard]] auto pluginHash() const -> ContentHash;
        [[nodiscard]] auto projectStateSchemaHash() const -> ContentHash;
        [[nodiscard]] auto toolCatalogHash() const -> ContentHash;

        // Each schema-owning authority is bound to the exact bytes the
        // registration names here, so that an owner cannot answer for a schema
        // this registration never pinned.
        [[nodiscard]] auto manifestSchemaHash() const -> ContentHash;
        [[nodiscard]] auto projectObservationSchemaHash() const -> ContentHash;
        [[nodiscard]] auto projectToolPreconditionSchemaHash() const -> ContentHash;
        [[nodiscard]] auto reconcilePayloadSchemaManifestHash() const -> ContentHash;
        [[nodiscard]] auto journalEventSchemaManifestHash() const -> ContentHash;

        [[nodiscard]] auto baselineEventType() const -> std::string;

        [[nodiscard]]
        auto projectArtifactRoots() const noexcept UF_LIFETIME_BOUND
            -> std::vector<NamedArtifactRoot> const&;
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
        ContentHash hostProtocolSchemaHash;
        ContentHash runtimeModelSchemaHash;
        ContentHash runtimeModelArtifactRootHash;
        ContentHash operatorProtocolSchemaHash;
        ContentHash projectRegistrationHash;
        ContentHash policyArtifactHash;
        ContentHash journalEnvelopeSchemaHash;
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
    };
}
