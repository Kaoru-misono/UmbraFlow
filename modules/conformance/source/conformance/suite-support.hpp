#pragma once

#include "observation-fixture.hpp"
#include "operator-protocol.hpp"

#include <deployment/project-directory.hpp>

#include <operator/ledger.hpp>
#include <operator/manifest.hpp>

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

    [[nodiscard]]
    auto canonical(
        deployment::ConformanceProject const& project,
        ProjectRole role,
        std::string value
    ) -> CanonicalJson;

    [[nodiscard]]
    auto journalEntry(
        deployment::ConformanceProject const& project,
        ProjectRole role,
        deployment::ProjectJournalDocument const& document
    ) -> ValidatedJournalEntryData;

    [[nodiscard]]
    auto toolInvocation(
        deployment::ConformanceProject const& project,
        ProjectRole role,
        std::string toolName
    ) -> ValidatedToolInvocation;

    [[nodiscard]]
    auto loadPlugin(
        deployment::ConformanceProject const& project,
        ProjectRole role
    ) -> ProjectPluginHandle;

    [[nodiscard]]
    auto reconcileOutcome(
        deployment::ConformanceProject const& project,
        ProjectRole role,
        ProjectPluginHandle const& plugin,
        std::string operationId,
        std::string input
    ) -> ValidatedReconcileOutcome;

    [[nodiscard]]
    auto sessionManifest(
        VerifiedProjectRegistration const& registration,
        ContentHash const& runtimeArtifactRootHash
    ) -> SessionManifest;

    // A coordinator holding one installed runtime artifact, one registered and
    // provisioned project, one pinned write session, one lease and one
    // snapshot: the state every ledger case starts from.
    struct PreparedStore final
    {
        OperatorCoordinator            store;
        ProjectPluginHandle            plugin;
        deployment::ConformanceProject project;
        SessionManifest                manifest;

        // The sole mint for an EffectivePlan. It is part of the prepared state
        // because a deployment builds one from the exact operator protocol
        // bytes its session manifest pins, and the suite must be unable to
        // freeze a plan any other way.
        OperatorPlanAuthority planAuthority;

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

    // One reserved dispatch carried through a real Host and recorded. The report
    // cannot be fabricated -- HostDeliveryReport's only friend is TaskHost --
    // so this is the only way a case reaches a recorded delivery outcome.
    [[nodiscard]]
    auto deliverAndRecord(
        PreparedStore& prepared,
        DeliveringHost& host,
        DispatchReservation const& reservation
    ) -> Result<StoredOperation>;

    // One further observation cycle on the prepared Host: a new capture, a new
    // observation id, and -- over an unchanged world -- the same resolution.
    [[nodiscard]]
    auto observeAgain(PreparedStore& prepared) -> task::UiObservationSnapshot;

    // A snapshot over the world as it now stands. A token references a
    // composition rather than a lease, so a reconciliation that advanced
    // ProjectState makes every earlier token stale -- which is the property
    // contract-state-s02 proves, and the reason a case that opens a second
    // Operation after a commit has to re-observe first.
    [[nodiscard]]
    auto freshSnapshot(PreparedStore& prepared) -> SnapshotRecord;

    [[nodiscard]]
    auto prepareStore(std::filesystem::path const& root) -> PreparedStore;

    [[nodiscard]]
    auto command(
        SnapshotRecord const& snapshot,
        std::string clientRequestId
    ) -> CommandRequest;

    // One Operation the Operator itself froze a plan for and minted the first
    // step of, which is the whole state a dispatch may be reserved from.
    [[nodiscard]]
    auto readyOperation(
        PreparedStore& prepared,
        std::string clientRequestId,
        std::string toolName
    ) -> StoredOperation;

    [[nodiscard]]
    auto frozenPlan(
        PreparedStore& prepared,
        StoredOperation const& operation
    ) -> Result<FrozenPlan>;

    [[nodiscard]]
    auto plannedStep(
        PreparedStore& prepared,
        StoredOperation const& operation
    ) -> Result<PlannedStep>;

    // A Ready operation carried through one dispatch and one Host outcome, so
    // that it is reconciling and a commit can be tested against it.
    [[nodiscard]]
    auto reconcilingOperation(
        PreparedStore& prepared,
        std::string clientRequestId,
        std::string toolName
    ) -> StoredOperation;

    [[nodiscard]]
    auto confirmedCommit(
        PreparedStore const& prepared,
        StoredOperation const& operation,
        uint64 expectedProjectStateRevision,
        std::string eventId,
        deployment::ProjectJournalDocument const& entry
    ) -> ReconciliationCommit;

    [[nodiscard]]
    auto occurrences(
        std::string_view text,
        std::string_view needle
    ) -> std::size_t;
}
