#include <operator/controller.hpp>
#include <operator/ledger.hpp>
#include <operator/manifest.hpp>
#include <operator/tool-admission-request.hpp>
#include <operator/tool-invocation.hpp>
#include <operator/tool-runtime.hpp>

#include "project-fixture.hpp"
#include "tool-call-fixture.hpp"

#include <core/error/result.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::operator_runtime
{
    namespace
    {
        [[nodiscard]]
        auto repositoryRoot() -> std::filesystem::path
        {
            auto source = std::filesystem::path{__FILE__};
            if (source.is_relative())
            {
                source = std::filesystem::absolute(source);
            }
            auto candidate = source.parent_path().parent_path().parent_path();
            if (std::filesystem::is_directory(candidate / "schema"))
            {
                return candidate;
            }

            candidate = std::filesystem::current_path();
            while (!candidate.empty())
            {
                if (std::filesystem::is_directory(candidate / "schema"))
                {
                    return candidate;
                }
                auto const parent = candidate.parent_path();
                if (parent == candidate)
                {
                    break;
                }
                candidate = parent;
            }

            FAIL("repository root containing schema/ was not found");
            return {};
        }

        [[nodiscard]]
        auto readSchema(std::string_view filename) -> std::string
        {
            auto stream = std::ifstream{
                repositoryRoot() / "schema" / filename,
                std::ios::binary,
            };
            REQUIRE(stream.good());
            return {
                std::istreambuf_iterator<char>{stream},
                std::istreambuf_iterator<char>{},
            };
        }

        // The stored DDL of one table, read out of the source that creates it.
        // p02's guarantee is an ABSENCE -- the finding table has no column a
        // Tool call could be spelled in -- and an absence has to be asserted
        // against the declaration, because no run of the code can demonstrate a
        // column that is not there.
        //
        // Two spellings of CREATE TABLE live in that source: the migrating
        // schema states IF NOT EXISTS, and the Tool Runtime's tables are
        // constexpr DDL strings that do not. The LAST declaration of a name is
        // the one a fresh production root is created from, because a
        // migration's rebuild of a table is written above the block that
        // creates it. The terminator carries no semicolon for the same reason:
        // a DDL string that is one statement of a C++ literal ends at STRICT
        // with nothing after it.
        [[nodiscard]]
        auto tableDeclaration(std::string_view tableName) -> std::string
        {
            auto stream = std::ifstream{
                repositoryRoot() / "modules" / "operator" / "source" / "operator"
                    / "ledger.cpp",
                std::ios::binary,
            };
            REQUIRE(stream.good());
            auto const source = std::string{
                std::istreambuf_iterator<char>{stream},
                std::istreambuf_iterator<char>{},
            };
            auto const named = std::string{tableName} + "(";
            auto begin = source.rfind("CREATE TABLE IF NOT EXISTS " + named);
            if (begin == std::string::npos)
            {
                begin = source.rfind("CREATE TABLE " + named);
            }
            REQUIRE(begin != std::string::npos);
            auto const end = source.find(") STRICT", begin);
            REQUIRE(end != std::string::npos);
            return source.substr(begin, end - begin);
        }

        [[nodiscard]]
        auto definition(
            std::string const& schema,
            std::string_view name
        ) -> std::string
        {
            auto const declaration  = std::string{"\""} + std::string{name} + "\"";
            auto const namePosition = schema.find(declaration);
            REQUIRE(namePosition != std::string::npos);
            auto const begin = schema.find('{', namePosition + declaration.size());
            REQUIRE(begin != std::string::npos);

            auto depth    = std::size_t{};
            auto inString = false;
            auto escaped  = false;
            for (auto index = begin; index < schema.size(); ++index)
            {
                auto const character = schema[index];
                if (inString)
                {
                    if (escaped)
                    {
                        escaped = false;
                    }
                    else if (character == '\\')
                    {
                        escaped = true;
                    }
                    else if (character == '"')
                    {
                        inString = false;
                    }
                    continue;
                }
                if (character == '"')
                {
                    inString = true;
                }
                else if (character == '{')
                {
                    ++depth;
                }
                else if (character == '}')
                {
                    REQUIRE(depth > 0);
                    --depth;
                    if (depth == 0)
                    {
                        return schema.substr(begin, index - begin + 1);
                    }
                }
            }

            FAIL("schema definition has no closing object delimiter");
            return {};
        }

        auto checkStrictObject(std::string const& value) -> void
        {
            CHECK(value.find("\"type\": \"object\"") != std::string::npos);
            CHECK(value.find("\"additionalProperties\": false") != std::string::npos);
            CHECK(value.find("\"required\": [") != std::string::npos);
            CHECK(value.find("\"properties\": {") != std::string::npos);
        }

        [[nodiscard]]
        auto hashOf(std::string_view value) -> ContentHash
        {
            auto const hash = sha256(std::as_bytes(std::span{value}));
            REQUIRE(hash.has_value());
            return *hash;
        }

        // One Tool call presented straight at the admission door, for the cases
        // whose property IS the answer that comes back.
        // test_support::startToolCall requires success, so a refusal has to be
        // built here; what it builds is the request that helper builds, so an
        // accepted and a refused call below differ in who presents what and
        // never in the shape of the request.
        //
        // prepared is mutated because admitting a call is a durable write to
        // its store, which is this function's whole operation.
        [[nodiscard]]
        auto admitToolCallDirectly(
            test_support::PreparedStore& prepared,
            ControllerBinding const& controller,
            ControlLease const& lease,
            std::string_view requestKey,
            ValidatedToolInvocation const& invocation,
            std::optional<ToolAdmissionRequest::Mutation> mutation = std::nullopt
        ) -> Result<ToolCallAdmission>
        {
            auto preimage = CanonicalJson::parseExact(
                R"({"objective":"fixture-tool-call"})"
            );
            REQUIRE(preimage.has_value());
            auto root = ToolRootRequestIdentity::create(
                std::string{controller.controllerId()},
                std::string{requestKey},
                *std::move(preimage)
            );
            REQUIRE(root.has_value());
            auto const execution = ToolExecutionIdentity{
                .runIdentity                 = prepared.manifest.hash(),
                .frameworkReleaseIdentity    = prepared.runtimeArtifactRootHash,
                .toolRuntimeProtocolIdentity = hashOf("fixture-tool-protocol"),
                .environmentIdentity         = hashOf("fixture-tool-environment"),
            };
            auto call = test_support::toolCallAt(
                *root,
                nullptr,
                1U,
                execution,
                invocation
            );
            REQUIRE(call.has_value());
            return prepared.store.admitToolCall(ToolAdmissionRequest{
                .controller = controller,
                .lease      = lease,
                .root       = *root,
                .call       = *call,
                .mutation   = std::move(mutation),
            });
        }

        [[nodiscard]]
        auto sessionSpec() -> SessionManifestSpec
        {
            return SessionManifestSpec{
                .runtimeModelArtifactRootHash = hashOf("runtime-root"),
                .operatorProtocolSchemaHash   = hashOf("operator"),
                .projectRegistrationHash      = hashOf("registration"),
                .policyArtifactHash           = hashOf("policy"),
                .agentProfileHash             = hashOf("agent"),
            };
        }

        // The registrar is the caller's, because the whole point below is that
        // two projects share one, and test_support::loadGeneration keeps its
        // own.
        [[nodiscard]]
        auto loadOn(
            ProjectGenerationRegistrar& registrar,
            test_support::ProjectFixture const& project
        ) -> Result<ProjectGenerationHandle>
        {
            return registrar.registerGeneration(
                project.generation,
                project.toolCatalogSchemaOwner,
                ProjectGenerationRegistrar::ClosureModules{
                    .entryModule = "main",
                    .modules     = test_support::closureModules(
                        test_support::toolClosureSource(
                            project.registration.pluginId()
                        )
                    ),
                },
                {},
                test_support::refusingToolRuntime()
            );
        }

        // What a loaded program answers for, and what it refuses. A Tool name
        // is namespaced by the registration that owns it, so the neighbour's
        // name is one this program can never have bound -- which is the whole
        // of "each program answers its own contract" once nothing folds.
        auto checkAnswersOwnTools(
            ProjectGenerationHandle const& program,
            test_support::ProjectFixture const& project,
            std::string_view foreignToolName
        ) -> void
        {
            CHECK(program.pluginId() == project.registration.pluginId());
            CHECK(
                program.projectRegistrationHash() == project.registration.hash()
            );

            auto const own = program.invokeBoundTool(
                project.toolName("command-1"),
                json::Value::ofObject({}),
                script::ScopedRunRequest{
                    .parentPosition = hashOf("product-p05-position"),
                }
            );
            REQUIRE(own.has_value());

            auto const foreign = program.invokeBoundTool(
                foreignToolName,
                json::Value::ofObject({}),
                script::ScopedRunRequest{
                    .parentPosition = hashOf("product-p05-position"),
                }
            );
            REQUIRE_FALSE(foreign.has_value());
            CHECK(foreign.error().message().contains("binds no Tool named"));
        }
    }

    // Two Project registrations of the same shape, deployed side by side on one
    // registrar, each answering its own contract. That is what P-05 pins: a
    // harness carrying real consumers rather than one, so that everything the
    // Operator keys on a registration is shown to be keyed on THIS one.
    //
    // Retargeted onto the one-closure generation, with what it proves
    // unchanged. What moved is the vehicle: nothing folds any more, so "each
    // program answers its own contract" is each program's own bound Tool names
    // rather than each project's own document.
    TEST_CASE("contract-product-p05-fixtures")
    {
        auto const catalogue = test_support::makeProject("fixture.catalogue");
        auto const workflow  = test_support::makeProject("fixture.workflow");
        CHECK(catalogue.registration.hash() != workflow.registration.hash());

        auto       registrar        = ProjectGenerationRegistrar{};
        auto const catalogueProgram = loadOn(registrar, catalogue);
        auto const workflowProgram  = loadOn(registrar, workflow);
        REQUIRE(catalogueProgram.has_value());
        REQUIRE(workflowProgram.has_value());

        // Each program answers its own project's Tools and refuses the
        // neighbour's: two loaded programs, not one consulted twice.
        checkAnswersOwnTools(
            *catalogueProgram,
            catalogue,
            workflow.toolName("command-1")
        );
        checkAnswersOwnTools(
            *workflowProgram,
            workflow,
            catalogue.toolName("command-1")
        );

        // A registry entry is (plugin id, registration root) and nothing less.
        REQUIRE(registrar
                    .findExact(
                        "fixture.catalogue",
                        catalogue.registration.hash()
                    )
                    .has_value());
        CHECK_FALSE(registrar
                        .findExact(
                            "fixture.catalogue",
                            workflow.registration.hash()
                        )
                        .has_value());
        CHECK_FALSE(registrar
                        .findExact(
                            "fixture.workflow",
                            catalogue.registration.hash()
                        )
                        .has_value());

        // A registration root is a generation, so the same root cannot be
        // loaded twice under two different closures.
        CHECK_FALSE(loadOn(registrar, catalogue).has_value());
    }

    TEST_CASE("schema-product-p01")
    {
        auto const schema     = readSchema("umbraflow-operator-v1.schema.json");
        auto const invocation = definition(schema, "ToolInvocation");
        auto const session    = definition(schema, "OperatorSession");
        checkStrictObject(invocation);
        checkStrictObject(session);
        CHECK(invocation.find("\"snapshot_token\"") != std::string::npos);
        CHECK(invocation.find("authenticated_controller_id") == std::string::npos);
        CHECK(invocation.find("receipt_ref") == std::string::npos);

        // Who is asking is the session's to state and never the invocation's.
        // Read here so that the absence above is a fact about where identity
        // lives, rather than a name this schema happens to use nowhere at all.
        CHECK(session.find("\"authenticated_controller_id\"") != std::string::npos);
    }

    TEST_CASE("schema-product-p02")
    {
        auto const schema        = readSchema("umbraflow-operator-v1.schema.json");
        auto const transition    = definition(schema, "ControlTransition");
        auto const externalInput = definition(schema, "ExternalInputFinding");
        checkStrictObject(transition);
        checkStrictObject(externalInput);
        CHECK(transition.find("\"takeover\"") != std::string::npos);
        CHECK(transition.find("\"fencing_token\"") != std::string::npos);
        CHECK(externalInput.find("\"freeze_and_reobserve\"") != std::string::npos);
        CHECK(externalInput.find("\"freeze_and_reconcile\"") != std::string::npos);
    }

    TEST_CASE("schema-product-p03")
    {
        auto const schema     = readSchema("umbraflow-operator-v1.schema.json");
        auto const capability = definition(schema, "ControllerCapability");
        checkStrictObject(capability);
        CHECK(capability.find("\"allowed_tools\"") != std::string::npos);
        CHECK(capability.find("\"allowed_effect_types\"") != std::string::npos);
        CHECK(capability.find("\"takeover\"") != std::string::npos);
        CHECK(capability.find("receipt") == std::string::npos);
        CHECK(capability.find("coordinate") == std::string::npos);
        CHECK(capability.find("native_input") == std::string::npos);
    }

    TEST_CASE("contract-product-p01")
    {
        auto const temporary = test_support::TemporaryDirectory{};
        auto prepared        = test_support::prepareStore(temporary.path());
        REQUIRE(prepared.controller.kind() == ControllerKind::Script);

        // A read-only Tool call from the Script, and the same tool from a Human
        // below. Read-only so that neither of them holds the target's mutation
        // barrier, which the second half of this case is about.
        auto const scriptRead = test_support::startToolCall(
            prepared,
            "request-1",
            prepared.project.toolName("observe-1")
        );

        // The Script's mutating call, left dispatching. It is the claim on
        // target-1 that the Human's mutating call contends with at the end.
        auto const scriptWrite = test_support::startToolCall(
            prepared,
            "request-2",
            prepared.project.toolName("command-1")
        );

        // The Script's read-only call reaches a terminal disposition through
        // the one completion door, before control moves.
        test_support::confirmToolCall(prepared, scriptRead);

        // A Human on its own ProjectInstance and the same controlled target.
        auto const human = test_support::addController(
            prepared,
            ControllerKind::Human,
            SessionMode::Write,
            "session-2",
            "instance-2",
            "target-1"
        );
        auto const takeover = prepared.store.takeoverLease(human, "a human sat down");
        REQUIRE(takeover.has_value());

        // One shared door: startToolCall is admitToolCall, and a Human reaches
        // it with the request a Script reaches it with. There is no
        // kind-specific entry point for either to have taken instead.
        //
        // The two calls are deliberately NOT compared for identity. A call
        // coordinate is minted per root request and position, so two calls are
        // two coordinates by construction; asserting an equality here would
        // assert the opposite of what the Tool Runtime guarantees. What is
        // shared is the door and the state machine behind it, and that is what
        // the lines around this one read.
        auto const humanRead = test_support::startToolCall(
            prepared,
            human,
            takeover->lease,
            "request-3",
            prepared.project.toolName("observe-1")
        );

        // The same completion takes the Human's call to the terminal the
        // Script's reached; confirmToolCall asserts that terminal for both.
        test_support::confirmToolCall(prepared, humanRead);

        // One mutation chain, contended across kinds: the Script still holds a
        // dispatching mutating call on target-1, so the Human's mutating call
        // is refused. The refusal names the barring call rather than only
        // failing, because a dead lease or a duplicated coordinate would refuse
        // this too and an Operator that had stopped looking at the barrier
        // would still produce a failure here.
        auto const humanEffects = std::vector{
            test_support::routineToolEffect(prepared.project),
        };
        auto const humanWrite = admitToolCallDirectly(
            prepared,
            human,
            takeover->lease,
            "request-4",
            test_support::toolInvocation(
                prepared.project,
                prepared.project.toolName("command-1")
            ),
            ToolAdmissionRequest::Mutation{
                .policyAuthority = prepared.policyAuthority,
                .effects         = humanEffects,
            }
        );
        REQUIRE_FALSE(humanWrite.has_value());
        CHECK(
            automationErrorKind(humanWrite.error())
            == AutomationErrorKind::ActionRejected
        );
        CHECK(humanWrite.error().message().contains(
            scriptWrite.call.identity().hex()
        ));

        REQUIRE(prepared.store.provisionProjectInstance(
            prepared.project.registration,
            "instance-3"
        ).has_value());
        auto const pinnedAgent = test_support::agentProfileFor(
            prepared,
            test_support::k_unconstrainedAgentBudget
        );
        auto const worldScope = operator_runtime::ObservedInstanceWorldScope::run(
            "target-3",
            1
        );
        REQUIRE(worldScope.has_value());
        REQUIRE(prepared.store.pinSession(
            SessionPin{
                .sessionId                 = "session-3",
                .authenticatedControllerId = "controller-1",
                .idempotencyNamespace      = "controller-1",
                .projectRegistrationHash   = prepared.project.registration.hash(),
                .controllerCapabilities = {
                    std::string{conformance::k_operateCapability},
                },
                .controlledTargetId = "target-3",
                .projectInstanceKey = "instance-3",
                .mode               = SessionMode::Read,
                .kind               = ControllerKind::Agent,
                .worldScope         = *worldScope,
            },
            pinnedAgent.manifest,
            pinnedAgent.profile
        ).has_value());
        auto const denied = prepared.store.bindController("session-3");
        CHECK_MESSAGE(
            !denied.has_value(),
            "a read-mode Agent must be refused at controller binding"
        );
        REQUIRE_FALSE(denied.has_value());
        CHECK(
            automationErrorKind(denied.error()) == AutomationErrorKind::ActionRejected
        );
    }

    TEST_CASE("contract-product-p02")
    {
        // A finding has no column a Tool call could be spelled in, and no Tool
        // call row has a column a required action could be spelled in. An
        // auditor tells the two apart by which table the row is in, and the two
        // column sets are disjoint by construction rather than by convention.
        auto const findings  = tableDeclaration("external_input_findings");
        auto const positions = tableDeclaration("tool_call_positions");
        auto const history   = tableDeclaration("tool_call_history");
        constexpr auto toolCallColumns = std::array{
            std::string_view{"call_identity"},
            std::string_view{"root_identity"},
            std::string_view{"tool_name"},
            std::string_view{"tool_version"},
            std::string_view{"canonical_args"},
            std::string_view{"observation_reference_hash"},
            std::string_view{"mutating"},
        };
        for (auto const column : toolCallColumns)
        {
            // Present on one of the call's own tables first, so the absence
            // beside it is a disjointness rather than a misspelled name that
            // nothing anywhere would have matched.
            CHECK((positions.contains(column) || history.contains(column)));
            CHECK_FALSE(findings.contains(column));
        }
        CHECK(findings.contains("required_action"));
        CHECK_FALSE(positions.contains("required_action"));
        CHECK_FALSE(history.contains("required_action"));

        auto const temporary = test_support::TemporaryDirectory{};
        auto prepared        = test_support::prepareStore(temporary.path());

        // A mutating Tool call left dispatching, so both findings below are
        // recorded while automation is genuinely mid-flight rather than over an
        // idle target.
        auto const inFlight = test_support::startToolCall(
            prepared,
            "request-1",
            prepared.project.toolName("command-1")
        );

        // A Script asserting that a human typed would be fabricating evidence
        // about a third party.
        auto const scriptReport = prepared.store.recordExternalInput(
            prepared.controller,
            ExternalInputReport{
                .requiredAction = ExternalInputAction::FreezeAndReobserve,
                .reason         = "a script claiming to be a human",
            }
        );
        REQUIRE_FALSE(scriptReport.has_value());
        CHECK(
            automationErrorKind(scriptReport.error())
            == AutomationErrorKind::ActionRejected
        );

        auto const human = test_support::addController(
            prepared,
            ControllerKind::Human,
            SessionMode::Write,
            "session-2",
            "instance-2",
            "target-1"
        );
        auto const first = prepared.store.recordExternalInput(
            human,
            ExternalInputReport{
                .requiredAction = ExternalInputAction::FreezeAndReobserve,
                .reason         = "a key was pressed on the controlled window",
            }
        );
        REQUIRE(first.has_value());

        // The revision every controller on this target must observe past before
        // it acts again, read back from the stored row rather than from what
        // the reporter said.
        CHECK(
            first->invalidatedSnapshotRevision == prepared.snapshot.snapshotRevision
        );

        // One event preceded this finding and the Operator itself appended it:
        // the lease prepareStore acquired. The count is exact rather than a
        // lower bound, because a cursor that skipped a producer would still be
        // monotone and would still read as plausible. A Tool call appends
        // nothing here -- this stream carries control movements and findings,
        // and a call's own states live in its durable history instead.
        CHECK(first->detectedAfterCursor == 1U);

        // A finding reports that the world moved; it does not end what was
        // running. The mutating call is still dispatching afterwards, and is
        // therefore still answerable and still holding its target's mutation
        // barrier -- which is the whole of the difference between a report and
        // a termination.
        auto const afterFinding = prepared.store.replayToolCall(
            inFlight.root,
            inFlight.call
        );
        REQUIRE(afterFinding.has_value());
        CHECK(afterFinding->state == ToolCallState::Dispatching);

        // Authorised human control is a lease movement rather than a finding,
        // and it is on the same ordered stream: an Agent has to be able to see
        // that a human intervened.
        REQUIRE(prepared.store.takeoverLease(human, "a human sat down").has_value());

        auto const second = prepared.store.recordExternalInput(
            human,
            ExternalInputReport{
                .requiredAction = ExternalInputAction::FreezeAndReconcile,
                .reason         = "and the window moved",
            }
        );
        REQUIRE(second.has_value());

        // Exactly two events since the first detection point: that finding's
        // own event, and the takeover.
        CHECK(second->detectedAfterCursor == first->detectedAfterCursor + 2U);
    }

    TEST_CASE("contract-product-p03")
    {
        // The rule, stated once and read here in both directions.
        CHECK_FALSE(toolSurfaceAllowed(
            controllerProfile(ControllerKind::Agent),
            ToolSurface::Privileged
        ));
        CHECK(toolSurfaceAllowed(
            controllerProfile(ControllerKind::Agent),
            ToolSurface::Semantic
        ));
        CHECK(toolSurfaceAllowed(
            controllerProfile(ControllerKind::Human),
            ToolSurface::Privileged
        ));
        CHECK(toolSurfaceAllowed(
            controllerProfile(ControllerKind::Script),
            ToolSurface::Privileged
        ));

        auto const temporary = test_support::TemporaryDirectory{};
        auto prepared        = test_support::prepareStore(temporary.path());

        // The four catalog names this case reads off a snapshot and presents at
        // admission. A fixture tool's namespace is its registration's
        // plugin_id, so they are composed from the project prepareStore
        // registered rather than respelled per site.
        auto const rawCoordinateTool = prepared.project.toolName("raw-coordinate-click");
        auto const gatedTool         = prepared.project.toolName("capability-gated");
        auto const observeTool       = prepared.project.toolName("observe-1");
        auto const commandTool       = prepared.project.toolName("command-1");

        auto const agent = test_support::addController(
            prepared,
            ControllerKind::Agent,
            SessionMode::Write,
            "session-3",
            "instance-3",
            "target-3",
            test_support::k_unconstrainedAgentBudget
        );
        auto const agentLease = prepared.store.acquireLease(agent);
        REQUIRE(agentLease.has_value());
        auto const agentSnapshot = prepared.store.createSnapshot(
            *agentLease,
            prepared.project.registration,
            prepared.project.toolCatalogSchemaOwner,
            prepared.project.observedInstanceIdentitySchemas,
            test_support::observeAgain(prepared)
        );
        REQUIRE(agentSnapshot.has_value());

        auto const human = test_support::addController(
            prepared,
            ControllerKind::Human,
            SessionMode::Write,
            "session-2",
            "instance-2",
            "target-2"
        );
        auto const humanLease = prepared.store.acquireLease(human);
        REQUIRE(humanLease.has_value());
        auto const humanSnapshot = prepared.store.createSnapshot(
            *humanLease,
            prepared.project.registration,
            prepared.project.toolCatalogSchemaOwner,
            prepared.project.observedInstanceIdentitySchemas,
            test_support::observeAgain(prepared)
        );
        REQUIRE(humanSnapshot.has_value());

        // The offer side. It is asserted before any admission below, and it is
        // a different failure from the refusals that follow: an Agent handed a
        // list naming raw-coordinate-click has already learned the machine
        // surface exists, whatever happens when it tries to use it. A test that
        // only presented calls would pass with no offer side at all. The
        // composition of each snapshot is the only mint of the offered set, so
        // the set is read off the snapshots above.
        auto const names = [](std::vector<OfferedTool> const& offered)
        {
            auto listed = std::vector<std::string>{};
            listed.reserve(offered.size());
            for (auto const& tool : offered)
            {
                listed.emplace_back(tool.name);
            }
            return listed;
        };
        auto const agentNames = names(agentSnapshot->availableTools);
        CHECK_FALSE(std::ranges::contains(agentNames, rawCoordinateTool));

        // The offered set is not empty of everything, so the absence above is
        // this tool's and not the whole catalog's.
        CHECK(std::ranges::contains(agentNames, observeTool));
        CHECK(std::ranges::contains(agentNames, commandTool));

        // A tool whose required_capabilities this session does not hold is
        // absent for a reason that is not its surface, so the two halves of the
        // derivation are told apart.
        CHECK_FALSE(std::ranges::contains(agentNames, gatedTool));

        // The same catalog, the same composition, a controller whose profile
        // is not restricted: the privileged tool is present. Without this the
        // check above would pass over a derivation that offered nothing to
        // anybody.
        auto const humanNames = names(humanSnapshot->availableTools);
        CHECK(std::ranges::contains(humanNames, rawCoordinateTool));
        CHECK_FALSE(std::ranges::contains(humanNames, gatedTool));

        auto const capabilityGated = test_support::toolInvocation(
            prepared.project,
            gatedTool
        );
        REQUIRE(capabilityGated.descriptor().surface == ToolSurface::Semantic);
        REQUIRE(
            capabilityGated.descriptor().requiredCapabilities
            == std::vector<std::string>{"authoring"}
        );
        auto const capabilityRefused = admitToolCallDirectly(
            prepared,
            agent,
            *agentLease,
            "request-capability-gated",
            capabilityGated
        );
        REQUIRE_MESSAGE(
            !capabilityRefused.has_value(),
            "admitToolCall must refuse fixture.control.capability-gated without authoring"
        );
        CHECK_MESSAGE(
            capabilityRefused.error().message().contains("authoring"),
            "the admission refusal must name the missing capability"
        );

        auto const privileged = test_support::toolInvocation(
            prepared.project,
            rawCoordinateTool
        );
        REQUIRE(privileged.descriptor().surface == ToolSurface::Privileged);

        // The identical invocation: admitted for the Human, refused for the
        // online Agent. Minting is the project's authority and says nothing
        // about who may present it, which is why the same value reaches both.
        CHECK(admitToolCallDirectly(
            prepared,
            human,
            *humanLease,
            "request-1",
            privileged
        ).has_value());

        auto const refused = admitToolCallDirectly(
            prepared,
            agent,
            *agentLease,
            "request-2",
            privileged
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(
            automationErrorKind(refused.error())
            == AutomationErrorKind::ActionRejected
        );

        // The Agent is not refused everything: a semantic tool goes through the
        // same door. Without this the refusal above would also pass over an
        // Agent that could be admitted to nothing at all.
        CHECK(admitToolCallDirectly(
            prepared,
            agent,
            *agentLease,
            "request-3",
            test_support::toolInvocation(prepared.project, observeTool)
        ).has_value());

        // A descriptor that states no surface is Privileged. The catalog below
        // answers for the same registration and simply omits the field, which
        // is the only way to read the default rather than the fixture.
        auto unstated = ProjectToolCatalogSchemaOwner::create(
            prepared.project.registration,
            prepared.project.toolCatalogBytes,
            []() -> Result<std::vector<ToolCatalogEntry>>
            {
                return std::vector<ToolCatalogEntry>{
                    ToolCatalogEntry{
                        .name       = "fixture.control.observe-1",
                        .descriptor = ToolDescriptor{
                            .toolVersion = "1",
                            .mutability  = ToolMutability::ReadOnly,
                        },
                    },
                };
            },
            [](std::string_view, std::string_view) -> Status { return ok(); }
        );
        REQUIRE(unstated.has_value());
        auto const silent = unstated->validate(
            observeTool,
            test_support::canonical("{\"value\":1}")
        );
        REQUIRE(silent.has_value());
        CHECK(silent->descriptor().surface == ToolSurface::Privileged);
        CHECK_FALSE(admitToolCallDirectly(
            prepared,
            agent,
            *agentLease,
            "request-4",
            *silent
        ).has_value());
    }

    // P-04: the registration root names what a project was deployed as, so
    // moving anything the document pins moves the root. The declaration is
    // what moves here rather than the closure, because the closure a fixture
    // ships is derived from its plugin id and two ids would move the root for
    // a second reason.
    TEST_CASE("contract-product-p04")
    {
        auto const first = test_support::makeProject("fixture.alpha");
        CHECK(
            first.registration.canonicalJcs().find(
                "\"plugin_id\":\"fixture.alpha\""
            ) != std::string::npos
        );

        auto const changedDeclaration = test_support::makeProject(
            "fixture.alpha",
            test_support::k_toolArgumentSchemaWithInstanceIds
        );
        CHECK(
            first.registration.hash() != changedDeclaration.registration.hash()
        );

        // And the environment it runs under, which is inside the root for the
        // same reason: a registration admitted under one environment is not the
        // same deployment as the same bytes admitted under another.
        auto const changedEnvironment = test_support::makeProject(
            "fixture.alpha",
            test_support::k_toolArgumentSchema,
            test_support::hashOf("another-plugin-environment")
        );
        CHECK(
            first.registration.hash() != changedEnvironment.registration.hash()
        );
    }

    TEST_CASE("contract-product-p06")
    {
        auto const schema  = readSchema("umbraflow-operator-v1.schema.json");
        auto const session = definition(schema, "OperatorSession");
        checkStrictObject(session);
        CHECK(session.find("\"project_instance_key\"") != std::string::npos);
        CHECK(session.find("\"project_registration_hash\"") != std::string::npos);
        CHECK(session.find("\"session_epoch\"") != std::string::npos);

        auto firstSpec   = sessionSpec();
        auto const first = SessionManifest::create(firstSpec);
        REQUIRE(first.has_value());
        firstSpec.policyArtifactHash = hashOf("other-policy");
        auto const changedPolicy = SessionManifest::create(firstSpec);
        REQUIRE(changedPolicy.has_value());
        CHECK(first->hash() != changedPolicy->hash());
    }
}
