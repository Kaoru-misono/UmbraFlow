#include "../support/runtime-v2-fixture.hpp"

#include <task/exploration-session.hpp>
#include <task/host-delivery.hpp>
#include <task/runtime-model-file.hpp>
#include <task/task-context.hpp>
#include <task/task-host.hpp>
#include <task/ui-observation.hpp>

#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>

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
        auto pixelRect(uint32 x, uint32 y, uint32 width, uint32 height) -> PixelRect
        {
            auto const value = PixelRect::create(x, y, width, height);
            REQUIRE(value.has_value());
            return *value;
        }

        // An OCR engine that answers with what the case scripted, and records
        // what it was asked. It is not a stub standing in for a real engine: the
        // property under test is what the resolver does with a reading, and a
        // real engine would make the text a fact about a PNG rather than about
        // this file, without making any of these assertions stronger.
        class ScriptedReader final : public ocr::IOcrEngine
        {
            std::string m_text;
            uint32      m_confidenceBp;
            uint32      m_calls{};
            PixelRect   m_lastRect{pixelRect(0, 0, 1, 1)};

        public:
            ScriptedReader(std::string text, uint32 confidenceBp)
                : m_text{std::move(text)}
                , m_confidenceBp{confidenceBp}
            {
            }

            [[nodiscard]] auto identity() const noexcept -> std::string_view override
            {
                return "scripted-reader";
            }

            [[nodiscard]]
            auto read(BgraImage const&, ocr::ReadSpec const& spec)
                -> Result<ocr::Readout> override
            {
                ++m_calls;
                if (spec.rect.has_value())
                {
                    m_lastRect = *spec.rect;
                }
                return ocr::Readout{
                    .lines = {
                        ocr::TextLine{
                            .text         = m_text,
                            .bounds       = m_lastRect,
                            .confidenceBp = m_confidenceBp,
                        },
                    },
                };
            }

            [[nodiscard]] auto calls() const noexcept -> uint32 { return m_calls; }

            [[nodiscard]] auto lastRect() const noexcept -> PixelRect
            {
                return m_lastRect;
            }
        };

        // The fixture's world plus one Binding that reports what a Reader read.
        // Its detector is the confirm mark, so the middle pixel decides whether
        // the reading Binding is present while the anchor still resolves the
        // Surface -- which is what lets an absent reading Binding be exercised
        // without also unresolving the state.
        [[nodiscard]] auto readingRuntimeModel() -> std::string
        {
            return R"toml(schema_version = 2
base_resolution = [3, 1]
base_dpi = [96, 96]

[[ui_target]]
id = "screen-marker"
kind = "region"

[[ui_target]]
id = "title"
kind = "region"

[[locator]]
id = "screen-anchor"
kind = "template"
asset_path = "assets/anchor.png"
threshold = 1

[[locator]]
id = "confirm-mark"
kind = "template"
asset_path = "assets/confirm.png"
threshold = 1

[[reader]]
id = "title.reader"
kind = "text"
confidence_floor = 0.5
normalization = "collapse_whitespace"

[[binding]]
id = "screen.anchor"
surface = "screen"
ui_target = "screen-marker"
variant = "primary"
placement = { kind = "fixed", rect = [0, 0, 1, 1] }
detector = { all = [{ kind = "locator_present", locator = "screen-anchor" }], any = [], none = [] }
actions = []

[[binding]]
id = "title.primary"
surface = "screen"
ui_target = "title"
variant = "primary"
placement = { kind = "fixed", rect = [1, 0, 1, 1] }
detector = { all = [{ kind = "locator_present", locator = "confirm-mark" }], any = [], none = [] }
actions = []
reads = ["title.reader"]

[[surface]]
id = "screen"
kind = "scene"
covers = []
identity = { all = ["screen.anchor"], any = [], none = [] }
)toml";
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

        // The fixture's model with `geometry` standing in for the two lines it
        // declares, and its body reused byte for byte. Every rectangle in that
        // body sits within 2x1, so any extent at or above the fixture's own
        // accepts the same bindings and the declared geometry is the only thing
        // that varies between the models below.
        [[nodiscard]] auto modelDeclaring(std::string_view geometry) -> std::string
        {
            auto const fixture = runtimeModel();
            auto const body    = fixture.find("\n\n[[ui_target]]");
            REQUIRE(body != std::string::npos);
            return "schema_version = 2\n" + std::string{geometry}
                + fixture.substr(body);
        }

        [[nodiscard]]
        auto activateDeclaring(
            TaskHost& host,
            TemporaryDirectory const& directory,
            std::string_view geometry
        ) -> Result<GenerationId>
        {
            auto const model = modelDeclaring(geometry);
            auto const rootHash = publish(directory.path(), model, runtimeAssets());
            return TaskHostTestAccess::activate(host, directory.path(), rootHash);
        }

        [[nodiscard]]
        auto bindingDeclaring(
            TaskHost& host,
            TemporaryDirectory const& directory,
            std::string_view geometry
        ) -> RuntimeModelBinding
        {
            auto const generation = activateDeclaring(host, directory, geometry);
            REQUIRE(generation.has_value());
            auto binding = host.runtimeModelBinding(*generation);
            REQUIRE(binding.has_value());
            return *std::move(binding);
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

        // readings is present and empty because this model declares no reads.
        // Empty and absent are two different documents, and a resolved state
        // always says which one it is.
        CHECK(
            first->canonicalJcs()
            == R"({"kind":"resolved_state","ordered_surface_stack":["screen"],)"
               R"("readings":[]})"
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

    // The whole reading path, from the trusted Reader to the bytes a plugin is
    // handed. Nothing below asserts against a string this file also produced:
    // the text comes out of the scripted Reader, the normalization out of the
    // page model, and the document out of the resolver.
    TEST_CASE("TaskHost::observe reports what a present Binding's Reader read")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const rootHash = publish(
            directory.path(),
            readingRuntimeModel(),
            runtimeAssets()
        );
        auto const generation = TaskHostTestAccess::activate(
            host,
            directory.path(),
            rootHash
        );
        REQUIRE(generation.has_value());

        auto reader = std::make_unique<ScriptedReader>("  Wandering   Merchant \n", 9'100);
        auto* const p_reader = reader.get();
        auto runtime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{31}),
            1'000,
            std::move(reader)
        };
        auto const observed = host.observe(*generation, runtime.context());
        REQUIRE(observed.has_value());

        // The reading is attributed to the UiTarget, carries the NORMALISED text
        // its Reader declared collapse_whitespace for, and is inside the document
        // whose sha256 is the state resolution hash.
        CHECK(
            observed->canonicalJcs()
            == R"({"kind":"resolved_state","ordered_surface_stack":["screen"],)"
               R"("readings":[{"reader":"title.reader","text":"Wandering Merchant",)"
               R"("ui_target":"title"}]})"
        );
        CHECK(observed->stateResolutionHash() == hash(observed->canonicalJcs()));

        // The Reader was pointed at the reading Binding's own rectangle and not
        // at the Surface, so a document that named the right UiTarget over the
        // wrong pixels would fail here rather than read as a correct answer.
        REQUIRE(p_reader->calls() == 1U);
        CHECK(p_reader->lastRect().x() == 1);
        CHECK(p_reader->lastRect().y() == 0);
        CHECK(p_reader->lastRect().width() == 1);
        CHECK(p_reader->lastRect().height() == 1);

        // Nothing the Reader knew beyond the text travels: not the score that
        // cleared the floor, not the rectangle, not the variant that matched,
        // and not the pre-normalisation string.
        CHECK(observed->canonicalJcs().find("confidence") == std::string::npos);
        CHECK(observed->canonicalJcs().find("0.91") == std::string::npos);
        CHECK(observed->canonicalJcs().find("rect") == std::string::npos);
        CHECK(observed->canonicalJcs().find("primary") == std::string::npos);
        CHECK(observed->canonicalJcs().find("title.primary") == std::string::npos);
        CHECK(observed->canonicalJcs().find("Wandering   Merchant") == std::string::npos);

        // Two captures of one unchanged screen are one decision. A score or a
        // capture identity inside the reading would break this even though the
        // world did not move.
        auto const again = host.observe(*generation, runtime.context());
        REQUIRE(again.has_value());
        CHECK(again->canonicalJcs() == observed->canonicalJcs());
        CHECK(again->stateResolutionHash() == observed->stateResolutionHash());
    }

    // The falsifier for the decision basis. state_resolution_hash is one member
    // of DecisionBasis and it is a digest over this whole document, so a reading
    // that changed while the document's hash did not would be a plugin input
    // outside everything a replay is checked against.
    TEST_CASE("TaskHost::observe moves the state resolution hash when a reading moves")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const rootHash = publish(
            directory.path(),
            readingRuntimeModel(),
            runtimeAssets()
        );
        auto const generation = TaskHostTestAccess::activate(
            host,
            directory.path(),
            rootHash
        );
        REQUIRE(generation.has_value());

        auto const pixels = std::vector<std::byte>{
            std::byte{k_anchorGray},
            std::byte{k_actionGray},
            std::byte{0},
        };
        auto firstRuntime = RuntimeContext{
            frame(pixels, FrameId{32}),
            1'000,
            std::make_unique<ScriptedReader>("Wandering Merchant", 9'100)
        };
        auto const first = host.observe(*generation, firstRuntime.context());
        REQUIRE(first.has_value());

        // A second Host, because observe.luau caches template handles per
        // RuntimeModel and a second TaskContext on one generation resolves
        // nothing -- the reason host-delivery-fixture.hpp already states.
        auto const secondDirectory = TemporaryDirectory{};
        auto secondHost = TaskHost{};
        auto const secondRoot = publish(
            secondDirectory.path(),
            readingRuntimeModel(),
            runtimeAssets()
        );
        auto const secondGeneration = TaskHostTestAccess::activate(
            secondHost,
            secondDirectory.path(),
            secondRoot
        );
        REQUIRE(secondGeneration.has_value());
        auto secondRuntime = RuntimeContext{
            frame(pixels, FrameId{33}),
            1'000,
            std::make_unique<ScriptedReader>("Abandoned Shrine", 9'100)
        };
        auto const second = secondHost.observe(
            *secondGeneration,
            secondRuntime.context()
        );
        REQUIRE(second.has_value());

        // Identical pixels, identical model, identical surface stack: the ONLY
        // difference between the two worlds is what the Reader read.
        CHECK(first->canonicalJcs() != second->canonicalJcs());
        CHECK(first->stateResolutionHash() != second->stateResolutionHash());
        CHECK(second->canonicalJcs().find("Abandoned Shrine") != std::string::npos);
        CHECK(
            second->canonicalJcs().find(R"("ordered_surface_stack":["screen"])")
            != std::string::npos
        );
    }

    // Below the floor is not a reading. The plugin sees an empty list, which is
    // the same answer a Reader that found nothing gives, and never the score or
    // the reason that decided it.
    TEST_CASE("TaskHost::observe reports no reading below its Reader's floor")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const rootHash = publish(
            directory.path(),
            readingRuntimeModel(),
            runtimeAssets()
        );
        auto const generation = TaskHostTestAccess::activate(
            host,
            directory.path(),
            rootHash
        );
        REQUIRE(generation.has_value());

        // The model floors title.reader at 0.5 and this clears 0.1, so the text
        // is one the Reader itself produced and the floor is what refuses it.
        auto runtime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{k_actionGray}, std::byte{0}}, FrameId{34}),
            1'000,
            std::make_unique<ScriptedReader>("Wandering Merchant", 1'000)
        };
        auto const observed = host.observe(*generation, runtime.context());
        REQUIRE(observed.has_value());
        CHECK(
            observed->canonicalJcs()
            == R"({"kind":"resolved_state","ordered_surface_stack":["screen"],)"
               R"("readings":[]})"
        );
        CHECK(observed->canonicalJcs().find("Wandering") == std::string::npos);
        CHECK(observed->canonicalJcs().find("low_confidence") == std::string::npos);
    }

    // A Binding that is not present reads nothing, and the state still resolves.
    // Without this the empty list above could be an artifact of the floor alone.
    TEST_CASE("TaskHost::observe reports no reading from an absent Binding")
    {
        auto const directory = TemporaryDirectory{};
        auto host = TaskHost{};
        auto const rootHash = publish(
            directory.path(),
            readingRuntimeModel(),
            runtimeAssets()
        );
        auto const generation = TaskHostTestAccess::activate(
            host,
            directory.path(),
            rootHash
        );
        REQUIRE(generation.has_value());

        // The middle pixel no longer matches the reading Binding's locator, so
        // the Surface still resolves off its anchor and title.primary does not.
        auto reader = std::make_unique<ScriptedReader>("Wandering Merchant", 9'100);
        auto* const p_reader = reader.get();
        auto runtime = RuntimeContext{
            frame({std::byte{k_anchorGray}, std::byte{0}, std::byte{0}}, FrameId{35}),
            1'000,
            std::move(reader)
        };
        auto const observed = host.observe(*generation, runtime.context());
        REQUIRE(observed.has_value());
        CHECK(
            observed->canonicalJcs()
            == R"({"kind":"resolved_state","ordered_surface_stack":["screen"],)"
               R"("readings":[]})"
        );

        // Nothing was read at all: an absent Binding does not spend a Host read
        // on pixels that belong to whatever is there instead.
        CHECK(p_reader->calls() == 0U);
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

    // Three models differing only in the geometry they declare. The first pins
    // each of the four components separately, so a fingerprint that transposed
    // an extent into a DPI passes none of them; the second changes only the
    // extent and the third only the DPI, so a constant, or a value carrying
    // half of what the model states, cannot satisfy all three.
    TEST_CASE("RuntimeModelBinding publishes the geometry its model declares")
    {
        auto const declaredDirectory = TemporaryDirectory{};
        auto declaredHost = TaskHost{};
        auto const declared = bindingDeclaring(
            declaredHost,
            declaredDirectory,
            "base_resolution = [1920, 1080]\nbase_dpi = [96, 120]\n"
        );
        CHECK(declared.fingerprint().width() == 1920U);
        CHECK(declared.fingerprint().height() == 1080U);
        CHECK(declared.fingerprint().dpiX() == 96U);
        CHECK(declared.fingerprint().dpiY() == 120U);

        // The geometry travels beside the three vocabularies and not inside
        // them: a fourth name here would be the Operator gaining a way to ask
        // what a name means, which is what DeclaredRuntimeUi exists to refuse.
        CHECK(declared.declaredUi().surfaces == std::vector<std::string>{"screen"});
        CHECK(
            declared.declaredUi().uiTargets
            == std::vector<std::string>{"confirm", "screen-marker"}
        );
        CHECK(declared.declaredUi().actions == std::vector<std::string>{"activate"});

        // Both extent components above 16 bits: a fingerprint that crossed the
        // native seam through a narrower integer arrives truncated rather than
        // merely different, and equality on the whole value would not say which.
        auto const wideDirectory = TemporaryDirectory{};
        auto wideHost = TaskHost{};
        auto const wide = bindingDeclaring(
            wideHost,
            wideDirectory,
            "base_resolution = [70000, 66000]\nbase_dpi = [96, 120]\n"
        );
        CHECK(wide.fingerprint().width() == 70000U);
        CHECK(wide.fingerprint().height() == 66000U);
        CHECK(wide.fingerprint() != declared.fingerprint());

        auto const dpiDirectory = TemporaryDirectory{};
        auto dpiHost = TaskHost{};
        auto const dpi = bindingDeclaring(
            dpiHost,
            dpiDirectory,
            "base_resolution = [1920, 1080]\nbase_dpi = [144, 144]\n"
        );
        CHECK(dpi.fingerprint().dpiX() == 144U);
        CHECK(dpi.fingerprint().dpiY() == 144U);
        CHECK(dpi.fingerprint() != declared.fingerprint());
    }

    // What a model may not leave out. Both halves are required and neither has
    // a default, so a model stating one and omitting the other is refused where
    // the omission is, and a zero component is not an extent. Each case reads
    // the whole refusal and not just the field it names: every rectangle in the
    // model is bounds-checked against base_resolution and says so, so a
    // substring stopping at the field name would go green off that second
    // refusal and prove nothing about this one.
    TEST_CASE("A RuntimeModel declaring one half of its geometry is refused")
    {
        auto const refusal = [](std::string_view geometry, std::string_view refused)
        {
            auto const directory = TemporaryDirectory{};
            auto host = TaskHost{};
            auto const activated = activateDeclaring(host, directory, geometry);
            REQUIRE_FALSE(activated.has_value());
            CHECK(activated.error().message().find(refused) != std::string::npos);
        };

        refusal("", "RuntimeModel.base_resolution must be a table");
        refusal(
            "base_resolution = [1920, 1080]\n",
            "RuntimeModel.base_dpi must be a table"
        );
        refusal(
            "base_dpi = [96, 120]\n",
            "RuntimeModel.base_resolution must be a table"
        );
        refusal(
            "base_resolution = [1920, 0]\nbase_dpi = [96, 120]\n",
            "RuntimeModel.base_resolution[2] must be positive"
        );
        refusal(
            "base_resolution = [1920, 1080]\nbase_dpi = [96, 0]\n",
            "RuntimeModel.base_dpi[2] must be positive"
        );
    }
}
