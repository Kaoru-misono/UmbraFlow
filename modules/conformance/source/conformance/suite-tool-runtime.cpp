// The Tool Runtime, as the thing a consumer's project directory is judged
// against: the admission funnel every actor reaches, the durable call history a
// coordinate addresses, the state one call moves through, and what a restart
// replays out of the ledger.
//
// What these cases add to the wide unit coverage in tests/operator is the
// subject. A unit case drives the Operator against a catalog and a closure that
// file wrote, so it can say what the Operator does; it cannot say anything
// about the project a consumer ships. Every case here drives THIS project's
// Tool Catalog, THIS project's bound handlers and THIS project's argument
// documents through the same admission function and the same dispatcher a
// shipped run uses, so what goes red is a fact about the directory the run was
// pointed at.
//
// Three properties are the consumer's rather than the framework's, and each has
// one case:
//
// - a Tool of this catalog admits identically whichever actor names it, which
//   a descriptor restricting a surface or a capability makes false;
// - a terminal call of this project answers again out of the ledger after a
//   restart, and a call arriving at an occupied coordinate is stopped at the
//   field that diverged; and
// - a handler of this project re-entered after a crash mid-dispatch answers
//   exactly what it answered when it ran uninterrupted, which is false for a
//   handler that depends on anything but its canonical arguments and recorded
//   child answers. This offline harness has no live Framework provider;
//   requests for native work are explicit refusals, never fabricated answers.

#include "suite-support.hpp"

#include <operator/controller.hpp>
#include <operator/ledger.hpp>
#include <operator/project-plugin.hpp>
#include <operator/tool-actor-adapters.hpp>
#include <operator/tool-admission-request.hpp>
#include <operator/tool-descriptor.hpp>
#include <operator/tool-invocation.hpp>
#include <operator/tool-runtime.hpp>

#include <json/value.hpp>

#include <core/error/result.hpp>

#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::operator_runtime::conformance
{
    namespace
    {
        // The Project session prepareStore already pinned, and the two this
        // case opens beside it. Three principals rather than one renamed
        // principal, because the axis under test is who is acting.
        constexpr auto k_scriptPrincipal = std::string_view{"controller-1"};
        constexpr auto k_agentPrincipal  = std::string_view{"controller-agent"};
        constexpr auto k_humanPrincipal  = std::string_view{"controller-human"};

        // These offline replay cases supply no live controller. A composition
        // requiring one must use a fixture-backed provider in its integration
        // test; manufacturing successful world answers here would prove nothing.
        [[nodiscard]]
        auto unavailableFrameworkTool(ToolCallPositionIdentity const& call)
            -> Result<ToolCallCompletion>
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "offline conformance has no Framework provider for " + call.toolName()
            );
        }

        template <typename T>
        [[nodiscard]]
        auto failureText(Result<T> const& result) -> std::string
        {
            return result.has_value()
                ? std::string{}
                : std::string{result.error().message()};
        }

        // The objective every producer states, in the two shapes their
        // transports carry it in. The text is deliberately not canonical: its
        // spacing is exactly what exact canonical form refuses, so a human
        // adapter that passed its bytes through rather than parsing and
        // re-rendering them could not state this objective at all.
        [[nodiscard]]
        auto objectiveValue() -> json::Value
        {
            return json::Value::ofObject({
                {"objective", json::Value::ofString("conformance")},
            });
        }

        constexpr auto k_objectiveText =
            std::string_view{R"( { "objective" : "conformance" } )"};

        [[nodiscard]]
        auto argumentsValue(std::string const& exactJcs) -> json::Value
        {
            auto parsed = json::parse(exactJcs);
            REQUIRE(parsed.has_value());
            return *std::move(parsed);
        }

        // The project's own argument document as a person could have typed it.
        //
        // Padding is the only non-canonical spelling derivable from ANY
        // project's bytes: reordering members or respacing them would need this
        // suite to parse and re-emit a document whose schema is the project's,
        // and a suite that chose a serialization would stop judging the
        // project's. It is enough for what the case is about -- exact canonical
        // form refuses these bytes, so the human adapter has to parse them.
        [[nodiscard]]
        auto argumentsText(std::string const& exactJcs) -> std::string
        {
            return " " + exactJcs + " ";
        }

        // What one producer produced and what came back out of the ledger.
        //
        // The request is kept whole rather than projected, because what a case
        // compares is the value that was admitted. The answer is the payload
        // replayToolCall read back off the durable row rather than the value
        // the handler returned, so two producers agreeing here agree about what
        // was recorded.
        //
        // No in-class initializer for the request: three of its four members
        // have no default state, so a start must come from construction.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
        struct ProducedStart final
        {
            std::string          principal{};
            ControllerKind       kind{ControllerKind::Script};
            ToolAdmissionRequest request;
            ToolCallState        state{ToolCallState::Proposed};
            std::string          answer{};
        };

        [[nodiscard]]
        auto recordStart(
            PreparedToolRuntime& runtime,
            std::string principal,
            ControllerKind kind,
            Result<ToolAdmissionRequest> produced
        ) -> ProducedStart
        {
            REQUIRE_MESSAGE(produced.has_value(), failureText(produced));
            auto const replay = runtime.dispatcher.dispatch(
                runtime.program,
                *produced,
                unavailableFrameworkTool,
                *runtime.observations,
                std::stop_token{}
            );
            REQUIRE_MESSAGE(replay.has_value(), failureText(replay));
            REQUIRE(replay->payload.has_value());
            return ProducedStart{
                .principal = std::move(principal),
                .kind      = kind,
                .request   = *std::move(produced),
                .state     = replay->state,
                .answer    = replay->payload->bytes(),
            };
        }

        // Everything two producers of one Tool call must agree on.
        //
        // The root identity, the parent coordinate and the call identity are
        // absent because they are the address a start hangs from, and two
        // actors address two roots by construction. Every attribute below is
        // one the Operator writes onto the durable position row from this
        // coordinate, so agreeing here is agreeing about the row.
        auto checkSameCall(
            ToolCallPositionIdentity const& left,
            ToolCallPositionIdentity const& right
        ) -> void
        {
            CHECK(left.canonicalArgs() == right.canonicalArgs());
            CHECK(left.canonicalArgsHash() == right.canonicalArgsHash());
            CHECK(left.toolName() == right.toolName());
            CHECK(left.toolVersion() == right.toolVersion());
            CHECK(left.sequence() == right.sequence());
            CHECK(left.observationReference() == right.observationReference());
            CHECK(
                toolEffectComposition(left.provider())
                == toolEffectComposition(right.provider())
            );

            auto const& leftExecution  = left.executionIdentity();
            auto const& rightExecution = right.executionIdentity();
            CHECK(leftExecution.runIdentity == rightExecution.runIdentity);
            CHECK(
                leftExecution.frameworkReleaseIdentity
                == rightExecution.frameworkReleaseIdentity
            );
            CHECK(
                leftExecution.toolRuntimeProtocolIdentity
                == rightExecution.toolRuntimeProtocolIdentity
            );
            CHECK(
                leftExecution.environmentIdentity
                == rightExecution.environmentIdentity
            );
            CHECK(left.descriptor().mutability == right.descriptor().mutability);
            CHECK(left.descriptor().surface == right.descriptor().surface);
            CHECK(
                left.descriptor().idempotency == right.descriptor().idempotency
            );
        }

        // One effect, in a spelling two lists can be compared by. It carries no
        // risk, because risk is the runtime's choice below the bound rather
        // than a value the catalog stated.
        [[nodiscard]]
        auto effectKey(
            std::string const& namespacedType,
            std::string const& scopeKind,
            ContentHash const& payloadSchemaHash
        ) -> std::string
        {
            return namespacedType + '|' + scopeKind + '|'
                + payloadSchemaHash.hex();
        }

        // What a mutating call proposes is the CATALOG's: one effect per bound
        // the descriptor declared, aimed at the target this session holds the
        // lease on, at or below the bound's own maximum risk, with no approval
        // any producer stated. A read-only call proposes none, and that absence
        // is the whole of the difference between the two in a request.
        auto checkProposalIsTheCatalogs(
            ToolAdmissionRequest const& request,
            std::string_view controlledTargetId
        ) -> void
        {
            auto const& descriptor = request.call.descriptor();
            CHECK(
                request.mutation.has_value()
                == (descriptor.mutability == ToolMutability::Mutating)
            );
            if (!request.mutation)
            {
                return;
            }

            auto const& mutation = *request.mutation;
            CHECK(mutation.approvals.empty());

            // Neither list can be empty here, so no guard says they are not:
            // a mutating descriptor declaring no bound proposes nothing, and
            // admission refuses that by name well before this runs, so a
            // non-empty check could never go red.
            auto proposed = std::vector<std::string>{};
            for (auto const& effect : mutation.effects)
            {
                CHECK(effect.scopeKey == controlledTargetId);
                proposed.emplace_back(effectKey(
                    effect.namespacedType,
                    effect.scopeKind,
                    effect.payloadSchemaHash
                ));
            }

            auto declared = std::vector<std::string>{};
            for (auto const& bound : descriptor.effectBounds)
            {
                declared.emplace_back(effectKey(
                    bound.namespacedType,
                    bound.scopeKind,
                    bound.payloadSchemaHash
                ));
            }
            std::ranges::sort(proposed);
            std::ranges::sort(declared);
            CHECK(proposed == declared);
        }

        // What the actor axis may move, and what it may not.
        auto checkOneToolAcrossActors(
            std::vector<ProducedStart> const& starts,
            std::string_view controlledTargetId
        ) -> void
        {
            REQUIRE(starts.size() == 3U);
            auto const& first = starts.front();

            auto principals = std::vector<std::string>{};
            auto kinds      = std::vector<ControllerKind>{};
            auto roots      = std::vector<std::string>{};
            for (auto const& start : starts)
            {
                CHECK(start.request.controller.controllerId() == start.principal);
                CHECK(start.request.controller.kind() == start.kind);

                // Each actor starts at a root; calls its handler makes will
                // have that handler's durable coordinate as their parent.
                CHECK(start.request.isRootPositioned());

                CHECK(start.state == ToolCallState::Confirmed);
                CHECK(start.answer == first.answer);
                checkSameCall(first.request.call, start.request.call);
                checkProposalIsTheCatalogs(start.request, controlledTargetId);

                principals.emplace_back(start.principal);
                kinds.emplace_back(start.kind);
                roots.emplace_back(start.request.root.identity().hex());
            }

            // The axis is real rather than vacuous: three principals, three
            // kinds, three roots. Without this the equalities above would hold
            // just as well over one actor named three times.
            std::ranges::sort(principals);
            std::ranges::sort(roots);
            CHECK(std::ranges::adjacent_find(principals) == principals.end());
            CHECK(std::ranges::adjacent_find(roots) == roots.end());
            CHECK(kinds.at(0) != kinds.at(1));
            CHECK(kinds.at(1) != kinds.at(2));
            CHECK(kinds.at(0) != kinds.at(2));
        }
    } // namespace

    TEST_CASE("this project's Tools admit identically for every actor")
    {
        auto const root = TemporaryDirectory{"tool-actors"};

        auto prepared = prepareStore(root.path());
        auto runtime  = toolRuntimeOver(prepared);

        auto const& words    = prepared.project.underTest.vocabulary;
        auto const arguments = argumentsValue(words.toolArguments);

        auto readOnly = std::vector<ProducedStart>{};
        auto mutating = std::vector<ProducedStart>{};

        // The Project's own automation, on the session prepareStore pinned and
        // the lease it already holds. It goes first for that reason; the target
        // is left unleased between actors, which is what a handover on one
        // controlled target actually is.
        {
            auto adapter    = ProjectAutomationAdapter{};
            auto const acting = ToolActorRun{
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .execution       = runtime.execution,
                .policyAuthority = prepared.policyAuthority,
                .catalog         = runtime.catalog,
            };
            readOnly.emplace_back(recordStart(
                runtime,
                std::string{k_scriptPrincipal},
                ControllerKind::Script,
                adapter.translate(
                    acting,
                    runtime.program.bindingTable(),
                    ProjectAutomationStart{
                        .requestKey    = "actors-read-only",
                        .objective     = objectiveValue(),
                        .entryToolName = words.readOnlyTool,
                        .arguments     = arguments,
                    }
                )
            ));
            mutating.emplace_back(recordStart(
                runtime,
                std::string{k_scriptPrincipal},
                ControllerKind::Script,
                adapter.translate(
                    acting,
                    runtime.program.bindingTable(),
                    ProjectAutomationStart{
                        .requestKey    = "actors-mutating",
                        .objective     = objectiveValue(),
                        .entryToolName = words.mutatingTool,
                        .arguments     = arguments,
                    }
                )
            ));
            REQUIRE(prepared.store.releaseLease(prepared.lease).has_value());
        }

        // An Agent, which is the one kind whose ControllerProfile restricts it
        // to the semantic surface and requires the ceilings a pinned
        // AgentProfile states. A catalog declaring its Tools Privileged reaches
        // this and goes red.
        {
            auto const session = openActorSession(
                prepared,
                "session-agent",
                "instance-agent",
                ControllerKind::Agent,
                k_agentPrincipal
            );
            auto adapter      = AgentToolAdapter{};
            auto const acting = ToolActorRun{
                .controller      = session.controller,
                .lease           = session.lease,
                .execution       = runtime.execution,
                .policyAuthority = prepared.policyAuthority,
                .catalog         = runtime.catalog,
            };
            readOnly.emplace_back(recordStart(
                runtime,
                std::string{k_agentPrincipal},
                ControllerKind::Agent,
                adapter.translate(
                    acting,
                    AgentToolUse{
                        .requestKey = "actors-read-only",
                        .objective  = objectiveValue(),
                        .toolName   = words.readOnlyTool,
                        .arguments  = arguments,
                    }
                )
            ));
            mutating.emplace_back(recordStart(
                runtime,
                std::string{k_agentPrincipal},
                ControllerKind::Agent,
                adapter.translate(
                    acting,
                    AgentToolUse{
                        .requestKey = "actors-mutating",
                        .objective  = objectiveValue(),
                        .toolName   = words.mutatingTool,
                        .arguments  = arguments,
                    }
                )
            ));
            REQUIRE(prepared.store.releaseLease(session.lease).has_value());
        }

        // A person, whose transport delivers text rather than a value. The
        // bytes below are the project's own document with a person's spacing
        // around it, so the coordinate this actor opens is the same one only
        // because the adapter parsed and re-rendered them.
        {
            auto const session = openActorSession(
                prepared,
                "session-human",
                "instance-human",
                ControllerKind::Human,
                k_humanPrincipal
            );
            auto adapter      = HumanToolAdapter{};
            auto const acting = ToolActorRun{
                .controller      = session.controller,
                .lease           = session.lease,
                .execution       = runtime.execution,
                .policyAuthority = prepared.policyAuthority,
                .catalog         = runtime.catalog,
            };
            readOnly.emplace_back(recordStart(
                runtime,
                std::string{k_humanPrincipal},
                ControllerKind::Human,
                adapter.translate(
                    acting,
                    HumanToolCommand{
                        .requestKey    = "actors-read-only",
                        .objectiveText = std::string{k_objectiveText},
                        .toolName      = words.readOnlyTool,
                        .argumentsText = argumentsText(words.toolArguments),
                    }
                )
            ));
            mutating.emplace_back(recordStart(
                runtime,
                std::string{k_humanPrincipal},
                ControllerKind::Human,
                adapter.translate(
                    acting,
                    HumanToolCommand{
                        .requestKey    = "actors-mutating",
                        .objectiveText = std::string{k_objectiveText},
                        .toolName      = words.mutatingTool,
                        .argumentsText = argumentsText(words.toolArguments),
                    }
                )
            ));
            REQUIRE(prepared.store.releaseLease(session.lease).has_value());
        }

        auto const target = prepared.controller.controlledTargetId();
        checkOneToolAcrossActors(readOnly, target);
        checkOneToolAcrossActors(mutating, target);

        // A witness that the mutating half measured something. Every comparison
        // inside checkProposalIsTheCatalogs is under a branch a read-only start
        // does not enter, so without this the mutating run could contribute
        // nothing and every assertion above would still be green.
        //
        // Which of the two Tools is mutating is deliberately NOT asserted here:
        // loadConformanceProject already refuses a directory whose vocabulary
        // names a mutating_tool the catalog carries as read_only, so a check
        // here could never go red.
        CHECK_FALSE(readOnly.front().request.mutation.has_value());
        CHECK(mutating.front().request.mutation.has_value());
    }

    TEST_CASE("a recorded Tool call answers again from the ledger after a restart")
    {
        auto const root     = TemporaryDirectory{"tool-replay"};
        auto recordedRoot   = std::optional<ToolRootRequestIdentity>{};
        auto recordedCall   = std::optional<ToolCallPositionIdentity>{};
        auto recordedAnswer = std::string{};

        {
            auto prepared = prepareStore(root.path());
            auto runtime  = toolRuntimeOver(prepared);

            auto const& words = prepared.project.underTest.vocabulary;

            auto adapter       = ProjectAutomationAdapter{};
            auto const started = adapter.translate(
                ToolActorRun{
                    .controller      = prepared.controller,
                    .lease           = prepared.lease,
                    .execution       = runtime.execution,
                    .policyAuthority = prepared.policyAuthority,
                    .catalog         = runtime.catalog,
                },
                runtime.program.bindingTable(),
                ProjectAutomationStart{
                    .requestKey    = "replay-1",
                    .objective     = objectiveValue(),
                    .entryToolName = words.readOnlyTool,
                    .arguments     = argumentsValue(words.toolArguments),
                }
            );
            REQUIRE_MESSAGE(started.has_value(), failureText(started));

            auto const answered = runtime.dispatcher.dispatch(
                runtime.program,
                *started,
                unavailableFrameworkTool,
                *runtime.observations,
                std::stop_token{}
            );
            REQUIRE_MESSAGE(answered.has_value(), failureText(answered));
            REQUIRE(answered->state == ToolCallState::Confirmed);
            REQUIRE(answered->payload.has_value());
            recordedRoot   = started->root;
            recordedCall   = started->call;
            recordedAnswer = answered->payload->bytes();

            // A second call arriving where that one is recorded is
            // deterministic-replay divergence, and the refusal names the field
            // rather than the address. Only the Tool name moves: the ordinal,
            // the parent, the pinned execution identity, the provider and the
            // catalog digest are the recorded call's own, so nothing earlier in
            // the field-by-field comparison can answer for this.
            auto context = ToolCallIssuingContext::forRoot(
                *recordedRoot,
                runtime.execution
            );
            auto const other = context.issue(toolInvocation(
                prepared.project,
                ProjectRole::UnderTest,
                words.mutatingTool
            ));
            REQUIRE_MESSAGE(other.has_value(), failureText(other));
            REQUIRE(other->sequence() == recordedCall->sequence());
            REQUIRE(other->parentIdentity() == recordedCall->parentIdentity());

            auto const diverged = prepared.store.persistToolCallPosition(
                *recordedRoot,
                *other
            );
            REQUIRE_FALSE(diverged.has_value());
            CHECK_MESSAGE(
                diverged.error().message().contains("tool_name changed"),
                "a divergence must name the attribute that changed, because a "
                "refusal naming only the coordinate leaves a caller unable to "
                "tell which of its own inputs moved"
            );
        }

        // The restart. Nothing in this scope compiles a closure, registers a
        // generation or holds a Luau state, so an answer that comes back here
        // came out of the ledger and no provider ran to produce it.
        auto reopened = reopenStore(root.path());

        auto const replayed = reopened.replayToolCall(*recordedRoot, *recordedCall);
        REQUIRE_MESSAGE(replayed.has_value(), failureText(replayed));
        CHECK(replayed->state == ToolCallState::Confirmed);
        REQUIRE(replayed->payload.has_value());
        CHECK_MESSAGE(
            replayed->payload->bytes() == recordedAnswer,
            "a terminal call must replay the exact recorded result across a "
            "restart"
        );

        // The same stable root key, re-presented with the exact material it was
        // created from, rejoins the run it created rather than minting a
        // second effect tree. That is what a client persisting its key before
        // sending buys, and it is the whole of the retry protocol a consumer
        // writes against.
        auto const rejoined = reopened.persistToolRootRequest(*recordedRoot);
        REQUIRE_MESSAGE(rejoined.has_value(), failureText(rejoined));
        CHECK(rejoined->lookup == ToolIdentityLookup::Existing);
        CHECK(rejoined->rootIdentity == recordedRoot->identity());

        // The same key carrying different material is new intent wearing an old
        // name, and is a conflict rather than a rejoin.
        auto other = CanonicalJson::parseExact(R"({"objective":"another"})");
        REQUIRE(other.has_value());
        auto const conflicting = ToolRootRequestIdentity::create(
            recordedRoot->callerNamespace().value(),
            recordedRoot->requestKey().value(),
            *std::move(other)
        );
        REQUIRE_MESSAGE(conflicting.has_value(), failureText(conflicting));
        REQUIRE(
            conflicting->relationTo(*recordedRoot) == RootRequestRelation::Conflict
        );
        auto const refused = reopened.persistToolRootRequest(*conflicting);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains("different canonical material"));
    }

    TEST_CASE("a Tool call interrupted mid-dispatch re-enters and answers the same")
    {
        auto const root = TemporaryDirectory{"tool-reentry"};

        auto prepared = prepareStore(root.path());
        auto runtime  = toolRuntimeOver(prepared);

        auto const& words = prepared.project.underTest.vocabulary;

        auto adapter      = ProjectAutomationAdapter{};
        auto const acting = ToolActorRun{
            .controller      = prepared.controller,
            .lease           = prepared.lease,
            .execution       = runtime.execution,
            .policyAuthority = prepared.policyAuthority,
            .catalog         = runtime.catalog,
        };
        auto const arguments = argumentsValue(words.toolArguments);

        // What this project's handler answers for these exact arguments, from
        // one uninterrupted dispatch. It is the oracle the re-entry below is
        // compared with, and it is produced by the same boundary rather than
        // written down here, because a literal would be this suite's idea of
        // the project rather than the project's answer.
        auto const completed = adapter.translate(
            acting,
            runtime.program.bindingTable(),
            ProjectAutomationStart{
                .requestKey    = "reentry-complete",
                .objective     = objectiveValue(),
                .entryToolName = words.readOnlyTool,
                .arguments     = arguments,
            }
        );
        REQUIRE_MESSAGE(completed.has_value(), failureText(completed));
        auto const answered = runtime.dispatcher.dispatch(
            runtime.program,
            *completed,
            unavailableFrameworkTool,
            *runtime.observations,
            std::stop_token{}
        );
        REQUIRE_MESSAGE(answered.has_value(), failureText(answered));
        REQUIRE(answered->state == ToolCallState::Confirmed);
        REQUIRE(answered->payload.has_value());
        auto const expected = answered->payload->bytes();

        // The same Tool at a second root, walked to the dispatch boundary one
        // durable state at a time.
        auto const interrupted = adapter.translate(
            acting,
            runtime.program.bindingTable(),
            ProjectAutomationStart{
                .requestKey    = "reentry-interrupted",
                .objective     = objectiveValue(),
                .entryToolName = words.readOnlyTool,
                .arguments     = arguments,
            }
        );
        REQUIRE_MESSAGE(interrupted.has_value(), failureText(interrupted));
        REQUIRE(
            prepared.store.persistToolRootRequest(interrupted->root).has_value()
        );
        REQUIRE(prepared.store.persistToolCallPosition(
            interrupted->root,
            interrupted->call
        ).has_value());

        auto const proposed = prepared.store.replayToolCall(
            interrupted->root,
            interrupted->call
        );
        REQUIRE_MESSAGE(proposed.has_value(), failureText(proposed));
        CHECK(proposed->state == ToolCallState::Proposed);

        auto const admitted = prepared.store.admitToolCall(*interrupted);
        REQUIRE_MESSAGE(admitted.has_value(), failureText(admitted));
        auto const afterAdmission = prepared.store.replayToolCall(
            interrupted->root,
            interrupted->call
        );
        REQUIRE_MESSAGE(afterAdmission.has_value(), failureText(afterAdmission));
        CHECK(afterAdmission->state == ToolCallState::Admitted);

        {
            // The capability to execute, taken and then dropped. That is what
            // an incarnation dying inside its own dispatch leaves behind: a
            // durable dispatching row, a live admission and no answer.
            auto const crossed = prepared.store.beginToolCallDispatch(*admitted);
            REQUIRE_MESSAGE(crossed.has_value(), failureText(crossed));
        }
        auto const afterBoundary = prepared.store.replayToolCall(
            interrupted->root,
            interrupted->call
        );
        REQUIRE_MESSAGE(afterBoundary.has_value(), failureText(afterBoundary));
        CHECK(afterBoundary->state == ToolCallState::Dispatching);

        // A call answered by a bound entry reaches the world only through child
        // Tool calls, so its interrupted dispatch is re-entered rather than
        // classified uncertain: the handler runs again from the top on a fresh
        // issuing context. What it answers is the project's business, and this
        // is where a handler that read a clock, a global or its module state
        // says so.
        auto const reentered = runtime.dispatcher.dispatch(
            runtime.program,
            *interrupted,
            unavailableFrameworkTool,
            *runtime.observations,
            std::stop_token{}
        );
        REQUIRE_MESSAGE(reentered.has_value(), failureText(reentered));
        CHECK(reentered->state == ToolCallState::Confirmed);
        REQUIRE(reentered->payload.has_value());
        CHECK_MESSAGE(
            reentered->payload->bytes() == expected,
            "a bound entry re-entered after a crash must answer exactly what it "
            "answered when it ran uninterrupted, or its run cannot be replayed"
        );

        // And once the row is terminal the same request is answered out of the
        // record. The revision is what says so: a second execution would have
        // written one.
        auto const again = runtime.dispatcher.dispatch(
            runtime.program,
            *interrupted,
            unavailableFrameworkTool,
            *runtime.observations,
            std::stop_token{}
        );
        REQUIRE_MESSAGE(again.has_value(), failureText(again));
        CHECK(again->state == ToolCallState::Confirmed);
        CHECK(again->revision == reentered->revision);
        REQUIRE(again->payload.has_value());
        CHECK(again->payload->bytes() == expected);
    }
}
