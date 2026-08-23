#include "suite-support.hpp"

#include <deployment/project-directory.hpp>

#include <operator/project-generation.hpp>
#include <operator/runtime-installation.hpp>

#include <script/scoped-tool-program.hpp>

#include <json/value.hpp>

#include <task/runtime-model-file.hpp>

#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace uf::operator_runtime::conformance
{
    namespace
    {
        // Written once by main before any case runs, and read from then on. It
        // is a function-local static rather than a namespace-scope variable so
        // that nothing outside the two accessors below can name it.
        [[nodiscard]] auto projectDirectorySlot() -> std::filesystem::path&
        {
            static auto s_directory = std::filesystem::path{};
            return s_directory;
        }


        // Where prepareStore puts the Operator's runtime directory under the
        // root a case owns. It is named once because reopenStore has to open
        // the same directory, and a second spelling would be the one that goes
        // stale.
        constexpr auto k_runtimeSubdirectory = std::string_view{"production"};

        // Wide enough that no case here reaches a ceiling. A case that IS about
        // a budget would state its own numbers; none of these are, so meeting
        // one would mean a case was measuring this table.
        constexpr auto k_conformanceAgentBudget = AgentBudget{
            .maximumToolCalls     = 1'000U,
            .maximumMutations     = 1'000U,
            .maximumObservations  = 1'000U,
            .maximumElapsedMillis = 3'600'000U,
            .maximumRiskUnits     = 1'000'000U,
        };

        [[nodiscard]]
        auto roleOf(
            deployment::ConformanceProject const& project UF_LIFETIME_BOUND,
            ProjectRole role
        ) -> deployment::ProjectConformanceRole const&
        {
            return role == ProjectRole::UnderTest
                ? project.underTest
                : project.foreign;
        }

        [[nodiscard]]
        auto profileCeiling(
            json::Value const& profile,
            std::string_view member
        ) -> std::optional<uint64>
        {
            auto const* const p_ceiling = profile.find(member);
            if (p_ceiling == nullptr || p_ceiling->kind() != json::ValueKind::Number)
            {
                return std::nullopt;
            }
            return static_cast<uint64>(p_ceiling->number());
        }

        // What answers a Framework Tool that a scoped run of the project under
        // test reaches.
        //
        // A conformance run has no Framework provider and cannot invent one:
        // the Tools that observe, wait, record and deliver are answered by
        // service::ProductLifecycle, privately, inside a composition root
        // holding a live Host session, and a second answer written here would
        // be a second account of what framework.* does with only one of them
        // being the Framework's. So it refuses by name.
        //
        // Which runs reach it is the project's decision rather than this
        // suite's. A catalog whose descriptors declare no child call cannot
        // reach it at all; a catalog whose handler calls a Framework Tool gets
        // this sentence, which is the honest answer to a capability the suite
        // does not have.
        [[nodiscard]]
        auto conformanceFrameworkTools() -> ToolProvider
        {
            return [](ToolCallPositionIdentity const& call)
                -> Result<ToolCallCompletion>
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "a conformance run has no Framework Tool provider, so it "
                    "cannot answer " + call.toolName()
                );
            };
        }
    }

    auto provisioningToolRuntime() -> script::ToolRuntimeInvoke
    {
        return [](
                   std::string_view,
                   json::Value const&,
                   script::ToolCallCoordinate const&,
                   std::stop_token
               ) -> Result<json::Value>
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "a registration compiled for provisioning admits no Tool call"
            );
        };
    }

    auto setProjectDirectory(std::filesystem::path directory) -> void
    {
        projectDirectorySlot() = std::move(directory);
    }

    auto projectDirectory() -> std::filesystem::path
    {
        return projectDirectorySlot();
    }

    auto loadedProject() -> deployment::ConformanceProject
    {
        auto const directory = projectDirectory();
        REQUIRE_MESSAGE(
            !directory.empty(),
            "no project directory was set; run umbra-flow-conformance --project"
        );
        auto loaded = deployment::loadConformanceProject(directory, {});
        REQUIRE_MESSAGE(
            loaded.has_value(),
            "the project directory could not be loaded: ",
            loaded.error().message()
        );
        return *std::move(loaded);
    }

    auto deploymentFor(
        deployment::ConformanceProject const& project,
        ProjectRole role
    ) -> deployment::LoadedDeployment const&
    {
        auto const* p_deployment = project.loaded.findDeployment(
            roleOf(project, role).deployment
        );
        REQUIRE(p_deployment != nullptr);
        return *p_deployment;
    }

    auto vocabularyFor(
        deployment::ConformanceProject const& project,
        ProjectRole role
    ) -> deployment::ProjectVocabulary const&
    {
        return roleOf(project, role).vocabulary;
    }

    auto uiActionOf(deployment::ProjectVocabulary const& vocabulary)
        -> task::UiActionUnderTest
    {
        return task::UiActionUnderTest{
            .surface  = vocabulary.uiAction.surface,
            .uiTarget = vocabulary.uiAction.uiTarget,
            .action   = vocabulary.uiAction.action,
        };
    }

    TemporaryDirectory::TemporaryDirectory(std::string_view label)
    {
        static auto s_sequence = std::atomic<uint64>{1};
        m_path = std::filesystem::temp_directory_path()
            / std::format(
                "umbraflow-conformance-{}-{}-{}",
                label,
                std::chrono::steady_clock::now().time_since_epoch().count(),
                s_sequence.fetch_add(1, std::memory_order_relaxed)
            );
        auto error         = std::error_code{};
        auto const created = std::filesystem::create_directory(m_path, error);
        REQUIRE(created);
        REQUIRE_FALSE(error);
    }

    TemporaryDirectory::~TemporaryDirectory() noexcept
    {
        auto error = std::error_code{};
        static_cast<void>(std::filesystem::remove_all(m_path, error));
    }

    auto TemporaryDirectory::path() const -> std::filesystem::path const&
    {
        return m_path;
    }

    auto hashOf(std::string_view value) -> ContentHash
    {
        auto const result = sha256(std::as_bytes(std::span{value}));
        REQUIRE(result.has_value());
        return *result;
    }

    auto canonical(
        deployment::ConformanceProject const& project,
        ProjectRole role,
        std::string value
    ) -> CanonicalJson
    {
        auto result = deploymentFor(project, role).schemaOwner.canonicalize(
            std::move(value)
        );
        REQUIRE(result.has_value());
        return *result;
    }

    auto journalEntry(
        deployment::ConformanceProject const& project,
        ProjectRole role,
        deployment::ProjectJournalDocument const& document
    ) -> ValidatedJournalEntryData
    {
        auto result = deploymentFor(project, role).journalSchemaOwner.validate(
            document.eventType,
            canonical(project, role, document.payload),
            canonical(project, role, vocabularyFor(project, role).provenance)
        );
        REQUIRE(result.has_value());
        return *result;
    }

    auto toolInvocation(
        deployment::ConformanceProject const& project,
        ProjectRole role,
        std::string toolName
    ) -> ValidatedToolInvocation
    {
        auto result = deploymentFor(project, role).toolCatalogSchemaOwner.validate(
            std::move(toolName),
            canonical(project, role, vocabularyFor(project, role).toolArguments)
        );
        REQUIRE(result.has_value());
        return *result;
    }

    auto loadGeneration(
        deployment::ConformanceProject const& project,
        ProjectRole role,
        script::ToolRuntimeInvoke invokeTool
    ) -> ProjectGenerationHandle
    {
        auto const& one = deploymentFor(project, role);
        auto registrar  = ProjectGenerationRegistrar{};
        auto result     = registrar.registerGeneration(
            one.generation,
            one.toolCatalogSchemaOwner,
            one.schemaOwner,
            ProjectGenerationRegistrar::ClosureModules{
                .entryModule = one.reducerClosure.entryModule,
                .modules     = one.reducerClosure.modules,
            },
            ProjectGenerationRegistrar::ClosureModules{
                .entryModule = one.toolClosure.entryModule,
                .modules     = one.toolClosure.modules,
            },
            one.projectResources,
            one.catalog.toolResultValidator(),
            std::move(invokeTool)
        );
        REQUIRE(result.has_value());
        return *result;
    }

    auto policyArtifact(
        deployment::LoadedDeployment const& deployed,
        deployment::ProjectVocabulary const& vocabulary
    ) -> std::string
    {
        // Read out of this project's own descriptors rather than named here: a
        // policy speaking about an effect type this project never proposes
        // would allow nothing, and every plan would meet the artifact's
        // default deny instead of the rule a case is about.
        auto types = std::vector<std::string>{};
        for (auto const& tool : std::array{
                 vocabulary.mutatingTool,
                 vocabulary.approvalRequiredPlanTool,
             })
        {
            auto const descriptor = deployed.catalog.carriedTool(tool);
            REQUIRE(descriptor.has_value());
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access): REQUIRE above proved engagement.
            for (auto const& bound : descriptor->effectBounds)
            {
                types.emplace_back(bound.namespacedType);
            }
        }
        std::ranges::sort(types);
        types.erase(std::ranges::unique(types).begin(), types.end());
        REQUIRE_FALSE(types.empty());
        return policyArtifactBytes(hashOf("operator"), types);
    }

    auto agentProfileBytes() -> std::string
    {
        // The AgentBudget's own member order, which is also JCS order, so these
        // bytes are exact canonical form rather than a spelling that happens to
        // parse.
        return std::format(
            R"({{"maximum_elapsed_ms":{},"maximum_mutations":{},)"
            R"("maximum_observations":{},"maximum_risk_units":{},)"
            R"("maximum_tool_calls":{}}})",
            k_conformanceAgentBudget.maximumElapsedMillis,
            k_conformanceAgentBudget.maximumMutations,
            k_conformanceAgentBudget.maximumObservations,
            k_conformanceAgentBudget.maximumRiskUnits,
            k_conformanceAgentBudget.maximumToolCalls
        );
    }

    auto agentProfileValidator() -> AgentProfileValidator
    {
        // Reads the five ceilings out of the exact bytes rather than answering
        // with the table above. The bytes are what agent_profile_hash attests
        // to, and a validator that ignored them would let any budget answer for
        // any manifest.
        return [](std::string_view exactJcs) -> Result<AgentBudget>
        {
            UF_TRY_VALUE(profile, json::parse(exactJcs));
            auto const toolCalls    = profileCeiling(profile, "maximum_tool_calls");
            auto const mutations    = profileCeiling(profile, "maximum_mutations");
            auto const observations = profileCeiling(
                profile,
                "maximum_observations"
            );
            auto const elapsed   = profileCeiling(profile, "maximum_elapsed_ms");
            auto const riskUnits = profileCeiling(profile, "maximum_risk_units");
            if (
                !toolCalls
                || !mutations
                || !observations
                || !elapsed
                || !riskUnits
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "AgentProfile bytes are not a complete budget document"
                );
            }
            return AgentBudget{
                .maximumToolCalls     = *toolCalls,
                .maximumMutations     = *mutations,
                .maximumObservations  = *observations,
                .maximumElapsedMillis = *elapsed,
                .maximumRiskUnits     = *riskUnits,
            };
        };
    }

    auto sessionManifest(
        ProjectIdentity const& registration,
        ContentHash const& runtimeArtifactRootHash,
        std::string_view exactPolicyArtifactBytes
    ) -> SessionManifest
    {
        auto const result = SessionManifest::create(
            SessionManifestSpec{
                .runtimeModelArtifactRootHash = runtimeArtifactRootHash,
                .operatorProtocolSchemaHash   = hashOf("operator"),
                .projectRegistrationHash      = registration.hash(),
                .policyArtifactHash           = hashOf(exactPolicyArtifactBytes),

                // The exact bytes rather than a label: an Agent session is
                // pinned by presenting the profile this hash names, so a
                // manifest naming bytes nothing produces admits no Agent at
                // all.
                .agentProfileHash = hashOf(agentProfileBytes()),
            }
        );
        REQUIRE(result.has_value());
        return *result;
    }

    auto prepareStore(std::filesystem::path const& root) -> PreparedStore
    {
        auto project             = loadedProject();
        auto const& underTest    = deploymentFor(project, ProjectRole::UnderTest);
        auto const& vocabulary   = vocabularyFor(project, ProjectRole::UnderTest);

        // umbraflow-conformance.json decides the baseline entry and
        // umbraflow-project.json decides the baseline event type. When they
        // disagree nothing below can run, and saying so here names the project
        // directory rather than the Operator.
        REQUIRE(
            vocabulary.baselineEntry.eventType
            == ProjectIdentity{underTest.generation}.baselineEventType()
        );

        auto const release = observationRelease(
            root / "session-handoff",
            project.loaded.runtimeArtifactRoot
        );
        auto storeResult   = OperatorCoordinator::open(root / k_runtimeSubdirectory);
        REQUIRE(storeResult.has_value());
        auto store     = *std::move(storeResult);
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

        auto const policy   = policyArtifact(underTest, vocabulary);
        auto const manifest = sessionManifest(
            ProjectIdentity{underTest.generation},
            installed->rootHash(),
            policy
        );
        auto const generation = loadGeneration(
            project,
            ProjectRole::UnderTest,
            provisioningToolRuntime()
        );
        REQUIRE(store.registerProject(underTest.generation).has_value());
        REQUIRE(store.provisionProjectInstance(
            ProjectIdentity{underTest.generation},
            generation,
            ProjectInstanceBaseline{
                .projectInstanceKey  = "instance-1",
                .eventId             = "baseline-1",
                .sessionManifestHash = manifest.hash(),
                .entry               = journalEntry(
                    project,
                    ProjectRole::UnderTest,
                    vocabulary.baselineEntry
                ),
            }
        ).has_value());
        auto const sessionWorldScope = ObservedInstanceWorldScope::run(
            "target-1",
            1
        );
        REQUIRE(sessionWorldScope.has_value());
        REQUIRE(store.pinSession(
            SessionPin{
                .sessionId                 = "session-1",
                .authenticatedControllerId = "controller-1",
                .idempotencyNamespace      = "controller-1",
                .projectRegistrationHash   = underTest.generation.hash(),
                .controllerCapabilities    = {std::string{k_operateCapability}},
                .controlledTargetId        = "target-1",
                .projectInstanceKey        = "instance-1",
                .mode                      = SessionMode::Write,
                .kind                      = ControllerKind::Script,
                .worldScope                = *sessionWorldScope,
            },
            manifest,
            std::nullopt
        ).has_value());

        auto const controller = store.bindController("session-1");
        REQUIRE(controller.has_value());
        auto const lease = store.acquireLease(*controller);
        REQUIRE(lease.has_value());

        // requireProbeGeometry runs inside this call rather than at the top of
        // this function: after the Q2 ruling the extent the capture must match
        // is the model's, and the model is not parsed until the Host activates
        // the artifact. It is still ahead of the observation below, so
        // requireResolvedSurface's account of what can reach it is unchanged.
        auto observation = activateObservationHost(
            *std::move(installed),
            project.probeFrame,
            FrameId{301}
        );
        auto const reading = observeOnce(observation);

        // Checked once, here, rather than per case: every case below plans on a
        // resolved state, so a probe frame this project's model does not satisfy
        // must be named where it was supplied.
        requireResolvedSurface(reading, vocabulary.uiAction.surface);

        auto snapshot = store.createSnapshot(
            *lease,
            ProjectIdentity{underTest.generation},
            deploymentFor(project, ProjectRole::UnderTest).toolCatalogSchemaOwner,
            deploymentFor(project, ProjectRole::UnderTest).observedInstanceIdentitySchemas,
            reading
        );
        REQUIRE(snapshot.has_value());

        // "operator" is the exact operator protocol schema sessionManifest
        // above pins. The authority hashes the bytes and compares, so a suite
        // that named the wrong ones could not build one at all.
        //
        // The RuntimeModel binding comes from the Host that just activated this
        // project's artifact. It is what lets the Operator refuse a step naming
        // UI the model does not define, and the suite cannot substitute one: the
        // binding is minted only by TaskHost and only for the artifact the
        // session manifest pins.
        auto runtimeModel = observation.host->runtimeModelBinding(
            observation.generation
        );
        REQUIRE(runtimeModel.has_value());
        auto authority = OperatorPolicyAuthority::create(
            ProjectIdentity{underTest.generation},
            manifest,
            *runtimeModel,
            "operator",
            policy
        );
        REQUIRE(authority.has_value());
        return PreparedStore{
            .store                   = std::move(store),
            .generation              = generation,
            .project                 = std::move(project),
            .manifest                = manifest,
            .policyAuthority         = *std::move(authority),
            .controller              = *controller,
            .lease                   = *lease,
            .snapshot                = *std::move(snapshot),
            .observation             = std::move(observation),
            .runtimeArtifactRootHash = artifactRootHash,
            .installedGeneration     = installedGeneration,
        };
    }

    auto deliveringHost(PreparedStore& prepared)
        -> std::unique_ptr<DeliveringHost>
    {
        return deliveringHostFor(
            prepared.store,
            prepared.lease,
            prepared.installedGeneration,
            prepared.runtimeArtifactRootHash,
            uiActionOf(prepared.project.underTest.vocabulary),
            prepared.project.probeFrame
        );
    }


    auto observeAgain(PreparedStore& prepared) -> task::UiObservationSnapshot
    {
        return observeOnce(prepared.observation);
    }

    auto freshSnapshot(PreparedStore& prepared) -> SnapshotRecord
    {
        auto snapshot = prepared.store.createSnapshot(
            prepared.lease,
            ProjectIdentity{deploymentFor(prepared.project, ProjectRole::UnderTest).generation},
            deploymentFor(
                prepared.project,
                ProjectRole::UnderTest
            ).toolCatalogSchemaOwner,
            deploymentFor(
                prepared.project,
                ProjectRole::UnderTest
            ).observedInstanceIdentitySchemas,
            observeAgain(prepared)
        );
        REQUIRE(snapshot.has_value());
        return *std::move(snapshot);
    }

    auto reopenStore(std::filesystem::path const& root) -> OperatorCoordinator
    {
        auto reopened = OperatorCoordinator::open(root / k_runtimeSubdirectory);
        REQUIRE(reopened.has_value());
        return *std::move(reopened);
    }

    auto toolRuntimeOver(PreparedStore& prepared) -> PreparedToolRuntime
    {
        auto observations = std::make_unique<SnapshotObservationAuthority>();
        auto dispatcher   = ProjectToolDispatcher::create(
            prepared.store,
            *observations,
            prepared.policyAuthority,
            conformanceFrameworkTools()
        );
        REQUIRE(dispatcher.has_value());

        // The one registration a run dispatches through, compiled with the
        // dispatcher's own seam. It is a second handle of the same registration
        // rather than the one prepareStore provisioned from, because the
        // dispatcher a seam reaches cannot exist until the session it serves
        // does, and provisioning runs before there is a session at all.
        auto program = loadGeneration(
            prepared.project,
            ProjectRole::UnderTest,
            dispatcher->toolRuntimeSeam()
        );
        auto catalog = ToolStartCatalog::create(
            deploymentFor(prepared.project, ProjectRole::UnderTest)
                .toolCatalogSchemaOwner
        );
        REQUIRE(catalog.has_value());
        auto const environmentIdentity = program.environmentIdentity();
        return PreparedToolRuntime{
            .observations = std::move(observations),
            .dispatcher   = *std::move(dispatcher),
            .program      = std::move(program),
            .catalog      = *std::move(catalog),
            .execution    = ToolExecutionIdentity{
                .runIdentity                 = hashOf("conformance-run"),
                .frameworkReleaseIdentity    = hashOf("conformance-release"),
                .toolRuntimeProtocolIdentity = hashOf("conformance-protocol"),
                .environmentIdentity         = environmentIdentity,
            },
        };
    }

    auto openActorSession(
        PreparedStore& prepared,
        std::string_view sessionId,
        std::string_view instanceKey,
        ControllerKind kind,
        std::string_view controllerId
    ) -> ActorSession
    {
        auto const& underTest = deploymentFor(
            prepared.project,
            ProjectRole::UnderTest
        );

        // Its own ProjectInstance, because one project instance admits one
        // active write session: two actors sharing a key could not both be
        // pinned, and the exclusion this case is about is the lease rather than
        // the instance. No baseline entry, because what these actors drive is
        // the Tool Runtime rather than a fold.
        REQUIRE(prepared.store.provisionProjectInstance(
            ProjectIdentity{underTest.generation},
            prepared.generation,
            ProjectInstanceBaseline{
                .projectInstanceKey  = std::string{instanceKey},
                .eventId             = "",
                .sessionManifestHash = prepared.manifest.hash(),
                .entry               = std::nullopt,
            }
        ).has_value());

        // Required for exactly the kinds whose ControllerProfile says budgets
        // are required and refused for the others, so the kind decides this
        // rather than this function's caller.
        auto profile = std::optional<AgentProfile>{};
        if (controllerProfile(kind).budgetsRequired)
        {
            auto verified = AgentProfile::verifyExact(
                prepared.manifest,
                "agent-profile.json",
                agentProfileBytes(),
                agentProfileValidator()
            );
            REQUIRE(verified.has_value());
            profile = *std::move(verified);
        }

        auto const worldScope = ObservedInstanceWorldScope::run(
            prepared.controller.controlledTargetId(),
            1
        );
        REQUIRE(worldScope.has_value());
        REQUIRE(prepared.store.pinSession(
            SessionPin{
                .sessionId                 = std::string{sessionId},
                .authenticatedControllerId = std::string{controllerId},
                .idempotencyNamespace      = std::string{controllerId},
                .projectRegistrationHash   = underTest.generation.hash(),
                .controllerCapabilities    = {std::string{k_operateCapability}},
                .controlledTargetId        = prepared.controller.controlledTargetId(),
                .projectInstanceKey        = std::string{instanceKey},
                .mode                      = SessionMode::Write,
                .kind                      = kind,
                .worldScope                = *worldScope,
            },
            prepared.manifest,
            profile
        ).has_value());

        auto controller = prepared.store.bindController(std::string{sessionId});
        REQUIRE(controller.has_value());
        auto lease = prepared.store.acquireLease(*controller);
        REQUIRE(lease.has_value());
        return ActorSession{
            .controller = *std::move(controller),
            .lease      = *std::move(lease),
        };
    }

    auto occurrences(
        std::string_view text,
        std::string_view needle
    ) -> std::size_t
    {
        auto count = std::size_t{0};
        for (
            auto at = text.find(needle);
            at != std::string_view::npos;
            at = text.find(needle, at + needle.size())
        )
        {
            ++count;
        }
        return count;
    }
}
