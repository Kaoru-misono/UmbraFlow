#include "../support/runtime-v2-fixture.hpp"

#include <task/exploration-session.hpp>
#include <task/page-model-file.hpp>
#include <task/task-context.hpp>
#include <task/task-host.hpp>
#include <task/ui-observation.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/ids.hpp>

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
        constexpr auto k_authorizeSource = std::string_view{R"lua(
            local cycle = observe.open(project.load_project())
            local state = cycle:resolve_state()
            local binding = cycle:resolve_binding(state, "confirm")
            local receipt, reason = cycle:authorize(binding, "activate")
            if receipt == nil then error(reason) end
            return 1
        )lua"};

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
        auto runtime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{14}),
            1'000
        };
        auto minted = TaskHostTestAccess::run(
            host,
            generation,
            runtime.context(),
            k_authorizeSource
        );
        REQUIRE(minted.has_value());
        CHECK(minted->number() == std::optional<double>{1.0});
        CHECK(runtime.actions().clicks() == 0U);
        CHECK(TaskHostTestAccess::pendingSemanticHash(host) != hash(runtimeModel()));

        auto const receipt = TaskHostTestAccess::pendingReceipt(host);
        REQUIRE(TaskHostTestAccess::deliver(host, receipt, runtime.context()).has_value());
        CHECK(runtime.actions().clicks() == 1U);
        CHECK_FALSE(TaskHostTestAccess::deliver(host, receipt, runtime.context()).has_value());
        CHECK(runtime.actions().clicks() == 1U);

        // A Receipt authorizes one delivery against the cycle it was minted in,
        // so presenting a fresh one with a different context is refused before
        // any click: no other context holds that cycle. That is what lets the
        // Host stop remembering a pointer to the context that minted it.
        auto const reminted = TaskHostTestAccess::run(
            host,
            generation,
            runtime.context(),
            k_authorizeSource
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
        CHECK_FALSE(
            TaskHostTestAccess::deliver(
                host,
                TaskHostTestAccess::pendingReceipt(host),
                other.context()
            ).has_value()
        );
        CHECK(other.actions().clicks() == 0U);
        CHECK(runtime.actions().clicks() == 1U);
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
