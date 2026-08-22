#pragma once

#include "controller.hpp"
#include "effective-plan.hpp"
#include "snapshot-reference.hpp"
#include "tool-admission-request.hpp"
#include "tool-invocation.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>

#include <domain/content-hash.hpp>

#include <map>
#include <string>

namespace uf::operator_runtime
{
    // The Tools one actor may name at the top of a run, and the one place a
    // name is resolved to the catalog that owns it.
    //
    // Resolution is not a choice a producer makes. Per `a Tool name is owned by
    // its namespace` the Framework owns `framework` and a Project owns its
    // registered namespace, so the name itself decides which catalog validates
    // it and a producer that tried to prefer one would be claiming a name it
    // does not own. Both catalogs are held by value because both are owners
    // rather than views: a start outlives whatever assembled it.
    class ToolStartCatalog final
    {
        FrameworkToolCatalogOwner     m_framework;
        ProjectToolCatalogSchemaOwner m_project;

        ToolStartCatalog(
            FrameworkToolCatalogOwner framework,
            ProjectToolCatalogSchemaOwner project
        );

    public:
        // The Project catalog is the one the actor's session pinned. There is
        // no second constructor for a run with no Project: a session is pinned
        // to a ProjectRegistration, so a run without one has no actor to serve.
        [[nodiscard]]
        static auto create(ProjectToolCatalogSchemaOwner project)
            -> Result<ToolStartCatalog>;

        [[nodiscard]] auto projectRegistrationHash() const -> ContentHash;

        [[nodiscard]]
        auto validate(
            std::string toolName,
            CanonicalJson canonicalArgs
        ) const -> Result<ValidatedToolInvocation>;
    };

    // What one actor's start at the top of a run is made of.
    //
    // Every reference member is a call-scoped borrow of state the caller owns
    // for the whole call; nothing here is stored, and the aggregate is built
    // inside one translation, passed to one producer, and destroyed before that
    // producer returns. It must never gain a member of its own or be returned.
    //
    // What is deliberately absent is the caller idempotency namespace. It is
    // the authenticated controller's own and the producer derives it from the
    // binding, so no producer can hang a request key on another principal's
    // durable root -- which is the same reason the call ordinal is absent and
    // is assigned by the issuing seam alone.
    //
    // Nothing here says whether the named entry is "a script". Per `a Tool
    // handler and an automation script are one mechanism` what makes an entry
    // startable at the top of a run is that the actor is admitted to start it,
    // which is the authority and policy question admission already answers over
    // the descriptor the invocation carries. A member that answered it here
    // would be the discriminator that ruling refuses.
    //
    // No in-class initializer for the borrows or the preimage: a borrow has no
    // default object to name and CanonicalJson has no default state, so every
    // start must come from construction.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct ToolRootStart final
    {
        ControllerBinding const&       controller;
        ControlLease const&            lease;
        ToolExecutionIdentity const&   execution;
        OperatorPlanAuthority const&   planAuthority;
        ValidatedToolInvocation const& invocation;

        // The caller-supplied pre-admission material of the root request: the
        // stable key this actor re-presents to rejoin its own run, and the
        // preimage those exact bytes are hashed from.
        std::string   requestKey{};
        CanonicalJson requestPreimage;
    };

    // Operator admission of an actor's start at the top of a run: the producer
    // that turns one start into the same ToolAdmissionRequest every other
    // caller path builds, differing only in carrying a root coordinate.
    //
    // It owns the run's own issuing contexts -- one per root request -- because
    // a root-positioned call's ordinal belongs to the seam alone. A producer
    // that opened a fresh context per start would hand every start of one root
    // the ordinal 1 and alias the previous start's durable row, which is the
    // aliasing `ToolCallPositionIdentity::create` is private to prevent.
    //
    // An entry is released only when this producer is destroyed. Nothing
    // releases one earlier because a start is never told its run is finished:
    // a start names its root and there is no termination signal to seal a
    // context on, so the map is bounded by the number of distinct root requests
    // one actor's adapter serves.
    //
    // Production-unreachable: its one non-test holder is
    // service::ProductLifecycle, whose only door onto it --
    // invokeFrameworkTool -- has no production caller and may not gain one
    // before the generation cut. See
    // docs/decisions/2026-08-22-production-reachability-is-the-cut-invariant.md.
    class ToolRootProducer final
    {
        std::map<ContentHash, ToolCallIssuingContext> m_contexts{};

        [[nodiscard]]
        auto contextFor(
            ToolRootRequestIdentity const& root,
            ToolExecutionIdentity const& execution
        ) UF_LIFETIME_BOUND -> ToolCallIssuingContext&;

        [[nodiscard]]
        auto requestFor(
            ToolRootStart const& start,
            ToolRootRequestIdentity root,
            ToolCallPositionIdentity call
        ) const -> Result<ToolAdmissionRequest>;

    public:
        [[nodiscard]]
        auto start(ToolRootStart const& start) -> Result<ToolAdmissionRequest>;

        // A start whose Tool consumes one observation. The reference is a
        // call-scoped borrow whose identity is copied into the coordinate;
        // nothing is retained.
        [[nodiscard]]
        auto startAgainstObservation(
            ToolRootStart const& start,
            SnapshotObservationReference const& observation
        ) -> Result<ToolAdmissionRequest>;
    };
}
