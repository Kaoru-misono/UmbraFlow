#include "effective-plan.hpp"

#include <core/error/contracts.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/text/json-text.hpp>
#include <core/text/utf8.hpp>

#include <json/value.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace uf::operator_runtime
{
    namespace
    {
        [[nodiscard]]
        auto requireField(
            std::string_view value,
            std::string_view field
        ) -> Status
        {
            if (value.empty() || !isValidUtf8(value))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::string{field} + " must be non-empty valid UTF-8"
                );
            }
            return ok();
        }

        auto appendHash(std::string& output, ContentHash const& hash) -> void
        {
            appendJsonString(output, hash.hex());
        }

        // OP:`EffectEnvelope` as an array, in JCS member order. JCS orders
        // members by UTF-16 code unit, which is why opaque_project_payload
        // precedes payload_schema_hash and scope_key precedes scope_kind.
        [[nodiscard]]
        auto effectsJcs(std::span<ProposedEffect const> effects) -> std::string
        {
            auto output = std::string{"["};
            auto first  = true;
            for (auto const& effect : effects)
            {
                if (!first)
                {
                    output.push_back(',');
                }
                first = false;
                output += "{\"namespaced_type\":";
                appendJsonString(output, effect.namespacedType);
                output += ",\"opaque_project_payload\":";
                output += effect.opaqueProjectPayload;
                output += ",\"payload_schema_hash\":";
                appendHash(output, effect.payloadSchemaHash);
                output += ",\"risk\":";
                appendJsonString(output, riskWireName(effect.risk));
                output += ",\"scope_key\":";
                appendJsonString(output, effect.scopeKey);
                output += ",\"scope_kind\":";
                appendJsonString(output, effect.scopeKind);
                output.push_back('}');
            }
            output.push_back(']');
            return output;
        }

    }

    auto deriveEffectiveEffectEnvelope(
        std::vector<ProposedEffect> effects
    ) -> Result<EffectiveEffectEnvelope>
    {
        auto ordered = std::move(effects);
        for (auto const& effect : ordered)
        {
            UF_TRY(requireField(effect.namespacedType, "effect namespaced_type"));
            UF_TRY(requireField(effect.scopeKind, "effect scope_kind"));
            UF_TRY(requireField(effect.scopeKey, "effect scope_key"));
            UF_TRY(requireField(
                effect.opaqueProjectPayload,
                "effect opaque_project_payload"
            ));
        }
        std::ranges::sort(ordered, {}, [](ProposedEffect const& effect) {
            return std::tie(effect.namespacedType, effect.scopeKind, effect.scopeKey);
        });
        for (auto index = std::size_t{1}; index < ordered.size(); ++index)
        {
            auto const& previous = ordered[index - 1U];
            auto const& current  = ordered[index];
            if (
                previous.namespacedType == current.namespacedType
                && previous.scopeKind == current.scopeKind
                && previous.scopeKey == current.scopeKey
            )
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Effect envelope declares one effect scope twice"
                );
            }
        }
        auto material = effectsJcs(ordered);
        UF_TRY_VALUE(hash, sha256(std::as_bytes(std::span{material})));
        return EffectiveEffectEnvelope{
            .effects      = std::move(ordered),
            .canonicalJcs = std::move(material),
            .hash         = hash,
        };
    }

    OperatorPolicyAuthority::OperatorPolicyAuthority(
        ContentHash projectRegistrationHash,
        VerifiedPolicyArtifact policy
    )
        : m_projectRegistrationHash{projectRegistrationHash}
        , m_policy{std::move(policy)}
    {
    }

    auto OperatorPolicyAuthority::create(
        ProjectIdentity const& registration,
        SessionManifest const& sessionManifest,
        task::RuntimeModelBinding const& runtimeModel,
        std::string_view exactOperatorProtocolSchemaBytes,
        std::string_view exactPolicyArtifactBytes
    ) -> Result<OperatorPolicyAuthority>
    {
        if (sessionManifest.projectRegistrationHash() != registration.hash())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "SessionManifest is pinned to a different ProjectRegistration"
            );
        }
        UF_TRY_VALUE(
            schemaHash,
            sha256(std::as_bytes(std::span{exactOperatorProtocolSchemaBytes}))
        );
        if (schemaHash != sessionManifest.operatorProtocolSchemaHash())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Operator protocol schema bytes do not match the pinned session manifest"
            );
        }
        if (runtimeModel.artifactRootHash() != sessionManifest.runtimeModelArtifactRootHash())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "RuntimeModel binding was parsed from a RuntimeArtifact this "
                "session manifest does not pin"
            );
        }
        UF_TRY_VALUE(
            policy,
            VerifiedPolicyArtifact::verifyExact(
                sessionManifest,
                exactPolicyArtifactBytes
            )
        );
        return OperatorPolicyAuthority{registration.hash(), std::move(policy)};
    }

    auto OperatorPolicyAuthority::projectRegistrationHash() const -> ContentHash
    {
        return m_projectRegistrationHash;
    }

    auto OperatorPolicyAuthority::policyHash() const -> ContentHash
    {
        return m_policy.hash();
    }

}
