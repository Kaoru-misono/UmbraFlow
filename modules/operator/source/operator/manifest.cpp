#include "manifest.hpp"

#include <core/text/json-text.hpp>
#include <core/text/utf8.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace uf::operator_runtime
{
    namespace
    {
        constexpr auto k_maximumRegistrationBytes = std::size_t{1024U} * 1024U;

        auto appendHash(
            std::string& output,
            ContentHash const& hash
        ) -> void
        {
            appendJsonString(output, hash.hex());
        }

        [[nodiscard]]
        auto validateDottedName(
            std::string_view value,
            std::string_view field,
            bool requireNamespace,
            std::size_t maximumSegments = 0U,
            std::size_t maximumSegmentBytes = 0U
        ) -> Status
        {
            if (
                value.empty()
                || value.size() > 128U
                || !isValidUtf8(value)
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format("{} must be non-empty bounded UTF-8", field)
                );
            }

            auto atSegmentStart = true;
            auto hasNamespace   = false;
            auto segments       = std::size_t{1U};
            auto segmentBytes   = std::size_t{0U};
            for (auto const character : value)
            {
                if (character == '.')
                {
                    if (atSegmentStart)
                    {
                        return fail(
                            AutomationErrorKind::InvalidResource,
                            std::format("{} is not a canonical dotted name", field)
                        );
                    }
                    if (
                        maximumSegmentBytes != 0U
                        && segmentBytes > maximumSegmentBytes
                    )
                    {
                        return fail(
                            AutomationErrorKind::InvalidResource,
                            std::format("{} has an overlong segment", field)
                        );
                    }
                    atSegmentStart = true;
                    hasNamespace   = true;
                    segmentBytes   = 0U;
                    ++segments;
                    if (maximumSegments != 0U && segments > maximumSegments)
                    {
                        return fail(
                            AutomationErrorKind::InvalidResource,
                            std::format("{} has too many segments", field)
                        );
                    }
                    continue;
                }
                auto const lower = character >= 'a' && character <= 'z';
                auto const digit = character >= '0' && character <= '9';
                if (
                    (atSegmentStart && !lower)
                    || (!atSegmentStart && !lower && !digit && character != '_' && character != '-')
                )
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format("{} is not a canonical dotted name", field)
                    );
                }
                atSegmentStart = false;
                ++segmentBytes;
            }
            if (
                atSegmentStart
                || (requireNamespace && !hasNamespace)
                || (maximumSegmentBytes != 0U && segmentBytes > maximumSegmentBytes)
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format("{} is not a canonical namespaced name", field)
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto canonicalSessionManifest(SessionManifestSpec const& spec) -> std::string
        {
            auto output = std::string{"{\"agent_profile_hash\":"};
            appendHash(output, spec.agentProfileHash);
            output += ",\"operator_protocol_schema_hash\":";
            appendHash(output, spec.operatorProtocolSchemaHash);
            output += ",\"policy_artifact_hash\":";
            appendHash(output, spec.policyArtifactHash);
            output += ",\"project_registration_hash\":";
            appendHash(output, spec.projectRegistrationHash);
            output += ",\"runtime_model_artifact_root_hash\":";
            appendHash(output, spec.runtimeModelArtifactRootHash);
            output.push_back('}');
            return output;
        }

        [[nodiscard]]
        auto validateClaims(ProjectRegistrationClaims const& claims) -> Status
        {
            // Both sides are named, because the whole of the diagnosis is which
            // generation the registration states and which one this binary was
            // built to read.
            if (claims.projectRegistrationFormat != k_projectRegistrationFormat)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "ProjectRegistration format is not supported by this "
                        "framework: the registration states {} and this "
                        "framework reads {}",
                        claims.projectRegistrationFormat,
                        k_projectRegistrationFormat
                    )
                );
            }
            UF_TRY(validateDottedName(claims.pluginId, "plugin_id", true));
            UF_TRY(validateDottedName(
                claims.baselineEventType,
                "baseline_event_type",
                true
            ));

            for (auto index = std::size_t{0}; index < claims.projectResources.size(); ++index)
            {
                auto const& resource = claims.projectResources[index];
                UF_TRY(validateDottedName(
                    resource.name,
                    "project resource name",
                    false,
                    16U,
                    64U
                ));
                // JCS orders by UTF-16 code unit, which is not byte order.
                // Comparing the names directly is right only while
                // validateDottedName above keeps them inside [a-z0-9._-],
                // where the two orders happen to coincide -- a property of the
                // validator rather than of the comparison. jsonMemberNameLess
                // is the rule itself, so relaxing the validator cannot silently
                // make this check wrong.
                if (
                    index != 0U
                    && !jsonMemberNameLess(
                        claims.projectResources[index - 1U].name,
                        resource.name
                    )
                )
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "ProjectRegistration resources must be unique and JCS-ordered"
                    );
                }
            }

            // The identity schema hashes are lowercase hex, so ContentHash's
            // byte comparison is the string order the registration schema
            // promises. A claim set stating any other order is a document the
            // loader never derived, because the loader sorts before it writes.
            for (
                auto index = std::size_t{0};
                index < claims.observedInstanceIdentitySchemaHashes.size();
                ++index
            )
            {
                if (
                    index != 0U
                    && !(claims.observedInstanceIdentitySchemaHashes[index - 1U]
                         < claims.observedInstanceIdentitySchemaHashes[index])
                )
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "ProjectRegistration observed instance identity schema "
                        "hashes must be unique and sorted"
                    );
                }
            }
            return ok();
        }
    }

    ProjectRegistrationSchemaOwner::ProjectRegistrationSchemaOwner(
        ProjectRegistrationExactValidator validate
    )
        : m_validate{std::move(validate)}
    {
    }

    auto ProjectRegistrationSchemaOwner::create(
        ProjectRegistrationExactValidator validate
    ) -> Result<ProjectRegistrationSchemaOwner>
    {
        if (!validate)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "ProjectRegistration schema owner requires an exact validator"
            );
        }
        return ProjectRegistrationSchemaOwner{std::move(validate)};
    }

    auto ProjectRegistrationSchemaOwner::validate(
        std::string_view exactJcs
    ) const -> Result<ProjectRegistrationClaims>
    {
        return m_validate(exactJcs);
    }

    VerifiedProjectRegistration::VerifiedProjectRegistration(
        ProjectRegistrationClaims claims,
        std::string canonicalJcs,
        ContentHash rootHash
    )
        : m_claims{std::move(claims)}
        , m_canonicalJcs{std::move(canonicalJcs)}
        , m_rootHash{rootHash}
    {
    }

    auto VerifiedProjectRegistration::canonicalJcs() const noexcept
        -> std::string const&
    {
        return m_canonicalJcs;
    }

    auto VerifiedProjectRegistration::hash() const -> ContentHash
    {
        return m_rootHash;
    }

    auto VerifiedProjectRegistration::pluginId() const -> std::string
    {
        return m_claims.pluginId;
    }

    auto VerifiedProjectRegistration::pluginModuleManifestHash() const -> ContentHash
    {
        return m_claims.pluginModuleManifestHash;
    }

    auto VerifiedProjectRegistration::pluginEnvironmentHash() const -> ContentHash
    {
        return m_claims.pluginEnvironmentHash;
    }

    auto VerifiedProjectRegistration::projectStateSchemaHash() const
        -> ContentHash
    {
        return m_claims.projectStateSchemaHash;
    }

    auto VerifiedProjectRegistration::toolCatalogHash() const -> ContentHash
    {
        return m_claims.toolCatalogHash;
    }

    auto VerifiedProjectRegistration::projectObservationSchemaHash() const
        -> ContentHash
    {
        return m_claims.projectObservationSchemaHash;
    }

    auto VerifiedProjectRegistration::projectToolPreconditionSchemaHash() const
        -> ContentHash
    {
        return m_claims.projectToolPreconditionSchemaHash;
    }

    auto VerifiedProjectRegistration::reconcilePayloadSchemaManifestHash() const
        -> ContentHash
    {
        return m_claims.reconcilePayloadSchemaManifestHash;
    }

    auto VerifiedProjectRegistration::journalEventSchemaManifestHash() const
        -> ContentHash
    {
        return m_claims.journalEventSchemaManifestHash;
    }

    auto VerifiedProjectRegistration::baselineEventType() const -> std::string
    {
        return m_claims.baselineEventType;
    }

    auto VerifiedProjectRegistration::projectResources() const noexcept
        -> std::vector<ProjectResource> const&
    {
        return m_claims.projectResources;
    }

    auto VerifiedProjectRegistration::observedInstanceIdentitySchemaHashes()
        const noexcept -> std::vector<ContentHash> const&
    {
        return m_claims.observedInstanceIdentitySchemaHashes;
    }

    auto ProjectRegistration::verifyExact(
        std::string canonicalJcs,
        ContentHash expectedRootHash,
        ProjectRegistrationSchemaOwner const& schemaOwner
    ) -> Result<VerifiedProjectRegistration>
    {
        if (
            canonicalJcs.empty()
            || canonicalJcs.size() > k_maximumRegistrationBytes
            || !isValidUtf8(canonicalJcs)
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "ProjectRegistration must be non-empty bounded UTF-8 JCS"
            );
        }

        UF_TRY_VALUE(
            actualRootHash,
            sha256(std::as_bytes(std::span{canonicalJcs}))
        );
        if (actualRootHash != expectedRootHash)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "ProjectRegistration bytes do not match the expected root "
                    "hash: expected {}, computed {}",
                    expectedRootHash.hex(),
                    actualRootHash.hex()
                )
            );
        }

        UF_TRY_VALUE_CONTEXT(
            claims,
            schemaOwner.validate(canonicalJcs),
            "validating exact ProjectRegistration JCS"
        );
        UF_TRY(validateClaims(claims));
        return VerifiedProjectRegistration{
            std::move(claims),
            std::move(canonicalJcs),
            actualRootHash,
        };
    }

    SessionManifest::SessionManifest(
        SessionManifestSpec spec,
        std::string canonicalBytes,
        ContentHash hash
    )
        : m_spec{spec}
        , m_canonicalBytes{std::move(canonicalBytes)}
        , m_hash{hash}
    {
    }

    auto SessionManifest::create(
        SessionManifestSpec const& spec
    ) -> Result<SessionManifest>
    {
        auto canonicalBytes = canonicalSessionManifest(spec);
        UF_TRY_VALUE(
            hash,
            sha256(std::as_bytes(std::span{canonicalBytes}))
        );
        return SessionManifest{
            spec,
            std::move(canonicalBytes),
            hash,
        };
    }

    auto SessionManifest::canonicalBytes() const -> std::string
    {
        return m_canonicalBytes;
    }

    auto SessionManifest::hash() const -> ContentHash
    {
        return m_hash;
    }

    auto SessionManifest::projectRegistrationHash() const -> ContentHash
    {
        return m_spec.projectRegistrationHash;
    }

    auto SessionManifest::runtimeModelArtifactRootHash() const -> ContentHash
    {
        return m_spec.runtimeModelArtifactRootHash;
    }

    auto SessionManifest::operatorProtocolSchemaHash() const -> ContentHash
    {
        return m_spec.operatorProtocolSchemaHash;
    }

    auto SessionManifest::policyArtifactHash() const -> ContentHash
    {
        return m_spec.policyArtifactHash;
    }

    auto SessionManifest::agentProfileHash() const -> ContentHash
    {
        return m_spec.agentProfileHash;
    }

}
