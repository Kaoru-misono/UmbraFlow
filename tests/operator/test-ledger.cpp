// What the Operator's own ledger owns: the production database, RuntimeArtifact
// installation and reclamation, and the exact reduce envelope its journal
// builds. The properties a project's registration decides -- catalog
// mutability, schema-owner binding, who owns a disposition -- are the exported
// contract suite's, because a consuming repository proves them against its own
// project; see contract-suite/source/. No property is asserted in both places.

#include <operator/ledger.hpp>
#include <operator/manifest.hpp>

#include "project-fixture.hpp"

#include <domain/content-hash.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace uf::operator_runtime
{
    namespace
    {
        // The one plugin every case here registers. It comes from the shared
        // fixture because plan and next_step now answer with real operator
        // protocol documents, and a second spelling of them would be a second
        // plugin_hash for one project.
        inline auto const k_pluginSource = test_support::pluginSource("fixture.alpha");

        // The same plugin except that reduce answers with a document the
        // pinned ProjectState schema refuses -- but only once a prior state
        // exists, so provisioning still succeeds and the failure lands inside
        // the reconciliation transaction, which is where the no-write-on-failure
        // test needs it.
        [[nodiscard]]
        inline auto rejectedReducePluginSource() -> std::string
        {
            auto source        = test_support::pluginSource("fixture.alpha");
            auto const accepted = std::string{"return '{\"revision\":1}'"};
            auto const at      = source.find(accepted);
            REQUIRE(at != std::string::npos);
            return source.replace(at, accepted.size(), "return '{\"value\":99}'");
        }

        class TemporaryDirectory final
        {
            std::filesystem::path m_path{};

        public:
            TemporaryDirectory()
            {
                static auto s_sequence = std::atomic<uint64>{1};
                m_path = std::filesystem::temp_directory_path()
                    / std::format(
                        "umbraflow-operator-ledger-{}-{}",
                        std::chrono::steady_clock::now().time_since_epoch().count(),
                        s_sequence.fetch_add(1, std::memory_order_relaxed)
                    );
                auto error = std::error_code{};
                auto const created = std::filesystem::create_directory(m_path, error);
                REQUIRE(created);
                REQUIRE_FALSE(error);
            }

            TemporaryDirectory(TemporaryDirectory const&) = delete;
            TemporaryDirectory(TemporaryDirectory&&) = delete;
            auto operator=(TemporaryDirectory const&) -> TemporaryDirectory& = delete;
            auto operator=(TemporaryDirectory&&) -> TemporaryDirectory& = delete;

            ~TemporaryDirectory() noexcept
            {
                auto error = std::error_code{};
                static_cast<void>(std::filesystem::remove_all(m_path, error));
            }

            [[nodiscard]] auto path() const -> std::filesystem::path const&
            {
                return m_path;
            }
        };

        using test_support::canonical;
        using test_support::hashOf;
        using test_support::journalEntry;
        using test_support::k_fixtureProvenance;
        using test_support::k_fixtureProvenanceViolations;
        using test_support::loadPlugin;
        using test_support::makeProject;
        using test_support::sessionManifest;
        using test_support::toolInvocation;

        // Re-adding any of these members would reopen the two P0 holes: a
        // reducer input beside the events lets the Journal say A while the
        // materialized state was reduced from B, and a request-owned tool or
        // mutability makes the mutation chain opt-out. The checks go through
        // concepts because a member lookup on a concrete type is an error
        // rather than a substitution failure.
        template <typename T>
        concept NamesReducerInput = requires(T value) { value.reducerInput; };

        template <typename T>
        concept NamesMutability = requires(T value) { value.mutating; };

        template <typename T>
        concept NamesTool = requires(T value) { value.toolName; };

        template <typename T>
        concept NamesCanonicalArgs = requires(T value) { value.canonicalArgs; };

        static_assert(!NamesReducerInput<ReconciliationCommit>);
        static_assert(!NamesReducerInput<ProjectInstanceBaseline>);
        static_assert(!NamesMutability<CommandRequest>);
        static_assert(!NamesTool<CommandRequest>);
        static_assert(!NamesCanonicalArgs<CommandRequest>);

        // The same guard for every authority-bearing value, not just the one
        // that happened to get it: an aggregate could be brace-initialized past
        // its owner, and a public constructor would make the owner optional.
        static_assert(!std::is_aggregate_v<ValidatedJournalEntryData>);
        static_assert(!std::is_aggregate_v<ValidatedToolInvocation>);
        static_assert(!std::is_aggregate_v<ValidatedReconcileOutcome>);
        static_assert(!std::is_aggregate_v<ValidatedDocument>);
        static_assert(!std::is_aggregate_v<CanonicalJson>);
        static_assert(
            !std::is_constructible_v<
                ValidatedToolInvocation,
                ContentHash,
                ContentHash,
                std::string,
                std::string,
                CanonicalJson,
                ToolMutability
            >
        );
        static_assert(
            !std::is_constructible_v<
                ValidatedReconcileOutcome,
                ContentHash,
                ContentHash,
                ValidatedDocument,
                ReconcileDisposition
            >
        );
        static_assert(
            !std::is_constructible_v<
                ValidatedJournalEntryData,
                ContentHash,
                std::string,
                ContentHash,
                CanonicalJson,
                CanonicalJson
            >
        );

        struct PreparedStore final
        {
            OperatorCoordinator          store;
            ProjectPluginHandle          plugin;
            test_support::ProjectFixture project;
            OperatorPlanAuthority        planAuthority;

            // The authenticated controller every entry point is reached
            // through. bindController is its only mint.
            ControllerBinding         controller;
            ControlLease              lease;
            SnapshotRecord            snapshot;
            contract::ObservationHost observation;

            // What a delivering Host is activated from. The observing Host above
            // cannot serve a second TaskContext, so a dispatch opens the same
            // installed artifact again rather than sharing it.
            ContentHash runtimeArtifactRootHash;
            uint64      installedGeneration{};
        };

        [[nodiscard]]
        auto prepareStore(
            std::filesystem::path const& path,
            std::string_view pluginSource = k_pluginSource
        ) -> PreparedStore
        {
            auto const release = test_support::runtimeRelease(path / "session-handoff");
            auto storeResult = OperatorCoordinator::open(path / "production");
            REQUIRE(storeResult.has_value());
            auto store = *std::move(storeResult);
            auto installed = store.installRuntimeArtifact(
                RuntimeArtifactInstallRequest{
                    .handoffRoot                 = release.handoffRoot,
                    .expectedReleaseManifestHash = release.releaseManifestHash,
                    .expectedInstalledGeneration = 0U,
                }
            );
            REQUIRE(installed.has_value());
            auto const artifactRootHash    = installed->rootHash();
            auto const installedGeneration = installed->installedGeneration();
            auto const project = makeProject("fixture.alpha", pluginSource);
            auto const manifest = sessionManifest(
                project.registration,
                installed->rootHash(),
                hashOf("agent")
            );
            auto const projectPlugin = loadPlugin(project, pluginSource);
            REQUIRE(store.registerProject(project.registration).has_value());
            REQUIRE(store.provisionProjectInstance(
                project.registration,
                projectPlugin,
                ProjectInstanceBaseline{
                    .projectInstanceKey  = "instance-1",
                    .eventId             = "baseline-1",
                    .sessionManifestHash = manifest.hash(),
                    .entry = journalEntry(
                        project,
                        project.registration.baselineEventType(),
                        "{\"kind\":\"baseline\"}"
                    ),
                }
            ).has_value());
            REQUIRE(store.pinSession(
                SessionPin{
                    .sessionId                 = "session-1",
                    .authenticatedControllerId = "controller-1",
                    .idempotencyNamespace      = "controller-1",
                    .projectRegistrationHash   = project.registration.hash(),
                    .capabilityProfileHash     = hashOf("capability"),
                    .controlledTargetId        = "target-1",
                    .projectInstanceKey        = "instance-1",
                    .mode                      = SessionMode::Write,
                    .kind                      = ControllerKind::Script,
                },
                manifest,
                std::nullopt
            ).has_value());
            auto controller = store.bindController("session-1");
            REQUIRE(controller.has_value());
            auto lease = store.acquireLease(*controller);
            REQUIRE(lease.has_value());
            auto observation = contract::activateObservationHost(
                *std::move(installed),
                contract::resolvedFramePixels(),
                FrameId{201}
            );
            auto snapshot = store.createSnapshot(
                *lease,
                projectPlugin,
                contract::observeOnce(observation)
            );
            REQUIRE(snapshot.has_value());
            auto planAuthority = contract::planAuthority(
                project.registration,
                manifest,
                "operator"
            );
            REQUIRE(planAuthority.has_value());
            return PreparedStore{
                .store                   = std::move(store),
                .plugin                  = projectPlugin,
                .project                 = project,
                .planAuthority           = *std::move(planAuthority),
                .controller              = *controller,
                .lease                   = *lease,
                .snapshot                = *std::move(snapshot),
                .observation             = std::move(observation),
                .runtimeArtifactRootHash = artifactRootHash,
                .installedGeneration     = installedGeneration,
            };
        }

        // A Host that can act under this store's current lease.
        [[nodiscard]]
        auto deliveringHost(PreparedStore& prepared)
            -> std::unique_ptr<contract::DeliveringHost>
        {
            return contract::deliveringHostFor(
                prepared.store,
                prepared.lease,
                prepared.installedGeneration,
                prepared.runtimeArtifactRootHash
            );
        }

        [[nodiscard]]
        auto reconciliationOutcome(
            PreparedStore const& prepared,
            std::string operationId,
            std::string document
        ) -> ValidatedReconcileOutcome
        {
            return test_support::reconcileOutcome(
                prepared.project,
                prepared.plugin,
                std::move(operationId),
                std::move(document)
            );
        }

        [[nodiscard]]
        auto command(
            SnapshotRecord const& snapshot,
            std::string clientRequestId
        ) -> CommandRequest
        {
            return CommandRequest{
                .snapshotToken        = snapshot.token,
                .idempotencyNamespace = "controller-1",
                .clientRequestId      = std::move(clientRequestId),
            };
        }

        [[nodiscard]]
        auto proposedOperation(
            PreparedStore& prepared,
            std::string clientRequestId,
            std::string_view toolName
        ) -> StoredOperation
        {
            auto operation = prepared.store.submitCommand(
                prepared.controller,
                command(prepared.snapshot, std::move(clientRequestId)),
                toolInvocation(prepared.project, std::string{toolName})
            );
            REQUIRE(operation.has_value());
            return operation->operation;
        }

        [[nodiscard]]
        auto freezePlanFor(
            PreparedStore& prepared,
            StoredOperation const& operation
        ) -> Result<FrozenPlan>
        {
            return prepared.store.freezePlan(
                operation.operationId,
                operation.revision,
                prepared.lease,
                prepared.plugin,
                prepared.planAuthority
            );
        }

        [[nodiscard]]
        auto mintStepFor(
            PreparedStore& prepared,
            StoredOperation const& operation
        ) -> Result<PlannedStep>
        {
            return prepared.store.mintNextStep(
                operation.operationId,
                operation.revision,
                prepared.lease,
                prepared.plugin,
                prepared.planAuthority
            );
        }

        // Proposed, plan frozen by the Operator, first step minted from the
        // plugin's own next_step: everything a dispatch may be reserved from.
        [[nodiscard]]
        auto createReadyOperation(
            PreparedStore& prepared,
            std::string clientRequestId,
            std::string_view toolName
        ) -> StoredOperation
        {
            auto const proposed = proposedOperation(
                prepared,
                std::move(clientRequestId),
                toolName
            );
            auto const frozen = freezePlanFor(prepared, proposed);
            REQUIRE(frozen.has_value());
            auto const step = mintStepFor(prepared, frozen->operation);
            REQUIRE(step.has_value());
            return step->operation;
        }

        // test_support::runtimeRelease always writes the same page model, so
        // every release it builds has the same content hash and shares one
        // production directory. Reclamation needs two that do not.
        // Builds a handoff whose release manifest names the two schema hashes
        // given, so a case can move exactly one of them off the value this
        // deployment principal accepts.
        [[nodiscard]]
        auto releaseWithSchemaHashes(
            std::filesystem::path const& root,
            std::string_view annotationWorkspaceSchemaHash,
            std::string_view workspaceSqliteSchemaHash
        ) -> test_support::RuntimeRelease
        {
            auto const handoff  = root / "release";
            auto const artifact = handoff / "runtime-artifact";
            auto const model    = std::string_view{"a page model\r\n"};
            test_support::writeFile(artifact / task::k_runtimeModelFileName, model);
            auto const manifest = std::format(
                "{{\"assets\":[],\"manifest_schema_hash\":\"{}\","
                "\"page_model\":{{\"path\":\"page-model.toml\",\"sha256\":\"{}\","
                "\"size\":{}}},\"runtime_model_schema_hash\":\"{}\"}}",
                task::k_runtimeArtifactSchemaHash,
                hashOf(model).hex(),
                model.size(),
                task::k_runtimeModelSchemaHash
            );
            test_support::writeFile(
                artifact / task::k_runtimeArtifactManifestFileName,
                manifest
            );
            auto const artifactRootHash = hashOf(manifest);
            auto const releaseManifest = std::format(
                "{{\"annotation_workspace_schema_hash\":\"{}\","
                "\"candidate_id\":\"candidate-1\",\"candidate_revision\":1,"
                "\"generation\":1,\"predecessor_publication_id\":null,"
                "\"replay_gate_hash\":\"{}\",\"runtime_artifact_root_hash\":\"{}\","
                "\"workspace_sqlite_schema_hash\":\"{}\"}}",
                annotationWorkspaceSchemaHash,
                hashOf("replay-gate").hex(),
                artifactRootHash.hex(),
                workspaceSqliteSchemaHash
            );
            test_support::writeFile(handoff / "release.manifest.json", releaseManifest);
            return test_support::RuntimeRelease{
                .handoffRoot         = handoff,
                .releaseManifestHash = hashOf(releaseManifest),
                .artifactRootHash    = artifactRootHash,
            };
        }

        [[nodiscard]]
        auto releaseWithModel(
            std::filesystem::path const& root,
            std::string_view model
        ) -> test_support::RuntimeRelease
        {
            auto const handoff  = root / "release";
            auto const artifact = handoff / "runtime-artifact";
            test_support::writeFile(artifact / task::k_runtimeModelFileName, model);
            auto const manifest = std::format(
                "{{\"assets\":[],\"manifest_schema_hash\":\"{}\","
                "\"page_model\":{{\"path\":\"page-model.toml\",\"sha256\":\"{}\","
                "\"size\":{}}},\"runtime_model_schema_hash\":\"{}\"}}",
                task::k_runtimeArtifactSchemaHash,
                hashOf(model).hex(),
                model.size(),
                task::k_runtimeModelSchemaHash
            );
            test_support::writeFile(
                artifact / task::k_runtimeArtifactManifestFileName,
                manifest
            );
            auto const artifactRootHash = hashOf(manifest);
            auto const releaseManifest = std::format(
                "{{\"annotation_workspace_schema_hash\":\"{}\","
                "\"candidate_id\":\"candidate-1\",\"candidate_revision\":1,"
                "\"generation\":1,\"predecessor_publication_id\":null,"
                "\"replay_gate_hash\":\"{}\",\"runtime_artifact_root_hash\":\"{}\","
                "\"workspace_sqlite_schema_hash\":\"{}\"}}",
                detail::k_annotationWorkspaceSchemaHash,
                hashOf("replay-gate").hex(),
                artifactRootHash.hex(),
                detail::k_workspaceSqliteSchemaHash
            );
            test_support::writeFile(handoff / "release.manifest.json", releaseManifest);
            return test_support::RuntimeRelease{
                .handoffRoot         = handoff,
                .releaseManifestHash = hashOf(releaseManifest),
                .artifactRootHash    = artifactRootHash,
            };
        }

        [[nodiscard]]
        auto installRequest(
            test_support::RuntimeRelease const& release,
            uint64 expectedInstalledGeneration
        ) -> RuntimeArtifactInstallRequest
        {
            return RuntimeArtifactInstallRequest{
                .handoffRoot                 = release.handoffRoot,
                .expectedReleaseManifestHash = release.releaseManifestHash,
                .expectedInstalledGeneration = expectedInstalledGeneration,
            };
        }

        // A directory link that needs no privilege on Windows and that the
        // portable inspection functions report as a plain directory, which is
        // what makes it the shape worth planting.
        [[nodiscard]]
        auto linkDirectory(
            std::filesystem::path const& link,
            std::filesystem::path const& target
        ) -> bool
        {
#if defined(_WIN32)
            auto const command = std::format(
                "cmd /c mklink /J \"{}\" \"{}\" >nul 2>&1",
                link.string(),
                target.string()
            );
            return std::system(command.c_str()) == 0;
#else
            auto error = std::error_code{};
            std::filesystem::create_directory_symlink(target, link, error);
            return !error;
#endif
        }
    }

    TEST_CASE("OperatorCoordinator creates only the production database name")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        CHECK(prepared.store.databasePath().filename() == "operator-runtime.sqlite");
        CHECK(std::filesystem::is_regular_file(prepared.store.databasePath()));
    }

    TEST_CASE("Journal schema owner prevents caller-attached payload and provenance labels")
    {
        auto const project    = makeProject("fixture.alpha", k_pluginSource);
        auto const provenance = std::string{k_fixtureProvenance};
        auto const accepted = project.journalSchemaOwner.validate(
            "fixture.progress",
            canonical(project.schemaOwner, "{\"value\":1}"),
            canonical(project.schemaOwner, provenance)
        );
        REQUIRE(accepted.has_value());
        CHECK(accepted->projectRegistrationHash() == project.registration.hash());
        CHECK(accepted->payloadSchemaHash() == hashOf("progress-schema"));

        CHECK_FALSE(project.journalSchemaOwner.validate(
            "fixture.progress",
            canonical(project.schemaOwner, "{\"value\":2}"),
            canonical(project.schemaOwner, provenance)
        ).has_value());
        CHECK_FALSE(project.journalSchemaOwner.validate(
            "fixture.unknown",
            canonical(project.schemaOwner, "{\"value\":99}"),
            canonical(project.schemaOwner, provenance)
        ).has_value());
        CHECK_FALSE(project.journalSchemaOwner.validate(
            "fixture.progress",
            canonical(project.schemaOwner, "{\"value\":1}"),
            canonical(
                project.schemaOwner,
                std::string{k_fixtureProvenanceViolations.front()}
            )
        ).has_value());
    }

    TEST_CASE("installation refuses a release manifest naming another schema")
    {
        // Both hashes are the deployment principal's half of a cross-boundary
        // agreement: the authoring side publishes them and this side decides
        // whether it can read what they describe. A pin only checked on the
        // publishing side compares that side against itself.
        auto temporary = TemporaryDirectory{};
        auto const production = temporary.path() / "production";
        auto coordinator = OperatorCoordinator::open(production);
        REQUIRE(coordinator.has_value());

        auto const foreign = hashOf("some other schema").hex();

        SUBCASE("the workspace SQLite schema hash must be the one this build reads")
        {
            auto const release = releaseWithSchemaHashes(
                temporary.path() / "wrong-sqlite",
                detail::k_annotationWorkspaceSchemaHash,
                foreign
            );
            CHECK_FALSE(
                coordinator->installRuntimeArtifact(installRequest(release, 0U))
                    .has_value()
            );
        }

        SUBCASE("the annotation workspace schema hash must be too")
        {
            auto const release = releaseWithSchemaHashes(
                temporary.path() / "wrong-annotation",
                foreign,
                detail::k_workspaceSqliteSchemaHash
            );
            CHECK_FALSE(
                coordinator->installRuntimeArtifact(installRequest(release, 0U))
                    .has_value()
            );
        }

        SUBCASE("both at the pinned values install")
        {
            auto const release = releaseWithSchemaHashes(
                temporary.path() / "correct",
                detail::k_annotationWorkspaceSchemaHash,
                detail::k_workspaceSqliteSchemaHash
            );
            CHECK(
                coordinator->installRuntimeArtifact(installRequest(release, 0U))
                    .has_value()
            );
        }
    }

    TEST_CASE("a second coordinator is refused while the first holds the directory")
    {
        auto temporary = TemporaryDirectory{};
        auto const production = temporary.path() / "production";

        // Opening clears every control lease, deactivates every session and
        // drops every publication claim, on the reading that whatever those
        // rows describe died with its process. A second open against a live
        // coordinator would perform those three clears against state that is
        // still in use, so it has to be refused rather than serialized.
        auto first = OperatorCoordinator::open(production);
        REQUIRE(first.has_value());

        auto const second = OperatorCoordinator::open(production);
        CHECK_FALSE(second.has_value());

        // The refusal is ownership, not a permanent property of the directory:
        // closing the first coordinator releases it.
        first = fail(AutomationErrorKind::Cancelled, "closed");
        auto const reopened = OperatorCoordinator::open(production);
        CHECK(reopened.has_value());
    }

    TEST_CASE("ProjectInstance provisioning rejects Journal data from another registration")
    {
        auto temporary = TemporaryDirectory{};
        auto const release = test_support::runtimeRelease(temporary.path());
        auto coordinator = OperatorCoordinator::open(temporary.path() / "production");
        REQUIRE(coordinator.has_value());
        auto const installed = coordinator->installRuntimeArtifact(
            RuntimeArtifactInstallRequest{
                .handoffRoot                 = release.handoffRoot,
                .expectedReleaseManifestHash = release.releaseManifestHash,
                .expectedInstalledGeneration = 0U,
            }
        );
        REQUIRE(installed.has_value());

        auto const project = makeProject("fixture.alpha", k_pluginSource);
        auto const foreign = makeProject("fixture.foreign", "foreign-plugin-bytes");
        auto const plugin = loadPlugin(project, k_pluginSource);
        auto const manifest = sessionManifest(
            project.registration,
            installed->rootHash(),
            hashOf("agent")
        );
        REQUIRE(coordinator->registerProject(project.registration).has_value());
        CHECK_FALSE(coordinator->provisionProjectInstance(
            project.registration,
            plugin,
            ProjectInstanceBaseline{
                .projectInstanceKey  = "instance-1",
                .eventId             = "baseline-1",
                .sessionManifestHash = manifest.hash(),
                .entry = journalEntry(
                    foreign,
                    foreign.registration.baselineEventType(),
                    "{\"kind\":\"baseline\"}"
                ),
            }
        ).has_value());
    }

    TEST_CASE("production RuntimeArtifact installation owns activation CAS")
    {
        auto temporary = TemporaryDirectory{};
        auto const release = test_support::runtimeRelease(temporary.path());
        auto coordinator = OperatorCoordinator::open(temporary.path() / "production");
        REQUIRE(coordinator.has_value());

        auto installed = coordinator->installRuntimeArtifact(
            RuntimeArtifactInstallRequest{
                .handoffRoot                 = release.handoffRoot,
                .expectedReleaseManifestHash = release.releaseManifestHash,
                .expectedInstalledGeneration = 0U,
            }
        );
        REQUIRE(installed.has_value());
        CHECK(installed->installedGeneration() == 1U);
        CHECK(installed->rootHash() == release.artifactRootHash);

        CHECK_FALSE(coordinator->installRuntimeArtifact(
            RuntimeArtifactInstallRequest{
                .handoffRoot                 = release.handoffRoot,
                .expectedReleaseManifestHash = release.releaseManifestHash,
                .expectedInstalledGeneration = 0U,
            }
        ).has_value());

        test_support::writeFile(
            release.handoffRoot / "runtime-artifact" / task::k_runtimeModelFileName,
            "authoring handoff changed"
        );
        auto reopened = coordinator->openInstalledRuntimeArtifact(
            1U,
            release.artifactRootHash
        );
        REQUIRE(reopened.has_value());
        CHECK(reopened->rootHash() == release.artifactRootHash);
    }

    TEST_CASE("reclamation removes a RuntimeArtifact directory nothing references")
    {
        auto temporary = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const artifactRoot = production / "runtime-artifacts";
        auto const installed = test_support::runtimeRelease(temporary.path() / "first");
        auto const orphan = releaseWithModel(
            temporary.path() / "second",
            "a different page model\r\n"
        );
        REQUIRE(installed.artifactRootHash != orphan.artifactRootHash);

        auto coordinator = OperatorCoordinator::open(production);
        REQUIRE(coordinator.has_value());
        REQUIRE(coordinator->installRuntimeArtifact(installRequest(installed, 0U)).has_value());

        // A-F8: the directory is published before the transaction, so losing
        // the generation CAS leaves it behind with nothing pointing at it.
        CHECK_FALSE(coordinator->installRuntimeArtifact(installRequest(orphan, 0U)).has_value());
        auto const orphanPath = artifactRoot / orphan.artifactRootHash.hex();
        REQUIRE(std::filesystem::is_directory(orphanPath));

        auto const reclaimed = coordinator->reclaimUnreferencedRuntimeArtifacts();
        REQUIRE(reclaimed.has_value());
        CHECK(reclaimed->artifactDirectories == 1U);
        CHECK_FALSE(std::filesystem::exists(orphanPath));
        CHECK(std::filesystem::is_directory(
            artifactRoot / installed.artifactRootHash.hex()
        ));
        CHECK(coordinator->openInstalledRuntimeArtifact(
            1U,
            installed.artifactRootHash
        ).has_value());

        // The reference set is the database's, so a second pass has nothing
        // left to decide about.
        auto const again = coordinator->reclaimUnreferencedRuntimeArtifacts();
        REQUIRE(again.has_value());
        CHECK(again->artifactDirectories == 0U);
    }

    TEST_CASE("reclamation keeps a RuntimeArtifact another publisher installed")
    {
        auto temporary = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const artifactRoot = production / "runtime-artifacts";
        auto const first = test_support::runtimeRelease(temporary.path() / "first");
        auto const second = releaseWithModel(
            temporary.path() / "second",
            "a different page model\r\n"
        );

        auto coordinator = OperatorCoordinator::open(production);
        REQUIRE(coordinator.has_value());
        REQUIRE(coordinator->installRuntimeArtifact(installRequest(first, 0U)).has_value());
        REQUIRE(coordinator->installRuntimeArtifact(installRequest(second, 1U)).has_value());

        // The A-F8 case: these bytes are already in place because another
        // publisher's installation won, so our own failed attempt is not
        // permission to remove their directory. The generation it belongs to is
        // no longer the active one, which is what keeps this case from being
        // decided by the active-root clause instead.
        CHECK_FALSE(coordinator->installRuntimeArtifact(installRequest(first, 0U)).has_value());

        auto const reclaimed = coordinator->reclaimUnreferencedRuntimeArtifacts();
        REQUIRE(reclaimed.has_value());
        CHECK(reclaimed->artifactDirectories == 0U);
        CHECK(std::filesystem::is_directory(artifactRoot / first.artifactRootHash.hex()));
        CHECK(std::filesystem::is_directory(artifactRoot / second.artifactRootHash.hex()));
        CHECK(coordinator->openInstalledRuntimeArtifact(1U, first.artifactRootHash).has_value());
        CHECK(coordinator->openInstalledRuntimeArtifact(2U, second.artifactRootHash).has_value());
    }

    TEST_CASE("reclamation removes staging directories no publication claims")
    {
        auto temporary = TemporaryDirectory{};
        auto const production = temporary.path() / "production";
        auto coordinator = OperatorCoordinator::open(production);
        REQUIRE(coordinator.has_value());

        // What a publisher that died between create_directory and rename leaves
        // behind. Nothing ever removed it before, because the staging token is
        // in no row and the filesystem cannot say whose it is.
        auto const staging = production / "runtime-artifacts" / ".staging" / "0123abcd";
        test_support::writeFile(staging / "page-model.toml", "half a deployment");

        auto const reclaimed = coordinator->reclaimUnreferencedRuntimeArtifacts();
        REQUIRE(reclaimed.has_value());
        CHECK(reclaimed->stagingDirectories == 1U);
        CHECK_FALSE(std::filesystem::exists(staging));
        CHECK(std::filesystem::is_directory(
            production / "runtime-artifacts" / ".staging"
        ));
    }

    TEST_CASE("reclamation refuses a tree with a link planted in it")
    {
        auto temporary = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const artifactRoot = production / "runtime-artifacts";
        auto const installed = test_support::runtimeRelease(temporary.path() / "first");
        auto const orphan = releaseWithModel(
            temporary.path() / "second",
            "a different page model\r\n"
        );

        auto coordinator = OperatorCoordinator::open(production);
        REQUIRE(coordinator.has_value());
        REQUIRE(coordinator->installRuntimeArtifact(installRequest(installed, 0U)).has_value());
        CHECK_FALSE(coordinator->installRuntimeArtifact(installRequest(orphan, 0U)).has_value());

        auto const outside = temporary.path() / "outside";
        std::filesystem::create_directories(outside);
        test_support::writeFile(outside / "canary.txt", "must survive");

        auto const orphanPath = artifactRoot / orphan.artifactRootHash.hex();
        auto const linked = linkDirectory(orphanPath / "assets", outside);
#if defined(_WIN32)
        // A junction needs no privilege here, so a failure is a broken test
        // rather than an unavailable feature.
        REQUIRE(linked);
#else
        if (!linked)
        {
            MESSAGE("this account cannot create a directory symlink");
            return;
        }
#endif

        // Everything about the row still says reclaimable; only the walk
        // refuses, and it refuses rather than unlinking through the link.
        CHECK_FALSE(coordinator->reclaimUnreferencedRuntimeArtifacts().has_value());
        CHECK(std::filesystem::is_regular_file(outside / "canary.txt"));
    }

    TEST_CASE("lease takeover advances fencing and invalidates stale snapshot creation")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto const takeover = prepared.store.takeoverLease(prepared.controller, "human takeover");
        REQUIRE(takeover.has_value());
        CHECK(takeover->lease.fencingToken > prepared.lease.fencingToken);
        CHECK(takeover->resolvedDispatches == 0U);
        CHECK_FALSE(prepared.store.createSnapshot(
            prepared.lease,
            prepared.plugin,
            contract::observeOnce(prepared.observation)
        ).has_value());
    }

    TEST_CASE("commands are durable-idempotent and mutation chains are exclusive")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto const request = command(prepared.snapshot, "request-1");
        auto first = prepared.store.submitCommand(
            prepared.controller,
            request,
            toolInvocation(prepared.project, "command-1")
        );
        REQUIRE(first.has_value());
        CHECK(first->operation.lookup == CommandLookup::Created);

        auto const repeated = prepared.store.submitCommand(
            prepared.controller,
            request,
            toolInvocation(prepared.project, "command-1")
        );
        REQUIRE(repeated.has_value());
        CHECK(repeated->operation.lookup == CommandLookup::Existing);
        CHECK(repeated->operation.operationId == first->operation.operationId);

        // Idempotency is by request identity and the fingerprint is over the
        // catalog's bytes, so one command submitted twice is one fingerprint.
        CHECK(repeated->commandFingerprint == first->commandFingerprint);

        // Same client_request_id, different tool: the stored fingerprint is
        // what decides, and it covers the tool the catalog named.
        CHECK_FALSE(prepared.store.submitCommand(
            prepared.controller,
            request,
            toolInvocation(prepared.project, "different-command")
        ).has_value());
        CHECK_FALSE(prepared.store.submitCommand(
            prepared.controller,
            command(prepared.snapshot, "request-2"),
            toolInvocation(prepared.project, "command-2")
        ).has_value());

        // A read-only tool takes no mutation chain, so it is admitted while the
        // mutating Operation above is still live.
        CHECK(prepared.store.submitCommand(
            prepared.controller,
            command(prepared.snapshot, "request-3"),
            toolInvocation(prepared.project, "observe-1")
        ).has_value());

        auto const cancelled = prepared.store.transitionOperation(
            first->operation.operationId,
            first->operation.revision,
            OperationSignal::Cancelled
        );
        REQUIRE(cancelled.has_value());
        CHECK(cancelled->state == OperationState::Cancelled);
        CHECK(prepared.store.submitCommand(
            prepared.controller,
            command(prepared.snapshot, "request-2"),
            toolInvocation(prepared.project, "command-2")
        ).has_value());
    }

    TEST_CASE("dispatch freezes once and every Host outcome enters reconciliation")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto const proposed = proposedOperation(prepared, "request-1", "command-1");
        auto const frozen   = freezePlanFor(prepared, proposed);
        REQUIRE(frozen.has_value());
        auto const step = mintStepFor(prepared, frozen->operation);
        REQUIRE(step.has_value());
        auto const operation = step->operation;

        auto host           = deliveringHost(prepared);
        auto const dispatch = prepared.store.reserveDispatch(
            operation.operationId,
            operation.revision,
            prepared.lease,
            host->generation(),
            AuthorityDecisionId{"authority-1"},
            std::nullopt
        );
        REQUIRE(dispatch.has_value());
        CHECK(dispatch->authority.frozenPlanHash == frozen->planHash);

        // The one pending step is now linked to that dispatch, so a second
        // reservation finds none and refuses rather than freezing again.
        CHECK_FALSE(prepared.store.reserveDispatch(
            operation.operationId,
            dispatch->operationRevision,
            prepared.lease,
            host->generation(),
            AuthorityDecisionId{"authority-2"},
            std::nullopt
        ).has_value());

        // A refused sink is the one situation the engine cannot describe, so
        // the Host under-claims: the click may have landed before the failure.
        host->refuseClicks();
        auto const unknown = host->deliverReport(dispatch->authority);
        REQUIRE(unknown.outcome() == task::DeliveryOutcome::TransportUnknown);
        auto const reconciles = prepared.store.recordDeliveryOutcome(
            prepared.lease,
            dispatch->operationRevision,
            unknown
        );
        REQUIRE(reconciles.has_value());
        CHECK(reconciles->state == OperationState::Reconciling);
        CHECK(reconciles->planFrozen);
        CHECK_FALSE(prepared.store.recordDeliveryOutcome(
            prepared.lease,
            reconciles->revision,
            host->deliverReport(dispatch->authority)
        ).has_value());

        auto const foreign = makeProject(
            "fixture.foreign",
            "foreign-plugin-bytes"
        );
        CHECK_FALSE(prepared.store.commitReconciliation(
            prepared.plugin,
            ReconciliationCommit{
                .operationId = operation.operationId,
                .expectedOperationRevision = reconciles->revision,
                .expectedProjectStateRevision = 0U,
                .outcome                      = reconciliationOutcome(prepared, operation.operationId, "{\"disposition\":\"confirmed\"}"),
                .journalEvents = {
                    JournalAppend{
                        .eventId = "event-foreign",
                        .entry = journalEntry(
                            foreign,
                            "fixture.confirmed",
                            "{\"value\":1}"
                        ),
                    },
                },
            }
        ).has_value());

        auto const committed = prepared.store.commitReconciliation(
            prepared.plugin,
            ReconciliationCommit{
                .operationId = operation.operationId,
                .expectedOperationRevision = reconciles->revision,
                .expectedProjectStateRevision = 0U,
                .outcome                      = reconciliationOutcome(prepared, operation.operationId, "{\"disposition\":\"confirmed\"}"),
                .journalEvents = {
                    JournalAppend{
                        .eventId = "event-1",
                        .entry = journalEntry(
                            prepared.project,
                            "fixture.confirmed",
                            "{\"value\":1}"
                        ),
                    },
                },
            }
        );
        REQUIRE(committed.has_value());
        CHECK(committed->state == OperationState::Confirmed);
    }

    // The live control_leases row is what a delivery report is matched against,
    // and it is not the same fact as the authority_decisions row the reservation
    // wrote: that row is an audit record and never moves, so after a release and
    // a re-acquire the two disagree. The dispatch is still unanswered here, so
    // the outcome compare-and-swap cannot be what refuses.
    //
    // The stranded dispatch this leaves behind is deliberate and is the whole of
    // open question Q3: a release is voluntary and does not resolve what it
    // abandons, so the next restart's recovery sweep is what answers for it.
    TEST_CASE("a released lease cannot answer for the dispatch it reserved")
    {
        // One variable: whether the lease is released and re-acquired between
        // the delivery and the record. Everything else is the same schedule, so
        // the accepting run is the positive control for the refusing one.
        auto const recordsAfter = [](bool reacquire)
        {
            auto temporary = TemporaryDirectory{};
            auto prepared  = prepareStore(temporary.path());
            auto const operation = createReadyOperation(
                prepared,
                "request-1",
                "command-1"
            );
            auto host           = deliveringHost(prepared);
            auto const reserved = prepared.store.reserveDispatch(
                operation.operationId,
                operation.revision,
                prepared.lease,
                host->generation(),
                AuthorityDecisionId{"authority-1"},
                std::nullopt
            );
            REQUIRE(reserved.has_value());
            auto const displaced = host->deliverReport(reserved->authority);
            REQUIRE(displaced.outcome() == task::DeliveryOutcome::Delivered);

            if (reacquire)
            {
                REQUIRE(prepared.store.releaseLease(prepared.lease).has_value());
                auto const fresh = prepared.store.acquireLease(prepared.controller);
                REQUIRE(fresh.has_value());
                REQUIRE(fresh->leaseId != prepared.lease.leaseId);
                REQUIRE(fresh->fencingToken > prepared.lease.fencingToken);
            }

            return prepared.store.recordDeliveryOutcome(
                prepared.lease,
                reserved->operationRevision,
                displaced
            ).has_value();
        };

        CHECK(recordsAfter(false));
        CHECK_FALSE(recordsAfter(true));
    }

    // The restart sweep and a takeover run one body: the sweep names no target
    // and answers for everything, a takeover names the one target it seized.
    // Nothing else in this suite leaves a dispatch unanswered across a restart,
    // so without this the sweep only ever ran over an empty set, where it cannot
    // fail. A release strands one, which is what open() then has to answer for.
    TEST_CASE("a restart answers for the dispatch a release stranded")
    {
        auto temporary = TemporaryDirectory{};
        {
            auto prepared = prepareStore(temporary.path());
            auto const operation = createReadyOperation(
                prepared,
                "request-1",
                "command-1"
            );
            auto host           = deliveringHost(prepared);
            auto const reserved = prepared.store.reserveDispatch(
                operation.operationId,
                operation.revision,
                prepared.lease,
                host->generation(),
                AuthorityDecisionId{"authority-1"},
                std::nullopt
            );
            REQUIRE(reserved.has_value());
            REQUIRE(host->deliver(reserved->authority).has_value());
            REQUIRE(prepared.store.releaseLease(prepared.lease).has_value());
        }

        // Dropping the coordinator closes the database. The reopen runs the
        // sweep, and a sweep that touched the wrong rows or lost a
        // compare-and-swap fails the open rather than opening a store whose
        // dispatch nobody answered for. One coordinator owns a runtime
        // directory at a time, so each restart is its own scope.
        {
            auto restarted = OperatorCoordinator::open(
                temporary.path() / "production"
            );
            REQUIRE(restarted.has_value());
        }

        // Second restart: the sweep is idempotent because the dispatch it
        // resolved is no longer unanswered.
        auto again = OperatorCoordinator::open(temporary.path() / "production");
        REQUIRE(again.has_value());
    }

    // Only not_delivered proves an external effect absent, and only a Host that
    // consumed its authorization and never called into the delivery path can
    // produce it. transport_unknown deliberately under-claims, so it must not
    // unlock the same conclusion.
    TEST_CASE("only a proven absence unlocks Rejected")
    {
        auto const rejectedFor = [](task::DeliveryOutcome outcome)
        {
            auto temporary = TemporaryDirectory{};
            auto prepared  = prepareStore(temporary.path());
            auto const operation = createReadyOperation(
                prepared,
                "request-1",
                "command-1"
            );
            auto host           = deliveringHost(prepared);
            auto const dispatch = prepared.store.reserveDispatch(
                operation.operationId,
                operation.revision,
                prepared.lease,
                host->generation(),
                AuthorityDecisionId{"authority-1"},
                std::nullopt
            );
            REQUIRE(dispatch.has_value());
            if (outcome == task::DeliveryOutcome::TransportUnknown)
            {
                host->refuseClicks();
            }
            auto const report = outcome == task::DeliveryOutcome::NotDelivered
                ? host->deliverIntoAnotherCycle(dispatch->authority)
                : host->deliverReport(dispatch->authority);
            REQUIRE(report.outcome() == outcome);
            auto const reconciling = prepared.store.recordDeliveryOutcome(
                prepared.lease,
                dispatch->operationRevision,
                report
            );
            REQUIRE(reconciling.has_value());
            CHECK(host->clicks() == 0U);

            return prepared.store.commitReconciliation(
                prepared.plugin,
                ReconciliationCommit{
                    .operationId                  = reconciling->operationId,
                    .expectedOperationRevision    = reconciling->revision,
                    .expectedProjectStateRevision = 0U,
                    .outcome                      = reconciliationOutcome(
                        prepared,
                        reconciling->operationId,
                        "{\"disposition\":\"rejected\"}"
                    ),
                    .journalEvents                = {},
                }
            ).has_value();
        };

        CHECK(rejectedFor(task::DeliveryOutcome::NotDelivered));
        CHECK_FALSE(rejectedFor(task::DeliveryOutcome::TransportUnknown));
    }

    namespace
    {
        // Drives one command all the way to Reconciling, which is the only
        // state commitReconciliation accepts.
        [[nodiscard]]
        auto reconcilingOperation(
            PreparedStore& prepared,
            std::string_view clientRequestId,
            std::string_view toolName
        ) -> StoredOperation
        {
            // Every dispatch needs its own authority decision id, so it is
            // derived from the request rather than fixed.
            auto const authority = AuthorityDecisionId{
                std::format("authority-{}", clientRequestId),
            };
            auto const operation = createReadyOperation(
                prepared,
                std::string{clientRequestId},
                toolName
            );
            auto host           = deliveringHost(prepared);
            auto const dispatch = prepared.store.reserveDispatch(
                operation.operationId,
                operation.revision,
                prepared.lease,
                host->generation(),
                authority,
                std::nullopt
            );
            REQUIRE(dispatch.has_value());
            auto reconciles = prepared.store.recordDeliveryOutcome(
                prepared.lease,
                dispatch->operationRevision,
                host->deliverReport(dispatch->authority)
            );
            REQUIRE(reconciles.has_value());
            REQUIRE(reconciles->state == OperationState::Reconciling);
            return *reconciles;
        }

        [[nodiscard]]
        auto confirmedCommit(
            PreparedStore const& prepared,
            StoredOperation const& operation,
            uint64 expectedProjectStateRevision,
            std::string eventId,
            std::string payload
        ) -> ReconciliationCommit
        {
            return ReconciliationCommit{
                .operationId                  = operation.operationId,
                .expectedOperationRevision    = operation.revision,
                .expectedProjectStateRevision = expectedProjectStateRevision,
                .outcome                      = reconciliationOutcome(prepared, operation.operationId, "{\"disposition\":\"confirmed\"}"),
                .journalEvents = {
                    JournalAppend{
                        .eventId = std::move(eventId),
                        .entry   = journalEntry(
                            prepared.project,
                            "fixture.confirmed",
                            std::move(payload)
                        ),
                    },
                },
            };
        }
    }

    TEST_CASE("the reducer is handed exactly the Journal prefix that is appended")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        // The baseline reduces its own creation event against no prior state.
        CHECK(
            *prepared.project.lastReduceInput
            == "{\"journal_events\":[{\"namespaced_event_type\":\"fixture.baseline\","
               "\"opaque_project_payload\":{\"kind\":\"baseline\"},"
               "\"provenance\":" + std::string{k_fixtureProvenance} + "}],"
               "\"prior_project_state\":null}"
        );

        auto const operation = reconcilingOperation(prepared, "request-1", "command-1");
        REQUIRE(prepared.store.commitReconciliation(
            prepared.plugin,
            confirmedCommit(prepared, operation, 0U, "event-1", "{\"value\":1}")
        ).has_value());

        // The envelope is a function of the appended events and the stored
        // state, so a caller that wanted the reducer to see something else has
        // nowhere to put it: the payload below is the one the Journal recorded.
        CHECK(
            *prepared.project.lastReduceInput
            == "{\"journal_events\":[{\"namespaced_event_type\":\"fixture.confirmed\","
               "\"opaque_project_payload\":{\"value\":1},"
               "\"provenance\":" + std::string{k_fixtureProvenance} + "}],"
               "\"prior_project_state\":{\"revision\":0}}"
        );
    }

    TEST_CASE("a reconciliation that fails after opening its transaction writes nothing")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path(), rejectedReducePluginSource());
        auto const operation = reconcilingOperation(prepared, "request-1", "command-1");

        // The reducer runs inside the transaction, so this fails after the
        // Journal insert would already have been prepared.
        CHECK_FALSE(prepared.store.commitReconciliation(
            prepared.plugin,
            confirmedCommit(prepared, operation, 0U, "event-1", "{\"value\":1}")
        ).has_value());

        // Both halves of "nothing was written" are observable: the same
        // event_id is still free, and the ProjectState is still at revision 0.
        auto const retried = prepared.store.commitReconciliation(
            prepared.plugin,
            confirmedCommit(prepared, operation, 0U, "event-1", "{\"value\":1}")
        );
        CHECK_FALSE(retried.has_value());
        CHECK(prepared.store.commitReconciliation(
            prepared.plugin,
            confirmedCommit(prepared, operation, 1U, "event-2", "{\"value\":1}")
        ).has_value() == false);
    }

    TEST_CASE("Rejected and Ambiguous reconciliations cannot append Journal events")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto const operation = reconcilingOperation(prepared, "request-1", "command-1");

        for (auto const document : {
                 std::string_view{"{\"disposition\":\"rejected\"}"},
                 std::string_view{"{\"disposition\":\"ambiguous\"}"},
             })
        {
            auto commit    = confirmedCommit(prepared, operation, 0U, "event-1", "{\"value\":1}");
            commit.outcome = reconciliationOutcome(
                prepared,
                operation.operationId,
                std::string{document}
            );
            CHECK_FALSE(
                prepared.store.commitReconciliation(prepared.plugin, commit).has_value()
            );
        }

        // Diverged is the mirror image: it may not claim a correction without
        // recording one.
        auto empty          = confirmedCommit(prepared, operation, 0U, "event-1", "{\"value\":1}");
        empty.outcome = reconciliationOutcome(
            prepared,
            operation.operationId,
            "{\"disposition\":\"diverged\"}"
        );
        empty.journalEvents = {};
        CHECK_FALSE(prepared.store.commitReconciliation(prepared.plugin, empty).has_value());
    }

    TEST_CASE("ApprovalToken is operation-bound and single-use")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        // The awaiting state is reached by freezing a plan whose derived risk
        // requires an approval; no caller can ask for it.
        auto const proposed = proposedOperation(prepared, "request-1", "approval-plan");
        auto const frozen   = freezePlanFor(prepared, proposed);
        REQUIRE(frozen.has_value());
        REQUIRE(frozen->approvalRequired);
        auto const step = mintStepFor(prepared, frozen->operation);
        REQUIRE(step.has_value());

        auto const request = ApprovalRequest{
            .operationId            = proposed.operationId,
            .lease                  = prepared.lease,
            .policyHash             = hashOf("policy"),
            .approverPrincipal      = "human-1",
            .approverCapabilityHash = hashOf("approval-capability"),
            .expiresAtUnixMillis    = 4'000'000'000'000U,
        };
        auto const approval = prepared.store.issueApproval(
            request,
            AuthorityDecisionId{"human-decision-1"}
        );
        REQUIRE(approval.has_value());
        auto host           = deliveringHost(prepared);
        auto const dispatch = prepared.store.reserveDispatch(
            proposed.operationId,
            step->operation.revision,
            prepared.lease,
            host->generation(),
            AuthorityDecisionId{"dispatch-authority-1"},
            *approval
        );
        REQUIRE(dispatch.has_value());

        // Recording an outcome moves no fence, which is what leaves the token
        // below differing from a usable one in the step it names and nothing
        // else. Resolving through a takeover would refuse it for its fence
        // instead, and the case would pass with the step binding removed.
        auto reconciles = prepared.store.recordDeliveryOutcome(
            prepared.lease,
            dispatch->operationRevision,
            host->deliverIntoAnotherCycle(dispatch->authority)
        );
        REQUIRE(reconciles.has_value());
        CHECK(host->clicks() == 0U);
        auto const waiting = mintStepFor(prepared, *reconciles);
        REQUIRE(waiting.has_value());
        REQUIRE(waiting->operation.state == OperationState::AwaitingApproval);

        // Single use: the token the first dispatch consumed does not answer for
        // the step that replaced it.
        CHECK_FALSE(prepared.store.reserveDispatch(
            proposed.operationId,
            waiting->operation.revision,
            prepared.lease,
            host->generation(),
            AuthorityDecisionId{"dispatch-authority-2"},
            *approval
        ).has_value());
    }
    TEST_CASE("plan authority is bound to its registration")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        // A second registration of the same shape, complete enough to build an
        // authority of its own. Authority is per registration, so this one must
        // not be able to freeze the first one's Operation.
        auto const foreignSource   = test_support::pluginSource("fixture.foreign");
        auto const foreign         = makeProject("fixture.foreign", foreignSource);
        auto const foreignManifest = sessionManifest(
            foreign.registration,
            hashOf("artifact-root"),
            hashOf("agent")
        );
        auto foreignAuthority = contract::planAuthority(
            foreign.registration,
            foreignManifest,
            "operator"
        );
        REQUIRE(foreignAuthority.has_value());

        auto const proposed = proposedOperation(prepared, "request-1", "command-1");
        CHECK_FALSE(prepared.store.freezePlan(
            proposed.operationId,
            proposed.revision,
            prepared.lease,
            prepared.plugin,
            *foreignAuthority
        ).has_value());
        CHECK(freezePlanFor(prepared, proposed).has_value());
    }

    TEST_CASE("the plugin cannot widen the workflow bound")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        auto const proposed = proposedOperation(prepared, "request-1", "oversized-plan");
        auto const frozen   = freezePlanFor(prepared, proposed);
        REQUIRE(frozen.has_value());

        // Every bound is a minimum against the ceiling, so widening is
        // arithmetically impossible rather than policy-checked.
        CHECK(frozen->limits.maximumSteps == k_workflowCeiling.maximumSteps);
        CHECK(frozen->limits.maximumDispatches == k_workflowCeiling.maximumDispatches);
        CHECK(frozen->limits.maximumObservations <= k_workflowCeiling.maximumObservations);
        CHECK(frozen->limits.maximumWaits <= k_workflowCeiling.maximumWaits);
        CHECK(frozen->limits.maximumElapsedMillis <= k_workflowCeiling.maximumElapsedMillis);
    }

    TEST_CASE("a step cannot be replayed at another index")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        // The plugin answers next_step with the identical document every time,
        // so the two steps differ in nothing except the position they were
        // minted at.
        auto const proposed = proposedOperation(prepared, "request-1", "command-1");
        auto const frozen   = freezePlanFor(prepared, proposed);
        REQUIRE(frozen.has_value());
        auto const first = mintStepFor(prepared, frozen->operation);
        REQUIRE(first.has_value());

        auto host           = deliveringHost(prepared);
        auto const dispatch = prepared.store.reserveDispatch(
            proposed.operationId,
            first->operation.revision,
            prepared.lease,
            host->generation(),
            AuthorityDecisionId{"authority-1"},
            std::nullopt
        );
        REQUIRE(dispatch.has_value());
        auto const reconciling = prepared.store.recordDeliveryOutcome(
            prepared.lease,
            dispatch->operationRevision,
            host->deliverReport(dispatch->authority)
        );
        REQUIRE(reconciling.has_value());

        auto const second = mintStepFor(prepared, *reconciling);
        REQUIRE(second.has_value());
        CHECK(second->stepKey == first->stepKey);
        CHECK(second->stepIndex == first->stepIndex + 1U);
        CHECK(second->stepIntentHash != first->stepIntentHash);
    }

    TEST_CASE("only one step may await dispatch")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        auto const proposed = proposedOperation(prepared, "request-1", "command-1");
        auto const frozen   = freezePlanFor(prepared, proposed);
        REQUIRE(frozen.has_value());
        auto const first = mintStepFor(prepared, frozen->operation);
        REQUIRE(first.has_value());

        // The check lives in mintNextStep alone. A partial unique index saying
        // the same thing would keep this green after the check was deleted.
        CHECK_FALSE(mintStepFor(prepared, first->operation).has_value());
    }

    TEST_CASE("a plan freezes once")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        auto const proposed = proposedOperation(prepared, "request-1", "command-1");
        auto const frozen   = freezePlanFor(prepared, proposed);
        REQUIRE(frozen.has_value());
        CHECK_FALSE(freezePlanFor(prepared, frozen->operation).has_value());

        // The stored plan is the first one: the dispatch still reports its
        // hash, so a second freeze did not replace the row underneath it.
        auto const step = mintStepFor(prepared, frozen->operation);
        REQUIRE(step.has_value());
        auto host           = deliveringHost(prepared);
        auto const dispatch = prepared.store.reserveDispatch(
            proposed.operationId,
            step->operation.revision,
            prepared.lease,
            host->generation(),
            AuthorityDecisionId{"authority-1"},
            std::nullopt
        );
        REQUIRE(dispatch.has_value());
        CHECK(dispatch->authority.frozenPlanHash == frozen->planHash);
    }

    TEST_CASE("read-only Operations get no plan")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        auto const readOnly = proposedOperation(prepared, "request-1", "observe-1");
        CHECK_FALSE(freezePlanFor(prepared, readOnly).has_value());
    }

    TEST_CASE("the effect envelope is order-independent")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        auto const first = proposedOperation(prepared, "request-1", "command-1");
        auto const one   = freezePlanFor(prepared, first);
        REQUIRE(one.has_value());

        // The mutation chain is per target, so the first Operation is retired
        // before the second is opened. `reordered-effects` declares the same
        // effect set in the opposite order and nothing else different.
        REQUIRE(prepared.store.transitionOperation(
            first.operationId,
            one->operation.revision,
            OperationSignal::Cancelled
        ).has_value());

        auto const second = proposedOperation(prepared, "request-2", "reordered-effects");
        auto const other  = freezePlanFor(prepared, second);
        REQUIRE(other.has_value());

        CHECK(other->effectEnvelopeHash == one->effectEnvelopeHash);
        CHECK(other->risk == one->risk);

        // The plans themselves still differ: the command is part of the plan.
        CHECK(other->planHash != one->planHash);
    }

    TEST_CASE("the dispatch records the frozen basis")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        auto const proposed = proposedOperation(prepared, "request-1", "command-1");
        auto const frozen   = freezePlanFor(prepared, proposed);
        REQUIRE(frozen.has_value());
        auto const step = mintStepFor(prepared, frozen->operation);
        REQUIRE(step.has_value());

        auto host           = deliveringHost(prepared);
        auto const dispatch = prepared.store.reserveDispatch(
            proposed.operationId,
            step->operation.revision,
            prepared.lease,
            host->generation(),
            AuthorityDecisionId{"authority-1"},
            std::nullopt
        );
        REQUIRE(dispatch.has_value());

        // None of the three was the caller's to say, and each is the value the
        // ledger derived rather than any other hash it holds.
        CHECK(dispatch->decisionBasisHash == frozen->decisionBasisHash);
        CHECK(dispatch->authority.frozenPlanHash == frozen->planHash);
        CHECK(dispatch->stepIntentHash == step->stepIntentHash);
        CHECK(dispatch->decisionBasisHash != frozen->planHash);
    }

    TEST_CASE("the caller cannot choose approval")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        auto const proposed = proposedOperation(prepared, "request-1", "approval-plan");
        auto const frozen   = freezePlanFor(prepared, proposed);
        REQUIRE(frozen.has_value());

        // The derived risk decided the edge. OperationSignal carries no
        // ReadyWithoutApproval, so no caller could have taken the other one.
        CHECK(frozen->risk == Risk::High);
        CHECK(frozen->approvalRequired);
        CHECK(frozen->operation.state == OperationState::AwaitingApproval);

        auto const step = mintStepFor(prepared, frozen->operation);
        REQUIRE(step.has_value());
        auto host = deliveringHost(prepared);
        CHECK_FALSE(prepared.store.reserveDispatch(
            proposed.operationId,
            step->operation.revision,
            prepared.lease,
            host->generation(),
            AuthorityDecisionId{"authority-1"},
            std::nullopt
        ).has_value());
    }
}
