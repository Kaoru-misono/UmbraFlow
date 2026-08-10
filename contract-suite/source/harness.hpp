#pragma once

#include <operator-contract/project-under-test.hpp>

#include <operator/ledger.hpp>
#include <operator/manifest.hpp>

#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace uf::operator_runtime::contract
{
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

    // Every helper below fails the running case rather than returning a
    // Result, because a project that cannot mint its own documents has no
    // property left to test and the first refusal is the whole diagnosis.
    [[nodiscard]]
    auto canonical(
        ProjectUnderTest const& project,
        std::string value
    ) -> CanonicalJson;

    [[nodiscard]]
    auto journalEntry(
        ProjectUnderTest const& project,
        JournalDocument const& document
    ) -> ValidatedJournalEntryData;

    [[nodiscard]]
    auto toolInvocation(
        ProjectUnderTest const& project,
        std::string toolName
    ) -> ValidatedToolInvocation;

    [[nodiscard]]
    auto loadPlugin(ProjectUnderTest const& project) -> ProjectPluginHandle;

    [[nodiscard]]
    auto reconcileOutcome(
        ProjectUnderTest const& project,
        ProjectPluginHandle const& plugin,
        std::string operationId,
        std::string input
    ) -> ValidatedReconcileOutcome;

    [[nodiscard]]
    auto sessionManifest(
        VerifiedProjectRegistration const& registration,
        ContentHash const& runtimeArtifactRootHash
    ) -> SessionManifest;

    // A RuntimeArtifact handoff on disk. Its shape is the Operator's, not the
    // project's, so the suite writes it rather than asking the deployment for
    // one.
    struct RuntimeRelease final
    {
        std::filesystem::path handoffRoot;
        ContentHash           releaseManifestHash;
        ContentHash           artifactRootHash;
    };

    [[nodiscard]]
    auto runtimeRelease(std::filesystem::path const& root) -> RuntimeRelease;

    // A coordinator holding one installed runtime artifact, one registered and
    // provisioned project, one pinned write session, one lease and one
    // snapshot: the state every ledger case starts from.
    struct PreparedStore final
    {
        OperatorCoordinator store;
        ProjectPluginHandle plugin;
        ProjectUnderTest    project;
        ControlLease        lease;
        SnapshotRecord      snapshot;
    };

    [[nodiscard]]
    auto prepareStore(std::filesystem::path const& root) -> PreparedStore;

    [[nodiscard]]
    auto command(
        SnapshotRecord const& snapshot,
        std::string clientRequestId
    ) -> CommandRequest;

    [[nodiscard]]
    auto readyOperation(
        PreparedStore& prepared,
        std::string clientRequestId,
        std::string toolName
    ) -> StoredOperation;

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
        JournalDocument const& entry
    ) -> ReconciliationCommit;

    [[nodiscard]]
    auto occurrences(
        std::string_view text,
        std::string_view needle
    ) -> std::size_t;
}
