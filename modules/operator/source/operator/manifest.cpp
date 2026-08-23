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

        // Both sides are named, because the whole of the diagnosis is which
        // generation the document states and which one this reader was built
        // to read. Each reader passes its OWN constant and refuses everything
        // else; no caller passes a number it read out of the document, which
        // is what keeps this an identity assertion rather than a dispatch.
        [[nodiscard]]
        auto validateFormat(uint64 stated, uint64 read) -> Status
        {
            if (stated == read)
            {
                return ok();
            }
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "ProjectRegistration format is not supported by this "
                    "framework: the registration states {} and this "
                    "framework reads {}",
                    stated,
                    read
                )
            );
        }

        // Everything a registration document states about its registrant
        // rather than about its closure: the namespace it owns and the
        // ordering rules its arrays obey.
        //
        // The format member is deliberately NOT read here. It is the one
        // member that identifies which document this is, so the reader that
        // owns the format is the one place that decides whether these bytes
        // are readable at all; a shared function that judged it too would be a
        // second opinion about a document's identity.
        [[nodiscard]]
        auto validateSharedClaims(ProjectGenerationClaims const& claims) -> Status
        {
            UF_TRY(validateDottedName(claims.pluginId, "plugin_id", true));

            // A registrant's plugin_id IS the namespace it owns Tool names in,
            // so this is where the Framework's ownership of `framework.*` is
            // enforced: refuse the claim once, here, rather than refusing each
            // Tool name a registrant inside that namespace would then own
            // legitimately. A plugin_id is required to carry a namespace, so
            // the bare word cannot be claimed at all and only the prefix needs
            // testing.
            if (
                claims.pluginId.starts_with(k_frameworkToolNamespace)
                && claims.pluginId.size() > k_frameworkToolNamespace.size()
                && claims.pluginId[k_frameworkToolNamespace.size()] == '.'
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "plugin_id {} claims the reserved {} namespace",
                        claims.pluginId,
                        k_frameworkToolNamespace
                    )
                );
            }
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

            // The binding table is sorted and unique by tool name for the same
            // reason the resources are: the loader sorts before it writes, so a
            // claim set in any other order is a document the loader never
            // derived. One tool binds to one entry, and a second row for the
            // same name would be two answers to the question the dispatcher
            // asks once.
            for (
                auto index = std::size_t{0};
                index < claims.projectToolBindings.size();
                ++index
            )
            {
                auto const& binding = claims.projectToolBindings[index];
                UF_TRY(validateToolName(binding.toolName, "tool binding name"));
                UF_TRY(validateDottedName(
                    binding.entryPoint,
                    "tool binding entry point",
                    false
                ));
                if (
                    index != 0U
                    && !jsonMemberNameLess(
                        claims.projectToolBindings[index - 1U].toolName,
                        binding.toolName
                    )
                )
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "ProjectRegistration Tool bindings must be unique and "
                        "JCS-ordered by tool name"
                    );
                }
            }
            return ok();
        }

        // The closure's stated export surface. The names are the same dotted
        // names an entry point is spelled with everywhere else, and the order
        // is the loader's own derivation, so a statement in any other order is
        // one no authoring path produced.
        [[nodiscard]]
        auto validateClosureClaims(
            ProjectClosureClaims const& closure,
            std::string_view field
        ) -> Status
        {
            for (
                auto index = std::size_t{0};
                index < closure.exportedEntryPoints.size();
                ++index
            )
            {
                UF_TRY(validateDottedName(
                    closure.exportedEntryPoints[index],
                    field,
                    false
                ));
                if (
                    index != 0U
                    && !jsonMemberNameLess(
                        closure.exportedEntryPoints[index - 1U],
                        closure.exportedEntryPoints[index]
                    )
                )
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "{} must be unique and JCS-ordered",
                            field
                        )
                    );
                }
            }
            return ok();
        }

        // The one reader. It accepts k_projectGenerationFormat and refuses
        // every other number, including the two-closure generation the state
        // cut deleted: a document stating another format is not a degraded
        // generation this reader falls back to, it is a document this
        // framework does not read.
        [[nodiscard]]
        auto validateGenerationClaims(
            ProjectGenerationClaims const& claims
        ) -> Status
        {
            UF_TRY(validateFormat(
                claims.projectRegistrationFormat,
                k_projectGenerationFormat
            ));
            UF_TRY(validateSharedClaims(claims));
            return validateClosureClaims(
                claims.toolClosure,
                "tool closure exported entry point"
            );
        }
    }

    auto validateToolName(
        std::string_view name,
        std::string_view field
    ) -> Status
    {
        return validateDottedName(name, field, true);
    }

    auto validateToolNameOwnership(
        std::string_view name,
        std::string_view ownedNamespace
    ) -> Status
    {
        UF_TRY(validateToolName(name, "Tool name"));
        if (
            !name.starts_with(ownedNamespace)
            || name.size() <= ownedNamespace.size() + 1U
            || name[ownedNamespace.size()] != '.'
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "Tool name {} is outside the namespace {} its registrant "
                    "owns",
                    name,
                    ownedNamespace
                )
            );
        }
        return ok();
    }

    auto validateProjectToolBindings(
        std::span<std::string const> declaredToolNames,
        std::span<ProjectToolBinding const> bindings,
        std::span<std::string const> exportedEntryPoints
    ) -> Status
    {
        for (auto const& binding : bindings)
        {
            if (!std::ranges::contains(declaredToolNames, binding.toolName))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "Project Tool binding names " + binding.toolName
                        + ", which this Tool Catalog does not declare"
                );
            }
            if (!std::ranges::contains(exportedEntryPoints, binding.entryPoint))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "Project Tool " + binding.toolName + " is bound to entry "
                        + binding.entryPoint
                        + ", which the tool closure does not declare"
                );
            }
        }
        for (auto const& entryPoint : exportedEntryPoints)
        {
            if (
                !std::ranges::contains(
                    bindings,
                    entryPoint,
                    &ProjectToolBinding::entryPoint
                )
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "the tool closure declares entry " + entryPoint
                        + ", which no Tool binds"
                );
            }
        }
        return ok();
    }

    VerifiedProjectGeneration::VerifiedProjectGeneration(
        ProjectGenerationClaims claims,
        std::string canonicalJcs,
        ContentHash rootHash
    )
        : m_claims{std::move(claims)}
        , m_canonicalJcs{std::move(canonicalJcs)}
        , m_rootHash{rootHash}
    {
    }

    auto VerifiedProjectGeneration::canonicalJcs() const noexcept
        -> std::string const&
    {
        return m_canonicalJcs;
    }

    auto VerifiedProjectGeneration::hash() const -> ContentHash
    {
        return m_rootHash;
    }

    auto VerifiedProjectGeneration::pluginId() const -> std::string
    {
        return m_claims.pluginId;
    }

    auto VerifiedProjectGeneration::pluginEnvironmentHash() const -> ContentHash
    {
        return m_claims.pluginEnvironmentHash;
    }

    auto VerifiedProjectGeneration::toolCatalogHash() const -> ContentHash
    {
        return m_claims.toolCatalogHash;
    }

    auto VerifiedProjectGeneration::toolClosure() const noexcept
        -> ProjectClosureClaims const&
    {
        return m_claims.toolClosure;
    }

    auto VerifiedProjectGeneration::projectResources() const noexcept
        -> std::vector<ProjectResource> const&
    {
        return m_claims.projectResources;
    }

    auto VerifiedProjectGeneration::projectToolBindings() const noexcept
        -> std::vector<ProjectToolBinding> const&
    {
        return m_claims.projectToolBindings;
    }

    auto ProjectGeneration::verifyExact(
        std::string canonicalJcs,
        ContentHash expectedRootHash,
        ProjectGenerationExactValidator const& validate
    ) -> Result<VerifiedProjectGeneration>
    {
        if (!validate)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "a ProjectRegistration generation requires an exact validator"
            );
        }
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
            validate(canonicalJcs),
            "validating exact ProjectRegistration JCS"
        );
        UF_TRY(validateGenerationClaims(claims));
        return VerifiedProjectGeneration{
            std::move(claims),
            std::move(canonicalJcs),
            actualRootHash,
        };
    }

    ProjectIdentity::ProjectIdentity(VerifiedProjectGeneration const& generation)
        : m_projectRegistrationHash{generation.m_rootHash}
        , m_pluginId{generation.m_claims.pluginId}
        , m_canonicalJcs{generation.m_canonicalJcs}
        , m_moduleIdentityHash{
              generation.m_claims.toolClosure.moduleManifestHash
          }
        , m_toolCatalogHash{generation.m_claims.toolCatalogHash}
        , m_observedInstanceIdentitySchemaHashes{
              generation.m_claims.observedInstanceIdentitySchemaHashes
          }
        , m_projectToolBindings{generation.m_claims.projectToolBindings}
    {
    }

    auto ProjectIdentity::hash() const -> ContentHash
    {
        return m_projectRegistrationHash;
    }

    auto ProjectIdentity::pluginId() const -> std::string
    {
        return m_pluginId;
    }

    auto ProjectIdentity::canonicalJcs() const noexcept -> std::string const&
    {
        return m_canonicalJcs;
    }

    auto ProjectIdentity::moduleIdentityHash() const -> ContentHash
    {
        return m_moduleIdentityHash;
    }

    auto ProjectIdentity::toolCatalogHash() const -> ContentHash
    {
        return m_toolCatalogHash;
    }

    auto ProjectIdentity::observedInstanceIdentitySchemaHashes() const noexcept
        -> std::vector<ContentHash> const&
    {
        return m_observedInstanceIdentitySchemaHashes;
    }

    auto ProjectIdentity::projectToolBindings() const noexcept
        -> std::vector<ProjectToolBinding> const&
    {
        return m_projectToolBindings;
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
