#include <operator/project-tool-dispatch.hpp>
#include <operator/tool-admission-request.hpp>
#include <operator/tool-invocation.hpp>
#include <operator/tool-runtime.hpp>
#include <operator/tool-root-producer.hpp>

#include <json/value.hpp>

#include "project-fixture.hpp"

#include <doctest/doctest.h>

#include <map>
#include <memory>
#include <stop_token>
#include <string>
#include <utility>

namespace uf::operator_runtime
{
    namespace
    {
        using test_support::TemporaryDirectory;
        using test_support::hashOf;

        template <typename Value>
        [[nodiscard]] auto why(Result<Value> const& value) -> std::string
        {
            return value.has_value()
                ? std::string{}
                : std::string{value.error().message()};
        }

        [[nodiscard]] auto replayPayload(ToolCallReplay const& replay) -> std::string
        {
            return replay.payload ? replay.payload->bytes() : std::string{};
        }

        struct PositionedCall final
        {
            ToolRootRequestIdentity  root;
            ToolCallPositionIdentity call;
        };

        [[nodiscard]]
        auto positionedCall(
            test_support::PreparedStore const& prepared,
            std::string_view requestKey,
            std::string_view localTool = "observe-1"
        ) -> PositionedCall
        {
            auto preimage = CanonicalJson::parseExact(
                R"({"objective":"dispatch a Project Tool"})"
            );
            REQUIRE(preimage.has_value());
            auto root = ToolRootRequestIdentity::create(
                std::string{prepared.controller.controllerId()},
                std::string{requestKey},
                *std::move(preimage)
            );
            REQUIRE(root.has_value());

            auto invocation = test_support::toolInvocation(
                prepared.project,
                prepared.project.toolName(localTool)
            );
            auto context = ToolCallIssuingContext::forRoot(
                *root,
                ToolExecutionIdentity{
                    .runIdentity                 = prepared.manifest.hash(),
                    .frameworkReleaseIdentity    = prepared.runtimeArtifactRootHash,
                    .toolRuntimeProtocolIdentity = hashOf("fixture-tool-protocol"),
                    .environmentIdentity         = hashOf("fixture-tool-environment"),
                }
            );
            auto call = context.issue(invocation);
            REQUIRE_MESSAGE(call.has_value(), why(call));
            return PositionedCall{
                .root = *std::move(root),
                .call = *std::move(call),
            };
        }

        [[nodiscard]]
        auto requestFor(
            test_support::PreparedStore const& prepared,
            PositionedCall const& positioned
        ) -> ToolAdmissionRequest
        {
            return ToolAdmissionRequest{
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = positioned.root,
                .call            = positioned.call,
                .policyAuthority = prepared.policyAuthority,
                .mutation = proposedToolMutation(
                    test_support::toolInvocation(prepared.project, positioned.call.toolName()),
                    prepared.controller.controlledTargetId()
                ),
            };
        }

        [[nodiscard]]
        auto sourceWith(std::map<std::string, std::string> const& bodies) -> std::string
        {
            auto source = test_support::toolClosureSource("fixture.control");
            for (auto const& [entry, body] : bodies)
            {
                auto const original = "function(_input) return { entry = \"" + entry + "\" } end";
                auto const position = source.find(original);
                REQUIRE(position != std::string::npos);
                source.replace(position, original.size(), "function(input) " + body + " end");
            }
            return source;
        }

        [[nodiscard]]
        auto childRequest(
            test_support::PreparedStore const& prepared,
            ToolAdmissionRequest const& parent,
            ToolCallIssuingContext& issuing,
            std::string toolName
        ) -> ToolAdmissionRequest
        {
            auto catalog = ToolStartCatalog::create(prepared.project.toolCatalogSchemaOwner);
            REQUIRE(catalog.has_value());
            auto arguments = test_support::canonical(
                toolName.starts_with("framework.") ? "{}" : "{\"value\":1}"
            );
            auto invocation = catalog->validate(
                std::move(toolName), std::move(arguments)
            );
            REQUIRE(invocation.has_value());
            auto call = issuing.issue(*invocation);
            REQUIRE(call.has_value());
            auto ancestors = parent.ancestors;
            ancestors.emplace_back(parent.call);
            return ToolAdmissionRequest{
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = parent.root,
                .call            = *call,
                .policyAuthority = prepared.policyAuthority,
                .mutation        = proposedToolMutation(*invocation, prepared.controller.controlledTargetId()),
                .ancestors       = std::move(ancestors),
            };
        }
    }

    TEST_CASE("a dispatched Project Tool leaf runs its bound handler and records its answer")
    {
        auto temporary  = TemporaryDirectory{};
        auto prepared   = test_support::prepareStore(temporary.path());
        auto dispatcher = ProjectToolDispatcher::create(prepared.store);
        REQUIRE_MESSAGE(dispatcher.has_value(), why(dispatcher));
        auto const positioned = positionedCall(prepared, "project-leaf-dispatch");
        auto const request    = requestFor(prepared, positioned);
        auto observations = SnapshotObservationAuthority{};
        auto provider = [](ToolCallPositionIdentity const&) -> Result<ToolCallCompletion>
        {
            FAIL_CHECK("this fixture handler must not call a Tool");
            return ToolCallCompletion::confirmed(test_support::canonical("{}"));
        };

        auto const answered = dispatcher->dispatch(
            prepared.generation,
            request,
            provider,
            observations,
            std::stop_token{}
        );
        REQUIRE_MESSAGE(answered.has_value(), why(answered));
        CHECK(answered->state == ToolCallState::Confirmed);
        CHECK(replayPayload(*answered) == R"({"entry":"observe-1"})");

        auto const replayed = dispatcher->dispatch(
            prepared.generation,
            request,
            provider,
            observations,
            std::stop_token{}
        );
        REQUIRE_MESSAGE(replayed.has_value(), why(replayed));
        CHECK(replayed->state == ToolCallState::Confirmed);
        CHECK(replayPayload(*replayed) == replayPayload(*answered));
    }

    TEST_CASE("registered Project composition dispatches both catalogs with durable parents")
    {
        auto temporary = TemporaryDirectory{};
        auto source = sourceWith({
            {"command-1", "local run = require(\"@umbraflow/fixture/control\")[\"command-2\"] "
                "run(input) return run(input)"},
            {"command-2", "return require(\"@umbraflow/screen\").capture{}"},
        });
        auto prepared   = test_support::prepareStore(temporary.path(), "fixture.control", {}, source);
        auto dispatcher = ProjectToolDispatcher::create(prepared.store);
        REQUIRE(dispatcher.has_value());
        auto observations = SnapshotObservationAuthority{};
        auto calls        = std::make_shared<unsigned>(0);
        auto provider = [calls](ToolCallPositionIdentity const& call) -> Result<ToolCallCompletion>
        {
            ++*calls;
            CHECK(call.toolName() == "framework.screen.capture");
            CHECK(call.parentIdentity() != call.rootIdentity());
            return ToolCallCompletion::confirmed(test_support::canonical(R"({"recognized":true})"));
        };
        auto request = requestFor(prepared, positionedCall(prepared, "composed", "command-1"));
        auto answer = dispatcher->dispatch(prepared.generation, request, provider, observations, {});
        REQUIRE_MESSAGE(answer.has_value(), why(answer));
        CHECK(answer->state == ToolCallState::Confirmed);
        CHECK(replayPayload(*answer) == R"({"recognized":true})");
        CHECK(*calls == 2);
        auto replay = dispatcher->dispatch(prepared.generation, request, provider, observations, {});
        REQUIRE(replay.has_value());
        CHECK(replay->revision == answer->revision);
        CHECK(*calls == 2);

        auto parentIssuer = ToolCallIssuingContext::forHandler(request.call);
        auto child        = childRequest(prepared, request, parentIssuer, "fixture.control.command-2");
        auto childIssuer  = ToolCallIssuingContext::forHandler(child.call);
        auto grandchild   = childRequest(prepared, child, childIssuer, "framework.screen.capture");
        for (auto const& recorded : {child, grandchild})
        {
            auto durable = prepared.store.replayToolCall(recorded.root, recorded.call);
            REQUIRE_MESSAGE(durable.has_value(), why(durable));
            CHECK(durable->state == ToolCallState::Confirmed);
            CHECK(replayPayload(*durable) == R"({"recognized":true})");
        }
    }

    TEST_CASE("handler child admission refuses cycles and bounds before native dispatch")
    {
        auto entry      = std::string{"command-1"};
        auto body       = std::string{};
        auto secondBody = std::string{"return {entry=\"command-2\"}"};
        auto expected   = std::string{};
        SUBCASE("self recursion")
        {
            body     = "return require(\"@umbraflow/fixture/control\")[\"command-1\"](input)";
            expected = "cyclic";
        }
        SUBCASE("read-only parent cannot mutate")
        {
            entry    = "observe-1";
            body     = "return require(\"@umbraflow/fixture/control\")[\"command-1\"](input)";
            expected = "read-only";
        }
        SUBCASE("indirect recursion")
        {
            body       = "return require(\"@umbraflow/fixture/control\")[\"command-2\"](input)";
            secondBody = "return require(\"@umbraflow/fixture/control\")[\"command-1\"](input)";
            expected   = "cyclic";
        }
        SUBCASE("capability bound")
        {
            body     = "return require(\"@umbraflow/fixture/control\")[\"capability-gated\"](input)";
            expected = "required_capabilities";
        }
        SUBCASE("UI action bound")
        {
            body     = "return require(\"@umbraflow/fixture/control\")[\"stray-action\"](input)";
            expected = "ui_action_bounds";
        }
        SUBCASE("effect bound")
        {
            body = "return require(\"@umbraflow/input\").key{key=\"a\", screenshot_sha256=\""
                + hashOf("screenshot").hex() + "\"}";
            expected = "effect_bounds";
        }
        auto temporary = TemporaryDirectory{};
        auto prepared = test_support::prepareStore(
            temporary.path(), "fixture.control", {},
            sourceWith({{entry, body}, {"command-2", secondBody}})
        );
        auto dispatcher = ProjectToolDispatcher::create(prepared.store);
        REQUIRE(dispatcher.has_value());
        auto observations = SnapshotObservationAuthority{};
        auto provider = [](ToolCallPositionIdentity const&) -> Result<ToolCallCompletion>
        {
            FAIL_CHECK("refused child must not reach its provider");
            return ToolCallCompletion::confirmed(test_support::canonical("{}"));
        };
        auto request = requestFor(prepared, positionedCall(prepared, "refused-composition", entry));
        auto answer = dispatcher->dispatch(prepared.generation, request, provider, observations, {});
        REQUIRE_MESSAGE(answer.has_value(), why(answer));
        CHECK(answer->state == ToolCallState::TerminalFailure);
        CHECK_MESSAGE(replayPayload(*answer).contains(expected), replayPayload(*answer));
    }

    TEST_CASE("an Agent's semantic handler uses privileged OCR only with an explicit grant")
    {
        auto granted = true;
        SUBCASE("granted child") {}
        SUBCASE("ungranted child")
        {
            granted = false;
        }
        auto temporary = TemporaryDirectory{};
        auto grants    = std::vector<std::string>{};
        if (granted)
        {
            grants.emplace_back("framework.screen.read_single_line");
        }
        auto const source = sourceWith({{
            "observe-1",
            "local screen = require(\"@umbraflow/screen\") "
            "local shot = screen.capture{} "
            "return screen.read_single_line{screenshot_sha256=shot.screenshot_sha256, "
            "x=0, y=0, width=10, height=20}",
        }});
        auto prepared = test_support::prepareStore(
            temporary.path(), "fixture.control", grants, source, ControllerKind::Agent
        );
        auto dispatcher = ProjectToolDispatcher::create(prepared.store);
        REQUIRE(dispatcher.has_value());
        auto observations = SnapshotObservationAuthority{};
        auto calls        = std::make_shared<unsigned>(0);
        auto provider = [calls](ToolCallPositionIdentity const& call) -> Result<ToolCallCompletion>
        {
            ++*calls;
            if (call.toolName() == "framework.screen.capture")
            {
                return ToolCallCompletion::confirmed(test_support::canonical(
                    "{\"screenshot_sha256\":\"" + hashOf("shot").hex() + "\"}"
                ));
            }
            CHECK(call.toolName() == "framework.screen.read_single_line");
            return ToolCallCompletion::confirmed(test_support::canonical(R"({"text":"ace"})"));
        };
        auto request = requestFor(prepared, positionedCall(prepared, "agent-composition"));
        auto answer = dispatcher->dispatch(prepared.generation, request, provider, observations, {});
        REQUIRE_MESSAGE(answer.has_value(), why(answer));
        CHECK(answer->state == (granted ? ToolCallState::Confirmed : ToolCallState::TerminalFailure));
        CHECK(*calls == (granted ? 2U : 1U));
        CHECK_MESSAGE(
            replayPayload(*answer).contains(granted ? "ace" : "Privileged"),
            replayPayload(*answer)
        );

        // The same grant does not expose the machine vocabulary at Agent roots.
        auto catalog = ToolStartCatalog::create(prepared.project.toolCatalogSchemaOwner);
        REQUIRE(catalog.has_value());
        auto invocation = catalog->validate(
            "framework.screen.read_single_line",
            test_support::canonical(
                "{\"height\":20,\"screenshot_sha256\":\"" + hashOf("shot").hex()
                    + "\",\"width\":10,\"x\":0,\"y\":0}"
            )
        );
        REQUIRE(invocation.has_value());
        auto issuing = ToolCallIssuingContext::forRoot(request.root, request.call.executionIdentity());
        REQUIRE(issuing.issue(test_support::toolInvocation(
            prepared.project, request.call.toolName()
        )).has_value());
        auto direct = issuing.issue(*invocation);
        REQUIRE(direct.has_value());
        request.call = *direct;
        auto refused = prepared.store.admitToolCall(request);
        REQUIRE_FALSE(refused.has_value());
        CHECK(std::string{refused.error().message()}.contains("Controller profile"));
        CHECK(*calls == (granted ? 2U : 1U));
    }

    TEST_CASE("recorded child observations replay without reviving their live authority")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto request   = requestFor(prepared, positionedCall(prepared, "observation-replay", "command-1"));
        REQUIRE(prepared.store.persistToolRootRequest(request.root).has_value());
        REQUIRE(prepared.store.persistToolCallPosition(request.root, request.call).has_value());
        auto authority = SnapshotObservationAuthority{};
        auto reference = authority.mint(SnapshotObservationSpec{
            .controlledTargetId      = "target-1",
            .runtimeArtifactRootHash = prepared.runtimeArtifactRootHash,
            .projectRegistrationHash = prepared.project.registration.hash(),
            .frameIdentityHash       = hashOf("frame"),
            .screenshotSha256        = hashOf("shot"),
            .hostGeneration          = 1U,
            .expiresAtUnixMillis     = 1'000U,
            .uiActions = {
                ObservedUiAction{
                    .uiTarget = "start-button",
                    .binding  = "start.present",
                    .action   = "activate",
                    .kind     = "click",
                },
            },
        });
        REQUIRE(reference.has_value());
        auto catalog = ToolStartCatalog::create(prepared.project.toolCatalogSchemaOwner);
        REQUIRE(catalog.has_value());
        auto invocation = catalog->validate(
            "framework.ui.click",
            test_support::canonical(
                "{\"action\":\"activate\",\"binding\":\"start.present\",\"observation_reference\":"
                    + reference->wire().bytes() + ",\"ui_target\":\"start-button\"}"
            )
        );
        REQUIRE(invocation.has_value());
        auto issuing  = ToolCallIssuingContext::forHandler(request.call);
        auto original = prepared.store.issueToolChild(issuing, *invocation, authority);
        REQUIRE_MESSAGE(original.has_value(), why(original));
        REQUIRE(prepared.store.persistToolCallPosition(request.root, *original).has_value());
        auto freshAuthority = SnapshotObservationAuthority{};
        auto reentry        = ToolCallIssuingContext::forHandler(request.call);
        auto replayed       = prepared.store.issueToolChild(reentry, *invocation, freshAuthority);
        REQUIRE_MESSAGE(replayed.has_value(), why(replayed));
        CHECK(replayed->identity() == original->identity());
        CHECK(replayed->observationReference() == original->observationReference());
        auto next = prepared.store.issueToolChild(reentry, *invocation, freshAuthority);
        CHECK_FALSE(next.has_value());
    }

    TEST_CASE("persisted Project descendants cannot refund their call ceiling on replay")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto request   = requestFor(prepared, positionedCall(prepared, "durable-call-limit", "command-1"));
        auto admitted  = prepared.store.admitToolCall(request);
        REQUIRE(admitted.has_value());
        REQUIRE(prepared.store.beginToolCallDispatch(*admitted).has_value());
        auto catalog = ToolStartCatalog::create(prepared.project.toolCatalogSchemaOwner);
        REQUIRE(catalog.has_value());
        auto invocation = catalog->validate("framework.screen.capture", test_support::canonical("{}"));
        REQUIRE(invocation.has_value());
        auto issuing = ToolCallIssuingContext::forHandler(request.call);
        for (auto index = 0U; index < script::ScopedToolProgram::k_maximumProjectToolCalls; ++index)
        {
            auto recorded = issuing.issue(*invocation);
            REQUIRE(recorded.has_value());
            auto persisted = prepared.store.persistToolCallPosition(request.root, *recorded);
            REQUIRE_MESSAGE(persisted.has_value(), why(persisted));
        }
        auto next    = childRequest(prepared, request, issuing, "framework.screen.capture");
        auto refused = prepared.store.admitToolCall(next);
        REQUIRE_FALSE(refused.has_value());
        CHECK(std::string{refused.error().message()}.contains("durable descendant call ceiling"));
    }

    TEST_CASE("a restarted handler replays its recorded prefix and refuses divergence")
    {
        auto temporary  = TemporaryDirectory{};
        auto divergence = false;
        auto shortened  = false;
        SUBCASE("continue beyond a recorded child") {}
        SUBCASE("changed child name")
        {
            divergence = true;
        }
        SUBCASE("shortened child sequence")
        {
            shortened = true;
        }
        auto source = sourceWith({{
            "command-1",
            "local screen = require(\"@umbraflow/screen\") "
            "local first = screen.capture{} "
            "local second = screen.capture{} return {first=first, second=second}",
        }});
        auto prepared = std::optional{test_support::prepareStore(
            temporary.path(), "fixture.control", {}, source
        )};
        auto request = requestFor(*prepared, positionedCall(*prepared, "restart-composition", "command-1"));
        auto admission = prepared->store.admitToolCall(request);
        REQUIRE_MESSAGE(admission.has_value(), why(admission));
        auto dispatch = prepared->store.beginToolCallDispatch(*admission);
        REQUIRE(dispatch.has_value());
        auto issuing = ToolCallIssuingContext::forHandler(request.call);
        auto first = childRequest(
            *prepared, request, issuing,
            divergence ? "framework.workflow.now" : "framework.screen.capture"
        );
        auto calls = std::make_shared<unsigned>(0);
        auto provider = [calls](ToolCallPositionIdentity const&) -> Result<ToolCallCompletion>
        {
            ++*calls;
            return ToolCallCompletion::confirmed(test_support::canonical("{\"value\":1}"));
        };
        REQUIRE(ToolRuntimeExecutor{prepared->store}.invoke(first, provider).has_value());
        if (shortened)
        {
            auto second = childRequest(*prepared, request, issuing, "framework.screen.capture");
            REQUIRE(ToolRuntimeExecutor{prepared->store}.invoke(second, provider).has_value());
            auto third = childRequest(*prepared, request, issuing, "framework.screen.capture");
            REQUIRE(ToolRuntimeExecutor{prepared->store}.invoke(third, provider).has_value());
        }
        auto const generation = prepared->generation;
        auto const manifest = prepared->manifest;
        prepared.reset();
        auto restarted = OperatorCoordinator::open(temporary.path() / "production");
        REQUIRE_MESSAGE(restarted.has_value(), why(restarted));
        auto controller = restarted->resumeSession(
            SessionResume{
                .authenticatedControllerId = "controller-1",
                .controlledTargetId        = "target-1",
                .mode                      = SessionMode::Write,
                .kind                      = ControllerKind::Script,
            },
            manifest
        );
        REQUIRE_MESSAGE(controller.has_value(), why(controller));
        auto lease = restarted->acquireLease(*controller);
        REQUIRE(lease.has_value());
        request.controller = *controller;
        request.lease      = *lease;
        auto dispatcher    = ProjectToolDispatcher::create(*restarted);
        REQUIRE(dispatcher.has_value());
        auto observations = SnapshotObservationAuthority{};
        auto answer = dispatcher->dispatch(generation, request, provider, observations, {});
        CHECK(*calls == (shortened ? 3U : divergence ? 1U : 2U));
        if (divergence || shortened)
        {
            REQUIRE_FALSE(answer.has_value());
            CHECK(std::string{answer.error().message()}.contains("diverg"));
        }
        else
        {
            REQUIRE_MESSAGE(answer.has_value(), why(answer));
            CHECK(answer->state == ToolCallState::Confirmed);
        }
    }

    TEST_CASE("a parent handler cannot hide a nested replay divergence")
    {
        auto temporary = TemporaryDirectory{};
        auto source = sourceWith({
            {
                "command-1",
                "pcall(function() "
                "require(\"@umbraflow/fixture/control\")[\"command-2\"](input) end) "
                "return {hidden=true}",
            },
            {
                "command-2",
                "return require(\"@umbraflow/screen\").capture{}",
            },
        });
        auto prepared = test_support::prepareStore(
            temporary.path(), "fixture.control", {}, source
        );
        auto request = requestFor(
            prepared,
            positionedCall(prepared, "nested-divergence", "command-1")
        );
        auto rootAdmission = prepared.store.admitToolCall(request);
        REQUIRE_MESSAGE(rootAdmission.has_value(), why(rootAdmission));
        REQUIRE(prepared.store.beginToolCallDispatch(*rootAdmission).has_value());

        auto rootIssuing = ToolCallIssuingContext::forHandler(request.call);
        auto child = childRequest(
            prepared, request, rootIssuing, "fixture.control.command-2"
        );
        auto childAdmission = prepared.store.admitToolCall(child);
        REQUIRE_MESSAGE(childAdmission.has_value(), why(childAdmission));
        REQUIRE(prepared.store.beginToolCallDispatch(*childAdmission).has_value());

        auto childIssuing = ToolCallIssuingContext::forHandler(child.call);
        auto recorded = childRequest(
            prepared, child, childIssuing, "framework.workflow.now"
        );
        auto seeded = ToolRuntimeExecutor{prepared.store}.invoke(
            recorded,
            [](ToolCallPositionIdentity const&) -> Result<ToolCallCompletion>
            {
                return ToolCallCompletion::confirmed(test_support::canonical(
                    R"({"unix_millis":1})"
                ));
            }
        );
        REQUIRE_MESSAGE(seeded.has_value(), why(seeded));

        auto dispatcher = ProjectToolDispatcher::create(prepared.store);
        REQUIRE(dispatcher.has_value());
        auto observations = SnapshotObservationAuthority{};
        auto provider = [](ToolCallPositionIdentity const&) -> Result<ToolCallCompletion>
        {
            FAIL_CHECK("a divergent coordinate must not reach a provider");
            return ToolCallCompletion::confirmed(test_support::canonical("{}"));
        };
        auto answered = dispatcher->dispatch(
            prepared.generation, request, provider, observations, {}
        );
        REQUIRE_FALSE(answered.has_value());
        CHECK(std::string{answered.error().message()}.contains("diverg"));

        auto rootReplay = prepared.store.replayToolCall(request.root, request.call);
        REQUIRE(rootReplay.has_value());
        CHECK(rootReplay->state == ToolCallState::Dispatching);
        auto childReplay = prepared.store.replayToolCall(child.root, child.call);
        REQUIRE(childReplay.has_value());
        CHECK(childReplay->state == ToolCallState::Dispatching);
    }

    TEST_CASE("an unresolved recorded child blocks its enclosing handler without redispatch")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared = test_support::prepareStore(
            temporary.path(), "fixture.control", {}, sourceWith({{
                "command-1",
                "pcall(function() require(\"@umbraflow/fixture/control\")[\"command-2\"](input) end) "
                "return {hidden=true}",
            }})
        );
        auto request   = requestFor(prepared, positionedCall(prepared, "unresolved-composition", "command-1"));
        auto admission = prepared.store.admitToolCall(request);
        REQUIRE(admission.has_value());
        REQUIRE(prepared.store.beginToolCallDispatch(*admission).has_value());
        auto issuing = ToolCallIssuingContext::forHandler(request.call);
        auto child   = childRequest(prepared, request, issuing, "fixture.control.command-2");
        // Seed the recorded unresolved frontier through the executor. The
        // parent must inherit it even when its source tries to catch the error.
        auto unresolved = ToolRuntimeExecutor{prepared.store}.invoke(
            child,
            [](ToolCallPositionIdentity const&) -> Result<ToolCallCompletion>
            {
                return ToolCallCompletion::possible(test_support::canonical(
                    R"({"code":"unknown","message":"delivery unresolved","retryable":false})"
                ));
            }
        );
        REQUIRE_MESSAGE(unresolved.has_value(), why(unresolved));
        REQUIRE(unresolved->state == ToolCallState::Possible);
        auto dispatcher = ProjectToolDispatcher::create(prepared.store);
        REQUIRE(dispatcher.has_value());
        auto observations = SnapshotObservationAuthority{};
        auto provider = [](ToolCallPositionIdentity const&) -> Result<ToolCallCompletion>
        {
            FAIL_CHECK("no provider may run beyond the unresolved frontier");
            return ToolCallCompletion::confirmed(test_support::canonical("{}"));
        };
        for (auto attempt = 0; attempt < 2; ++attempt)
        {
            auto answer = dispatcher->dispatch(prepared.generation, request, provider, observations, {});
            REQUIRE_MESSAGE(answer.has_value(), why(answer));
            CHECK(answer->state == ToolCallState::Dispatching);
            CHECK_FALSE(answer->payload.has_value());
            auto replay = prepared.store.replayToolCall(child.root, child.call);
            REQUIRE(replay.has_value());
            CHECK(replay->revision == unresolved->revision);
        }
    }
}
