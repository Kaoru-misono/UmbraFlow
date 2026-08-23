#pragma once

#include "observation-fixture.hpp"
#include "operator-protocol.hpp"

#include <deployment/project-directory.hpp>

#include <operator/agent-profile.hpp>
#include <operator/controller.hpp>
#include <operator/ledger.hpp>
#include <operator/manifest.hpp>
#include <operator/project-generation.hpp>
#include <operator/project-tool-dispatch.hpp>
#include <operator/snapshot-reference.hpp>
#include <operator/tool-invocation.hpp>
#include <operator/tool-root-producer.hpp>

#include <script/scoped-tool-program.hpp>

#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace uf::operator_runtime::conformance
{
    // The project directory this run was pointed at.
    //
    // doctest hands a TEST_CASE no parameters, so the path reaches the cases
    // through process scope: main writes it once before context.run() and
    // nothing writes it afterwards. Cases run single-threaded in one process,
    // which TemporaryDirectory below already relies on. Returned by copy, so no
    // case holds a view of storage it does not own. See
    // docs/archive/plans/2026-08-11-project-as-data.md 7.0 Q9.
    auto setProjectDirectory(std::filesystem::path directory) -> void;

    [[nodiscard]] auto projectDirectory() -> std::filesystem::path;

    // Which registration the suite is asking for. Authority is per registration,
    // so proving that it does not cross needs a second one that is complete
    // enough to mint documents of its own. Which deployment plays each role is
    // umbraflow-conformance.json's answer, not this suite's.
    enum class ProjectRole : uint8
    {
        UnderTest,
        Foreign,
    };

    // A directory the suite owns for the duration of one case. Cases run in one
    // process against real SQLite files, so each needs a private root and a
    // destructor that removes it even when an assertion ended the case.
    class TemporaryDirectory final
    {
        std::filesystem::path m_path{};

    public:
        explicit TemporaryDirectory(std::string_view label);

        TemporaryDirectory(TemporaryDirectory const&) = delete;
        TemporaryDirectory(TemporaryDirectory&&) = delete;
        auto operator=(TemporaryDirectory const&) -> TemporaryDirectory& = delete;
        auto operator=(TemporaryDirectory&&) -> TemporaryDirectory& = delete;
        ~TemporaryDirectory() noexcept;

        [[nodiscard]] auto path() const -> std::filesystem::path const&;
    };

    [[nodiscard]] auto hashOf(std::string_view value) -> ContentHash;

    // Everything below `loadedProject` fails the running case rather than
    // returning a Result, because a project that cannot mint its own documents
    // has no property left to test and the first refusal is the whole diagnosis.

    // The project directory, read. A case loads it rather than sharing one
    // load: the two recorders on it are written while that case runs, so a
    // shared load would let one case read what another observed.
    [[nodiscard]] auto loadedProject() -> deployment::ConformanceProject;

    // The deployment playing `role`, and the vocabulary that drives it. Both are
    // views into `project` and are call-scoped: every case holds the load on its
    // own stack, or in the PreparedStore below, for as long as it uses them.
    [[nodiscard]]
    auto deploymentFor(
        deployment::ConformanceProject const& project UF_LIFETIME_BOUND,
        ProjectRole role
    ) -> deployment::LoadedDeployment const&;

    [[nodiscard]]
    auto vocabularyFor(
        deployment::ConformanceProject const& project UF_LIFETIME_BOUND,
        ProjectRole role
    ) -> deployment::ProjectVocabulary const&;

    // The one UI action a run drives, in the shape task names it. Two types
    // rather than one because the loader may not depend on the Host's
    // vocabulary of what is under test; the three members are the same three.
    [[nodiscard]]
    auto uiActionOf(deployment::ProjectVocabulary const& vocabulary)
        -> task::UiActionUnderTest;

    [[nodiscard]] auto canonical(std::string value) -> CanonicalJson;

    [[nodiscard]]
    auto toolInvocation(
        deployment::ConformanceProject const& project,
        ProjectRole role,
        std::string toolName
    ) -> ValidatedToolInvocation;

    // The Tool Runtime seam a registration is compiled with when it is
    // registered only to be provisioned from.
    //
    // At the point prepareStore registers it, no session, controller, lease or
    // observation authority exists yet, so no scoped call could be admitted
    // through any seam it was handed. That is why it refuses, and it is a fact
    // about the setup rather than about the suite -- the runs a case builds
    // with toolRuntimeOver below dispatch real Tool calls through the
    // dispatcher's own seam.
    [[nodiscard]]
    auto provisioningToolRuntime() -> script::ToolRuntimeInvoke;

    // `invokeTool` is the seam the compiled tool closure reaches the Tool
    // Runtime through, and it is a parameter rather than a default because the
    // two callers want opposite things: provisioning wants the refusal above,
    // and a run wants its own dispatcher's seam.
    [[nodiscard]]
    auto loadGeneration(
        deployment::ConformanceProject const& project,
        ProjectRole role,
        script::ToolRuntimeInvoke invokeTool
    ) -> ProjectGenerationHandle;

    // The PolicyArtifact bytes a run pins, built from the effect types this
    // deployment's own descriptors bound. It is published because prepareStore
    // pins the manifest to their hash and the plan authority is built from the
    // same bytes.
    [[nodiscard]]
    auto policyArtifact(
        deployment::LoadedDeployment const& deployed,
        deployment::ProjectVocabulary const& vocabulary
    ) -> std::string;

    // The exact AgentProfile bytes a run pins, and the validator that reads the
    // ceilings back out of them rather than being handed a budget.
    //
    // An Agent is the one controller kind whose ControllerProfile requires
    // budgets, so a run that pinned no profile could open no Agent session and
    // the actor axis would be missing the kind that differs. The ceilings are
    // wide enough that no case here reaches one: a case that met a ceiling it
    // did not choose would be testing this file.
    [[nodiscard]]
    auto agentProfileBytes() -> std::string;

    [[nodiscard]]
    auto agentProfileValidator() -> AgentProfileValidator;

    [[nodiscard]]
    auto sessionManifest(
        ProjectIdentity const& registration,
        ContentHash const& runtimeArtifactRootHash,
        std::string_view exactPolicyArtifactBytes
    ) -> SessionManifest;

    // A coordinator holding one installed runtime artifact, one registered and
    // provisioned project, one pinned write session, one lease and one
    // snapshot: the state every ledger case starts from.
    struct PreparedStore final
    {
        OperatorCoordinator            store;
        ProjectGenerationHandle        generation;
        deployment::ConformanceProject project;
        SessionManifest                manifest;

        // The session's evaluated policy, bound to the registration root. It
        // is part of the prepared state because a deployment builds one from
        // the exact operator protocol bytes its session manifest pins, and the
        // suite must be unable to reach a PolicyArtifact any other way.
        OperatorPolicyAuthority policyAuthority;

        // The authenticated controller every entry point below is reached
        // through. It is part of the prepared state because there is no other
        // way in: bindController is the only mint, and a suite that could
        // assemble one would be asserting its own identity.
        ControllerBinding controller;
        ControlLease      lease;
        SnapshotRecord    snapshot;

        // The Host whose observations this store composes snapshots from. Only
        // TaskHost can mint one, so the suite carries a live Host rather than a
        // recorded value.
        ObservationHost       observation;

        // What a delivering Host is activated from. A dispatch needs a Host that
        // can act, and the observing one above cannot serve a second
        // TaskContext, so every delivery opens the same installed artifact
        // again rather than sharing that Host.
        ContentHash runtimeArtifactRootHash;
        uint64      installedGeneration{};
    };

    // A Host that can act under the store's current lease. It is a separate
    // Host per call on purpose; see DeliveringHost.
    [[nodiscard]]
    auto deliveringHost(PreparedStore& prepared)
        -> std::unique_ptr<DeliveringHost>;


    // One further observation cycle on the prepared Host: a new capture, a new
    // observation id, and -- over an unchanged world -- the same resolution.
    [[nodiscard]]
    auto observeAgain(PreparedStore& prepared) -> task::UiObservationSnapshot;

    // A snapshot over the world as it now stands. A token references a
    // composition rather than a lease, so an observation that read a moved
    // world makes every earlier token stale.
    [[nodiscard]]
    auto freshSnapshot(PreparedStore& prepared) -> SnapshotRecord;

    [[nodiscard]]
    auto prepareStore(std::filesystem::path const& root) -> PreparedStore;

    // The same runtime directory, opened again with nothing else beside it.
    //
    // This is the restart: opening clears every lease and deactivates every
    // session, and the value it returns reaches the ledger and no VM at all --
    // no dispatcher, no compiled closure, no Luau state anywhere in the
    // process. That is what makes a replay through it a statement that no
    // provider ran, rather than a statement that none was observed to.
    [[nodiscard]]
    auto reopenStore(std::filesystem::path const& root) -> OperatorCoordinator;

    // The Tool Runtime one case drives, standing on a prepared store.
    //
    // Every borrow inside it points into that PreparedStore, so the store must
    // outlive this value and must not be moved after it was built: the
    // dispatcher reaches the coordinator on every call, and the compiled
    // program holds the dispatcher's seam for as long as the program lives.
    //
    // No in-class initializer for any member below the authority: a dispatcher,
    // a compiled generation, a start catalog and an execution identity all have
    // to come from construction, and there is no default any of them could
    // carry.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct PreparedToolRuntime final
    {
        // Held behind a unique_ptr so the authority keeps one address while
        // this aggregate is moved out of its factory. The dispatcher borrows
        // it for its whole life, and a borrow of a member that moved with the
        // aggregate would be a borrow of storage nothing owns.
        std::unique_ptr<SnapshotObservationAuthority> observations{};

        ProjectToolDispatcher   dispatcher;
        ProjectGenerationHandle program;
        ToolStartCatalog        catalog;

        // The pinned execution identity every call of this run repeats. Only
        // the environment member is derived -- it is the compiled tool
        // closure's own -- because nothing yet derives the other three from
        // release bytes, and a suite that invented a derivation would be
        // publishing one.
        ToolExecutionIdentity execution;
    };

    [[nodiscard]]
    auto toolRuntimeOver(PreparedStore& prepared UF_LIFETIME_BOUND)
        -> PreparedToolRuntime;

    // One further authenticated actor on the prepared store's controlled
    // target: its own ProjectInstance, its own pinned session, and the
    // target's one lease.
    //
    // The lease is exclusive per controlled target, so whoever held it must
    // have released it first. Taking it in turn is what a handover between a
    // Project run, an Agent and a person on one target actually is, and it is
    // what keeps the target, the registration and the policy identical across
    // the three.
    //
    // No in-class initializer for either member: a binding and a lease must
    // both come from construction.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct ActorSession final
    {
        ControllerBinding controller;
        ControlLease      lease;
    };

    [[nodiscard]]
    auto openActorSession(
        PreparedStore& prepared,
        std::string_view sessionId,
        std::string_view instanceKey,
        ControllerKind kind,
        std::string_view controllerId
    ) -> ActorSession;

    [[nodiscard]]
    auto occurrences(
        std::string_view text,
        std::string_view needle
    ) -> std::size_t;
}
