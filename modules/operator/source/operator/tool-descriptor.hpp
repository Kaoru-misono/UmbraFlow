#pragma once

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace uf::operator_runtime
{
    // How much one declared effect can cost. It is OP:`Risk` and it is never a
    // request field: a caller that could state its own risk could state
    // read_only for a tool the catalog marked mutating.
    enum class Risk : uint8
    {
        ReadOnly,
        Low,
        Medium,
        High,
        Critical,
    };

    [[nodiscard]] auto riskWireName(Risk risk) noexcept -> std::string_view;

    // Whether a tool changes anything outside the Operator. It is a property of
    // the Tool Catalog descriptor and never of a request, because the whole
    // point of the mutation chain is that it cannot be opted out of.
    enum class ToolMutability : uint8
    {
        ReadOnly,
        Mutating,
    };

    // Whether a tool's arguments and results are stated in the project's own
    // vocabulary, or in the machine's -- coordinates, pixels, key codes,
    // receipts, fencing tokens, bindings, frames. It is a property of the Tool
    // Catalog descriptor and never of a request.
    //
    // The catalog is project-owned, so this is a declaration the project makes
    // about itself, not isolation the Operator imposes. A project that marks a
    // coordinate tool Semantic is not contained by p03; it is attributable,
    // because the catalog bytes are inside plugin_hash, which is inside
    // project_registration_hash, which pins the session. What p03 enforces is
    // that the Operator never offers or accepts a Privileged tool for an online
    // Agent. It is the same limit the Operator accepts for a plugin that
    // under-declares its own effects, and it is deliberate: a second trust
    // model beside ToolMutability's would be worse than one documented limit.
    enum class ToolSurface : uint8
    {
        Semantic,
        Privileged,
    };

    // What redelivering one call of this tool would cost, declared strongest
    // first. The order is the whole of the type's meaning: a step may claim at
    // most the safety its tool declares, and `<=` on the enumerator is that
    // comparison.
    enum class ToolIdempotency : uint8
    {
        ReadSafe,
        DeliverySafe,
        KeyedExternal,
        NonIdempotent,
    };

    // OP:`UIActionIntent`.delivery_class, in the same strongest-first order.
    // There is no ReadSafe: a step that is delivered is not a read.
    enum class DeliveryClass : uint8
    {
        DeliverySafe,
        KeyedExternal,
        NonIdempotent,
    };

    // Whether a step claiming this delivery class stays within what the tool
    // declared about itself. The descriptor is the weaker-or-equal bound, so a
    // tool that admits it is not idempotent cannot have a step claim it is.
    [[nodiscard]]
    auto deliveryClassWithin(
        DeliveryClass claimed,
        ToolIdempotency declared
    ) noexcept -> bool;

    // What a timed-out step does next. Never a domain success: a postcondition
    // that did not arrive is a reason to look again, not evidence that the
    // effect landed.
    enum class TimeoutAction : uint8
    {
        Reobserve,
        Reconcile,
        Stop,
    };

    // OP:`TimeoutPolicy`.
    struct TimeoutPolicy final
    {
        uint64        maximumElapsedMillis{};
        TimeoutAction onTimeout{TimeoutAction::Stop};
    };

    // OP:`WorkflowLimits`. Every member is an upper bound, so clamping is a
    // minimum and a plan can only ever become more restricted.
    struct WorkflowLimits final
    {
        uint32 maximumSteps{};
        uint32 maximumDispatches{};
        uint32 maximumObservations{};
        uint32 maximumWaits{};
        uint64 maximumElapsedMillis{};
    };

    // One entry of a descriptor's effect_bounds: the whole of what a tool is
    // allowed to declare about one effect it may propose. A proposed
    // OP:`ExpectedEffect` is matched against it by namespaced_type and
    // scope_kind, and is refused unless its risk is at or below maximumRisk and
    // its payload_schema_hash is exactly this one -- which is what stops a plan
    // widening a tool's blast radius without moving tool_catalog_hash.
    //
    // No in-class initializer for the hash: ContentHash has no default state.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct EffectBound final
    {
        std::string namespacedType{};
        std::string scopeKind{};
        ContentHash payloadSchemaHash;
        Risk        maximumRisk{Risk::ReadOnly};
    };

    // One OP:`ExpectedEffect` in the terms the Operator acts on. The project
    // payload stays opaque: it is carried so that the minted plan is the exact
    // document the checked-in schema defines, and it is never interpreted.
    //
    // It sits beside the bound that judges it so the two cannot drift into two
    // files.
    //
    // No in-class initializer for the hash: ContentHash has no default state.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct ProposedEffect final
    {
        std::string namespacedType{};

        // Critical is the default for the same reason ToolMutability defaults
        // to Mutating: an effect whose risk failed to parse must be treated as
        // the most restricted of the five, never the least.
        Risk        risk{Risk::Critical};
        std::string scopeKind{};
        std::string scopeKey{};
        ContentHash payloadSchemaHash;
        std::string opaqueProjectPayload{};
    };

    // What one Tool Catalog descriptor says about a tool. Returned by the
    // catalog owner; there is no path by which a request proposes it.
    //
    // Every bound here is static and per tool. There is deliberately no second,
    // compiled-in ceiling beside them: a limit stated per tool and another
    // stated in C++ are two authorities over one number, and only one of them
    // is inside tool_catalog_hash.
    struct ToolDescriptor final
    {
        std::string toolVersion{};

        // The controller capabilities a session must hold before this tool is
        // offered to it. Empty means the tool asks for none, which is a
        // statement the catalog makes rather than an absence the Operator
        // fills in.
        std::vector<std::string> requiredCapabilities{};

        std::vector<EffectBound> effectBounds{};

        // The OP:`EffectivePlan`.allowed_ui_actions entries this tool may
        // propose. A plan naming anything outside it is refused at the freeze,
        // which is what keeps a step key from reaching mintStep at all.
        std::vector<std::string> uiActionBounds{};

        WorkflowLimits limits{};
        TimeoutPolicy  timeout{};

        // Mutating is the default so that a descriptor which failed to state a
        // mutability is treated as the more restricted of the two.
        ToolMutability mutability{ToolMutability::Mutating};

        // Privileged is the default for the same reason: a descriptor that
        // failed to state a surface gets the more restricted of the two, so a
        // catalog cannot widen the Agent ceiling by omission.
        ToolSurface surface{ToolSurface::Privileged};

        // NonIdempotent for the same reason again: the weakest claim a tool can
        // make about redelivery is what an unstated one is read as.
        ToolIdempotency idempotency{ToolIdempotency::NonIdempotent};
    };

    // Whether this descriptor declared a bound that admits this effect. Both
    // arguments are call-scoped borrows and nothing is retained.
    [[nodiscard]]
    auto effectWithinBounds(
        ToolDescriptor const& descriptor,
        ProposedEffect const& effect
    ) -> Status;
}
