#include "../support/runtime-v2-fixture.hpp"

#include <task/exploration-session.hpp>
#include <task/host-delivery.hpp>
#include <task/page-model-file.hpp>
#include <task/task-context.hpp>
#include <task/task-host.hpp>
#include <task/ui-observation.hpp>

#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/ids.hpp>

#include <engine/session.hpp>

#include <script/engine.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace uf::task
{
    namespace
    {
        [[nodiscard]]
        auto runText(
            TaskHost& host,
            GenerationId generation,
            RuntimeContext& runtime,
            std::string_view source
        ) -> std::string
        {
            auto value = TaskHostTestAccess::run(
                host,
                generation,
                runtime.context(),
                source
            );
            REQUIRE(value.has_value());
            REQUIRE(value->text() != nullptr);
            return *value->text();
        }

        [[nodiscard]]
        auto explorationConfig(Frame value, std::filesystem::path tracePath)
            -> TaskRunConfig
        {
            return TaskRunConfig{
                .frameSource     = std::make_unique<FrameSource>(std::move(value)),
                .actionSink      = std::make_unique<ActionSink>(),
                .liveFingerprint = fingerprint(),
                .maximumPixelComparisons = 1'000,
                .recognitionTimeout = std::chrono::seconds{1},
                .tracePath          = std::move(tracePath),
            };
        }
    }

    TEST_CASE("contract-runtime-u01")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const generation = loadedRuntime(host, directory);
        auto status = host.queryTask(generation);
        REQUIRE(status.has_value());
        CHECK(status->runtimeModelBound);

        auto modelBytes = host.runtimeModelBytes(generation);
        REQUIRE(modelBytes.has_value());
        CHECK(*modelBytes == bytes(runtimeModel()));

        auto const invalidDirectory = TemporaryDirectory{};
        auto const invalidModel = std::string{"schema_version = 2\n"};
        auto const invalidRoot = publish(invalidDirectory.path(), invalidModel, {});
        auto invalidHost = TaskHost{};
        CHECK_FALSE(
            TaskHostTestAccess::activate(
                invalidHost,
                invalidDirectory.path(),
                invalidRoot
            ).has_value()
        );
    }

    TEST_CASE("contract-runtime-u02")
    {
        auto const missingDirectory = TemporaryDirectory{};
        auto assets = runtimeAssets();
        assets.pop_back();
        auto const missingRoot = publish(missingDirectory.path(), runtimeModel(), assets);
        auto missingHost = TaskHost{};
        CHECK_FALSE(
            TaskHostTestAccess::activate(
                missingHost,
                missingDirectory.path(),
                missingRoot
            ).has_value()
        );

        auto const extraDirectory = TemporaryDirectory{};
        assets = runtimeAssets();
        assets.emplace_back(
            ArtifactFile{.path = "assets/unreferenced.png", .bytes = templatePng(9)}
        );
        auto const extraRoot = publish(extraDirectory.path(), runtimeModel(), assets);
        auto extraHost = TaskHost{};
        CHECK_FALSE(TaskHostTestAccess::activate(
            extraHost,
            extraDirectory.path(),
            extraRoot
        ).has_value());
    }

    TEST_CASE("contract-runtime-u03")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const generation = loadedRuntime(host, directory);
        auto runtime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{11}),
            1'000
        };
        CHECK(
            runText(
                host,
                generation,
                runtime,
                R"lua(
                    local cycle = observe.open(project.load_project())
                    local state = cycle:resolve_state()
                    local binding = cycle:resolve_binding(state, "confirm")
                    cycle:close()
                    return state.kind .. ":" .. state.ordered_surface_stack[1]
                        .. ":" .. binding.kind .. ":" .. binding.binding
                )lua"
            )
            == "resolved_state:screen:resolved_binding:confirm.primary"
        );
    }

    TEST_CASE("contract-runtime-u04")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const generation = loadedRuntime(host, directory);
        auto runtime = RuntimeContext{
            frame({std::byte{0}, std::byte{k_actionGray}, std::byte{0}}, FrameId{12}),
            1'000
        };
        CHECK(
            runText(
                host,
                generation,
                runtime,
                R"lua(
                    local cycle = observe.open(project.load_project())
                    local state = cycle:resolve_state()
                    cycle:close()
                    return state.kind .. ":" .. state.reason
                )lua"
            )
            == "unknown_state:no_scene_candidate"
        );
    }

    TEST_CASE("contract-runtime-u05")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const generation = loadedRuntime(host, directory);
        auto runtime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{13}),
            0
        };
        CHECK(
            runText(
                host,
                generation,
                runtime,
                R"lua(
                    local cycle = observe.open(project.load_project())
                    local state = cycle:resolve_state()
                    cycle:close()
                    return state.kind .. ":" .. state.reason
                )lua"
            )
            == "unknown_state:unknown_scene_competitor"
        );
        CHECK(runtime.actions().clicks() == 0U);
    }

    TEST_CASE("contract-runtime-u06")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const generation = loadedRuntime(host, directory);
        auto const fence = controlFence(7);
        REQUIRE(TaskHostTestAccess::adoptControlFence(host, fence).has_value());
        auto const authority = dispatchAuthority(fence, generation);
        auto runtime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{14}),
            1'000
        };
        auto minted = TaskHostTestAccess::run(
            host,
            generation,
            runtime.context(),
            authorizeClickSource(k_runtimeUiAction)
        );
        REQUIRE(minted.has_value());
        CHECK(minted->number() == std::optional<double>{1.0});
        CHECK(runtime.actions().clicks() == 0U);
        CHECK(TaskHostTestAccess::pendingSemanticHash(host) != hash(runtimeModel()));

        auto const receipt = TaskHostTestAccess::pendingReceipt(host, k_runtimeUiAction);
        auto const delivered = TaskHostTestAccess::deliver(
            host,
            authority,
            receipt,
            runtime.context()
        );
        REQUIRE(delivered.has_value());
        CHECK(delivered->outcome() == DeliveryOutcome::Delivered);
        CHECK(delivered->reason().empty());
        REQUIRE(delivered->act().has_value());
        CHECK(runtime.actions().clicks() == 1U);

        // The reservation comes back untouched, which is what lets the ledger
        // recognise its own row instead of taking the Host's word for it.
        CHECK(delivered->authority().operationId == authority.operationId);
        CHECK(delivered->authority().dispatchSequence == authority.dispatchSequence);
        CHECK(
            delivered->authority().authorityDecisionId
            == authority.authorityDecisionId
        );
        CHECK(delivered->authority().leaseId == authority.leaseId);
        CHECK(delivered->authority().frozenPlanHash == authority.frozenPlanHash);
        CHECK(delivered->authority().targetGeneration == authority.targetGeneration);

        // A second presentation is an ERROR rather than a report: nothing was
        // consumed, so there is no fact for the ledger to record.
        CHECK_FALSE(
            TaskHostTestAccess::deliver(
                host,
                authority,
                receipt,
                runtime.context()
            ).has_value()
        );
        CHECK(runtime.actions().clicks() == 1U);

        // A Receipt authorizes one delivery against the cycle it was minted in,
        // so presenting a fresh one with a different context posts no click: no
        // other context holds that cycle. That is what lets the Host stop
        // remembering a pointer to the context that minted it. The refusal is a
        // NotDelivered report rather than an error, because the Receipt is gone.
        auto const reminted = TaskHostTestAccess::run(
            host,
            generation,
            runtime.context(),
            authorizeClickSource(k_runtimeUiAction)
        );
        REQUIRE(reminted.has_value());
        auto other = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{19}),
            1'000
        };

        // The other context must be holding a cycle of its own, or the refusal
        // proves only that it has none -- which would still hold with the
        // generation stamp removed. With both open, the stamp is what separates
        // them.
        auto const otherCycle = TaskHostTestAccess::run(
            host,
            generation,
            other.context(),
            "return observe.open(project.load_project()) ~= nil"
        );
        REQUIRE(otherCycle.has_value());
        CHECK(otherCycle->boolean() == std::optional<bool>{true});
        auto const refused = TaskHostTestAccess::deliver(
            host,
            authority,
            TaskHostTestAccess::pendingReceipt(host, k_runtimeUiAction),
            other.context()
        );
        REQUIRE(refused.has_value());
        CHECK(refused->outcome() == DeliveryOutcome::NotDelivered);
        CHECK_FALSE(refused->reason().empty());
        CHECK_FALSE(refused->act().has_value());
        CHECK(refused->receiptId() != delivered->receiptId());
        CHECK(other.actions().clicks() == 0U);
        CHECK(runtime.actions().clicks() == 1U);
    }

    // One friend, and it is TaskHost. TaskHostTestAccess reaches the Host's
    // privates and can therefore call deliver, but it cannot assemble what
    // deliver returns -- without which every case above would be asserting on a
    // value the test itself could have written.
    static_assert(!std::is_default_constructible_v<HostDeliveryReport>);
    static_assert(
        !std::is_constructible_v<
            HostDeliveryReport,
            DispatchAuthority,
            DeliveryOutcome,
            std::string,
            uint64,
            std::optional<engine::ActReceipt>
        >
    );
    static_assert(std::is_copy_constructible_v<HostDeliveryReport>);

    TEST_CASE("TaskHost::deliver refuses an authority the adopted fence does not name")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const generation = loadedRuntime(host, directory);
        auto const fence = controlFence(7);
        REQUIRE(TaskHostTestAccess::adoptControlFence(host, fence).has_value());
        auto runtime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{26}),
            1'000
        };
        REQUIRE(
            TaskHostTestAccess::run(
                host,
                generation,
                runtime.context(),
                authorizeClickSource(k_runtimeUiAction)
            ).has_value()
        );
        auto const receipt = TaskHostTestAccess::pendingReceipt(host, k_runtimeUiAction);
        auto const authority = dispatchAuthority(fence, generation);

        // One forgery per checked field: a Host that compared only three of the
        // four would still refuse a value that broke all four at once.
        auto const forgeries = std::vector<DispatchAuthority>{
            dispatchAuthority(controlFenceOn("another-target", 7), generation),
            dispatchAuthority(
                ControlFence{
                    .controlledTargetId = fence.controlledTargetId,
                    .sessionEpoch       = fence.sessionEpoch + 1,
                    .fencingToken       = fence.fencingToken,
                },
                generation
            ),
            dispatchAuthority(controlFence(fence.fencingToken + 1), generation),
            dispatchAuthority(fence, GenerationId{generation.value() + 1}),
        };
        for (auto const& forged : forgeries)
        {
            auto const rejected = TaskHostTestAccess::deliver(
                host,
                forged,
                receipt,
                runtime.context()
            );
            CHECK_FALSE(rejected.has_value());
        }
        CHECK(runtime.actions().clicks() == 0U);

        // An Err has to mean the Receipt was never spent, or "refused" would be
        // indistinguishable from "consumed and lost".
        auto const delivered = TaskHostTestAccess::deliver(
            host,
            authority,
            receipt,
            runtime.context()
        );
        REQUIRE(delivered.has_value());
        CHECK(delivered->outcome() == DeliveryOutcome::Delivered);
        CHECK(runtime.actions().clicks() == 1U);
    }

    TEST_CASE("A Host with no adopted control fence can mint no Receipt")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const generation = loadedRuntime(host, directory);
        auto unfenced = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{27}),
            1'000
        };
        CHECK_FALSE(
            TaskHostTestAccess::run(
                host,
                generation,
                unfenced.context(),
                authorizeClickSource(k_runtimeUiAction)
            ).has_value()
        );
        CHECK(unfenced.actions().clicks() == 0U);

        // The positive control: the same artifact and the same chunk mint as
        // soon as a fence exists, so the refusal above is the fence and not the
        // world the chunk observed. It needs its own Host because a Runtime
        // generation caches its template handles against the context that
        // measured them, so a second context on one Host resolves nothing --
        // measured, and independent of anything W4 changed.
        auto const fencedDirectory = TemporaryDirectory{};
        auto fencedHost = TaskHost{};
        auto const fencedGeneration = loadedRuntime(fencedHost, fencedDirectory);
        REQUIRE(
            TaskHostTestAccess::adoptControlFence(
                fencedHost,
                controlFence(7)
            ).has_value()
        );
        auto fenced = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{28}),
            1'000
        };
        CHECK(
            TaskHostTestAccess::run(
                fencedHost,
                fencedGeneration,
                fenced.context(),
                authorizeClickSource(k_runtimeUiAction)
            ).has_value()
        );
    }

    TEST_CASE("TaskHost::adoptControlFence is strictly monotone and binds one target")
    {
        auto host = TaskHost{};
        CHECK_FALSE(
            TaskHostTestAccess::adoptControlFence(
                host,
                controlFenceOn("", 1)
            ).has_value()
        );
        CHECK(TaskHostTestAccess::fence(host).fencingToken == 0U);

        REQUIRE(
            TaskHostTestAccess::adoptControlFence(host, controlFence(7)).has_value()
        );
        CHECK(TaskHostTestAccess::fence(host).fencingToken == 7U);
        CHECK(
            TaskHostTestAccess::fence(host).controlledTargetId == k_controlledTarget
        );

        // At or below is refused, so a lease a takeover already superseded
        // cannot re-arm the Host it fenced out.
        CHECK_FALSE(
            TaskHostTestAccess::adoptControlFence(host, controlFence(7)).has_value()
        );
        CHECK_FALSE(
            TaskHostTestAccess::adoptControlFence(host, controlFence(6)).has_value()
        );
        CHECK(TaskHostTestAccess::fence(host).fencingToken == 7U);

        CHECK_FALSE(
            TaskHostTestAccess::adoptControlFence(
                host,
                controlFenceOn("another-target", 8)
            ).has_value()
        );
        CHECK(
            TaskHostTestAccess::fence(host).controlledTargetId == k_controlledTarget
        );

        REQUIRE(
            TaskHostTestAccess::adoptControlFence(host, controlFence(8)).has_value()
        );
        CHECK(TaskHostTestAccess::fence(host).fencingToken == 8U);
    }

    TEST_CASE("A takeover fence turns an outstanding Receipt into proof of absence")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const generation = loadedRuntime(host, directory);
        REQUIRE(
            TaskHostTestAccess::adoptControlFence(host, controlFence(7)).has_value()
        );
        auto runtime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{29}),
            1'000
        };
        REQUIRE(
            TaskHostTestAccess::run(
                host,
                generation,
                runtime.context(),
                authorizeClickSource(k_runtimeUiAction)
            ).has_value()
        );
        auto const receipt = TaskHostTestAccess::pendingReceipt(host, k_runtimeUiAction);

        auto const seized = controlFence(8);
        REQUIRE(TaskHostTestAccess::adoptControlFence(host, seized).has_value());

        // The Receipt is still consumed, and the report is what makes the
        // in-flight case recordable: an Err here would leave the ledger unable
        // to state that this dispatch posted nothing.
        auto const report = TaskHostTestAccess::deliver(
            host,
            dispatchAuthority(seized, generation),
            receipt,
            runtime.context()
        );
        REQUIRE(report.has_value());
        CHECK(report->outcome() == DeliveryOutcome::NotDelivered);
        CHECK_FALSE(report->reason().empty());
        CHECK_FALSE(report->act().has_value());
        CHECK(runtime.actions().clicks() == 0U);
        CHECK_FALSE(
            TaskHostTestAccess::deliver(
                host,
                dispatchAuthority(seized, generation),
                receipt,
                runtime.context()
            ).has_value()
        );
    }

    TEST_CASE("A refused click is transport unknown and never proof of absence")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const generation = loadedRuntime(host, directory);
        auto const fence = controlFence(7);
        REQUIRE(TaskHostTestAccess::adoptControlFence(host, fence).has_value());
        auto runtime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{30}),
            1'000
        };
        REQUIRE(
            TaskHostTestAccess::run(
                host,
                generation,
                runtime.context(),
                authorizeClickSource(k_runtimeUiAction)
            ).has_value()
        );

        runtime.actions().refuseClicks();
        auto const report = TaskHostTestAccess::deliver(
            host,
            dispatchAuthority(fence, generation),
            TaskHostTestAccess::pendingReceipt(host, k_runtimeUiAction),
            runtime.context()
        );
        REQUIRE(report.has_value());

        // Nothing reached the target here, and the Host still may not say so:
        // clickPoint returns one Err whether it failed before the sink, at it,
        // or after the click had already landed. Only NotDelivered proves
        // absence, so a click-path failure must never spell it.
        CHECK(report->outcome() == DeliveryOutcome::TransportUnknown);
        CHECK_FALSE(report->reason().empty());
        CHECK_FALSE(report->act().has_value());
        CHECK(runtime.actions().clicks() == 0U);
    }

    TEST_CASE("contract-runtime-u07")
    {
        auto const runtimeDirectory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const runtimeGeneration = loadedRuntime(host, runtimeDirectory);
        auto runtime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{15}),
            1'000
        };
        auto runtimeSurface = TaskHostTestAccess::run(
            host,
            runtimeGeneration,
            runtime.context(),
            "return project ~= nil and observe ~= nil and explore == nil "
            "and model == nil and resolution == nil and evidence == nil"
        );
        REQUIRE(runtimeSurface.has_value());
        CHECK(runtimeSurface->boolean() == std::optional<bool>{true});
        CHECK_FALSE(
            host.startExplorationSession(
                runtimeGeneration,
                explorationConfig(
                    frame({std::byte{0}, std::byte{0}, std::byte{0}}, FrameId{16}),
                    runtimeDirectory.path() / "forbidden-trace.jsonl"
                )
            ).has_value()
        );

        auto const authoringDirectory = TemporaryDirectory{};
        write(authoringDirectory.path() / "annotation-screenshot.png", "authoring pixels");
        auto annotation = host.openAnnotationProject(authoringDirectory.path());
        REQUIRE(annotation.has_value());
        CHECK_FALSE(host.runtimeModelBytes(*annotation).has_value());
        auto session = host.startExplorationSession(
            *annotation,
            explorationConfig(
                frame({std::byte{0}, std::byte{0}, std::byte{0}}, FrameId{17}),
                authoringDirectory.path() / "authoring-trace.jsonl"
            )
        );
        REQUIRE(session.has_value());
        auto authoringSurface = (*session)->evaluate(
            R"lua(
                local blob = explore.cycle(function(cycle)
                    return cycle:crop(0, 0, 1, 1)
                end)
                local measured = explore.probe(blob, 0, 0, 1, 1)
                return explore ~= nil and project == nil and observe == nil
                    and model == nil and resolution == nil and evidence == nil
                    and type(blob) == "string" and #blob > 0
                    and measured.image_width == 1 and measured.image_height == 1
            )lua",
            "authoring-boundary"
        );
        REQUIRE(authoringSurface.has_value());
        CHECK(authoringSurface->boolean() == std::optional<bool>{true});
    }

    TEST_CASE("contract-runtime-u08")
    {
        auto const directory = TemporaryDirectory{};
        auto const rootHash = publish(directory.path(), runtimeModel(), runtimeAssets());
        write(directory.path() / "annotation-screenshot.png", "pixels must stay offline");

        CHECK_FALSE(loadRuntimeArtifact(directory.path(), rootHash).has_value());
        auto host = TaskHost{};
        CHECK_FALSE(TaskHostTestAccess::activate(
            host,
            directory.path(),
            rootHash
        ).has_value());
    }

    // resolve_state stamps every resolved state with an id drawn from a
    // module-level counter, and the trusted VM outlives one observation, so two
    // readings of ONE unchanged screen carry two different ids. Comparing the
    // two canonical documents is therefore what proves the id is outside them:
    // a single observation's bytes could not tell the difference.
    TEST_CASE("TaskHost::observe keeps the resolver's counter out of the document")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const rootHash = publish(
            directory.path(),
            runtimeModel(),
            runtimeAssets()
        );
        auto const generation = TaskHostTestAccess::activate(
            host,
            directory.path(),
            rootHash
        );
        REQUIRE(generation.has_value());
        auto runtime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{21}),
            1'000
        };

        // One friend, and it is the Host. A caller holding every field still
        // cannot assemble a snapshot, so there is no second producer to
        // disagree with the one the resolver fed.
        CHECK_FALSE(
            (std::is_constructible_v<
                UiObservationSnapshot,
                std::string,
                GenerationId,
                TargetGeneration,
                ContentHash,
                ContentHash,
                std::string
            >)
        );
        CHECK(std::is_copy_constructible_v<UiObservationSnapshot>);

        auto const first = host.observe(*generation, runtime.context());
        REQUIRE(first.has_value());
        auto const second = host.observe(*generation, runtime.context());
        REQUIRE(second.has_value());

        CHECK(first->canonicalJcs() == second->canonicalJcs());
        CHECK(first->stateResolutionHash() == second->stateResolutionHash());
        CHECK(first->observationId() != second->observationId());

        CHECK(
            first->canonicalJcs()
            == R"({"kind":"resolved_state","ordered_surface_stack":["screen"]})"
        );
        CHECK(first->stateResolutionHash() == hash(first->canonicalJcs()));
        CHECK(first->generation() == *generation);
        CHECK(first->targetGeneration() == TargetGeneration::fromValue(3));
        CHECK(first->artifactRootHash() == rootHash);
        CHECK(first->semanticHash() != rootHash);
        CHECK(first->semanticHash() != hash(runtimeModel()));

        // The chunk leaves its cycle open for the Host to read the capture off;
        // the Host closing it again is what lets a second observation open one.
        CHECK_FALSE(runtime.context().hasOpenCycle());
    }

    TEST_CASE("TaskHost::observe canonicalizes an unresolved state and its reason")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const generation = loadedRuntime(host, directory);
        auto runtime = RuntimeContext{
            frame({std::byte{0}, std::byte{k_actionGray}, std::byte{0}}, FrameId{22}),
            1'000
        };
        auto const unresolved = host.observe(generation, runtime.context());
        REQUIRE(unresolved.has_value());
        CHECK(
            unresolved->canonicalJcs()
            == R"({"kind":"unknown_state","reason":"no_scene_candidate"})"
        );
        CHECK(unresolved->stateResolutionHash() == hash(unresolved->canonicalJcs()));

        auto const resolvedDirectory = TemporaryDirectory{};
        auto resolvedHost = TaskHost{};
        auto const resolvedGeneration = loadedRuntime(resolvedHost, resolvedDirectory);
        auto resolvedRuntime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{23}),
            1'000
        };
        auto const resolved = resolvedHost.observe(
            resolvedGeneration,
            resolvedRuntime.context()
        );
        REQUIRE(resolved.has_value());

        // Two worlds, two documents, two digests: the hash follows the bytes
        // rather than the occasion that produced them.
        CHECK(unresolved->canonicalJcs() != resolved->canonicalJcs());
        CHECK(unresolved->stateResolutionHash() != resolved->stateResolutionHash());
    }

    TEST_CASE("TaskHost::observe canonicalizes an ambiguous state through its conflict")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const rootHash = publish(
            directory.path(),
            twoSceneRuntimeModel(),
            runtimeAssets()
        );
        auto const generation = TaskHostTestAccess::activate(
            host,
            directory.path(),
            rootHash
        );
        REQUIRE(generation.has_value());
        auto runtime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{24}),
            1'000
        };
        auto const ambiguous = host.observe(*generation, runtime.context());
        REQUIRE(ambiguous.has_value());
        CHECK(
            ambiguous->canonicalJcs()
            == R"({"kind":"ambiguous_state","reason":"multiple_scenes"})"
        );
    }

    TEST_CASE("TaskHost::observe refuses an Annotation generation before the VM")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const generation = loadedRuntime(host, directory);
        auto runtime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{25}),
            1'000
        };
        auto const authoringDirectory = TemporaryDirectory{};
        auto const annotation = host.openAnnotationProject(authoringDirectory.path());
        REQUIRE(annotation.has_value());

        auto const refused = host.observe(*annotation, runtime.context());
        REQUIRE_FALSE(refused.has_value());

        // UnsupportedCapability and not InvalidResource: the kind gate refuses
        // before any VM is reached. bindRuntimeContext would refuse the same
        // call afterwards and would say InvalidResource, so the kind is what
        // says WHICH gate held.
        CHECK(
            automationErrorKind(refused.error())
            == std::optional<AutomationErrorKind>{
                AutomationErrorKind::UnsupportedCapability
            }
        );
        CHECK_FALSE(runtime.context().hasOpenCycle());
        CHECK(host.observe(generation, runtime.context()).has_value());
    }
}
