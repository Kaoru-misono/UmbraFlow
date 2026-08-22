#pragma once

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <optional>
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

    // The inverse of riskWireName, for a reader that must turn a document's
    // enum member back into the domain it was rendered from. std::nullopt is
    // "no enumerator is spelled this way", which is the one verdict every
    // schema that constrains the member and every reader that does not share
    // that schema can agree on. The mapping is the forward function's and is
    // never restated: the enumerators are listed, the projection is the
    // spelling, so a name that drifts drifts in the one place it was written.
    [[nodiscard]]
    auto parseRisk(std::string_view wire) noexcept -> std::optional<Risk>;

    // Whether a tool changes anything outside the Operator. It is a property of
    // the Tool Catalog descriptor and never of a request, because the whole
    // point of the mutation chain is that it cannot be opted out of.
    enum class ToolMutability : uint8
    {
        ReadOnly,
        Mutating,
    };

    [[nodiscard]]
    auto toolMutabilityWireName(ToolMutability mutability) noexcept
        -> std::string_view;

    [[nodiscard]]
    auto parseToolMutability(std::string_view wire) noexcept
        -> std::optional<ToolMutability>;

    // Whether a tool's arguments and results are stated in the project's own
    // vocabulary, or in the machine's -- coordinates, pixels, key codes,
    // receipts, fencing tokens, bindings, frames. It is a property of the Tool
    // Catalog descriptor and never of a request.
    //
    // The catalog is project-owned, so this is a declaration the project makes
    // about itself, not isolation the Operator imposes. A project that marks a
    // coordinate tool Semantic is not contained by p03; it is attributable,
    // because the catalog bytes are inside the ProjectRegistration root, which is inside
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

    [[nodiscard]]
    auto toolSurfaceWireName(ToolSurface surface) noexcept -> std::string_view;

    [[nodiscard]]
    auto parseToolSurface(std::string_view wire) noexcept
        -> std::optional<ToolSurface>;

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

    [[nodiscard]]
    auto toolIdempotencyWireName(ToolIdempotency idempotency) noexcept
        -> std::string_view;

    [[nodiscard]]
    auto parseToolIdempotency(std::string_view wire) noexcept
        -> std::optional<ToolIdempotency>;

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

    [[nodiscard]]
    auto timeoutActionWireName(TimeoutAction action) noexcept -> std::string_view;

    [[nodiscard]]
    auto parseTimeoutAction(std::string_view wire) noexcept
        -> std::optional<TimeoutAction>;

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
    // OP:`EffectEnvelope` is matched against it by namespaced_type and
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

    // One OP:`EffectEnvelope` in the terms the Operator acts on. The project
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

    // What one Tool's handler may issue as child calls, and how far. It is the
    // parent half of section 3.3's intersection: a child call is admitted only
    // from what this declares, intersected with the admitted root effect
    // envelope, current policy and approvals, target and session authority,
    // lease and fence, and remaining budgets. A descriptor can request an
    // envelope; it can never grant or widen one.
    //
    // An empty declaration is the whole statement "this tool issues no child
    // calls", which is what a descriptor that says nothing about children
    // declares. There is deliberately no second spelling of that: no optional
    // wrapper, and no flag beside the list.
    struct ChildEffectDeclaration final
    {
        // The exact child Tool names this handler may issue, by the name the
        // child's own catalog declares it under. A name absent from this list
        // is refused even when every other bound would admit it, which is what
        // makes delegated authority enumerated rather than inferred.
        std::vector<std::string> childToolNames{};

        // The strongest child surface, mutability and effect risk this handler
        // may delegate. Every default is the most restricted of its kind, so a
        // declaration that lists a name without stating a ceiling delegates the
        // least rather than the most.
        ToolSurface    maximumChildSurface{ToolSurface::Semantic};
        ToolMutability maximumChildMutability{ToolMutability::ReadOnly};
        Risk           maximumChildRisk{Risk::ReadOnly};

        // How many child calls one handler invocation may issue. Zero is the
        // only value a declaration naming no child may carry, and a
        // declaration naming a child must carry at least one.
        uint32 maximumChildCalls{};
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

        // What this tool's handler may call while it runs. See
        // ChildEffectDeclaration: empty is "no child call at all".
        ChildEffectDeclaration childEffects{};

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

    // Whether one declaration is well formed at all. A declaration that names
    // a child while admitting no child call, or admits child calls while
    // naming none, states two halves of one permission that contradict each
    // other, and a catalog owner refuses it rather than picking a half.
    [[nodiscard]]
    auto childEffectDeclarationValid(ChildEffectDeclaration const& declaration)
        -> Status;

    // Whether the parent declaration admits this child tool at all. Risk is
    // judged per proposed effect and is deliberately not folded in here: a
    // read-only child proposes none, and folding the two would make the
    // absence of an effect look like a risk verdict.
    [[nodiscard]]
    auto childToolWithinDeclaration(
        ChildEffectDeclaration const& declaration,
        std::string_view parentToolName,
        std::string_view childToolName,
        ToolDescriptor const& childDescriptor
    ) -> Status;

    [[nodiscard]]
    auto childEffectWithinDeclaration(
        ChildEffectDeclaration const& declaration,
        std::string_view parentToolName,
        ProposedEffect const& effect
    ) -> Status;

    // Whether this descriptor declared a bound that admits this effect. Both
    // arguments are call-scoped borrows and nothing is retained.
    [[nodiscard]]
    auto effectWithinBounds(
        ToolDescriptor const& descriptor,
        ProposedEffect const& effect
    ) -> Status;
}
