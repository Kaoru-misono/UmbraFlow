#pragma once

#include "manifest.hpp"
#include "policy.hpp"
#include "project-plugin.hpp"
#include "tool-descriptor.hpp"

#include <task/runtime-model-file.hpp>

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <functional>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uf::operator_runtime
{
    class OperatorCoordinator;
    class OperatorPlanAuthority;

    // The canonical, order-independent form of one proposed effect set. It is
    // a value rather than an authority: callers may propose effects, while
    // descriptor bounds and the Operator-owned PolicyArtifact still decide
    // whether those effects may be admitted. Keeping this derivation shared
    // prevents Tool admission and EffectivePlan minting from assigning two
    // hashes to the same set.
    //
    // No in-class initializer for the hash: ContentHash has no default state.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct EffectiveEffectEnvelope final
    {
        std::vector<ProposedEffect> effects{};
        std::string                 canonicalJcs{};
        ContentHash                 hash;
    };

    [[nodiscard]]
    auto deriveEffectiveEffectEnvelope(
        std::vector<ProposedEffect> effects
    ) -> Result<EffectiveEffectEnvelope>;

    // The session's evaluated policy, bound to the ProjectRegistration root and
    // to the RuntimeArtifact the session manifest pins. It is constructible
    // only through create(), which proves every one of those pins before an
    // authority exists at all, and OperatorCoordinator is its only friend, so
    // no path reaches the verified PolicyArtifact except through the ledger.
    class OperatorPlanAuthority final
    {
        friend class OperatorCoordinator;

        ContentHash            m_projectRegistrationHash;
        VerifiedPolicyArtifact m_policy;

        OperatorPlanAuthority(
            ContentHash projectRegistrationHash,
            VerifiedPolicyArtifact policy
        );

    public:
        // The exact operator protocol schema bytes are required for the same
        // reason the Journal and Tool Catalog owners require theirs: an owner
        // that merely names a hash is a convention. The session manifest
        // supplies the hash to satisfy, and must itself be pinned to this
        // registration.
        //
        // runtimeModel is the same argument one level down. Only TaskHost can
        // mint one, so an authority for a model nobody parsed cannot be
        // constructed at all, and one parsed from another RuntimeArtifact is
        // refused here rather than at the seam that would have used it.
        //
        // The PolicyArtifact is the third argument of the same kind. The
        // Operator evaluates policy rather than accepting a hash a caller
        // supplied, so this authority is built from the exact bytes the
        // manifest's policy_artifact_hash names and can evaluate nothing else.
        [[nodiscard]]
        static auto create(
            ProjectIdentity const& registration,
            SessionManifest const& sessionManifest,
            task::RuntimeModelBinding const& runtimeModel,
            std::string_view exactOperatorProtocolSchemaBytes,
            std::string_view exactPolicyArtifactBytes
        ) -> Result<OperatorPlanAuthority>;

        [[nodiscard]] auto projectRegistrationHash() const -> ContentHash;

        // The policy this authority evaluates, so that the ledger can record
        // which artifact ruled a Tool call without a caller naming one.
        [[nodiscard]] auto policyHash() const -> ContentHash;
    };
}
