#include "snapshot-reference.hpp"

#include <json/value.hpp>

#include <core/error/contracts.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::operator_runtime
{
    namespace
    {
        // The wire generation. It is inside the hashed material because a
        // reader that could not tell one generation of this document from
        // another would have to guess, and this repository migrates rather than
        // teaching a reader two shapes.
        constexpr auto k_referenceSchema = std::string_view{
            "framework.observation_reference/2"
        };

        [[nodiscard]]
        auto refuseSpec(std::string message) -> std::unexpected<Error>
        {
            return fail(AutomationErrorKind::InvalidResource, std::move(message));
        }

        [[nodiscard]]
        auto actionNamesAreValid(ObservedUiAction const& action) -> bool
        {
            return !action.uiTarget.empty() && !action.binding.empty()
                && !action.action.empty() && !action.kind.empty();
        }

        [[nodiscard]]
        auto uiActionsMaterial(std::span<ObservedUiAction const> actions)
            -> json::Value
        {
            auto rendered = std::vector<json::Value>{};
            rendered.reserve(actions.size());
            for (auto const& action : actions)
            {
                rendered.emplace_back(json::Value::ofObject({
                    {"action", json::Value::ofString(action.action)},
                    {"binding", json::Value::ofString(action.binding)},
                    {"kind", json::Value::ofString(action.kind)},
                    {"ui_target", json::Value::ofString(action.uiTarget)},
                }));
            }
            return json::Value::ofArray(std::move(rendered));
        }

        // Both counters are rendered as decimal strings rather than JSON
        // numbers. RFC 8785 numbers are IEEE-754 doubles, so a generation or an
        // instant above 2^53 would round, and a reference whose bindings round
        // is a reference two different worlds can share.
        [[nodiscard]]
        auto counterMaterial(uint64 value) -> json::Value
        {
            return json::Value::ofString(std::to_string(value));
        }

        [[nodiscard]]
        auto referenceMaterial(SnapshotObservationSpec const& spec)
            -> json::Value
        {
            return json::Value::ofObject({
                {"controlled_target_id",
                 json::Value::ofString(spec.controlledTargetId)},
                {"expires_at_unix_ms",
                 counterMaterial(spec.expiresAtUnixMillis)},
                {"frame_identity_hash",
                 json::Value::ofString(spec.frameIdentityHash.hex())},
                {"host_generation", counterMaterial(spec.hostGeneration)},
                {"project_registration_hash",
                 json::Value::ofString(spec.projectRegistrationHash.hex())},
                {"runtime_artifact_root_hash",
                 json::Value::ofString(spec.runtimeArtifactRootHash.hex())},
                {"schema", json::Value::ofString(std::string{k_referenceSchema})},
                {"screenshot_sha256",
                 json::Value::ofString(spec.screenshotSha256.hex())},
                {"ui_actions", uiActionsMaterial(spec.uiActions)},
            });
        }

        using BindingPredicate = bool (*)(
            SnapshotObservationSpec const&,
            SnapshotObservationConsumption const&
        );

        // One row of the binding half of the refusal matrix: the verdict, and
        // the single comparison that earns it. A table rather than a chain of
        // else-ifs, so the set is closed and every row is one testable channel.
        struct BindingRefusal final
        {
            ObservationRefusal kind{ObservationRefusal::Stale};
            BindingPredicate   refused{};
        };

        [[nodiscard]]
        auto refusedAsStale(
            SnapshotObservationSpec const& spec,
            SnapshotObservationConsumption const& consumption
        ) -> bool
        {
            return consumption.presentedAtUnixMillis >= spec.expiresAtUnixMillis;
        }

        [[nodiscard]]
        auto refusedAsForeignTarget(
            SnapshotObservationSpec const& spec,
            SnapshotObservationConsumption const& consumption
        ) -> bool
        {
            return consumption.controlledTargetId != spec.controlledTargetId;
        }

        [[nodiscard]]
        auto refusedAsForeignRegistration(
            SnapshotObservationSpec const& spec,
            SnapshotObservationConsumption const& consumption
        ) -> bool
        {
            return consumption.projectRegistrationHash
                != spec.projectRegistrationHash;
        }

        [[nodiscard]]
        auto refusedAsForeignRuntimeArtifact(
            SnapshotObservationSpec const& spec,
            SnapshotObservationConsumption const& consumption
        ) -> bool
        {
            return consumption.runtimeArtifactRootHash
                != spec.runtimeArtifactRootHash;
        }

        [[nodiscard]]
        auto refusedAsChangedGeneration(
            SnapshotObservationSpec const& spec,
            SnapshotObservationConsumption const& consumption
        ) -> bool
        {
            return consumption.hostGeneration != spec.hostGeneration;
        }

        [[nodiscard]]
        auto sameActionIdentity(
            ObservedUiAction const& action,
            SnapshotObservationConsumption const& consumption
        ) -> bool
        {
            return action.uiTarget == consumption.uiTarget
                && action.binding == consumption.binding
                && action.action == consumption.action;
        }

        [[nodiscard]]
        auto refusedAsDuplicateIdentifier(
            SnapshotObservationSpec const& spec,
            SnapshotObservationConsumption const& consumption
        ) -> bool
        {
            return std::ranges::count_if(
                spec.uiActions,
                [&consumption](ObservedUiAction const& action)
                {
                    return sameActionIdentity(action, consumption);
                }
            ) > 1;
        }

        [[nodiscard]]
        auto refusedAsUnknownUiTarget(
            SnapshotObservationSpec const& spec,
            SnapshotObservationConsumption const& consumption
        ) -> bool
        {
            return std::ranges::none_of(
                spec.uiActions,
                [&consumption](ObservedUiAction const& action)
                {
                    return action.uiTarget == consumption.uiTarget;
                }
            );
        }

        [[nodiscard]]
        auto refusedAsUnknownBinding(
            SnapshotObservationSpec const& spec,
            SnapshotObservationConsumption const& consumption
        ) -> bool
        {
            return std::ranges::none_of(
                spec.uiActions,
                [&consumption](ObservedUiAction const& action)
                {
                    return action.uiTarget == consumption.uiTarget
                        && action.binding == consumption.binding;
                }
            );
        }

        [[nodiscard]]
        auto refusedAsUnknownAction(
            SnapshotObservationSpec const& spec,
            SnapshotObservationConsumption const& consumption
        ) -> bool
        {
            return std::ranges::none_of(
                spec.uiActions,
                [&consumption](ObservedUiAction const& action)
                {
                    return sameActionIdentity(action, consumption);
                }
            );
        }

        [[nodiscard]]
        auto refusedAsActionKindMismatch(
            SnapshotObservationSpec const& spec,
            SnapshotObservationConsumption const& consumption
        ) -> bool
        {
            auto const found = std::ranges::find_if(
                spec.uiActions,
                [&consumption](ObservedUiAction const& action)
                {
                    return sameActionIdentity(action, consumption);
                }
            );
            return found != spec.uiActions.end()
                && found->kind != consumption.expectedActionKind;
        }

        constexpr auto k_bindingRefusals = std::array{
            BindingRefusal{ObservationRefusal::Stale, &refusedAsStale},
            BindingRefusal{
                ObservationRefusal::ForeignTarget,
                &refusedAsForeignTarget,
            },
            BindingRefusal{
                ObservationRefusal::ForeignRegistration,
                &refusedAsForeignRegistration,
            },
            BindingRefusal{
                ObservationRefusal::ForeignRuntimeArtifact,
                &refusedAsForeignRuntimeArtifact,
            },
            BindingRefusal{
                ObservationRefusal::ChangedGeneration,
                &refusedAsChangedGeneration,
            },
            BindingRefusal{
                ObservationRefusal::DuplicateIdentifier,
                &refusedAsDuplicateIdentifier,
            },
            BindingRefusal{
                ObservationRefusal::UnknownUiTarget,
                &refusedAsUnknownUiTarget,
            },
            BindingRefusal{
                ObservationRefusal::UnknownBinding,
                &refusedAsUnknownBinding,
            },
            BindingRefusal{
                ObservationRefusal::UnknownAction,
                &refusedAsUnknownAction,
            },
            BindingRefusal{
                ObservationRefusal::ActionKindMismatch,
                &refusedAsActionKindMismatch,
            },
        };

        // Which automation vocabulary a refusal belongs to. StaleObservation
        // already exists for exactly the expiry case; everything else is a
        // judged and rejected action, except bytes this Framework never minted,
        // which is a malformed resource rather than a rejected one.
        [[nodiscard]]
        auto refusalErrorKind(ObservationRefusal refusal) noexcept
            -> AutomationErrorKind
        {
            switch (refusal)
            {
            case ObservationRefusal::Unminted:
                return AutomationErrorKind::InvalidResource;
            case ObservationRefusal::Stale:
                return AutomationErrorKind::StaleObservation;
            case ObservationRefusal::AlreadyConsumed:
            case ObservationRefusal::ForeignTarget:
            case ObservationRefusal::ForeignRegistration:
            case ObservationRefusal::ForeignRuntimeArtifact:
            case ObservationRefusal::ChangedGeneration:
            case ObservationRefusal::DuplicateIdentifier:
            case ObservationRefusal::UnknownUiTarget:
            case ObservationRefusal::UnknownBinding:
            case ObservationRefusal::UnknownAction:
            case ObservationRefusal::ActionKindMismatch:
                return AutomationErrorKind::ActionRejected;
            }

            UF_UNREACHABLE_MSG("Unknown ObservationRefusal value");
        }
    }

    auto observationRefusalWireName(ObservationRefusal refusal) noexcept
        -> std::string_view
    {
        switch (refusal)
        {
        case ObservationRefusal::Unminted: return "unminted";
        case ObservationRefusal::AlreadyConsumed: return "already_consumed";
        case ObservationRefusal::Stale: return "stale";
        case ObservationRefusal::ForeignTarget: return "foreign_target";
        case ObservationRefusal::ForeignRegistration:
            return "foreign_registration";
        case ObservationRefusal::ForeignRuntimeArtifact:
            return "foreign_runtime_artifact";
        case ObservationRefusal::ChangedGeneration: return "changed_generation";
        case ObservationRefusal::DuplicateIdentifier:
            return "duplicate_identifier";
        case ObservationRefusal::UnknownUiTarget: return "unknown_ui_target";
        case ObservationRefusal::UnknownBinding: return "unknown_binding";
        case ObservationRefusal::UnknownAction: return "unknown_action";
        case ObservationRefusal::ActionKindMismatch:
            return "action_kind_mismatch";
        }

        UF_UNREACHABLE_MSG("Unknown ObservationRefusal value");
    }

    auto observationRefusalDiagnostic(ObservationRefusal refusal) noexcept
        -> std::string_view
    {
        switch (refusal)
        {
        case ObservationRefusal::Unminted:
            return "unminted: these bytes are not an observation reference this "
                   "Framework minted";
        case ObservationRefusal::AlreadyConsumed:
            return "already_consumed: this observation authority was already "
                   "spent by one native input";
        case ObservationRefusal::Stale:
            return "stale: this observation reference expired before it was "
                   "presented";
        case ObservationRefusal::ForeignTarget:
            return "foreign_target: this observation reference was minted for "
                   "another controlled target";
        case ObservationRefusal::ForeignRegistration:
            return "foreign_registration: this observation reference was minted "
                   "under another Project registration";
        case ObservationRefusal::ForeignRuntimeArtifact:
            return "foreign_runtime_artifact: this observation reference was "
                   "read through another RuntimeArtifact";
        case ObservationRefusal::ChangedGeneration:
            return "changed_generation: the Host generation moved after this "
                   "observation reference was minted";
        case ObservationRefusal::DuplicateIdentifier:
            return "duplicate_identifier: this observation names one UI action "
                   "identity more than once";
        case ObservationRefusal::UnknownUiTarget:
            return "unknown_ui_target: the named observation contains no such "
                   "ui_target identifier";
        case ObservationRefusal::UnknownBinding:
            return "unknown_binding: the named observation contains no such "
                   "binding identifier for that ui_target";
        case ObservationRefusal::UnknownAction:
            return "unknown_action: the named observation contains no such "
                   "action identifier for that binding";
        case ObservationRefusal::ActionKindMismatch:
            return "action_kind_mismatch: the named observation declares a "
                   "different action kind than this Tool";
        }

        UF_UNREACHABLE_MSG("Unknown ObservationRefusal value");
    }

    SnapshotObservationReference::SnapshotObservationReference(
        SnapshotObservationSpec spec,
        CanonicalJson wire
    )
        : m_spec{std::move(spec)}
        , m_wire{std::move(wire)}
    {
    }

    auto SnapshotObservationReference::identity() const -> ContentHash
    {
        return m_wire.contentHash();
    }

    auto SnapshotObservationReference::wire() const noexcept
        -> CanonicalJson const&
    {
        return m_wire;
    }

    auto SnapshotObservationReference::spec() const noexcept
        -> SnapshotObservationSpec const&
    {
        return m_spec;
    }

    ResolvedSnapshotObservation::ResolvedSnapshotObservation(
        ContentHash referenceIdentity,
        ContentHash frameIdentityHash,
        ContentHash screenshotSha256,
        std::string controlledTargetId,
        std::string uiTarget,
        std::string binding,
        std::string action,
        std::string actionKind,
        uint64 hostGeneration
    )
        : m_referenceIdentity{referenceIdentity}
        , m_frameIdentityHash{frameIdentityHash}
        , m_screenshotSha256{screenshotSha256}
        , m_controlledTargetId{std::move(controlledTargetId)}
        , m_uiTarget{std::move(uiTarget)}
        , m_binding{std::move(binding)}
        , m_action{std::move(action)}
        , m_actionKind{std::move(actionKind)}
        , m_hostGeneration{hostGeneration}
    {
    }

    auto ResolvedSnapshotObservation::referenceIdentity() const -> ContentHash
    {
        return m_referenceIdentity;
    }

    auto ResolvedSnapshotObservation::frameIdentityHash() const -> ContentHash
    {
        return m_frameIdentityHash;
    }

    auto ResolvedSnapshotObservation::screenshotSha256() const -> ContentHash
    {
        return m_screenshotSha256;
    }

    auto ResolvedSnapshotObservation::controlledTargetId() const noexcept
        -> std::string const&
    {
        return m_controlledTargetId;
    }

    auto ResolvedSnapshotObservation::uiTarget() const noexcept
        -> std::string const&
    {
        return m_uiTarget;
    }

    auto ResolvedSnapshotObservation::binding() const noexcept
        -> std::string const&
    {
        return m_binding;
    }

    auto ResolvedSnapshotObservation::action() const noexcept
        -> std::string const&
    {
        return m_action;
    }

    auto ResolvedSnapshotObservation::actionKind() const noexcept
        -> std::string const&
    {
        return m_actionKind;
    }

    auto ResolvedSnapshotObservation::hostGeneration() const noexcept -> uint64
    {
        return m_hostGeneration;
    }

    auto SnapshotObservationAuthority::findMinted(
        std::string_view exactReferenceJcs
    ) const noexcept -> SnapshotObservationReference const*
    {
        auto const found = std::ranges::find_if(
            m_minted,
            [exactReferenceJcs](SnapshotObservationReference const& reference)
            {
                return reference.wire().bytes() == exactReferenceJcs;
            }
        );
        return found == m_minted.end() ? nullptr : &*found;
    }

    auto SnapshotObservationAuthority::mint(SnapshotObservationSpec spec)
        -> Result<SnapshotObservationReference>
    {
        if (spec.controlledTargetId.empty())
        {
            return refuseSpec(
                "an observation reference must name the controlled target it "
                "was read from"
            );
        }
        if (spec.expiresAtUnixMillis == 0U)
        {
            return refuseSpec(
                "an observation reference must carry a non-zero expiry instant"
            );
        }
        if (!std::ranges::all_of(spec.uiActions, &actionNamesAreValid))
        {
            return refuseSpec(
                "an observed UI action must name its ui_target, binding, "
                "action and kind"
            );
        }
        UF_TRY_VALUE_CONTEXT(
            wire,
            CanonicalJson::parseExact(
                json::canonicalBytes(referenceMaterial(spec))
            ),
            "rendering the observation reference"
        );
        if (findMinted(wire.bytes()) != nullptr)
        {
            // Two observations with every binding equal are one observation,
            // and a second record of it would carry its own spent state. That
            // is a second authority over one frame, so it is refused rather
            // than aliased.
            return fail(
                AutomationErrorKind::ActionRejected,
                "an observation reference with these exact bytes was already "
                "minted"
            );
        }
        m_minted.emplace_back(
            SnapshotObservationReference{std::move(spec), std::move(wire)}
        );
        return m_minted.back();
    }

    auto SnapshotObservationAuthority::presented(
        CanonicalJson const& canonicalArgs
    ) const -> Result<std::optional<SnapshotObservationReference>>
    {
        auto const* const p_presented = canonicalArgs.value().find(
            k_observationReferenceArgument
        );
        if (p_presented == nullptr)
        {
            return std::optional<SnapshotObservationReference>{};
        }
        auto const* const p_minted = findMinted(
            json::canonicalBytes(*p_presented)
        );
        if (p_minted == nullptr)
        {
            return fail(
                refusalErrorKind(ObservationRefusal::Unminted),
                std::string{
                    observationRefusalDiagnostic(ObservationRefusal::Unminted)
                }
            );
        }
        return std::optional{*p_minted};
    }

    auto SnapshotObservationAuthority::refuse(
        SnapshotObservationConsumption const& consumption
    ) const -> std::optional<ObservationRefusal>
    {
        auto const* const p_reference = findMinted(
            consumption.exactReferenceJcs
        );
        if (p_reference == nullptr)
        {
            return ObservationRefusal::Unminted;
        }
        if (std::ranges::contains(m_spent, p_reference->identity()))
        {
            return ObservationRefusal::AlreadyConsumed;
        }
        auto const& spec  = p_reference->spec();
        auto const  first = std::ranges::find_if(
            k_bindingRefusals,
            [&spec, &consumption](BindingRefusal const& candidate)
            {
                return candidate.refused(spec, consumption);
            }
        );
        if (first == k_bindingRefusals.end())
        {
            return std::nullopt;
        }
        return first->kind;
    }

    auto SnapshotObservationAuthority::resolve(
        SnapshotObservationConsumption const& consumption
    ) -> Result<ResolvedSnapshotObservation>
    {
        auto const refusal = refuse(consumption);
        if (refusal)
        {
            auto diagnostic = std::string{observationRefusalDiagnostic(*refusal)};
            switch (*refusal)
            {
            case ObservationRefusal::UnknownUiTarget:
                diagnostic += ": " + consumption.uiTarget;
                break;
            case ObservationRefusal::UnknownBinding:
                diagnostic += ": " + consumption.binding;
                break;
            case ObservationRefusal::UnknownAction:
            case ObservationRefusal::ActionKindMismatch:
                diagnostic += ": " + consumption.action;
                break;
            case ObservationRefusal::Unminted:
            case ObservationRefusal::AlreadyConsumed:
            case ObservationRefusal::Stale:
            case ObservationRefusal::ForeignTarget:
            case ObservationRefusal::ForeignRegistration:
            case ObservationRefusal::ForeignRuntimeArtifact:
            case ObservationRefusal::ChangedGeneration:
            case ObservationRefusal::DuplicateIdentifier:
                break;
            }
            return fail(
                refusalErrorKind(*refusal),
                std::move(diagnostic)
            );
        }
        auto const* const p_reference = findMinted(
            consumption.exactReferenceJcs
        );
        UF_CHECK(p_reference != nullptr);

        // Appending to the spent set leaves m_minted untouched, so the borrow
        // above outlives the record of the spend it authorises.
        auto const& spec = p_reference->spec();
        m_spent.emplace_back(p_reference->identity());
        return ResolvedSnapshotObservation{
            p_reference->identity(),
            spec.frameIdentityHash,
            spec.screenshotSha256,
            spec.controlledTargetId,
            consumption.uiTarget,
            consumption.binding,
            consumption.action,
            consumption.expectedActionKind,
            spec.hostGeneration,
        };
    }
}
