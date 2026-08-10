#include <operator/controller.hpp>
#include <operator/manifest.hpp>
#include <operator/tool-invocation.hpp>

#include "project-fixture.hpp"

#include <core/error/result.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>

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
        // command could be spelled in -- and an absence has to be asserted
        // against the declaration, because no run of the code can demonstrate a
        // column that is not there.
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
            auto const opening =
                std::string{"CREATE TABLE IF NOT EXISTS "} + std::string{tableName}
                + "(";
            auto const begin = source.find(opening);
            REQUIRE(begin != std::string::npos);
            auto const end = source.find(") STRICT;", begin);
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

        [[nodiscard]]
        auto sessionSpec() -> SessionManifestSpec
        {
            return SessionManifestSpec{
                .hostProtocolSchemaHash       = hashOf("host"),
                .runtimeModelSchemaHash       = hashOf("runtime-schema"),
                .runtimeModelArtifactRootHash = hashOf("runtime-root"),
                .operatorProtocolSchemaHash   = hashOf("operator"),
                .projectRegistrationHash      = hashOf("registration"),
                .policyArtifactHash           = hashOf("policy"),
                .journalEnvelopeSchemaHash    = hashOf("journal"),
                .agentProfileHash             = hashOf("agent"),
            };
        }
    }

    TEST_CASE("schema-product-p01")
    {
        auto const schema     = readSchema("umbraflow-operator-v1.schema.json");
        auto const invocation = definition(schema, "ToolInvocation");
        auto const command    = definition(schema, "CommandRecord");
        auto const operation  = definition(schema, "Operation");
        checkStrictObject(invocation);
        checkStrictObject(command);
        checkStrictObject(operation);
        CHECK(invocation.find("\"snapshot_token\"") != std::string::npos);
        CHECK(invocation.find("authenticated_controller_id") == std::string::npos);
        CHECK(invocation.find("receipt_ref") == std::string::npos);
        CHECK(command.find("\"authenticated_controller_id\"") != std::string::npos);
        CHECK(command.find("\"command_fingerprint\"") != std::string::npos);
        CHECK(operation.find("\"plan_versions\"") != std::string::npos);
        CHECK(operation.find("\"dispatches\"") != std::string::npos);
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

        // A read-only command from the Script, and the identical one from a
        // Human later. Read-only so that the fingerprint comparison is not
        // entangled with the mutation chain the second half of this case is
        // about.
        auto const scriptRead = prepared.store.submitCommand(
            prepared.controller,
            test_support::command(prepared.snapshot, "request-1"),
            test_support::toolInvocation(prepared.project, "observe-1")
        );
        REQUIRE(scriptRead.has_value());

        auto const scriptWrite = prepared.store.submitCommand(
            prepared.controller,
            test_support::command(prepared.snapshot, "request-2"),
            test_support::toolInvocation(prepared.project, "command-1")
        );
        REQUIRE(scriptWrite.has_value());

        // The Script's read-only Operation reaches a terminal disposition
        // through one named signal, before control moves.
        auto const scriptConfirmed = prepared.store.transitionOperation(
            scriptRead->operation.operationId,
            scriptRead->operation.revision,
            OperationSignal::ReadCompleted
        );
        REQUIRE(scriptConfirmed.has_value());
        CHECK(scriptConfirmed->state == OperationState::Confirmed);

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
        auto humanSnapshot = prepared.store.createSnapshot(
            takeover->lease,
            prepared.plugin,
            test_support::observeAgain(prepared)
        );
        REQUIRE(humanSnapshot.has_value());

        auto const humanRead = prepared.store.submitCommand(
            human,
            test_support::command(*humanSnapshot, "request-3"),
            test_support::toolInvocation(prepared.project, "observe-1")
        );
        REQUIRE(humanRead.has_value());

        // One command fingerprint for one command. Who submitted it is not
        // among the hashed bytes, so two kinds naming the identical tool and
        // arguments produce the identical value.
        CHECK(humanRead->commandFingerprint == scriptRead->commandFingerprint);

        // And one state machine: the same signal takes the Human's Operation to
        // the same disposition the Script's reached.
        auto const humanConfirmed = prepared.store.transitionOperation(
            humanRead->operation.operationId,
            humanRead->operation.revision,
            OperationSignal::ReadCompleted
        );
        REQUIRE(humanConfirmed.has_value());
        CHECK(humanConfirmed->state == scriptConfirmed->state);

        // One mutation chain, contended across kinds: the Script still holds a
        // non-terminal mutating Operation on target-1, so the Human's mutating
        // command is refused. The kind is checked rather than only the failure,
        // because the unique index would refuse it too and an Operator that had
        // stopped looking would fail as a database error instead.
        auto const humanWrite = prepared.store.submitCommand(
            human,
            test_support::command(*humanSnapshot, "request-4"),
            test_support::toolInvocation(prepared.project, "command-1")
        );
        REQUIRE_FALSE(humanWrite.has_value());
        CHECK(
            automationErrorKind(humanWrite.error())
            == AutomationErrorKind::ActionRejected
        );

        // The door is one door for reading too: an observing controller is
        // authenticated exactly the same way and is refused control, rather
        // than being refused later at the snapshot it could not have taken.
        auto const observer = test_support::addController(
            prepared,
            ControllerKind::Agent,
            SessionMode::Read,
            "session-3",
            "instance-3",
            "target-3",
            test_support::k_unconstrainedAgentBudget
        );
        auto const denied = prepared.store.acquireLease(observer);
        REQUIRE_FALSE(denied.has_value());
        CHECK(
            automationErrorKind(denied.error()) == AutomationErrorKind::ActionRejected
        );
    }

    TEST_CASE("contract-product-p02")
    {
        // A finding has no column a command could be spelled in, and an
        // Operation has no column a required action could be spelled in. An
        // auditor tells the two apart by which table the row is in, and the two
        // column sets are disjoint by construction rather than by convention.
        auto const findings = tableDeclaration("external_input_findings");
        constexpr auto commandColumns = std::array{
            std::string_view{"tool_name"},
            std::string_view{"tool_version"},
            std::string_view{"canonical_args"},
            std::string_view{"command_fingerprint"},
            std::string_view{"client_request_id"},
            std::string_view{"snapshot_token"},
            std::string_view{"mutating"},
        };
        for (auto const column : commandColumns)
        {
            CHECK(findings.find(column) == std::string::npos);
        }
        CHECK(findings.find("required_action") != std::string::npos);
        CHECK(
            tableDeclaration("operations").find("required_action")
            == std::string::npos
        );

        auto const temporary = test_support::TemporaryDirectory{};
        auto prepared        = test_support::prepareStore(temporary.path());

        auto const inFlight = prepared.store.submitCommand(
            prepared.controller,
            test_support::command(prepared.snapshot, "request-1"),
            test_support::toolInvocation(prepared.project, "command-1")
        );
        REQUIRE(inFlight.has_value());

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
        REQUIRE(first->operationId.has_value());
        CHECK(*first->operationId == inFlight->operation.operationId);
        CHECK(
            first->invalidatedSnapshotRevision == prepared.snapshot.snapshotRevision
        );

        // Two events preceded this finding and both were appended by the
        // Operator itself: the lease prepareStore acquired, and the Operation
        // submitted above. The count is exact rather than a lower bound,
        // because a cursor that skipped either producer would still be
        // monotone and would still read as plausible.
        CHECK(first->detectedAfterCursor == 2U);

        // Every snapshot taken up to the finding is refused afterwards, and a
        // fresh one is not. This is the whole effect a finding is allowed to
        // have: the controller must look again before it acts.
        CHECK_FALSE(prepared.store.submitCommand(
            prepared.controller,
            test_support::command(prepared.snapshot, "request-2"),
            test_support::toolInvocation(prepared.project, "observe-1")
        ).has_value());
        auto const fresh = test_support::freshSnapshot(prepared);
        CHECK(prepared.store.submitCommand(
            prepared.controller,
            test_support::command(fresh, "request-3"),
            test_support::toolInvocation(prepared.project, "observe-1")
        ).has_value());

        // The frozen Operation is in NeedsRevalidation: Revalidated is
        // reachable from that state and from no other, so accepting it is the
        // proof that the finding froze rather than terminated.
        auto const revalidated = prepared.store.transitionOperation(
            inFlight->operation.operationId,
            inFlight->operation.revision + 1U,
            OperationSignal::Revalidated
        );
        REQUIRE(revalidated.has_value());
        CHECK(revalidated->state == OperationState::Proposed);

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

        // Exactly three events since the first finding: the first finding
        // itself, the read-only Operation, and the takeover. Each of the three
        // producers is therefore read back here, which is the only reason the
        // stored column is worth having.
        CHECK(second->detectedAfterCursor == first->detectedAfterCursor + 3U);

        // The revalidated Operation was in flight again, so the second finding
        // froze the same one.
        REQUIRE(second->operationId.has_value());
        CHECK(*second->operationId == inFlight->operation.operationId);
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
            prepared.plugin,
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
            prepared.plugin,
            test_support::observeAgain(prepared)
        );
        REQUIRE(humanSnapshot.has_value());

        auto const privileged = test_support::toolInvocation(
            prepared.project,
            "raw-coordinate-click"
        );
        REQUIRE(privileged.surface() == ToolSurface::Privileged);

        // The identical invocation: accepted for the Human, refused for the
        // online Agent. Minting is the project's authority and says nothing
        // about who may present it, which is why the same value reaches both.
        CHECK(prepared.store.submitCommand(
            human,
            test_support::command(*humanSnapshot, "request-1"),
            privileged
        ).has_value());

        auto const refused = prepared.store.submitCommand(
            agent,
            test_support::command(*agentSnapshot, "request-2"),
            privileged
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(
            automationErrorKind(refused.error())
            == AutomationErrorKind::ActionRejected
        );

        // The Agent is not refused everything: a semantic tool goes through the
        // same call. Without this the refusal above would also pass over an
        // Agent that could submit nothing at all.
        CHECK(prepared.store.submitCommand(
            agent,
            test_support::command(*agentSnapshot, "request-3"),
            test_support::toolInvocation(prepared.project, "observe-1")
        ).has_value());

        // A descriptor that states no surface is Privileged. The catalog below
        // answers for the same registration and simply omits the field, which
        // is the only way to read the default rather than the fixture.
        auto unstated = ProjectToolCatalogSchemaOwner::create(
            prepared.project.registration,
            "catalogue",
            [](std::string_view toolName,
               std::string_view) -> Result<ToolDescriptor>
            {
                if (toolName != "observe-1")
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "the surface-silent catalog names one tool"
                    );
                }
                return ToolDescriptor{
                    .toolVersion = "1",
                    .mutability  = ToolMutability::ReadOnly,
                };
            }
        );
        REQUIRE(unstated.has_value());
        auto const silent = unstated->validate(
            "observe-1",
            test_support::canonical(prepared.project.schemaOwner, "{\"value\":1}")
        );
        REQUIRE(silent.has_value());
        CHECK(silent->surface() == ToolSurface::Privileged);
        CHECK_FALSE(prepared.store.submitCommand(
            agent,
            test_support::command(*agentSnapshot, "request-4"),
            *silent
        ).has_value());
    }

    TEST_CASE("contract-product-p04")
    {
        auto const first = test_support::makeProject(
            "fixture.alpha",
            "plugin-alpha"
        );
        CHECK(
            first.registration.canonicalJcs().find(
                "\"plugin_id\":\"fixture.alpha\""
            ) != std::string::npos
        );

        auto const changedCode = test_support::makeProject(
            "fixture.alpha",
            "plugin-beta"
        );
        CHECK(first.registration.hash() != changedCode.registration.hash());
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
