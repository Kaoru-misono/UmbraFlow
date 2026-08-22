#include "tool-invocation.hpp"

#include "snapshot-reference.hpp"

#include <json/value.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uf::operator_runtime
{
    namespace
    {
        constexpr auto k_auditTool = std::string_view{
            "framework.audit.record"
        };
        constexpr auto k_coordinateInputTool = std::string_view{
            "framework.input.coordinate"
        };
        constexpr auto k_semanticInputTool = std::string_view{
            "framework.input.semantic_target"
        };
        constexpr auto k_captureTool = std::string_view{
            "framework.screen.capture"
        };
        constexpr auto k_observeTool = std::string_view{
            "framework.screen.observe"
        };
        constexpr auto k_reconcileTool = std::string_view{
            "framework.workflow.reconcile"
        };
        constexpr auto k_statusTool = std::string_view{
            "framework.workflow.status"
        };
        constexpr auto k_waitTool = std::string_view{
            "framework.workflow.wait"
        };

        // The one effect a Framework input Tool proposes. Framework owns this
        // vocabulary the way a Project owns its own: the type and scope are
        // spelled here, and their bytes reach tool_catalog_hash through the
        // descriptor material, so widening either moves the catalog identity.
        constexpr auto k_inputEffectType = std::string_view{
            "framework.input.deliver"
        };
        constexpr auto k_inputEffectScope = std::string_view{
            "controlled_target"
        };

        constexpr auto k_frameworkToolVersion = std::string_view{"1"};
        constexpr auto k_maximumObserveMillis = uint64{10'000U};
        constexpr auto k_maximumCaptureMillis = uint64{10'000U};
        constexpr auto k_maximumWaitMillis = uint64{60'000U};
        constexpr auto k_maximumAuditMillis = uint64{1'000U};
        constexpr auto k_maximumStatusMillis = uint64{1'000U};
        constexpr auto k_maximumInputMillis = uint64{15'000U};
        constexpr auto k_maximumReconcileMillis = uint64{30'000U};
        constexpr auto k_maximumCallerIdentityBytes = std::size_t{256U};

        auto appendIdentityPart(
            std::string& material,
            std::string_view value
        ) -> void
        {
            material += std::to_string(value.size());
            material.push_back(':');
            material += value;
        }

        auto appendIdentityHash(
            std::string& material,
            ContentHash const& hash
        ) -> void
        {
            appendIdentityPart(material, hash.toString());
        }

        struct AppendProviderIdentity final
        {
            std::string& material;

            auto operator()(FrameworkToolProvider const& provider) const -> void
            {
                appendIdentityPart(material, "framework");
                appendIdentityHash(material, provider.toolCatalogHash);
            }

            auto operator()(ProjectToolProvider const& provider) const -> void
            {
                appendIdentityPart(material, "project");
                appendIdentityHash(material, provider.projectRegistrationHash);
                appendIdentityHash(material, provider.toolCatalogHash);
            }
        };

        [[nodiscard]]
        auto rootIdentityMaterial(
            CallerIdempotencyNamespace const& callerNamespace,
            RootRequestKey const& requestKey,
            CanonicalJson const& requestPreimage
        ) -> std::string
        {
            auto material = std::string{"umbraflow-internal-tool-root-v0"};
            appendIdentityPart(material, callerNamespace.value());
            appendIdentityPart(material, requestKey.value());
            appendIdentityHash(material, requestPreimage.contentHash());
            return material;
        }

        // The preimage moved to v1 when the root run became a real positioned
        // call: every position now names a durable parent coordinate, so the
        // child/top-level discriminator and the conditional parent hash are
        // gone and the parent is appended unconditionally.
        [[nodiscard]]
        auto callIdentityMaterial(
            ContentHash const& rootIdentity,
            ContentHash const& parentIdentity,
            uint32 sequence,
            ToolExecutionIdentity const& executionIdentity,
            ValidatedToolInvocation const& invocation,
            std::optional<ContentHash> const& observationReference
        ) -> std::string
        {
            auto material = std::string{"umbraflow-internal-tool-call-v1"};
            appendIdentityHash(material, rootIdentity);
            appendIdentityHash(material, parentIdentity);
            appendIdentityPart(material, std::to_string(sequence));
            appendIdentityHash(material, executionIdentity.runIdentity);
            appendIdentityHash(
                material,
                executionIdentity.frameworkReleaseIdentity
            );
            appendIdentityHash(
                material,
                executionIdentity.toolRuntimeProtocolIdentity
            );
            appendIdentityHash(material, executionIdentity.environmentIdentity);
            std::visit(AppendProviderIdentity{material}, invocation.provider());
            appendIdentityPart(material, invocation.toolName());
            appendIdentityPart(material, invocation.descriptor().toolVersion);
            appendIdentityHash(material, invocation.canonicalArgs().contentHash());
            appendIdentityPart(
                material,
                observationReference.has_value() ? "observed" : "unobserved"
            );
            if (observationReference)
            {
                appendIdentityHash(material, *observationReference);
            }
            return material;
        }

        using FrameworkArgumentValidator = Status (*)(CanonicalJson const&);
        using FrameworkArgumentMaterial = json::Value (*)();

        // Result rather than a plain descriptor: a mutating Framework Tool
        // declares an effect bound, and an effect bound names the sha256 of a
        // payload schema. Deriving that digest can fail, and a factory that
        // could not say so would have to invent a hash.
        using FrameworkDescriptorFactory = Result<ToolDescriptor> (*)();

        struct FrameworkToolDefinition final
        {
            std::string_view          name{};
            FrameworkDescriptorFactory descriptor{};
            FrameworkArgumentValidator validateArguments{};
            FrameworkArgumentMaterial argumentMaterial{};
        };

        [[nodiscard]]
        auto invalidFrameworkArguments(std::string message)
            -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::move(message)
            );
        }

        // The payload shape a Framework input effect carries. It is rendered
        // here rather than read from a file so that the Framework owns its own
        // effect schema exactly the way it owns its argument contracts, and so
        // that its digest is derived from material already inside
        // tool_catalog_hash.
        [[nodiscard]]
        auto inputEffectPayloadMaterial() -> json::Value
        {
            return json::Value::ofObject({
                {"additional_properties", json::Value::ofBoolean(false)},
                {"required",
                 json::Value::ofArray({
                     json::Value::ofString("controlled_target_id"),
                     json::Value::ofString("frame_identity_hash"),
                     json::Value::ofString("ui_action"),
                 })},
                {"type", json::Value::ofString("object")},
            });
        }

        [[nodiscard]]
        auto inputEffectPayloadSchemaHash() -> Result<ContentHash>
        {
            auto const bytes = json::canonicalBytes(
                inputEffectPayloadMaterial()
            );
            return sha256(std::as_bytes(std::span{bytes}));
        }

        // The bound a Framework input Tool declares for its one effect. Risk is
        // the caller's argument: a semantic target was resolved against an
        // observation the Framework minted, while a bare coordinate was named
        // by the caller and can land anywhere.
        [[nodiscard]]
        auto inputEffectBounds(Risk maximumRisk)
            -> Result<std::vector<EffectBound>>
        {
            UF_TRY_VALUE(payloadSchemaHash, inputEffectPayloadSchemaHash());
            auto bounds = std::vector<EffectBound>{};
            bounds.emplace_back(EffectBound{
                .namespacedType    = std::string{k_inputEffectType},
                .scopeKind         = std::string{k_inputEffectScope},
                .payloadSchemaHash = payloadSchemaHash,
                .maximumRisk       = maximumRisk,
            });
            return bounds;
        }

        [[nodiscard]]
        auto observeDescriptor() -> Result<ToolDescriptor>
        {
            return ToolDescriptor{
                .toolVersion          = std::string{k_frameworkToolVersion},
                .requiredCapabilities = {},
                .effectBounds         = {},
                .uiActionBounds       = {},
                .limits               = WorkflowLimits{
                    .maximumSteps         = 1U,
                    .maximumDispatches    = 0U,
                    .maximumObservations  = 1U,
                    .maximumWaits         = 0U,
                    .maximumElapsedMillis = k_maximumObserveMillis,
                },
                .timeout = TimeoutPolicy{
                    .maximumElapsedMillis = k_maximumObserveMillis,
                    .onTimeout            = TimeoutAction::Stop,
                },
                .mutability  = ToolMutability::ReadOnly,
                .surface     = ToolSurface::Semantic,
                .idempotency = ToolIdempotency::ReadSafe,
            };
        }

        // Raw frame material, which is why the surface is Privileged while
        // observe's is Semantic. Section 6 of the cycle SPI plan makes whether
        // a profile may receive image bytes an offered-Tool decision, and the
        // surface IS that decision: a second capability gate beside it would be
        // two authorities over one question.
        [[nodiscard]]
        auto captureDescriptor() -> Result<ToolDescriptor>
        {
            return ToolDescriptor{
                .toolVersion          = std::string{k_frameworkToolVersion},
                .requiredCapabilities = {},
                .effectBounds         = {},
                .uiActionBounds       = {},
                .limits               = WorkflowLimits{
                    .maximumSteps         = 1U,
                    .maximumDispatches    = 0U,
                    .maximumObservations  = 1U,
                    .maximumWaits         = 0U,
                    .maximumElapsedMillis = k_maximumCaptureMillis,
                },
                .timeout = TimeoutPolicy{
                    .maximumElapsedMillis = k_maximumCaptureMillis,
                    .onTimeout            = TimeoutAction::Stop,
                },
                .mutability  = ToolMutability::ReadOnly,
                .surface     = ToolSurface::Privileged,
                .idempotency = ToolIdempotency::ReadSafe,
            };
        }

        // Run and call-tree status. It observes no frame and spends no
        // observation, so its limits admit neither.
        [[nodiscard]]
        auto statusDescriptor() -> Result<ToolDescriptor>
        {
            return ToolDescriptor{
                .toolVersion          = std::string{k_frameworkToolVersion},
                .requiredCapabilities = {},
                .effectBounds         = {},
                .uiActionBounds       = {},
                .limits               = WorkflowLimits{
                    .maximumSteps         = 1U,
                    .maximumDispatches    = 0U,
                    .maximumObservations  = 0U,
                    .maximumWaits         = 0U,
                    .maximumElapsedMillis = k_maximumStatusMillis,
                },
                .timeout = TimeoutPolicy{
                    .maximumElapsedMillis = k_maximumStatusMillis,
                    .onTimeout            = TimeoutAction::Stop,
                },
                .mutability  = ToolMutability::ReadOnly,
                .surface     = ToolSurface::Semantic,
                .idempotency = ToolIdempotency::ReadSafe,
            };
        }

        // Per ruling R5 this is a genuine catalog Tool and not an Operator-side
        // append: the scoped SDK exposes no native primitive except by making a
        // normal Tool call, and an append outside the Tool boundary would
        // re-append on every deterministic restart because only Tool calls
        // short-circuit on replay.
        //
        // ReadOnly is the correct classification even though the call persists
        // a durable record, for the same reason framework.screen.observe is
        // read-only while persisting a durable snapshot reference: read-only
        // here means no external-world effect requiring plan authority and
        // approval grants. It therefore declares no effect bound and needs no
        // OperatorPlanAuthority, while still spending Tool-call budget.
        [[nodiscard]]
        auto auditDescriptor() -> Result<ToolDescriptor>
        {
            return ToolDescriptor{
                .toolVersion          = std::string{k_frameworkToolVersion},
                .requiredCapabilities = {},
                .effectBounds         = {},
                .uiActionBounds       = {},
                .limits               = WorkflowLimits{
                    .maximumSteps         = 1U,
                    .maximumDispatches    = 0U,
                    .maximumObservations  = 0U,
                    .maximumWaits         = 0U,
                    .maximumElapsedMillis = k_maximumAuditMillis,
                },
                .timeout = TimeoutPolicy{
                    .maximumElapsedMillis = k_maximumAuditMillis,
                    .onTimeout            = TimeoutAction::Stop,
                },
                .mutability  = ToolMutability::ReadOnly,
                .surface     = ToolSurface::Semantic,
                .idempotency = ToolIdempotency::ReadSafe,
            };
        }

        // The Framework-owned reconciliation Tool section 5.4 requires: the one
        // transition that may consume fresh Host evidence and classify a
        // previously possible mutating call. It observes and does not deliver,
        // so it is ReadOnly; it is Privileged because resolving another call's
        // ambiguity is Framework authority and not an ordinary actor's.
        [[nodiscard]]
        auto reconcileDescriptor() -> Result<ToolDescriptor>
        {
            return ToolDescriptor{
                .toolVersion          = std::string{k_frameworkToolVersion},
                .requiredCapabilities = {},
                .effectBounds         = {},
                .uiActionBounds       = {},
                .limits               = WorkflowLimits{
                    .maximumSteps         = 1U,
                    .maximumDispatches    = 0U,
                    .maximumObservations  = 1U,
                    .maximumWaits         = 0U,
                    .maximumElapsedMillis = k_maximumReconcileMillis,
                },
                .timeout = TimeoutPolicy{
                    .maximumElapsedMillis = k_maximumReconcileMillis,
                    .onTimeout            = TimeoutAction::Stop,
                },
                .mutability  = ToolMutability::ReadOnly,
                .surface     = ToolSurface::Privileged,
                .idempotency = ToolIdempotency::ReadSafe,
            };
        }

        // Native input against a snapshot-scoped semantic target. Semantic
        // surface deliberately: this is the input an ordinary online actor is
        // meant to reach, and the target it names was resolved by Framework
        // against an observation Framework minted.
        //
        // NonIdempotent because the question the field answers is what
        // REDELIVERING one call would cost, and a second delivered click is a
        // second click. Single use of the observation authority is a separate
        // and stronger guarantee, and neither substitutes for the other.
        [[nodiscard]]
        auto semanticInputDescriptor() -> Result<ToolDescriptor>
        {
            UF_TRY_VALUE(bounds, inputEffectBounds(Risk::Medium));
            return ToolDescriptor{
                .toolVersion          = std::string{k_frameworkToolVersion},
                .requiredCapabilities = {},
                .effectBounds         = std::move(bounds),
                .uiActionBounds       = {std::string{k_semanticInputTool}},
                .limits               = WorkflowLimits{
                    .maximumSteps         = 1U,
                    .maximumDispatches    = 1U,
                    .maximumObservations  = 0U,
                    .maximumWaits         = 0U,
                    .maximumElapsedMillis = k_maximumInputMillis,
                },
                .timeout = TimeoutPolicy{
                    .maximumElapsedMillis = k_maximumInputMillis,
                    .onTimeout            = TimeoutAction::Reconcile,
                },
                .mutability  = ToolMutability::Mutating,
                .surface     = ToolSurface::Semantic,
                .idempotency = ToolIdempotency::NonIdempotent,
            };
        }

        // Bare coordinates. Section 3.2 keeps low-level input a Privileged
        // surface, so being a Framework Tool does not make it generally
        // available, and its effect bound admits a higher risk because nothing
        // resolved the point it lands on.
        [[nodiscard]]
        auto coordinateInputDescriptor() -> Result<ToolDescriptor>
        {
            UF_TRY_VALUE(bounds, inputEffectBounds(Risk::High));
            return ToolDescriptor{
                .toolVersion          = std::string{k_frameworkToolVersion},
                .requiredCapabilities = {},
                .effectBounds         = std::move(bounds),
                .uiActionBounds       = {std::string{k_coordinateInputTool}},
                .limits               = WorkflowLimits{
                    .maximumSteps         = 1U,
                    .maximumDispatches    = 1U,
                    .maximumObservations  = 0U,
                    .maximumWaits         = 0U,
                    .maximumElapsedMillis = k_maximumInputMillis,
                },
                .timeout = TimeoutPolicy{
                    .maximumElapsedMillis = k_maximumInputMillis,
                    .onTimeout            = TimeoutAction::Reconcile,
                },
                .mutability  = ToolMutability::Mutating,
                .surface     = ToolSurface::Privileged,
                .idempotency = ToolIdempotency::NonIdempotent,
            };
        }

        [[nodiscard]]
        auto waitDescriptor() -> Result<ToolDescriptor>
        {
            return ToolDescriptor{
                .toolVersion          = std::string{k_frameworkToolVersion},
                .requiredCapabilities = {},
                .effectBounds         = {},
                .uiActionBounds       = {},
                .limits               = WorkflowLimits{
                    .maximumSteps         = 1U,
                    .maximumDispatches    = 0U,
                    .maximumObservations  = 0U,
                    .maximumWaits         = 1U,
                    .maximumElapsedMillis = k_maximumWaitMillis,
                },
                .timeout = TimeoutPolicy{
                    .maximumElapsedMillis = k_maximumWaitMillis,
                    .onTimeout            = TimeoutAction::Stop,
                },
                .mutability  = ToolMutability::ReadOnly,
                .surface     = ToolSurface::Semantic,
                .idempotency = ToolIdempotency::ReadSafe,
            };
        }

        // Three Framework Tools take no arguments at all. The tool name is a
        // parameter so that each still refuses in its own words, while the one
        // rule they share is stated once.
        [[nodiscard]]
        auto requireNoArguments(
            CanonicalJson const& arguments,
            std::string_view toolName
        ) -> Status
        {
            auto const& value = arguments.value();
            if (
                value.kind() != json::ValueKind::Object
                || !value.members().empty()
            )
            {
                return invalidFrameworkArguments(
                    std::string{toolName} + " arguments must be exactly {}"
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto validateObserveArguments(CanonicalJson const& arguments) -> Status
        {
            return requireNoArguments(arguments, k_observeTool);
        }

        [[nodiscard]]
        auto validateCaptureArguments(CanonicalJson const& arguments) -> Status
        {
            return requireNoArguments(arguments, k_captureTool);
        }

        [[nodiscard]]
        auto validateStatusArguments(CanonicalJson const& arguments) -> Status
        {
            return requireNoArguments(arguments, k_statusTool);
        }

        // The record is the whole of the call's durable outcome, so it is the
        // whole of the call's arguments. There is deliberately no attribution
        // member: per R5 the root and position are supplied by the seam, and a
        // caller that could state them could attribute its record to another
        // call's position.
        [[nodiscard]]
        auto validateAuditArguments(CanonicalJson const& arguments) -> Status
        {
            auto const& value = arguments.value();
            auto const* const p_record = value.find("record");
            if (
                value.kind() != json::ValueKind::Object
                || value.members().size() != 1U
                || p_record == nullptr
                || p_record->kind() != json::ValueKind::Object
            )
            {
                return invalidFrameworkArguments(
                    "framework.audit.record requires only an object record"
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto validateReconcileArguments(CanonicalJson const& arguments)
            -> Status
        {
            auto const& value = arguments.value();
            auto const* const p_call = value.find("call_identity");
            if (
                value.kind() != json::ValueKind::Object
                || value.members().size() != 1U
                || p_call == nullptr
                || p_call->kind() != json::ValueKind::String
                || !ContentHash::parse(p_call->string()).has_value()
            )
            {
                return invalidFrameworkArguments(
                    "framework.workflow.reconcile requires only a "
                    "call_identity content hash"
                );
            }
            return ok();
        }

        // observation_reference is an OBJECT and never a string carrying JSON.
        // Per R3 a handle stays plain JSON data, so the reference travels as
        // the value it is; re-rendering that member canonically reproduces the
        // exact bytes the Framework minted, which is what the resolution
        // boundary recognises it by.
        [[nodiscard]]
        auto validateSemanticInputArguments(CanonicalJson const& arguments)
            -> Status
        {
            auto const& value = arguments.value();
            auto const* const p_reference = value.find("observation_reference");
            auto const* const p_target = value.find("semantic_target");
            auto const* const p_action = value.find("ui_action");
            if (
                value.kind() != json::ValueKind::Object
                || value.members().size() != 3U
                || p_reference == nullptr
                || p_reference->kind() != json::ValueKind::Object
                || p_target == nullptr
                || p_target->kind() != json::ValueKind::String
                || p_target->string().empty()
                || p_action == nullptr
                || p_action->kind() != json::ValueKind::String
                || p_action->string().empty()
            )
            {
                return invalidFrameworkArguments(
                    "framework.input.semantic_target requires an object "
                    "observation_reference and a non-empty semantic_target "
                    "and ui_action"
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto validateCoordinateInputArguments(CanonicalJson const& arguments)
            -> Status
        {
            auto const& value = arguments.value();
            auto const* const p_action = value.find("action");
            auto const* const p_x = value.find("x");
            auto const* const p_y = value.find("y");
            if (
                value.kind() != json::ValueKind::Object
                || value.members().size() != 3U
                || p_action == nullptr
                || p_action->kind() != json::ValueKind::String
                || p_action->string().empty()
                || p_x == nullptr
                || !p_x->isInteger()
                || p_y == nullptr
                || !p_y->isInteger()
            )
            {
                return invalidFrameworkArguments(
                    "framework.input.coordinate requires a non-empty action "
                    "and integer x and y"
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto validateWaitArguments(CanonicalJson const& arguments) -> Status
        {
            auto const& value = arguments.value();
            auto const* const p_duration = value.find("duration_ms");
            if (
                value.kind() != json::ValueKind::Object
                || value.members().size() != 1U
                || p_duration == nullptr
                || !p_duration->isInteger()
                || p_duration->number() < 0.0
                || p_duration->number()
                    > static_cast<double>(k_maximumWaitMillis)
            )
            {
                return invalidFrameworkArguments(
                    "framework.workflow.wait requires only integer duration_ms "
                    "in [0, 60000]"
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto noArgumentsMaterial() -> json::Value
        {
            return json::Value::ofObject({
                {"additional_properties", json::Value::ofBoolean(false)},
                {"type", json::Value::ofString("object")},
            });
        }

        [[nodiscard]]
        auto requiredMembersMaterial(std::vector<json::Value> required)
            -> json::Value
        {
            return json::Value::ofObject({
                {"additional_properties", json::Value::ofBoolean(false)},
                {"required", json::Value::ofArray(std::move(required))},
                {"type", json::Value::ofString("object")},
            });
        }

        [[nodiscard]]
        auto auditArgumentMaterial() -> json::Value
        {
            return requiredMembersMaterial({
                json::Value::ofString("record"),
            });
        }

        [[nodiscard]]
        auto reconcileArgumentMaterial() -> json::Value
        {
            return requiredMembersMaterial({
                json::Value::ofString("call_identity"),
            });
        }

        [[nodiscard]]
        auto semanticInputArgumentMaterial() -> json::Value
        {
            return requiredMembersMaterial({
                json::Value::ofString("observation_reference"),
                json::Value::ofString("semantic_target"),
                json::Value::ofString("ui_action"),
            });
        }

        [[nodiscard]]
        auto coordinateInputArgumentMaterial() -> json::Value
        {
            return requiredMembersMaterial({
                json::Value::ofString("action"),
                json::Value::ofString("x"),
                json::Value::ofString("y"),
            });
        }

        [[nodiscard]]
        auto waitArgumentMaterial() -> json::Value
        {
            return json::Value::ofObject({
                {"additional_properties", json::Value::ofBoolean(false)},
                {"maximum_duration_ms",
                 json::Value::ofNumber(
                     static_cast<double>(k_maximumWaitMillis)
                 )},
                {"required",
                 json::Value::ofArray({
                     json::Value::ofString("duration_ms"),
                 })},
                {"type", json::Value::ofString("object")},
            });
        }

        // Declared in UTF-8 byte order of the tool names, so the rendered tools
        // array is already sorted and the catalog material has one spelling
        // rather than one per declaration order.
        constexpr auto k_frameworkTools = std::array{
            FrameworkToolDefinition{
                k_auditTool,
                &auditDescriptor,
                &validateAuditArguments,
                &auditArgumentMaterial,
            },
            FrameworkToolDefinition{
                k_coordinateInputTool,
                &coordinateInputDescriptor,
                &validateCoordinateInputArguments,
                &coordinateInputArgumentMaterial,
            },
            FrameworkToolDefinition{
                k_semanticInputTool,
                &semanticInputDescriptor,
                &validateSemanticInputArguments,
                &semanticInputArgumentMaterial,
            },
            FrameworkToolDefinition{
                k_captureTool,
                &captureDescriptor,
                &validateCaptureArguments,
                &noArgumentsMaterial,
            },
            FrameworkToolDefinition{
                k_observeTool,
                &observeDescriptor,
                &validateObserveArguments,
                &noArgumentsMaterial,
            },
            FrameworkToolDefinition{
                k_reconcileTool,
                &reconcileDescriptor,
                &validateReconcileArguments,
                &reconcileArgumentMaterial,
            },
            FrameworkToolDefinition{
                k_statusTool,
                &statusDescriptor,
                &validateStatusArguments,
                &noArgumentsMaterial,
            },
            FrameworkToolDefinition{
                k_waitTool,
                &waitDescriptor,
                &validateWaitArguments,
                &waitArgumentMaterial,
            },
        };

        [[nodiscard]]
        auto frameworkDefinition(std::string_view name)
            -> FrameworkToolDefinition const*
        {
            auto const found = std::ranges::find(
                k_frameworkTools,
                name,
                &FrameworkToolDefinition::name
            );
            return found == k_frameworkTools.end() ? nullptr : &*found;
        }

        [[nodiscard]]
        auto stringArray(std::span<std::string const> values) -> json::Value
        {
            auto rendered = std::vector<json::Value>{};
            rendered.reserve(values.size());
            for (auto const& value : values)
            {
                rendered.emplace_back(json::Value::ofString(value));
            }
            return json::Value::ofArray(std::move(rendered));
        }

        [[nodiscard]]
        auto effectBoundsMaterial(std::span<EffectBound const> bounds)
            -> json::Value
        {
            auto rendered = std::vector<json::Value>{};
            rendered.reserve(bounds.size());
            for (auto const& bound : bounds)
            {
                rendered.emplace_back(json::Value::ofObject({
                    {"maximum_risk",
                     json::Value::ofString(
                         std::string{riskWireName(bound.maximumRisk)}
                     )},
                    {"namespaced_type",
                     json::Value::ofString(bound.namespacedType)},
                    {"payload_schema_hash",
                     json::Value::ofString(bound.payloadSchemaHash.hex())},
                    {"scope_kind", json::Value::ofString(bound.scopeKind)},
                }));
            }
            return json::Value::ofArray(std::move(rendered));
        }

        // A declaration is catalog material like every other bound: its bytes
        // reach tool_catalog_hash through the descriptor, so widening what a
        // Tool may delegate moves the catalog identity.
        [[nodiscard]]
        auto childEffectsMaterial(ChildEffectDeclaration const& declaration)
            -> json::Value
        {
            return json::Value::ofObject({
                {"child_tool_names", stringArray(declaration.childToolNames)},
                {"maximum_child_calls",
                 json::Value::ofNumber(
                     static_cast<double>(declaration.maximumChildCalls)
                 )},
                {"maximum_child_mutability",
                 json::Value::ofString(
                     std::string{
                         toolMutabilityWireName(
                             declaration.maximumChildMutability
                         )
                     }
                 )},
                {"maximum_child_risk",
                 json::Value::ofString(
                     std::string{riskWireName(declaration.maximumChildRisk)}
                 )},
                {"maximum_child_surface",
                 json::Value::ofString(
                     std::string{
                         toolSurfaceWireName(declaration.maximumChildSurface)
                     }
                 )},
            });
        }

        [[nodiscard]]
        auto descriptorMaterial(
            FrameworkToolDefinition const& definition,
            ToolDescriptor const& descriptor
        ) -> json::Value
        {
            return json::Value::ofObject({
                {"argument_contract", definition.argumentMaterial()},
                {"child_effects", childEffectsMaterial(descriptor.childEffects)},
                {"effect_bounds",
                 effectBoundsMaterial(descriptor.effectBounds)},
                {"idempotency",
                 json::Value::ofString(
                     std::string{
                         toolIdempotencyWireName(descriptor.idempotency)
                     }
                 )},
                {"mutability",
                 json::Value::ofString(
                     std::string{
                         toolMutabilityWireName(descriptor.mutability)
                     }
                 )},
                {"name", json::Value::ofString(std::string{definition.name})},
                {"required_capabilities",
                 stringArray(descriptor.requiredCapabilities)},
                {"surface",
                 json::Value::ofString(
                     std::string{toolSurfaceWireName(descriptor.surface)}
                 )},
                {"timeout",
                 json::Value::ofObject({
                     {"maximum_elapsed_ms",
                      json::Value::ofNumber(
                          static_cast<double>(
                              descriptor.timeout.maximumElapsedMillis
                          )
                      )},
                     {"on_timeout",
                      json::Value::ofString(
                          std::string{
                              timeoutActionWireName(
                                  descriptor.timeout.onTimeout
                              )
                          }
                      )},
                 })},
                {"tool_version",
                 json::Value::ofString(descriptor.toolVersion)},
                {"ui_action_bounds", stringArray(descriptor.uiActionBounds)},
                {"workflow_limits",
                 json::Value::ofObject({
                     {"maximum_dispatches",
                      json::Value::ofNumber(
                          static_cast<double>(
                              descriptor.limits.maximumDispatches
                          )
                      )},
                     {"maximum_elapsed_ms",
                      json::Value::ofNumber(
                          static_cast<double>(
                              descriptor.limits.maximumElapsedMillis
                          )
                      )},
                     {"maximum_observations",
                      json::Value::ofNumber(
                          static_cast<double>(
                              descriptor.limits.maximumObservations
                          )
                      )},
                     {"maximum_steps",
                      json::Value::ofNumber(
                          static_cast<double>(
                              descriptor.limits.maximumSteps
                          )
                      )},
                     {"maximum_waits",
                      json::Value::ofNumber(
                          static_cast<double>(
                              descriptor.limits.maximumWaits
                          )
                      )},
                 })},
            });
        }

        [[nodiscard]]
        auto describeTool(
            std::span<ToolCatalogEntry const> tools,
            std::string_view toolName
        ) -> Result<ToolDescriptor>
        {
            auto const found = std::ranges::find(
                tools,
                toolName,
                &ToolCatalogEntry::name
            );
            if (found == tools.end())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "Tool Catalog declares no tool named "
                        + std::string{toolName}
                );
            }
            return found->descriptor;
        }

        [[nodiscard]]
        auto offerTools(
            std::span<ToolCatalogEntry const> tools,
            ControllerProfile profile,
            std::span<std::string const> heldCapabilities
        ) -> std::vector<OfferedTool>
        {
            auto offered = std::vector<OfferedTool>{};
            for (auto const& entry : tools)
            {
                if (!toolSurfaceAllowed(profile, entry.descriptor.surface))
                {
                    continue;
                }
                if (
                    missingRequiredToolCapability(
                        heldCapabilities,
                        entry.descriptor.requiredCapabilities
                    )
                )
                {
                    continue;
                }
                offered.emplace_back(OfferedTool{
                    .name    = entry.name,
                    .version = entry.descriptor.toolVersion,
                });
            }
            return offered;
        }
    }

    auto missingRequiredToolCapability(
        std::span<std::string const> heldCapabilities,
        std::span<std::string const> requiredCapabilities
    ) -> std::optional<std::string>
    {
        auto const missing = std::ranges::find_if(
            requiredCapabilities,
            [heldCapabilities](std::string const& capability)
            {
                return !std::ranges::contains(heldCapabilities, capability);
            }
        );
        if (missing == requiredCapabilities.end())
        {
            return std::nullopt;
        }
        return *missing;
    }

    CallerIdempotencyNamespace::CallerIdempotencyNamespace(std::string value)
        : m_value{std::move(value)}
    {
    }

    auto CallerIdempotencyNamespace::create(std::string value)
        -> Result<CallerIdempotencyNamespace>
    {
        if (
            value.empty()
            || value.size() > k_maximumCallerIdentityBytes
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "caller idempotency namespace must contain 1 to 256 bytes"
            );
        }
        return CallerIdempotencyNamespace{std::move(value)};
    }

    auto CallerIdempotencyNamespace::value() const noexcept
        -> std::string const&
    {
        return m_value;
    }

    RootRequestKey::RootRequestKey(std::string value)
        : m_value{std::move(value)}
    {
    }

    auto RootRequestKey::create(std::string value) -> Result<RootRequestKey>
    {
        if (
            value.empty()
            || value.size() > k_maximumCallerIdentityBytes
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "root request key must contain 1 to 256 bytes"
            );
        }
        return RootRequestKey{std::move(value)};
    }

    auto RootRequestKey::value() const noexcept -> std::string const&
    {
        return m_value;
    }

    ToolRootRequestIdentity::ToolRootRequestIdentity(
        CallerIdempotencyNamespace callerNamespace,
        RootRequestKey requestKey,
        CanonicalJson requestPreimage,
        ContentHash identity
    )
        : m_callerNamespace{std::move(callerNamespace)}
        , m_requestKey{std::move(requestKey)}
        , m_requestPreimage{std::move(requestPreimage)}
        , m_identity{identity}
    {
    }

    auto ToolRootRequestIdentity::create(
        std::string callerNamespace,
        std::string requestKey,
        CanonicalJson requestPreimage
    ) -> Result<ToolRootRequestIdentity>
    {
        UF_TRY_VALUE(
            validatedNamespace,
            CallerIdempotencyNamespace::create(std::move(callerNamespace))
        );
        UF_TRY_VALUE(
            validatedKey,
            RootRequestKey::create(std::move(requestKey))
        );
        auto const material = rootIdentityMaterial(
            validatedNamespace,
            validatedKey,
            requestPreimage
        );
        UF_TRY_VALUE(identity, sha256(std::as_bytes(std::span{material})));
        return ToolRootRequestIdentity{
            std::move(validatedNamespace),
            std::move(validatedKey),
            std::move(requestPreimage),
            identity,
        };
    }

    auto ToolRootRequestIdentity::identity() const -> ContentHash
    {
        return m_identity;
    }

    auto ToolRootRequestIdentity::callerNamespace() const noexcept
        -> CallerIdempotencyNamespace const&
    {
        return m_callerNamespace;
    }

    auto ToolRootRequestIdentity::requestKey() const noexcept
        -> RootRequestKey const&
    {
        return m_requestKey;
    }

    auto ToolRootRequestIdentity::requestPreimage() const noexcept
        -> CanonicalJson const&
    {
        return m_requestPreimage;
    }

    auto ToolRootRequestIdentity::relationTo(
        ToolRootRequestIdentity const& other
    ) const noexcept -> RootRequestRelation
    {
        if (
            m_callerNamespace != other.m_callerNamespace
            || m_requestKey != other.m_requestKey
        )
        {
            return RootRequestRelation::Distinct;
        }
        return m_requestPreimage.bytes() == other.m_requestPreimage.bytes()
            ? RootRequestRelation::SameRequest
            : RootRequestRelation::Conflict;
    }

    ToolCallParent::ToolCallParent(
        ContentHash rootIdentity,
        ContentHash identity
    )
        : m_rootIdentity{rootIdentity}
        , m_identity{identity}
    {
    }

    auto ToolCallParent::rootIdentity() const -> ContentHash
    {
        return m_rootIdentity;
    }

    auto ToolCallParent::identity() const -> ContentHash
    {
        return m_identity;
    }

    ValidatedToolInvocation::ValidatedToolInvocation(
        ToolProviderIdentity provider,
        std::string toolName,
        CanonicalJson canonicalArgs,
        ToolDescriptor descriptor
    )
        : m_provider{std::move(provider)}
        , m_toolName{std::move(toolName)}
        , m_canonicalArgs{std::move(canonicalArgs)}
        , m_descriptor{std::move(descriptor)}
    {
    }

    auto ValidatedToolInvocation::provider() const noexcept
        -> ToolProviderIdentity const&
    {
        return m_provider;
    }

    auto ValidatedToolInvocation::toolName() const noexcept -> std::string const&
    {
        return m_toolName;
    }

    auto ValidatedToolInvocation::canonicalArgs() const noexcept
        -> CanonicalJson const&
    {
        return m_canonicalArgs;
    }

    auto ValidatedToolInvocation::descriptor() const noexcept
        -> ToolDescriptor const&
    {
        return m_descriptor;
    }

    ToolCallPositionIdentity::ToolCallPositionIdentity(
        ContentHash identity,
        ContentHash rootIdentity,
        ContentHash parentIdentity,
        uint32 sequence,
        ToolExecutionIdentity executionIdentity,
        ToolProviderIdentity provider,
        std::string toolName,
        std::string toolVersion,
        std::string canonicalArgs,
        ContentHash canonicalArgsHash,
        std::optional<ContentHash> observationReference,
        ToolDescriptor descriptor
    )
        : m_identity{identity}
        , m_rootIdentity{rootIdentity}
        , m_parentIdentity{parentIdentity}
        , m_sequence{sequence}
        , m_executionIdentity{std::move(executionIdentity)}
        , m_provider{std::move(provider)}
        , m_toolName{std::move(toolName)}
        , m_toolVersion{std::move(toolVersion)}
        , m_canonicalArgs{std::move(canonicalArgs)}
        , m_canonicalArgsHash{canonicalArgsHash}
        , m_observationReference{std::move(observationReference)}
        , m_descriptor{std::move(descriptor)}
    {
    }

    auto ToolCallPositionIdentity::create(
        ContentHash const& rootIdentity,
        ToolCallParent const& parent,
        uint32 sequence,
        ToolExecutionIdentity const& executionIdentity,
        ValidatedToolInvocation const& invocation,
        std::optional<ContentHash> observationReference
    ) -> Result<ToolCallPositionIdentity>
    {
        if (parent.rootIdentity() != rootIdentity)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "tool call parent belongs to a different root request"
            );
        }
        auto const parentIdentity = parent.identity();
        auto const material       = callIdentityMaterial(
            rootIdentity,
            parentIdentity,
            sequence,
            executionIdentity,
            invocation,
            observationReference
        );
        UF_TRY_VALUE(identity, sha256(std::as_bytes(std::span{material})));
        return ToolCallPositionIdentity{
            identity,
            rootIdentity,
            parentIdentity,
            sequence,
            executionIdentity,
            invocation.provider(),
            invocation.toolName(),
            invocation.descriptor().toolVersion,
            invocation.canonicalArgs().bytes(),
            invocation.canonicalArgs().contentHash(),
            std::move(observationReference),
            invocation.descriptor(),
        };
    }

    auto ToolCallPositionIdentity::identity() const -> ContentHash
    {
        return m_identity;
    }

    auto ToolCallPositionIdentity::rootIdentity() const -> ContentHash
    {
        return m_rootIdentity;
    }

    auto ToolCallPositionIdentity::parentIdentity() const -> ContentHash
    {
        return m_parentIdentity;
    }

    auto ToolCallPositionIdentity::sequence() const noexcept -> uint32
    {
        return m_sequence;
    }

    auto ToolCallPositionIdentity::executionIdentity() const noexcept
        -> ToolExecutionIdentity const&
    {
        return m_executionIdentity;
    }

    auto ToolCallPositionIdentity::provider() const noexcept
        -> ToolProviderIdentity const&
    {
        return m_provider;
    }

    auto ToolCallPositionIdentity::toolName() const noexcept
        -> std::string const&
    {
        return m_toolName;
    }

    auto ToolCallPositionIdentity::toolVersion() const noexcept
        -> std::string const&
    {
        return m_toolVersion;
    }

    auto ToolCallPositionIdentity::canonicalArgs() const noexcept
        -> std::string const&
    {
        return m_canonicalArgs;
    }

    auto ToolCallPositionIdentity::observationReference() const noexcept
        -> std::optional<ContentHash> const&
    {
        return m_observationReference;
    }

    auto ToolCallPositionIdentity::canonicalArgsHash() const -> ContentHash
    {
        return m_canonicalArgsHash;
    }

    auto ToolCallPositionIdentity::descriptor() const noexcept
        -> ToolDescriptor const&
    {
        return m_descriptor;
    }

    auto ToolCallPositionIdentity::asParent() const -> ToolCallParent
    {
        return ToolCallParent{m_rootIdentity, m_identity};
    }

    ToolCallIssuingContext::ToolCallIssuingContext(
        ContentHash rootIdentity,
        ToolCallParent parent,
        ToolExecutionIdentity executionIdentity
    )
        : m_rootIdentity{rootIdentity}
        , m_parent{parent}
        , m_executionIdentity{std::move(executionIdentity)}
    {
    }

    auto ToolCallIssuingContext::forRoot(
        ToolRootRequestIdentity const& root,
        ToolExecutionIdentity executionIdentity
    ) -> ToolCallIssuingContext
    {
        return ToolCallIssuingContext{
            root.identity(),
            ToolCallParent{root.identity(), root.identity()},
            std::move(executionIdentity),
        };
    }

    auto ToolCallIssuingContext::forHandler(
        ToolCallPositionIdentity const& handlerCall
    ) -> ToolCallIssuingContext
    {
        return ToolCallIssuingContext{
            handlerCall.rootIdentity(),
            handlerCall.asParent(),
            handlerCall.executionIdentity(),
        };
    }

    auto ToolCallIssuingContext::issueNext(
        ValidatedToolInvocation const& invocation,
        std::optional<ContentHash> observationReference
    ) -> Result<ToolCallPositionIdentity>
    {
        if (m_issuedChildren == std::numeric_limits<uint32>::max())
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "Tool call child index is exhausted for this issuing context"
            );
        }
        UF_TRY_VALUE(
            position,
            ToolCallPositionIdentity::create(
                m_rootIdentity,
                m_parent,
                m_issuedChildren + 1U,
                m_executionIdentity,
                invocation,
                std::move(observationReference)
            )
        );
        ++m_issuedChildren;
        return position;
    }

    auto ToolCallIssuingContext::issue(
        ValidatedToolInvocation const& invocation
    ) -> Result<ToolCallPositionIdentity>
    {
        return issueNext(invocation, std::nullopt);
    }

    auto ToolCallIssuingContext::issueAgainstObservation(
        ValidatedToolInvocation const& invocation,
        SnapshotObservationReference const& observation
    ) -> Result<ToolCallPositionIdentity>
    {
        return issueNext(invocation, observation.identity());
    }

    auto ToolCallIssuingContext::rootIdentity() const -> ContentHash
    {
        return m_rootIdentity;
    }

    auto ToolCallIssuingContext::parent() const noexcept
        -> ToolCallParent const&
    {
        return m_parent;
    }

    auto ToolCallIssuingContext::issuedChildren() const noexcept -> uint32
    {
        return m_issuedChildren;
    }

    auto toolSurfaceAllowed(
        ControllerProfile profile,
        ToolSurface surface
    ) noexcept -> bool
    {
        return !profile.semanticToolsOnly || surface == ToolSurface::Semantic;
    }

    ProjectToolCatalogSchemaOwner::ProjectToolCatalogSchemaOwner(
        ContentHash projectRegistrationHash,
        ContentHash toolCatalogHash,
        std::vector<ToolCatalogEntry> tools,
        ToolArgumentValidator validateArguments
    )
        : m_projectRegistrationHash{projectRegistrationHash}
        , m_toolCatalogHash{toolCatalogHash}
        , m_tools{std::move(tools)}
        , m_validateArguments{std::move(validateArguments)}
    {
    }

    auto ProjectToolCatalogSchemaOwner::create(
        VerifiedProjectRegistration const& registration,
        std::string_view exactToolCatalogBytes,
        ToolCatalogReader const& readCatalog,
        ToolArgumentValidator validateArguments
    ) -> Result<ProjectToolCatalogSchemaOwner>
    {
        if (!readCatalog || !validateArguments)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "ProjectToolCatalogSchemaOwner requires both catalog readers"
            );
        }
        UF_TRY_VALUE(
            catalogHash,
            sha256(std::as_bytes(std::span{exactToolCatalogBytes}))
        );
        if (catalogHash != registration.toolCatalogHash())
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool Catalog bytes do not match the registration's tool_catalog_hash"
            );
        }
        UF_TRY_VALUE_CONTEXT(
            tools,
            readCatalog(),
            "reading the Tool Catalog's declared tools"
        );
        if (tools.empty())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Tool Catalog declares no tool at all"
            );
        }
        for (auto const& entry : tools)
        {
            // The positive half, and the only half. A Project owns the
            // namespace it registered -- its plugin_id -- and a descriptor
            // naming anything outside it is declaring a Tool that is not this
            // registrant's, whether the namespace is the Framework's or another
            // Project's. The reserved-prefix refusal this replaced could only
            // catch the first of those two, and is unreachable behind this one
            // because a plugin_id inside `framework.` is refused at the
            // registration (manifest.cpp validateClaims).
            UF_TRY(validateToolNameOwnership(entry.name, registration.pluginId()));
            if (entry.descriptor.toolVersion.empty())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "Tool Catalog descriptor must carry a tool version"
                );
            }
            UF_TRY_CONTEXT(
                childEffectDeclarationValid(entry.descriptor.childEffects),
                "reading the child_effects of " + entry.name
            );
        }
        std::ranges::sort(tools, {}, &ToolCatalogEntry::name);
        auto const repeated = std::ranges::adjacent_find(
            tools,
            {},
            &ToolCatalogEntry::name
        );
        if (repeated != tools.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Tool Catalog declares one tool name twice"
            );
        }
        return ProjectToolCatalogSchemaOwner{
            registration.hash(),
            catalogHash,
            std::move(tools),
            std::move(validateArguments),
        };
    }

    auto ProjectToolCatalogSchemaOwner::projectRegistrationHash() const
        -> ContentHash
    {
        return m_projectRegistrationHash;
    }

    auto ProjectToolCatalogSchemaOwner::toolCatalogHash() const -> ContentHash
    {
        return m_toolCatalogHash;
    }

    auto ProjectToolCatalogSchemaOwner::toolNames() const
        -> std::vector<std::string>
    {
        auto names = std::vector<std::string>{};
        names.reserve(m_tools.size());
        for (auto const& tool : m_tools)
        {
            names.emplace_back(tool.name);
        }
        return names;
    }

    auto ProjectToolCatalogSchemaOwner::validate(
        std::string toolName,
        CanonicalJson canonicalArgs
    ) const -> Result<ValidatedToolInvocation>
    {
        UF_TRY_VALUE(descriptor, describe(toolName));
        UF_TRY_CONTEXT(
            m_validateArguments(toolName, canonicalArgs.bytes()),
            "validating the arguments against the schema this descriptor names"
        );
        return ValidatedToolInvocation{
            ToolProviderIdentity{ProjectToolProvider{
                .projectRegistrationHash = m_projectRegistrationHash,
                .toolCatalogHash         = m_toolCatalogHash,
            }},
            std::move(toolName),
            std::move(canonicalArgs),
            std::move(descriptor),
        };
    }

    auto ProjectToolCatalogSchemaOwner::describe(
        std::string_view toolName
    ) const -> Result<ToolDescriptor>
    {
        return withContext(
            describeTool(m_tools, toolName),
            "reading the Project Tool Catalog"
        );
    }

    auto ProjectToolCatalogSchemaOwner::offeredTools(
        ControllerProfile profile,
        std::span<std::string const> heldCapabilities
    ) const -> std::vector<OfferedTool>
    {
        return offerTools(m_tools, profile, heldCapabilities);
    }

    ProjectToolBindingTable::ProjectToolBindingTable(
        ContentHash projectRegistrationHash,
        std::vector<ProjectToolBinding> bindings
    )
        : m_projectRegistrationHash{projectRegistrationHash}
        , m_bindings{std::move(bindings)}
    {
    }

    auto ProjectToolBindingTable::bind(
        VerifiedProjectRegistration const& registration,
        ProjectToolCatalogSchemaOwner const& catalog,
        std::span<std::string const> exportedEntryPoints
    ) -> Result<ProjectToolBindingTable>
    {
        if (catalog.projectRegistrationHash() != registration.hash())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Project Tool binding requires the Tool Catalog this "
                "registration pinned"
            );
        }

        auto const& bindings = registration.projectToolBindings();
        auto const names     = catalog.toolNames();
        for (auto const& binding : bindings)
        {
            if (!std::ranges::contains(names, binding.toolName))
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
                        + ", which the Project closure does not export"
                );
            }
        }
        for (auto const& name : names)
        {
            auto const bound = std::ranges::find(
                bindings,
                name,
                &ProjectToolBinding::toolName
            );
            if (bound == bindings.end())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "Project Tool " + name
                        + " is declared with no binding to a Project entry"
                );
            }
        }
        return ProjectToolBindingTable{registration.hash(), bindings};
    }

    auto ProjectToolBindingTable::projectRegistrationHash() const -> ContentHash
    {
        return m_projectRegistrationHash;
    }

    auto ProjectToolBindingTable::bindings() const noexcept
        -> std::vector<ProjectToolBinding> const&
    {
        return m_bindings;
    }

    auto ProjectToolBindingTable::entryPointFor(
        std::string_view toolName
    ) const -> Result<std::string>
    {
        auto const bound = std::ranges::find(
            m_bindings,
            toolName,
            &ProjectToolBinding::toolName
        );
        if (bound == m_bindings.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "this Project registration binds no Tool named "
                    + std::string{toolName}
            );
        }
        return bound->entryPoint;
    }

    auto ProjectToolBindingTable::entryPoints() const -> std::vector<std::string>
    {
        auto entries = std::vector<std::string>{};
        entries.reserve(m_bindings.size());
        for (auto const& binding : m_bindings)
        {
            entries.emplace_back(binding.entryPoint);
        }
        std::ranges::sort(entries);
        auto const repeated = std::ranges::unique(entries);
        entries.erase(repeated.begin(), repeated.end());
        return entries;
    }

    FrameworkToolCatalogOwner::FrameworkToolCatalogOwner(
        ContentHash toolCatalogHash,
        std::string canonicalJcs,
        std::vector<ToolCatalogEntry> tools
    )
        : m_toolCatalogHash{toolCatalogHash}
        , m_canonicalJcs{std::move(canonicalJcs)}
        , m_tools{std::move(tools)}
    {
    }

    auto FrameworkToolCatalogOwner::create()
        -> Result<FrameworkToolCatalogOwner>
    {
        auto tools    = std::vector<ToolCatalogEntry>{};
        auto material = std::vector<json::Value>{};
        tools.reserve(k_frameworkTools.size());
        material.reserve(k_frameworkTools.size());
        for (auto const& definition : k_frameworkTools)
        {
            UF_TRY_VALUE_CONTEXT(
                descriptor,
                definition.descriptor(),
                "building a Framework Tool Catalog descriptor"
            );
            material.emplace_back(descriptorMaterial(definition, descriptor));
            tools.emplace_back(ToolCatalogEntry{
                .name       = std::string{definition.name},
                .descriptor = std::move(descriptor),
            });
        }
        auto canonicalJcs = json::canonicalBytes(json::Value::ofObject({
            // This material is deliberately internal until the execution
            // adapters and durable runtime can publish one atomic wire cut.
            {"internal_generation", json::Value::ofNumber(0.0)},
            {"owner", json::Value::ofString("framework")},
            {"tools", json::Value::ofArray(std::move(material))},
        }));
        UF_TRY_VALUE(
            catalogHash,
            sha256(std::as_bytes(std::span{canonicalJcs}))
        );
        return FrameworkToolCatalogOwner{
            catalogHash,
            std::move(canonicalJcs),
            std::move(tools),
        };
    }

    auto FrameworkToolCatalogOwner::toolCatalogHash() const -> ContentHash
    {
        return m_toolCatalogHash;
    }

    auto FrameworkToolCatalogOwner::canonicalJcs() const noexcept
        -> std::string const&
    {
        return m_canonicalJcs;
    }

    auto FrameworkToolCatalogOwner::toolNames() const -> std::vector<std::string>
    {
        auto names = std::vector<std::string>{};
        names.reserve(m_tools.size());
        for (auto const& tool : m_tools)
        {
            names.emplace_back(tool.name);
        }
        return names;
    }

    auto FrameworkToolCatalogOwner::validate(
        std::string toolName,
        CanonicalJson canonicalArgs
    ) const -> Result<ValidatedToolInvocation>
    {
        auto const* const p_definition = frameworkDefinition(toolName);
        if (p_definition == nullptr)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Framework Tool Catalog declares no tool named " + toolName
            );
        }
        UF_TRY(p_definition->validateArguments(canonicalArgs));
        UF_TRY_VALUE(descriptor, describe(toolName));
        return ValidatedToolInvocation{
            ToolProviderIdentity{FrameworkToolProvider{
                .toolCatalogHash = m_toolCatalogHash,
            }},
            std::move(toolName),
            std::move(canonicalArgs),
            std::move(descriptor),
        };
    }

    auto FrameworkToolCatalogOwner::describe(
        std::string_view toolName
    ) const -> Result<ToolDescriptor>
    {
        return withContext(
            describeTool(m_tools, toolName),
            "reading the Framework Tool Catalog"
        );
    }

    auto FrameworkToolCatalogOwner::offeredTools(
        ControllerProfile profile,
        std::span<std::string const> heldCapabilities
    ) const -> std::vector<OfferedTool>
    {
        return offerTools(m_tools, profile, heldCapabilities);
    }
}
