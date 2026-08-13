// What the Operator's own ledger owns: the production database, RuntimeArtifact
// installation and reclamation, and the exact reduce envelope its journal
// builds. The properties a project's registration decides -- catalog
// mutability, schema-owner binding, who owns a disposition -- are the exported
// conformance suite's, because a consuming repository proves them against its own
// project; see conformance/source/. No property is asserted in both places.

#include <operator/ledger.hpp>
#include <operator/manifest.hpp>

#include "project-fixture.hpp"

#include <domain/content-hash.hpp>

#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

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
            auto const accepted = std::string{"return { revision = 1 }"};
            auto const at      = source.find(accepted);
            REQUIRE(at != std::string::npos);
            return source.replace(at, accepted.size(), "return { value = 99 }");
        }

        // The same plugin except that its OP:`UIActionIntent` names one
        // identifier the installed RuntimeModel does not define. Only the one
        // member moves, so a refusal is about that member and not about a
        // document the reader stopped understanding.
        [[nodiscard]]
        inline auto pluginNamingUndefinedUi(
            std::string_view spelled,
            std::string_view replacement
        ) -> std::string
        {
            auto source   = test_support::pluginSource("fixture.alpha");
            auto const at = source.find(spelled);
            REQUIRE(at != std::string::npos);
            REQUIRE(source.find(spelled, at + spelled.size()) == std::string::npos);
            return source.replace(at, spelled.size(), replacement);
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

        // Where SQLite keeps PRAGMA user_version, and a value the Operator's
        // DDL never writes there.
        constexpr auto k_userVersionOffset      = std::streamoff{60};
        constexpr auto k_nonIdentityUserVersion = std::array<char, 4>{
            '\0',
            '\0',
            '\0',
            '\x2a',
        };

        auto writeNonIdentityUserVersion(
            std::filesystem::path const& databasePath
        ) -> void
        {
            auto database = std::fstream{
                databasePath,
                std::ios::binary | std::ios::in | std::ios::out,
            };
            REQUIRE(database.good());
            database.seekp(k_userVersionOffset);
            database.write(
                k_nonIdentityUserVersion.data(),
                std::ssize(k_nonIdentityUserVersion)
            );
            REQUIRE(database.good());
        }

        [[nodiscard]]
        auto storedUserVersion(
            std::filesystem::path const& databasePath
        ) -> std::array<char, 4>
        {
            auto database = std::ifstream{databasePath, std::ios::binary};
            REQUIRE(database.good());
            database.seekg(k_userVersionOffset);
            auto stored = std::array<char, 4>{};
            database.read(stored.data(), std::ssize(stored));
            REQUIRE(database.good());
            return stored;
        }

        // Rewrites the separator inside one stored CREATE statement, which
        // changes the exact DDL text without changing what the schema means.
        //
        // Every copy of that statement is rewritten, not the first. A b-tree
        // split leaves the pre-split cell bytes in the freed space of the page
        // it split, so one CREATE statement's text can appear more than once
        // and the live copy is not the earliest; rewriting only the first moves
        // a byte SQLite never reads and leaves the identity intact.
        [[nodiscard]]
        auto mutateStoredDdlSeparator(
            std::filesystem::path const& databasePath
        ) -> std::vector<std::streamoff>
        {
            auto database = std::fstream{
                databasePath,
                std::ios::binary | std::ios::in | std::ios::out,
            };
            REQUIRE(database.good());
            auto const bytes = std::string{
                std::istreambuf_iterator<char>{database},
                std::istreambuf_iterator<char>{},
            };
            auto constexpr opening = std::string_view{"CREATE TABLE runtime_artifacts("};

            auto offsets = std::vector<std::streamoff>{};
            auto at      = bytes.find(opening);
            while (at != std::string::npos)
            {
                auto const separator = at + std::string_view{"CREATE"}.size();
                REQUIRE(bytes[separator] == ' ');
                offsets.emplace_back(static_cast<std::streamoff>(separator));
                at = bytes.find(opening, at + opening.size());
            }
            REQUIRE_FALSE(offsets.empty());

            database.clear();
            for (auto const offset : offsets)
            {
                database.seekp(offset);
                database.put('\n');
            }
            REQUIRE(database.good());
            return offsets;
        }

        [[nodiscard]]
        auto storedDdlSeparators(
            std::filesystem::path const& databasePath,
            std::vector<std::streamoff> const& offsets
        ) -> std::string
        {
            auto database = std::ifstream{databasePath, std::ios::binary};
            REQUIRE(database.good());
            auto separators = std::string{};
            for (auto const offset : offsets)
            {
                database.seekg(offset);
                separators.push_back(static_cast<char>(database.get()));
            }
            REQUIRE(database.good());
            return separators;
        }

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

        template <typename T>
        concept NamesSessionId = requires(T value) { value.sessionId; };

        template <typename T>
        concept NamesProjectInstanceKey = requires(T value) {
            value.projectInstanceKey;
        };

        template <typename T>
        concept NamesObservedInstanceId = requires(T value) {
            value.observedInstanceId;
        };

        static_assert(!NamesReducerInput<ReconciliationCommit>);
        static_assert(!NamesReducerInput<ProjectInstanceBaseline>);
        static_assert(!NamesMutability<CommandRequest>);
        static_assert(!NamesTool<CommandRequest>);
        static_assert(!NamesCanonicalArgs<CommandRequest>);
        static_assert(
            !NamesSessionId<SessionResume>,
            "SessionResume must not accept an internal session_id"
        );
        static_assert(
            !NamesProjectInstanceKey<SessionResume>,
            "SessionResume must not accept an internal project_instance_key"
        );
        static_assert(
            !NamesObservedInstanceId<ObservedInstanceProposal>,
            "ObservedInstanceProposal must not accept a final observed_instance_id"
        );

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
            ControllerBinding            controller;
            ControlLease                 lease;
            SnapshotRecord               snapshot;
            conformance::ObservationHost observation;

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
                hashOf("agent"),
                test_support::policyArtifactBytes()
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
                    .controllerCapabilities    = {std::string{conformance::k_operateCapability}},
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
            auto observation = conformance::activateObservationHost(
                *std::move(installed),
                test_support::umbraflowProbeFrame(),
                FrameId{201}
            );
            auto snapshot = store.createSnapshot(
                *lease,
                projectPlugin,
                conformance::observeOnce(observation)
            );
            REQUIRE(snapshot.has_value());
            auto runtimeModel = observation.host->runtimeModelBinding(
                observation.generation
            );
            REQUIRE(runtimeModel.has_value());
            auto planAuthority = conformance::planAuthority(
                project.registration,
                manifest,
                *runtimeModel,
                "operator",
                test_support::policyArtifactBytes(),
                test_support::k_fixtureUiAction
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
            -> std::unique_ptr<conformance::DeliveringHost>
        {
            return conformance::deliveringHostFor(
                prepared.store,
                prepared.lease,
                prepared.installedGeneration,
                prepared.runtimeArtifactRootHash,
                test_support::k_fixtureUiAction,
                test_support::umbraflowProbeFrame()
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
                prepared.project.toolCatalogSchemaOwner,
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
                prepared.project.toolCatalogSchemaOwner,
                prepared.planAuthority
            );
        }

        // A plan authority carrying nothing but the Operator's own protocol
        // readers, which is what a production deployment builds.
        // conformance::planAuthority wraps the step reader in a check that the
        // step names the run's one agreed UI action, and that check would
        // answer the cases below before the Operator did.
        [[nodiscard]]
        auto deploymentAuthority(
            PreparedStore& prepared,
            ContentHash const& runtimeArtifactRootHash
        ) -> Result<OperatorPlanAuthority>
        {
            auto runtimeModel = prepared.observation.host->runtimeModelBinding(
                prepared.observation.generation
            );
            REQUIRE(runtimeModel.has_value());
            return OperatorPlanAuthority::create(
                prepared.project.registration,
                sessionManifest(
                    prepared.project.registration,
                    runtimeArtifactRootHash,
                    hashOf("agent"),
                    test_support::policyArtifactBytes()
                ),
                *runtimeModel,
                "operator",
                test_support::policyArtifactBytes(),
                deployment::readPlanProposal,
                deployment::readStepIntent
            );
        }

        [[nodiscard]]
        auto mintStepUnder(
            PreparedStore& prepared,
            OperatorPlanAuthority const& authority
        ) -> Result<PlannedStep>
        {
            auto const proposed = proposedOperation(prepared, "request-1", "command-1");
            auto const frozen   = prepared.store.freezePlan(
                proposed.operationId,
                proposed.revision,
                prepared.lease,
                prepared.plugin,
                prepared.project.toolCatalogSchemaOwner,
                authority
            );
            REQUIRE(frozen.has_value());
            return prepared.store.mintNextStep(
                frozen->operation.operationId,
                frozen->operation.revision,
                prepared.lease,
                prepared.plugin,
                prepared.project.toolCatalogSchemaOwner,
                authority
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
        ) -> conformance::ObservationRelease
        {
            auto const handoff  = root / "release";
            auto const artifact = handoff / "runtime-artifact";
            auto const model    = std::string_view{"a page model\r\n"};
            test_support::writeFile(artifact / task::k_runtimeModelFileName, model);
            auto const manifest = std::format(
                "{{\"assets\":[],\"manifest_schema_hash\":\"{}\","
                "\"page_model\":{{\"path\":\"runtime-model.toml\",\"sha256\":\"{}\","
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
            return conformance::ObservationRelease{
                .handoffRoot         = handoff,
                .releaseManifestHash = hashOf(releaseManifest),
                .artifactRootHash    = artifactRootHash,
            };
        }

        [[nodiscard]]
        auto releaseWithModel(
            std::filesystem::path const& root,
            std::string_view model
        ) -> conformance::ObservationRelease
        {
            auto const handoff  = root / "release";
            auto const artifact = handoff / "runtime-artifact";
            test_support::writeFile(artifact / task::k_runtimeModelFileName, model);
            auto const manifest = std::format(
                "{{\"assets\":[],\"manifest_schema_hash\":\"{}\","
                "\"page_model\":{{\"path\":\"runtime-model.toml\",\"sha256\":\"{}\","
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
            return conformance::ObservationRelease{
                .handoffRoot         = handoff,
                .releaseManifestHash = hashOf(releaseManifest),
                .artifactRootHash    = artifactRootHash,
            };
        }

        [[nodiscard]]
        auto installRequest(
            conformance::ObservationRelease const& release,
            uint64 expectedInstalledGeneration
        ) -> RuntimeArtifactInstallRequest
        {
            return RuntimeArtifactInstallRequest{
                .handoffRoot                 = release.handoffRoot,
                .expectedReleaseManifestHash = release.releaseManifestHash,
                .expectedInstalledGeneration = expectedInstalledGeneration,
            };
        }

        // The whole ledger file. Compared rather than any one column, because
        // "wrote nothing" is a claim about every table at once and a case that
        // named one would absorb the next write silently. Reading it after the
        // coordinator was destroyed is what makes it complete: WAL frames are
        // checkpointed into this file on close.
        [[nodiscard]]
        auto ledgerBytes(std::filesystem::path const& databasePath) -> std::string
        {
            auto stream = std::ifstream{databasePath, std::ios::binary};
            REQUIRE(stream.good());
            return std::string{
                std::istreambuf_iterator<char>{stream},
                std::istreambuf_iterator<char>{},
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

        // Neither pinSession refusal case below needs a registered project or
        // a provisioned instance: the registration-disagreement refusal fires
        // by comparing the pin against the manifest, before any table is
        // read; the missing-instance refusal fires on a query that finds no
        // row, which a project that was never registered also produces.
        // Naming only what pinSession touches keeps each case pinned to the
        // one check under test.
        [[nodiscard]]
        auto storeWithInstalledArtifact(std::filesystem::path const& path)
            -> std::pair<OperatorCoordinator, ContentHash>
        {
            auto const release =
                test_support::runtimeRelease(path / "session-handoff");
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
            return std::pair{std::move(store), installed->rootHash()};
        }

        [[nodiscard]]
        auto manifestNamingRegistration(
            ContentHash runtimeArtifactRootHash,
            ContentHash projectRegistrationHash
        ) -> SessionManifest
        {
            auto manifest = SessionManifest::create(
                SessionManifestSpec{
                    .hostProtocolSchemaHash       = hashOf("host"),
                    .runtimeModelSchemaHash       = hashOf("runtime-schema"),
                    .runtimeModelArtifactRootHash = runtimeArtifactRootHash,
                    .operatorProtocolSchemaHash   = hashOf("operator"),
                    .projectRegistrationHash      = projectRegistrationHash,
                    .policyArtifactHash           = hashOf("policy"),
                    .journalEnvelopeSchemaHash    = hashOf("journal-envelope"),
                    .agentProfileHash             = hashOf("agent"),
                }
            );
            REQUIRE(manifest.has_value());
            return *std::move(manifest);
        }

        [[nodiscard]]
        auto observedInstanceIdentitySchemas(
            VerifiedProjectRegistration const& registration
        ) -> ObservedInstanceIdentitySchemas
        {
            auto schemas = ObservedInstanceIdentitySchemas::create(
                registration,
                {
                    ObservedInstanceIdentitySchema{
                        .schemaId = "https://fixture.example/identity/overlay/v1",
                        .validate = [](json::Value const& basis) -> Status
                        {
                            auto const nativeId = basis.find("native_id");
                            auto const epoch    = basis.find("surface_epoch");
                            if (
                                basis.kind() != json::ValueKind::Object
                                || basis.members().size() != 2U
                                || nativeId == nullptr
                                || nativeId->kind() != json::ValueKind::String
                                || nativeId->string().empty()
                                || epoch == nullptr
                                || epoch->kind() != json::ValueKind::Number
                                || !epoch->isInteger()
                                || epoch->number() < 0.0
                            )
                            {
                                return fail(
                                    AutomationErrorKind::InvalidResource,
                                    "fixture observed-instance basis violates its schema"
                                );
                            }
                            return ok();
                        },
                    },
                }
            );
            REQUIRE(schemas.has_value());
            return *std::move(schemas);
        }

        [[nodiscard]]
        auto semanticBasis(
            std::string nativeId,
            double surfaceEpoch,
            bool reversed = false
        ) -> json::Value
        {
            if (reversed)
            {
                return json::Value::ofObject({
                    json::Member{
                        "surface_epoch",
                        json::Value::ofNumber(surfaceEpoch),
                    },
                    json::Member{
                        "native_id",
                        json::Value::ofString(std::move(nativeId)),
                    },
                });
            }
            return json::Value::ofObject({
                json::Member{
                    "native_id",
                    json::Value::ofString(std::move(nativeId)),
                },
                json::Member{
                    "surface_epoch",
                    json::Value::ofNumber(surfaceEpoch),
                },
            });
        }

        [[nodiscard]]
        auto observedInstanceProposal(
            std::string localRef,
            std::string nativeId,
            std::optional<std::string> parentLocalRef = std::nullopt,
            double surfaceEpoch = 4.0,
            bool reversedBasis = false
        ) -> ObservedInstanceProposal
        {
            return ObservedInstanceProposal{
                .localRef         = std::move(localRef),
                .parentLocalRef   = std::move(parentLocalRef),
                .kind             = "fixture.overlay",
                .identitySchemaId = "https://fixture.example/identity/overlay/v1",
                .semanticIdentityBasis = semanticBasis(
                    std::move(nativeId),
                    surfaceEpoch,
                    reversedBasis
                ),
                .opaqueProjectPayload = json::Value::ofObject({
                    json::Member{
                        "visible",
                        json::Value::ofBoolean(true),
                    },
                }),
            };
        }

        [[nodiscard]]
        auto observationProposal(
            std::vector<ObservedInstanceProposal> instances
        ) -> ProjectObservationProposal
        {
            return ProjectObservationProposal{
                .schema                 = "umbraflow-project-observation-proposal/v1",
                .canonicalOpaquePayload = json::Value::ofObject({
                    json::Member{
                        "surface",
                        json::Value::ofString("fixture.surface"),
                    },
                }),
                .projectToolPreconditions = {
                    ProjectToolPrecondition{
                        .name   = "fixture.overlay_clear",
                        .status = ProjectToolPreconditionStatus::Known,
                    },
                },
                .observedInstanceProposals = std::move(instances),
            };
        }

        [[nodiscard]]
        auto runScope(
            std::string scopeId = "run-7",
            uint64 generation = 7U
        ) -> ObservedInstanceWorldScope
        {
            auto scope = ObservedInstanceWorldScope::run(
                std::move(scopeId),
                generation
            );
            REQUIRE(scope.has_value());
            return *std::move(scope);
        }
    }

    // The two operator protocol readers, on documents this registration's
    // schema owner stamped. They are read here rather than in tests/deployment
    // because each takes a ValidatedDocument and only a ProjectSchemaOwner can
    // mint one, so reaching a reader at all needs a plugin.
    TEST_CASE("the operator protocol readers read a stamped document")
    {
        auto const project = makeProject("fixture.alpha", k_pluginSource);
        auto const plugin  = loadPlugin(project, k_pluginSource);

        auto const proposal = plugin.plan(canonical(
            project.schemaOwner,
            "{\"canonical_args\":{\"value\":1},\"project_observation\":{},"
            "\"project_state\":{\"revision\":0},\"tool_name\":\"command-1\","
            "\"tool_version\":\"1\"}"
        ));
        REQUIRE(proposal.has_value());
        auto const intent = plugin.nextStep(canonical(
            project.schemaOwner,
            "{\"frozen_plan_hash\":\"" + hashOf("plan").hex()
                + "\",\"project_observation\":{},"
                  "\"project_state\":{\"revision\":0},\"step_index\":1}"
        ));
        REQUIRE(intent.has_value());

        auto const claims = deployment::readPlanProposal(*proposal);
        REQUIRE(claims.has_value());
        CHECK(claims->toolName == "command-1");
        CHECK(claims->toolVersion == "1");
        CHECK(claims->canonicalArgs == "{\"value\":1}");
        REQUIRE(claims->allowedUiActions.size() == 1U);
        CHECK(claims->allowedUiActions.front() == "fixture.step");
        REQUIRE(claims->effects.size() == 2U);
        CHECK(claims->effects.front().namespacedType == "fixture.write");
        CHECK(claims->effects.front().risk == Risk::Low);
        CHECK(claims->effects.front().scopeKind == "instance");
        CHECK(claims->effects.front().scopeKey == "alpha");
        CHECK(claims->effects.front().opaqueProjectPayload == "{\"value\":1}");
        CHECK(claims->effects.back().risk == Risk::Medium);
        CHECK(claims->effects.back().scopeKey == "beta");
        CHECK(claims->limits.maximumSteps == 8U);
        CHECK(claims->limits.maximumDispatches == 8U);
        CHECK(claims->limits.maximumObservations == 16U);
        CHECK(claims->limits.maximumWaits == 4U);
        CHECK(claims->limits.maximumElapsedMillis == 60000U);

        auto const step = deployment::readStepIntent(*intent);
        REQUIRE(step.has_value());
        CHECK(step->kind == StepKind::UiAction);
        CHECK(step->stepKey == "fixture.step");
        CHECK(step->surfaceId == "fixture.surface");
        CHECK(step->uiTargetId == "fixture.target");
        CHECK(step->actionId == "fixture.press");

        // The one claim a ValidatedDocument does not carry, and the whole of
        // what each reader still refuses. Both documents are exact JCS this
        // owner's schema accepted, so neither refusal is about canonical form
        // or about the definition -- only about which function stamped it.
        CHECK_FALSE(deployment::readPlanProposal(*intent).has_value());
        CHECK_FALSE(deployment::readStepIntent(*proposal).has_value());
    }

    TEST_CASE("OperatorCoordinator creates only the production database name")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        CHECK(prepared.store.databasePath().filename() == "operator-runtime.sqlite");
        CHECK(std::filesystem::is_regular_file(prepared.store.databasePath()));
    }

    TEST_CASE("the proposal cannot state an observed instance ID or authority binding")
    {
        static_assert(std::is_aggregate_v<ProjectObservationProposal>);
        static_assert(std::is_aggregate_v<ObservedInstanceProposal>);
        static_assert(!std::is_aggregate_v<ProjectObservation>);
        static_assert(
            !std::is_constructible_v<ObservedInstanceId, std::string>,
            "Only OperatorCoordinator may construct a final observed-instance ID"
        );
        static_assert(
            !std::is_constructible_v<ObservedInstanceId, std::string_view>,
            "A wire spelling must not construct observed-instance authority"
        );
        static_assert(
            !std::is_constructible_v<
                ProjectObservation,
                json::Value,
                std::vector<ProjectToolPrecondition>,
                std::vector<ObservedInstance>,
                std::string,
                ContentHash
            >,
            "Only OperatorCoordinator may construct the final observation"
        );
        CHECK(ProjectObservation::schema() == "umbraflow-project-observation/v1");
    }

    TEST_CASE("observed instance mint is opaque stable scoped and projects parents")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto schemas = observedInstanceIdentitySchemas(
            prepared.project.registration
        );
        auto const scope = runScope();
        auto first = prepared.store.publishProjectObservation(
            prepared.lease,
            prepared.plugin,
            scope,
            schemas,
            observationProposal({
                observedInstanceProposal("event", "overlay.event"),
                observedInstanceProposal(
                    "choice",
                    "overlay.choice",
                    std::string{"event"}
                ),
            })
        );
        REQUIRE(first.has_value());
        REQUIRE(first->observedInstances().size() == 2U);
        auto const eventId  = first->observedInstances()[0].observedInstanceId.value();
        auto const choiceId = first->observedInstances()[1].observedInstanceId.value();
        CHECK(choiceId != eventId);
        CHECK(eventId.size() == 68U);
        CHECK(eventId.starts_with("oi1_"));
        CHECK(std::ranges::all_of(
            eventId.substr(4),
            [](char character)
            {
                return (character >= '0' && character <= '9')
                    || (character >= 'a' && character <= 'f');
            }
        ));
        REQUIRE(first->observedInstances()[1].parentObservedInstanceId.has_value());
        CHECK(
            first->observedInstances()[1].parentObservedInstanceId->value()
            == eventId
        );
        CHECK(first->projectToolPreconditions().size() == 1U);
        CHECK(
            first->projectToolPreconditions()[0].status
            == ProjectToolPreconditionStatus::Known
        );
        CHECK(first->canonicalBytes().find("semantic_identity_basis") == std::string::npos);
        CHECK(first->canonicalBytes().find("identity_schema_id") == std::string::npos);
        CHECK(first->canonicalBytes().find("local_ref") == std::string::npos);
        CHECK(first->canonicalBytes().find(eventId) != std::string::npos);
        CHECK(first->canonicalBytes().find(choiceId) != std::string::npos);

        auto equivalentProposal = observationProposal({
            observedInstanceProposal(
                "renamed-event",
                "overlay.event",
                std::nullopt,
                4.0,
                true
            ),
            observedInstanceProposal(
                "renamed-choice",
                "overlay.choice",
                std::string{"renamed-event"},
                4.0,
                true
            ),
        });
        equivalentProposal.canonicalOpaquePayload = json::Value::ofObject({
            json::Member{
                "surface",
                json::Value::ofString("different opaque envelope payload"),
            },
        });
        for (auto& instance : equivalentProposal.observedInstanceProposals)
        {
            instance.opaqueProjectPayload = json::Value::ofObject({
                json::Member{
                    "visible",
                    json::Value::ofBoolean(false),
                },
            });
        }
        auto equivalent = prepared.store.publishProjectObservation(
            prepared.lease,
            prepared.plugin,
            scope,
            schemas,
            equivalentProposal
        );
        REQUIRE(equivalent.has_value());
        CHECK(
            equivalent->observedInstances()[0].observedInstanceId.value()
            == eventId
        );
        CHECK(
            equivalent->observedInstances()[1].observedInstanceId.value()
            == choiceId
        );

        auto changedBasis = prepared.store.publishProjectObservation(
            prepared.lease,
            prepared.plugin,
            scope,
            schemas,
            observationProposal({
                observedInstanceProposal("event", "overlay.other"),
            })
        );
        REQUIRE(changedBasis.has_value());
        CHECK(
            changedBasis->observedInstances()[0].observedInstanceId.value()
            != eventId
        );

        auto changedBasisMember = prepared.store.publishProjectObservation(
            prepared.lease,
            prepared.plugin,
            scope,
            schemas,
            observationProposal({
                observedInstanceProposal(
                    "event",
                    "overlay.event",
                    std::nullopt,
                    5.0
                ),
            })
        );
        REQUIRE(changedBasisMember.has_value());
        CHECK(
            changedBasisMember->observedInstances()[0].observedInstanceId.value()
            != eventId
        );

        auto changedKindProposal = observationProposal({
            observedInstanceProposal("event", "overlay.event"),
        });
        changedKindProposal.observedInstanceProposals.front().kind =
            "fixture.other-overlay";
        auto changedKind = prepared.store.publishProjectObservation(
            prepared.lease,
            prepared.plugin,
            scope,
            schemas,
            changedKindProposal
        );
        REQUIRE(changedKind.has_value());
        CHECK(
            changedKind->observedInstances()[0].observedInstanceId.value()
            != eventId
        );

        auto changedScope = prepared.store.publishProjectObservation(
            prepared.lease,
            prepared.plugin,
            runScope("run-8", 8U),
            schemas,
            observationProposal({
                observedInstanceProposal("event", "overlay.event"),
            })
        );
        REQUIRE(changedScope.has_value());
        CHECK(
            changedScope->observedInstances()[0].observedInstanceId.value()
            != eventId
        );
    }

    TEST_CASE("observed instance proposal refusals follow the normative precedence")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto schemas = observedInstanceIdentitySchemas(
            prepared.project.registration
        );
        auto const scope = runScope();
        auto const expectCode = [](
            Result<ProjectObservation> const& result,
            ProjectObservationErrorCode expected
        )
        {
            CAPTURE(projectObservationErrorWireName(expected));
            REQUIRE_FALSE(result.has_value());
            CHECK(projectObservationErrorCode(result.error()) == expected);
            CHECK(
                result.error().detailCode().message()
                == projectObservationErrorWireName(expected)
            );
        };

        auto invalidStatusAndName = observationProposal({});
        invalidStatusAndName.projectToolPreconditions.front().status =
            static_cast<ProjectToolPreconditionStatus>(0xFFU);
        invalidStatusAndName.projectToolPreconditions.emplace_back(
            ProjectToolPrecondition{
                .name   = "not_namespaced",
                .status = ProjectToolPreconditionStatus::Known,
            }
        );
        expectCode(
            prepared.store.publishProjectObservation(
                prepared.lease,
                prepared.plugin,
                scope,
                schemas,
                invalidStatusAndName
            ),
            ProjectObservationErrorCode::PreconditionNameNotNamespaced
        );

        auto invalidStatus = observationProposal({});
        invalidStatus.projectToolPreconditions.front().status =
            static_cast<ProjectToolPreconditionStatus>(0xFFU);
        expectCode(
            prepared.store.publishProjectObservation(
                prepared.lease,
                prepared.plugin,
                scope,
                schemas,
                invalidStatus
            ),
            ProjectObservationErrorCode::PreconditionStatusOutsideFactDomain
        );

        auto duplicatePrecondition = observationProposal({
            observedInstanceProposal("repeat", "overlay.a"),
            observedInstanceProposal("repeat", "overlay.b"),
        });
        duplicatePrecondition.projectToolPreconditions.emplace_back(
            duplicatePrecondition.projectToolPreconditions.front()
        );
        expectCode(
            prepared.store.publishProjectObservation(
                prepared.lease,
                prepared.plugin,
                scope,
                schemas,
                duplicatePrecondition
            ),
            ProjectObservationErrorCode::DuplicatePreconditionName
        );

        expectCode(
            prepared.store.publishProjectObservation(
                prepared.lease,
                prepared.plugin,
                scope,
                schemas,
                observationProposal({
                    observedInstanceProposal("repeat", "overlay.a"),
                    observedInstanceProposal("repeat", "overlay.b"),
                    observedInstanceProposal(
                        "orphan",
                        "overlay.orphan",
                        std::string{"missing"}
                    ),
                })
            ),
            ProjectObservationErrorCode::DuplicateObservedInstanceLocalRef
        );

        expectCode(
            prepared.store.publishProjectObservation(
                prepared.lease,
                prepared.plugin,
                scope,
                schemas,
                observationProposal({
                    observedInstanceProposal(
                        "cycle-a",
                        "overlay.a",
                        std::string{"cycle-b"}
                    ),
                    observedInstanceProposal(
                        "cycle-b",
                        "overlay.b",
                        std::string{"cycle-a"}
                    ),
                    observedInstanceProposal(
                        "orphan",
                        "overlay.orphan",
                        std::string{"missing"}
                    ),
                })
            ),
            ProjectObservationErrorCode::ObservedInstanceParentMissing
        );

        auto cycleBeforeRegistration = observationProposal({
            observedInstanceProposal(
                "cycle-a",
                "overlay.a",
                std::string{"cycle-b"}
            ),
            observedInstanceProposal(
                "cycle-b",
                "overlay.b",
                std::string{"cycle-a"}
            ),
            observedInstanceProposal("stray", "overlay.stray"),
        });
        cycleBeforeRegistration.observedInstanceProposals.back().identitySchemaId =
            "https://fixture.example/identity/unregistered/v1";
        expectCode(
            prepared.store.publishProjectObservation(
                prepared.lease,
                prepared.plugin,
                scope,
                schemas,
                cycleBeforeRegistration
            ),
            ProjectObservationErrorCode::ObservedInstanceParentCycle
        );

        auto unregisteredBeforeBasis = observationProposal({
            observedInstanceProposal(
                "invalid",
                "overlay.invalid",
                std::nullopt,
                -1.0
            ),
            observedInstanceProposal("stray", "overlay.stray"),
        });
        unregisteredBeforeBasis.observedInstanceProposals.back().identitySchemaId =
            "https://fixture.example/identity/unregistered/v1";
        expectCode(
            prepared.store.publishProjectObservation(
                prepared.lease,
                prepared.plugin,
                scope,
                schemas,
                unregisteredBeforeBasis
            ),
            ProjectObservationErrorCode::ObservedInstanceIdentitySchemaNotRegistered
        );

        expectCode(
            prepared.store.publishProjectObservation(
                prepared.lease,
                prepared.plugin,
                scope,
                schemas,
                observationProposal({
                    observedInstanceProposal("duplicate-a", "overlay.same"),
                    observedInstanceProposal("duplicate-b", "overlay.same"),
                    observedInstanceProposal(
                        "invalid",
                        "overlay.invalid",
                        std::nullopt,
                        -1.0
                    ),
                })
            ),
            ProjectObservationErrorCode::SemanticIdentityBasisSchemaViolation
        );

        expectCode(
            prepared.store.publishProjectObservation(
                prepared.lease,
                prepared.plugin,
                scope,
                schemas,
                observationProposal({
                    observedInstanceProposal("duplicate-a", "overlay.same"),
                    observedInstanceProposal("duplicate-b", "overlay.same"),
                })
            ),
            ProjectObservationErrorCode::ObservedInstanceCollision
        );
    }

    TEST_CASE("observed instance authorization checks scope before freshness")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto schemas = observedInstanceIdentitySchemas(
            prepared.project.registration
        );
        auto const scope = runScope();
        auto first = prepared.store.publishProjectObservation(
            prepared.lease,
            prepared.plugin,
            scope,
            schemas,
            observationProposal({
                observedInstanceProposal("first", "overlay.first"),
            })
        );
        REQUIRE(first.has_value());
        auto const id = first->observedInstances()[0].observedInstanceId.value();
        auto const allowed = prepared.store.resolveObservedInstance(
            prepared.lease,
            scope,
            *first,
            id
        );
        REQUIRE(allowed.has_value());
        CHECK(allowed->value() == id);

        auto const wrongScope = prepared.store.resolveObservedInstance(
            prepared.lease,
            runScope("run-8", 8U),
            *first,
            id
        );
        REQUIRE_FALSE(wrongScope.has_value());
        CHECK(
            projectObservationErrorCode(wrongScope.error())
            == ProjectObservationErrorCode::ObservedInstanceScopeMismatch
        );

        auto second = prepared.store.publishProjectObservation(
            prepared.lease,
            prepared.plugin,
            scope,
            schemas,
            observationProposal({
                observedInstanceProposal("second", "overlay.second"),
            })
        );
        REQUIRE(second.has_value());
        auto const wrongScopeAndStale = prepared.store.resolveObservedInstance(
            prepared.lease,
            runScope("run-8", 8U),
            *second,
            id
        );
        REQUIRE_FALSE(wrongScopeAndStale.has_value());
        CHECK_MESSAGE(
            projectObservationErrorCode(wrongScopeAndStale.error())
                == ProjectObservationErrorCode::ObservedInstanceScopeMismatch,
            "scope authorization must precede fresh-observation membership"
        );
        auto const stale = prepared.store.resolveObservedInstance(
            prepared.lease,
            scope,
            *second,
            id
        );
        REQUIRE_FALSE(stale.has_value());
        CHECK(
            projectObservationErrorCode(stale.error())
            == ProjectObservationErrorCode::ObservedInstanceStale
        );
    }

    TEST_CASE("observed instance binding survives a Coordinator reopen")
    {
        auto temporary = TemporaryDirectory{};
        auto retained = [&temporary]()
        {
            auto prepared = test_support::prepareStore(temporary.path());
            auto schemas = observedInstanceIdentitySchemas(
                prepared.project.registration
            );
            auto first = prepared.store.publishProjectObservation(
                prepared.lease,
                prepared.plugin,
                runScope(),
                schemas,
                observationProposal({
                    observedInstanceProposal("first", "overlay.stable"),
                })
            );
            REQUIRE(first.has_value());
            return std::tuple{
                prepared.project,
                prepared.plugin,
                prepared.manifest,
                std::string{
                    first->observedInstances()[0].observedInstanceId.value()
                }
            };
        }();

        auto reopenedResult = OperatorCoordinator::open(
            temporary.path() / "production"
        );
        REQUIRE(reopenedResult.has_value());
        auto reopened = *std::move(reopenedResult);
        auto const& [project, plugin, manifest, firstId] = retained;
        REQUIRE(reopened.pinSession(
            SessionPin{
                .sessionId                 = "session-after-reopen",
                .authenticatedControllerId = "controller-after-reopen",
                .idempotencyNamespace      = "controller-after-reopen",
                .projectRegistrationHash   = project.registration.hash(),
                .controllerCapabilities    = {
                    std::string{conformance::k_operateCapability},
                },
                .controlledTargetId = "target-after-reopen",
                .projectInstanceKey = "instance-1",
                .mode               = SessionMode::Write,
                .kind               = ControllerKind::Script,
            },
            manifest,
            std::nullopt
        ).has_value());
        auto controller = reopened.bindController("session-after-reopen");
        REQUIRE(controller.has_value());
        auto lease = reopened.acquireLease(*controller);
        REQUIRE(lease.has_value());
        auto schemas = observedInstanceIdentitySchemas(project.registration);
        auto afterReopen = reopened.publishProjectObservation(
            *lease,
            plugin,
            runScope(),
            schemas,
            observationProposal({
                observedInstanceProposal(
                    "different-local-ref",
                    "overlay.stable",
                    std::nullopt,
                    4.0,
                    true
                ),
            })
        );
        REQUIRE(afterReopen.has_value());
        CHECK(
            afterReopen->observedInstances()[0].observedInstanceId.value()
            == firstId
        );
    }

    TEST_CASE("observed instance mint separates project and registration domains")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto const scope = runScope();
        auto schemas = observedInstanceIdentitySchemas(
            prepared.project.registration
        );
        auto first = prepared.store.publishProjectObservation(
            prepared.lease,
            prepared.plugin,
            scope,
            schemas,
            observationProposal({
                observedInstanceProposal("first", "overlay.domain"),
            })
        );
        REQUIRE(first.has_value());
        auto const firstId = first->observedInstances()[0].observedInstanceId.value();

        REQUIRE(prepared.store.provisionProjectInstance(
            prepared.project.registration,
            prepared.plugin,
            ProjectInstanceBaseline{
                .projectInstanceKey  = "instance-2",
                .eventId             = "baseline-2",
                .sessionManifestHash = prepared.manifest.hash(),
                .entry = journalEntry(
                    prepared.project,
                    prepared.project.registration.baselineEventType(),
                    "{\"kind\":\"baseline\"}"
                ),
            }
        ).has_value());
        REQUIRE(prepared.store.pinSession(
            SessionPin{
                .sessionId                 = "session-project-2",
                .authenticatedControllerId = "controller-project-2",
                .idempotencyNamespace      = "controller-project-2",
                .projectRegistrationHash =
                    prepared.project.registration.hash(),
                .controllerCapabilities = {
                    std::string{conformance::k_operateCapability},
                },
                .controlledTargetId = "target-project-2",
                .projectInstanceKey = "instance-2",
                .mode               = SessionMode::Write,
                .kind               = ControllerKind::Script,
            },
            prepared.manifest,
            std::nullopt
        ).has_value());
        auto projectController = prepared.store.bindController("session-project-2");
        REQUIRE(projectController.has_value());
        auto projectLease = prepared.store.acquireLease(*projectController);
        REQUIRE(projectLease.has_value());
        auto otherProject = prepared.store.publishProjectObservation(
            *projectLease,
            prepared.plugin,
            scope,
            schemas,
            observationProposal({
                observedInstanceProposal("first", "overlay.domain"),
            })
        );
        REQUIRE(otherProject.has_value());
        CHECK(
            otherProject->observedInstances()[0].observedInstanceId.value()
            != firstId
        );
        auto const crossProject = prepared.store.resolveObservedInstance(
            *projectLease,
            scope,
            *otherProject,
            firstId
        );
        REQUIRE_FALSE(crossProject.has_value());
        CHECK(
            projectObservationErrorCode(crossProject.error())
            == ProjectObservationErrorCode::ObservedInstanceScopeMismatch
        );

        auto const foreignSource = test_support::pluginSource("fixture.foreign");
        auto foreignProject = makeProject("fixture.foreign", foreignSource);
        auto foreignPlugin  = loadPlugin(foreignProject, foreignSource);
        auto foreignManifest = test_support::sessionManifest(
            foreignProject.registration,
            prepared.runtimeArtifactRootHash,
            hashOf("agent"),
            test_support::policyArtifactBytes()
        );
        REQUIRE(prepared.store.registerProject(
            foreignProject.registration
        ).has_value());
        REQUIRE(prepared.store.provisionProjectInstance(
            foreignProject.registration,
            foreignPlugin,
            ProjectInstanceBaseline{
                .projectInstanceKey  = "instance-1",
                .eventId             = "baseline-foreign",
                .sessionManifestHash = foreignManifest.hash(),
                .entry = journalEntry(
                    foreignProject,
                    foreignProject.registration.baselineEventType(),
                    "{\"kind\":\"baseline\"}"
                ),
            }
        ).has_value());
        REQUIRE(prepared.store.pinSession(
            SessionPin{
                .sessionId                 = "session-foreign",
                .authenticatedControllerId = "controller-foreign",
                .idempotencyNamespace      = "controller-foreign",
                .projectRegistrationHash   = foreignProject.registration.hash(),
                .controllerCapabilities    = {
                    std::string{conformance::k_operateCapability},
                },
                .controlledTargetId = "target-foreign",
                .projectInstanceKey = "instance-1",
                .mode               = SessionMode::Write,
                .kind               = ControllerKind::Script,
            },
            foreignManifest,
            std::nullopt
        ).has_value());
        auto foreignController = prepared.store.bindController("session-foreign");
        REQUIRE(foreignController.has_value());
        auto foreignLease = prepared.store.acquireLease(*foreignController);
        REQUIRE(foreignLease.has_value());
        auto foreignSchemas = observedInstanceIdentitySchemas(
            foreignProject.registration
        );
        auto foreignObservation = prepared.store.publishProjectObservation(
            *foreignLease,
            foreignPlugin,
            scope,
            foreignSchemas,
            observationProposal({
                observedInstanceProposal("first", "overlay.domain"),
            })
        );
        REQUIRE(foreignObservation.has_value());
        CHECK(
            foreignObservation->observedInstances()[0].observedInstanceId.value()
            != firstId
        );

        auto const crossRegistration = prepared.store.resolveObservedInstance(
            *foreignLease,
            scope,
            *foreignObservation,
            firstId
        );
        REQUIRE_FALSE(crossRegistration.has_value());
        CHECK(
            projectObservationErrorCode(crossRegistration.error())
            == ProjectObservationErrorCode::ObservedInstanceScopeMismatch
        );
    }

    TEST_CASE("PRAGMA user_version is no part of Operator schema identity")
    {
        auto temporary          = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const databasePath = production / "operator-runtime.sqlite";
        {
            auto created = OperatorCoordinator::open(production);
            REQUIRE_MESSAGE(
                created.has_value(),
                "a created schema must equal the pinned exact DDL schema identity"
            );
        }

        writeNonIdentityUserVersion(databasePath);
        {
            auto reopened = OperatorCoordinator::open(production);
            REQUIRE_MESSAGE(
                reopened.has_value(),
                "a user_version the Operator never writes must not refuse the open"
            );
        }

        // Without this the case would also pass against an open that reset the
        // header, which is a second identity mechanism rather than none.
        CHECK_MESSAGE(
            storedUserVersion(databasePath) == k_nonIdentityUserVersion,
            "the open must neither read nor write user_version"
        );
    }

    TEST_CASE("a different exact DDL identity is refused without replacement")
    {
        auto temporary          = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const databasePath = production / "operator-runtime.sqlite";
        {
            auto created = OperatorCoordinator::open(production);
            REQUIRE_MESSAGE(
                created.has_value(),
                "a created schema must equal the pinned exact DDL schema identity"
            );
        }

        auto const mutated = mutateStoredDdlSeparator(databasePath);
        auto const refused = OperatorCoordinator::open(production);
        REQUIRE_FALSE_MESSAGE(
            refused.has_value(),
            "a different exact DDL identity must not be upgraded or replaced"
        );

        // Names the guard, because a corrupt file or a foreign application_id
        // would refuse this open just as flatly and prove nothing about
        // identity.
        CHECK_MESSAGE(
            refused.error().message().contains("schema identity"),
            "the refusal must come from the schema-identity gate"
        );
        CHECK_MESSAGE(
            storedDdlSeparators(databasePath, mutated)
                == std::string(mutated.size(), '\n'),
            "a refused database keeps the exact bytes it was refused for"
        );
    }

    TEST_CASE("pinSession names both registration hashes when the pin and manifest disagree")
    {
        auto temporary = TemporaryDirectory{};
        auto [store, artifactRootHash] = storeWithInstalledArtifact(temporary.path());

        auto const manifestRegistration = hashOf("registration-manifest-names");
        auto const pinRegistration      = hashOf("registration-pin-selects");
        auto const manifest =
            manifestNamingRegistration(artifactRootHash, manifestRegistration);

        auto const disagreeing = store.pinSession(
            SessionPin{
                .sessionId                 = "session-mismatch",
                .authenticatedControllerId = "controller-mismatch",
                .idempotencyNamespace      = "controller-mismatch",
                .projectRegistrationHash   = pinRegistration,
                .controllerCapabilities    = {std::string{conformance::k_operateCapability}},
                .controlledTargetId        = "target-mismatch",
                .projectInstanceKey        = "instance-mismatch",
                .mode                      = SessionMode::Write,
                .kind                      = ControllerKind::Script,
            },
            manifest,
            std::nullopt
        );
        REQUIRE_FALSE(disagreeing.has_value());
        CHECK(
            disagreeing.error().message().contains("does not bind the selected")
        );
        CHECK(disagreeing.error().message().contains(manifestRegistration.hex()));
        CHECK(disagreeing.error().message().contains(pinRegistration.hex()));
    }

    // What the SessionManifest pin buys. A session row stores the manifest hash
    // it was pinned under, and a later pin of the same session is refused
    // unless it presents the same one. The manifest binds the plugin
    // environment (contract-state-s05), so a framework whose Luau bridge or
    // global whitelist moved mints a different hash for the same spec and every
    // session stored under the old one stops being re-pinnable -- which is the
    // whole reason the environment is in the manifest at all.
    TEST_CASE("a session stored under one manifest is refused under another")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        auto const samePin = SessionPin{
            .sessionId                 = "session-1",
            .authenticatedControllerId = "controller-1",
            .idempotencyNamespace      = "controller-1",
            .projectRegistrationHash   = prepared.project.registration.hash(),
            .controllerCapabilities    = {std::string{conformance::k_operateCapability}},
            .controlledTargetId        = "target-1",
            .projectInstanceKey        = "instance-1",
            .mode                      = SessionMode::Write,
            .kind                      = ControllerKind::Script,
        };

        // The positive control: the stored session accepts its own manifest,
        // so the refusal below is about the manifest and not about re-pinning.
        auto const stored = sessionManifest(
            prepared.project.registration,
            prepared.runtimeArtifactRootHash,
            hashOf("agent"),
            test_support::policyArtifactBytes()
        );
        REQUIRE(prepared.store.pinSession(samePin, stored, std::nullopt).has_value());

        auto movedResult = SessionManifest::create(
            SessionManifestSpec{
                .hostProtocolSchemaHash       = hashOf("host"),
                .runtimeModelSchemaHash       = hashOf("runtime-schema"),
                .runtimeModelArtifactRootHash = prepared.runtimeArtifactRootHash,
                .operatorProtocolSchemaHash   = hashOf("operator"),
                .projectRegistrationHash      = prepared.project.registration.hash(),
                .policyArtifactHash           = hashOf("a policy this session was not pinned to"),
                .journalEnvelopeSchemaHash    = hashOf("journal-envelope"),
                .agentProfileHash             = hashOf("agent"),
            }
        );
        REQUIRE(movedResult.has_value());
        auto const moved = *std::move(movedResult);
        REQUIRE(moved.hash() != stored.hash());
        auto const refused = prepared.store.pinSession(samePin, moved, std::nullopt);
        REQUIRE_FALSE(refused.has_value());
        CHECK(
            refused.error().message().contains(
                "already names a different immutable session tuple"
            )
        );
    }

    TEST_CASE(
        "pinSession names the registration and instance key it required when "
        "no ProjectInstance exists"
    )
    {
        auto temporary = TemporaryDirectory{};
        auto [store, artifactRootHash] = storeWithInstalledArtifact(temporary.path());

        auto const registrationHash = hashOf("registration-never-provisioned");
        auto const manifest =
            manifestNamingRegistration(artifactRootHash, registrationHash);

        auto const missingInstance = store.pinSession(
            SessionPin{
                .sessionId                 = "session-no-instance",
                .authenticatedControllerId = "controller-no-instance",
                .idempotencyNamespace      = "controller-no-instance",
                .projectRegistrationHash   = registrationHash,
                .controllerCapabilities    = {std::string{conformance::k_operateCapability}},
                .controlledTargetId        = "target-no-instance",
                .projectInstanceKey        = "instance-never-provisioned",
                .mode                      = SessionMode::Write,
                .kind                      = ControllerKind::Script,
            },
            manifest,
            std::nullopt
        );
        REQUIRE_FALSE(missingInstance.has_value());
        CHECK(
            missingInstance.error().message().contains(
                "requires an existing ProjectInstance"
            )
        );
        CHECK(missingInstance.error().message().contains(registrationHash.hex()));
        CHECK(
            missingInstance.error().message().contains(
                "instance-never-provisioned"
            )
        );
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
        // The sha256 of the schema that judged the payload, which is what the
        // journal event schema manifest names for this event type. It used to
        // be hashOf("progress-schema") -- a hash of the word, from when no
        // schema stood behind it.
        CHECK(
            accepted->payloadSchemaHash() == hashOf(test_support::k_progressPayloadSchema)
        );

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
            hashOf("agent"),
            test_support::policyArtifactBytes()
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

    // The three properties readInstalledRuntimeArtifact's declaration states, one
    // case each. They are here rather than beside the verb that calls it because
    // the guarantee belongs to the door: any second caller inherits it.
    TEST_CASE("the read-only door answers for a pin without writing a byte")
    {
        auto temporary = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const databasePath = production / "operator-runtime.sqlite";
        auto const release = test_support::runtimeRelease(temporary.path());

        // Scoped, so the coordinator's connection is closed and its WAL
        // checkpointed before the ledger is measured.
        {
            auto coordinator = OperatorCoordinator::open(production);
            REQUIRE(coordinator.has_value());
            REQUIRE(
                coordinator->installRuntimeArtifact(installRequest(release, 0U))
                    .has_value()
            );
        }
        auto const installed = ledgerBytes(databasePath);

        auto const artifact = OperatorCoordinator::readInstalledRuntimeArtifact(
            production,
            1U,
            release.artifactRootHash
        );
        REQUIRE(artifact.has_value());
        CHECK(artifact->installedGeneration() == 1U);
        CHECK(artifact->rootHash() == release.artifactRootHash);
        CHECK_MESSAGE(
            ledgerBytes(databasePath) == installed,
            "the read-only door wrote to the ledger its declaration says it only reads"
        );

        // A wrong pin is refused by the same query the coordinator's door uses,
        // and a refusal writes nothing either.
        CHECK_FALSE(OperatorCoordinator::readInstalledRuntimeArtifact(
            production,
            2U,
            release.artifactRootHash
        ).has_value());
        CHECK_MESSAGE(
            ledgerBytes(databasePath) == installed,
            "a refused read wrote to the ledger"
        );
    }

    TEST_CASE("the active read-only door derives the generation without writing")
    {
        auto temporary = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const databasePath = production / "operator-runtime.sqlite";
        auto const first = test_support::runtimeRelease(temporary.path() / "first");
        auto const second = releaseWithModel(
            temporary.path() / "second",
            "a different active page model\r\n"
        );
        {
            auto coordinator = OperatorCoordinator::open(production);
            REQUIRE(coordinator.has_value());
            REQUIRE(
                coordinator->installRuntimeArtifact(installRequest(first, 0U))
                    .has_value()
            );
            REQUIRE(
                coordinator->installRuntimeArtifact(installRequest(second, 1U))
                    .has_value()
            );
        }
        auto const installed = ledgerBytes(databasePath);

        auto const active = OperatorCoordinator::readActiveInstalledRuntimeArtifact(
            production,
            second.artifactRootHash
        );
        REQUIRE(active.has_value());
        CHECK(active->installedGeneration() == 2U);
        CHECK(active->rootHash() == second.artifactRootHash);
        CHECK_FALSE(OperatorCoordinator::readActiveInstalledRuntimeArtifact(
            production,
            first.artifactRootHash
        ).has_value());
        CHECK_MESSAGE(
            ledgerBytes(databasePath) == installed,
            "active installation selection must not mutate the Operator ledger"
        );
    }

    TEST_CASE("the read-only door bootstraps no Operator layout")
    {
        auto temporary = TemporaryDirectory{};
        auto const production = temporary.path() / "production";
        auto const release = test_support::runtimeRelease(temporary.path());

        auto const artifact = OperatorCoordinator::readInstalledRuntimeArtifact(
            production,
            1U,
            release.artifactRootHash
        );
        CHECK_FALSE(artifact.has_value());
        CHECK_MESSAGE(
            !std::filesystem::exists(production),
            "reading an Operator root that does not exist created one"
        );
    }

    TEST_CASE("the read-only door is refused while a coordinator holds the directory")
    {
        auto temporary = TemporaryDirectory{};
        auto const production = temporary.path() / "production";
        auto const release = test_support::runtimeRelease(temporary.path());

        auto coordinator = OperatorCoordinator::open(production);
        REQUIRE(coordinator.has_value());
        REQUIRE(
            coordinator->installRuntimeArtifact(installRequest(release, 0U)).has_value()
        );

        // claimExclusiveOwnership holds SQLite's lock for the connection's
        // lifetime, so this read cannot proceed beside a live coordinator. That
        // is the refusal the declaration promises, and it is the reason a
        // read-only door needs no lock of its own.
        CHECK_FALSE(OperatorCoordinator::readInstalledRuntimeArtifact(
            production,
            1U,
            release.artifactRootHash
        ).has_value());
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
        test_support::writeFile(staging / "runtime-model.toml", "half a deployment");

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
            conformance::observeOnce(prepared.observation)
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

    TEST_CASE(
        "restart recovery reports actionable revisions after automatic session resume"
    )
    {
        auto temporary = TemporaryDirectory{};
        auto const production   = temporary.path() / "production";
        auto const databasePath = production / "operator-runtime.sqlite";
        auto retained = [&temporary]()
        {
            auto prepared = prepareStore(temporary.path());
            auto const operation = createReadyOperation(
                prepared,
                "request-restart-action",
                "command-1"
            );
            auto host = deliveringHost(prepared);
            auto const reserved = prepared.store.reserveDispatch(
                operation.operationId,
                operation.revision,
                prepared.lease,
                host->generation(),
                AuthorityDecisionId{"authority-restart-action"},
                std::nullopt
            );
            REQUIRE(reserved.has_value());
            REQUIRE(host->deliver(reserved->authority).has_value());
            REQUIRE(prepared.store.releaseLease(prepared.lease).has_value());
            return std::tuple{
                prepared.project,
                prepared.plugin,
                prepared.runtimeArtifactRootHash,
                operation.operationId,
            };
        }();
        auto const& [project, plugin, artifactRootHash, operationId] = retained;
        auto const beforeRead = ledgerBytes(databasePath);

        auto const artifact = OperatorCoordinator::readActiveInstalledRuntimeArtifact(
            production,
            artifactRootHash
        );
        REQUIRE(artifact.has_value());
        CHECK_MESSAGE(
            ledgerBytes(databasePath) == beforeRead,
            "read-only active selection must not consume restart recovery"
        );

        auto restarted = OperatorCoordinator::open(production);
        REQUIRE(restarted.has_value());
        auto recovered = restarted->recoveredUncertainDispatches();
        REQUIRE(recovered.has_value());
        REQUIRE(recovered->size() == 1U);
        CHECK(recovered->front().operationId == operationId);
        CHECK(recovered->front().expectedOperationRevision > 1U);
        CHECK(recovered->front().expectedProjectStateRevision == 0U);

        auto const manifest = sessionManifest(
            project.registration,
            artifactRootHash,
            hashOf("agent"),
            test_support::policyArtifactBytes()
        );
        auto const budgeted = restarted->resumeSession(
            SessionResume{
                .authenticatedControllerId = "controller-1",
                .controlledTargetId        = "target-1",
                .mode                      = SessionMode::Write,
                .kind                      = ControllerKind::Agent,
            },
            manifest
        );
        REQUIRE_FALSE(budgeted.has_value());
        CHECK_MESSAGE(
            budgeted.error().message().contains(
                "cannot resume across a process epoch"
            ),
            "budgeted Agent sessions must not resume across a process epoch"
        );

        auto controller = restarted->resumeSession(
            SessionResume{
                .authenticatedControllerId = "controller-1",
                .controlledTargetId        = "target-1",
                .mode                      = SessionMode::Write,
                .kind                      = ControllerKind::Script,
            },
            manifest
        );
        REQUIRE(controller.has_value());
        CHECK(controller->sessionId() == "session-1");
        auto lease = restarted->acquireLease(*controller);
        REQUIRE(lease.has_value());

        auto stillRecovered = restarted->recoveredUncertainDispatches();
        REQUIRE(stillRecovered.has_value());
        REQUIRE(stillRecovered->size() == 1U);
        CHECK(stillRecovered->front().operationId == operationId);
        auto const committed = restarted->commitReconciliation(
            plugin,
            ReconciliationCommit{
                .operationId = operationId,
                .expectedOperationRevision =
                    stillRecovered->front().expectedOperationRevision,
                .expectedProjectStateRevision =
                    stillRecovered->front().expectedProjectStateRevision,
                .outcome = test_support::reconcileOutcome(
                    project,
                    plugin,
                    operationId,
                    "{\"disposition\":\"ambiguous\"}"
                ),
                .journalEvents = {},
            }
        );
        REQUIRE(committed.has_value());
        CHECK(committed->state == OperationState::Ambiguous);
        auto const completed = restarted->recoveredUncertainDispatches();
        REQUIRE(completed.has_value());
        CHECK(completed->empty());
    }

    TEST_CASE("ambiguous prior sessions refuse automatic resume and remain readable")
    {
        auto temporary = TemporaryDirectory{};
        auto const production = temporary.path() / "production";
        auto retained = [&temporary]()
        {
            auto prepared = prepareStore(temporary.path());
            auto const manifest = sessionManifest(
                prepared.project.registration,
                prepared.runtimeArtifactRootHash,
                hashOf("agent"),
                test_support::policyArtifactBytes()
            );
            REQUIRE(prepared.store.provisionProjectInstance(
                prepared.project.registration,
                prepared.plugin,
                ProjectInstanceBaseline{
                    .projectInstanceKey  = "instance-ambiguous",
                    .eventId             = "baseline-ambiguous",
                    .sessionManifestHash = manifest.hash(),
                    .entry = journalEntry(
                        prepared.project,
                        prepared.project.registration.baselineEventType(),
                        "{\"kind\":\"baseline\"}"
                    ),
                }
            ).has_value());
            REQUIRE(prepared.store.pinSession(
                SessionPin{
                    .sessionId                 = "session-ambiguous",
                    .authenticatedControllerId = "controller-1",
                    .idempotencyNamespace      = "controller-ambiguous",
                    .projectRegistrationHash =
                        prepared.project.registration.hash(),
                    .controllerCapabilities = {
                        std::string{conformance::k_operateCapability},
                    },
                    .controlledTargetId = "target-1",
                    .projectInstanceKey = "instance-ambiguous",
                    .mode               = SessionMode::Write,
                    .kind               = ControllerKind::Script,
                },
                manifest,
                std::nullopt
            ).has_value());
            return std::pair{manifest, prepared.runtimeArtifactRootHash};
        }();
        auto const& [manifest, artifactRootHash] = retained;
        {
            auto restarted = OperatorCoordinator::open(production);
            REQUIRE(restarted.has_value());
            auto const resumed = restarted->resumeSession(
                SessionResume{
                    .authenticatedControllerId = "controller-1",
                    .controlledTargetId        = "target-1",
                    .mode                      = SessionMode::Write,
                    .kind                      = ControllerKind::Script,
                },
                manifest
            );
            REQUIRE_FALSE(resumed.has_value());
            CHECK_MESSAGE(
                resumed.error().message().contains("More than one most-recent"),
                "automatic resume must refuse equally recent prior sessions"
            );
        }

        CHECK(OperatorCoordinator::readActiveInstalledRuntimeArtifact(
            production,
            artifactRootHash
        ).has_value());
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
            prepared.project.documentInputLog->lastReduceInput()
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
            prepared.project.documentInputLog->lastReduceInput()
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
        REQUIRE_FALSE(frozen->requiredApprovals.empty());
        auto const step = mintStepFor(prepared, frozen->operation);
        REQUIRE(step.has_value());

        auto const request = ApprovalRequest{
            .operationId       = proposed.operationId,
            .lease             = prepared.lease,
            .approverPrincipal = "human-1",
            .approverCapability  = frozen->requiredApprovals.front(),
            .expiresAtUnixMillis = 4'000'000'000'000U,
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

        // The same RuntimeArtifact the prepared session pinned. An authority can
        // no longer be built against an artifact root nobody installed, so the
        // only difference left between the two authorities is the registration
        // -- which is the one this case is about.
        auto const foreignManifest = sessionManifest(
            foreign.registration,
            prepared.runtimeArtifactRootHash,
            hashOf("agent"),
            test_support::policyArtifactBytes()
        );
        auto runtimeModel = prepared.observation.host->runtimeModelBinding(
            prepared.observation.generation
        );
        REQUIRE(runtimeModel.has_value());
        auto foreignAuthority = conformance::planAuthority(
            foreign.registration,
            foreignManifest,
            *runtimeModel,
            "operator",
            test_support::policyArtifactBytes(),
            test_support::k_fixtureUiAction
        );
        REQUIRE(foreignAuthority.has_value());

        auto const proposed = proposedOperation(prepared, "request-1", "command-1");
        CHECK_FALSE(prepared.store.freezePlan(
            proposed.operationId,
            proposed.revision,
            prepared.lease,
            prepared.plugin,
            prepared.project.toolCatalogSchemaOwner,
            *foreignAuthority
        ).has_value());
        CHECK(freezePlanFor(prepared, proposed).has_value());
    }

    // A plan's surface_id, ui_target_id and action_id reach no Host: the
    // Receipt's intent is minted by the trusted chunk out of the model, and
    // task::DispatchAuthority carries no UI identifier. Until the step check
    // below existed they were decoration, and a plan could name UI that exists
    // in no RuntimeModel and still be dispatched.
    TEST_CASE("a step naming UI the installed RuntimeModel does not define is refused")
    {
        struct UndefinedUi final
        {
            std::string_view member{};
            std::string_view spelled{};
            std::string_view replacement{};
        };
        auto const undefined = std::array{
            UndefinedUi{
                "surface_id",
                R"(surface_id = "fixture.surface")",
                R"(surface_id = "fixture.absent")",
            },
            UndefinedUi{
                "ui_target_id",
                R"(ui_target_id = "fixture.target")",
                R"(ui_target_id = "fixture.absent")",
            },
            UndefinedUi{
                "action_id",
                R"(action_id = "fixture.press")",
                R"(action_id = "fixture.absent")",
            },
        };
        for (auto const& named : undefined)
        {
            CAPTURE(named.member);
            auto temporary = TemporaryDirectory{};
            auto const source = pluginNamingUndefinedUi(
                named.spelled,
                named.replacement
            );
            auto prepared  = prepareStore(temporary.path(), source);
            auto authority = deploymentAuthority(
                prepared,
                prepared.runtimeArtifactRootHash
            );
            REQUIRE(authority.has_value());
            CHECK_FALSE(mintStepUnder(prepared, *authority).has_value());
        }
    }

    // The positive control the three refusals above are worthless without: the
    // identical route, the identical authority, and the fixture's own plan,
    // whose three identifiers this project's model does define.
    TEST_CASE("a step naming UI the installed RuntimeModel defines is minted")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto authority = deploymentAuthority(
            prepared,
            prepared.runtimeArtifactRootHash
        );
        REQUIRE(authority.has_value());

        auto const step = mintStepUnder(prepared, *authority);
        REQUIRE(step.has_value());
        CHECK(step->kind == StepKind::UiAction);
    }

    TEST_CASE("a plan authority answers only for the pinned RuntimeArtifact")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        // The Host parsed the installed artifact; this manifest pins another.
        // Without the refusal an authority could carry one project's declared
        // vocabulary into a session pinned to a different model.
        CHECK_FALSE(
            deploymentAuthority(prepared, hashOf("another-artifact")).has_value()
        );
        CHECK(
            deploymentAuthority(prepared, prepared.runtimeArtifactRootHash)
                .has_value()
        );
    }

    // What binding the authority at creation does not close: a manifest is not a
    // session, and two manifests of one registration may pin two RuntimeArtifacts.
    // The authority below is honestly built -- its manifest and its model agree --
    // and still answers for a model this Operation's session row does not name,
    // so only the ledger's own column can refuse it.
    TEST_CASE("a plan authority for another installed artifact cannot mint a step")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        // A second model differing only by one further scene, so its declared
        // vocabulary still contains every identifier the plan names and the
        // artifact root is the only thing left that can decide.
        auto const second = conformance::observationRelease(
            temporary.path() / "second",
            conformance::ProjectRuntimeArtifact{
                .model  = test_support::ambiguousRuntimeModel(),
                .assets = test_support::umbraflowRuntimeAssets(),
            }
        );
        auto installed = prepared.store.installRuntimeArtifact(
            installRequest(second, 1U)
        );
        REQUIRE(installed.has_value());
        auto const secondRootHash = installed->rootHash();
        REQUIRE(secondRootHash != prepared.runtimeArtifactRootHash);

        auto secondHost = conformance::activateObservationHost(
            *std::move(installed),
            test_support::umbraflowProbeFrame(),
            FrameId{909}
        );
        auto secondModel = secondHost.host->runtimeModelBinding(
            secondHost.generation
        );
        REQUIRE(secondModel.has_value());

        auto authority = OperatorPlanAuthority::create(
            prepared.project.registration,
            sessionManifest(
                prepared.project.registration,
                secondRootHash,
                hashOf("agent"),
                test_support::policyArtifactBytes()
            ),
            *secondModel,
            "operator",
            test_support::policyArtifactBytes(),
            deployment::readPlanProposal,
            deployment::readStepIntent
        );
        REQUIRE(authority.has_value());
        CHECK_FALSE(mintStepUnder(prepared, *authority).has_value());
    }

    TEST_CASE("the plugin cannot widen the workflow bound")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        // The bound is this tool's own descriptor and not a number compiled in
        // beside it, so the case reads the catalog rather than a constant: a
        // catalog that raised the tool's ceiling would raise this expectation
        // with it, which is what makes tool_catalog_hash the single authority.
        auto const declared =
            prepared.project.toolCatalogSchemaOwner.describe("oversized-plan");
        REQUIRE(declared.has_value());

        auto const proposed = proposedOperation(prepared, "request-1", "oversized-plan");
        auto const frozen   = freezePlanFor(prepared, proposed);
        REQUIRE(frozen.has_value());

        // Every bound is a minimum against the descriptor, so widening is
        // arithmetically impossible rather than policy-checked. The proposal
        // asks for far more than the descriptor allows on the first two.
        CHECK(frozen->limits.maximumSteps == declared->limits.maximumSteps);
        CHECK(frozen->limits.maximumDispatches == declared->limits.maximumDispatches);
        CHECK(frozen->limits.maximumObservations <= declared->limits.maximumObservations);
        CHECK(frozen->limits.maximumWaits <= declared->limits.maximumWaits);
        CHECK(frozen->limits.maximumElapsedMillis <= declared->limits.maximumElapsedMillis);
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
        CHECK(
            frozen->requiredApprovals
            == std::vector<std::string>{
                std::string{conformance::k_approveCapability},
            }
        );
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
