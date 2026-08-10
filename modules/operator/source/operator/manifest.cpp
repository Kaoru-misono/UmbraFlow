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
            bool requireNamespace
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
                    atSegmentStart = true;
                    hasNamespace   = true;
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
            }
            if (atSegmentStart || (requireNamespace && !hasNamespace))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format("{} is not a canonical namespaced name", field)
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto canonicalSessionManifest(
            SessionManifestSpec const& spec
        ) -> std::string
        {
            auto output = std::string{"{\"agent_profile_hash\":"};
            appendHash(output, spec.agentProfileHash);
            output += ",\"host_protocol_schema_hash\":";
            appendHash(output, spec.hostProtocolSchemaHash);
            output += ",\"journal_envelope_schema_hash\":";
            appendHash(output, spec.journalEnvelopeSchemaHash);
            output += ",\"operator_protocol_schema_hash\":";
            appendHash(output, spec.operatorProtocolSchemaHash);
            output += ",\"policy_artifact_hash\":";
            appendHash(output, spec.policyArtifactHash);
            output += ",\"project_registration_hash\":";
            appendHash(output, spec.projectRegistrationHash);
            output += ",\"runtime_model_artifact_root_hash\":";
            appendHash(output, spec.runtimeModelArtifactRootHash);
            output += ",\"runtime_model_schema_hash\":";
            appendHash(output, spec.runtimeModelSchemaHash);
            output.push_back('}');
            return output;
        }

        [[nodiscard]]
        auto validateClaims(
            ProjectRegistrationClaims const& claims,
            ContentHash schemaHash
        ) -> Status
        {
            if (claims.manifestSchemaHash != schemaHash)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "ProjectRegistration was validated against a different schema root"
                );
            }
            UF_TRY(validateDottedName(claims.pluginId, "plugin_id", true));
            UF_TRY(validateDottedName(
                claims.baselineEventType,
                "baseline_event_type",
                true
            ));

            for (auto index = std::size_t{0}; index < claims.projectArtifactRoots.size(); ++index)
            {
                auto const& root = claims.projectArtifactRoots[index];
                UF_TRY(validateDottedName(
                    root.name,
                    "project artifact root name",
                    false
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
                        claims.projectArtifactRoots[index - 1U].name,
                        root.name
                    )
                )
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "ProjectRegistration artifact roots must be unique and JCS-ordered"
                    );
                }
            }
            return ok();
        }
    }

    ProjectRegistrationSchemaOwner::ProjectRegistrationSchemaOwner(
        ContentHash schemaHash,
        ProjectRegistrationExactValidator validate
    )
        : m_schemaHash{schemaHash}
        , m_validate{std::move(validate)}
    {
    }

    auto ProjectRegistrationSchemaOwner::create(
        ContentHash schemaHash,
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
        return ProjectRegistrationSchemaOwner{schemaHash, std::move(validate)};
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

    auto VerifiedProjectRegistration::pluginHash() const -> ContentHash
    {
        return m_claims.pluginHash;
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

    auto VerifiedProjectRegistration::manifestSchemaHash() const -> ContentHash
    {
        return m_claims.manifestSchemaHash;
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

    auto VerifiedProjectRegistration::projectArtifactRoots() const noexcept
        -> std::vector<NamedArtifactRoot> const&
    {
        return m_claims.projectArtifactRoots;
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
                "ProjectRegistration bytes do not match the expected root hash"
            );
        }

        UF_TRY_VALUE_CONTEXT(
            claims,
            schemaOwner.validate(canonicalJcs),
            "validating exact ProjectRegistration JCS"
        );
        UF_TRY(validateClaims(claims, schemaOwner.m_schemaHash));
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
}
