#include <task/framework-bundle.hpp>
#include <task/script-bindings.hpp>
#include <task/task-context.hpp>

#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/ids.hpp>
#include <domain/key.hpp>
#include <domain/space.hpp>

#include <engine/ports.hpp>
#include <engine/session.hpp>

#include <script/engine.hpp>

#include <trace/event.hpp>
#include <trace/recorder.hpp>
#include <trace/sink.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// The exploration front end's acting verbs: that they deliver what the chunk
// asked for, that every delivered act leaves a line in the trace, and that no
// other environment can reach any of them.
//
// It builds its own world rather than including tests/support/runtime-v2-
// fixture.hpp, which reaches TaskHostTestAccess through a conformance header
// and would carry that Host privilege into test-task for cases that activate no
// artifact and mint no Receipt. What is needed here is three pixels and a sink
// that remembers, and none of it is the artifact bytes that fixture exists to
// keep single.
namespace uf::task
{
    namespace
    {
        [[nodiscard]] auto manifestHash(std::string_view text) -> ContentHash
        {
            auto result = sha256(std::as_bytes(std::span{text}));
            REQUIRE(result.has_value());
            return *result;
        }

        // Generous next to a 3x1 frame. Nothing here measures the matcher, so
        // the budget only has to be a number no case can exhaust.
        constexpr auto k_comparisonBudget = uint64{1'000};

        [[nodiscard]] auto fingerprint() -> ProjectFingerprint
        {
            auto result = ProjectFingerprint::create(3, 1, 96, 96);
            REQUIRE(result.has_value());
            return *result;
        }

        // Three Gray8 pixels in a row, so a chunk has x = 0, 1 and 2 to aim at
        // and every rectangle a case draws fits the frame.
        [[nodiscard]] auto syntheticFrame() -> Frame
        {
            auto transform = CoordinateTransform::create(
                Point<DesktopSpace>{0.0F, 0.0F},
                3.0F,
                1.0F,
                3,
                1
            );
            REQUIRE(transform.has_value());
            auto result = Frame::create(
                FrameId{11},
                CaptureSessionId{7},
                TargetGeneration::fromValue(3),
                MonotonicInstant::now(),
                3,
                1,
                3,
                PixelFormat::Gray8,
                std::make_shared<FrameBuffer const>(
                    std::vector<std::byte>{
                        std::byte{2},
                        std::byte{5},
                        std::byte{2},
                    }
                ),
                *transform
            );
            REQUIRE(result.has_value());
            return *std::move(result);
        }

        class FrameSource final : public engine::IFrameSource
        {
            Frame m_frame;

        public:
            explicit FrameSource(Frame value) noexcept : m_frame{std::move(value)} {}

            [[nodiscard]] auto capture(CaptureBudget const&) -> Result<Frame> override
            {
                return m_frame;
            }

            [[nodiscard]] auto validateTargetInstance() -> Status override
            {
                return ok();
            }

            // One synthetic frame for as long as this source exists. Claiming
            // Live would only make these cases fail on how fast they compiled.
            [[nodiscard]] auto targetWorld() const noexcept -> TargetWorld override
            {
                return TargetWorld::Recorded;
            }
        };
        // Every act the sink was asked to perform, in order, flattened to the
        // facts an assertion here reads. One record type rather than one per
        // verb because the cases compare a sequence, and a sequence of variants
        // would be read through a visitor that adds nothing.
        struct DeliveredAct final
        {
            std::string            verb{};
            std::optional<KeyName> key{};

            int32  notches{};
            uint64 holdMillis{};
        };

        // Records what reached the last port before a platform adapter. What
        // arrives here is exactly what ControllerActionSink would hand to the
        // OS, which is what makes "THAT key was delivered" a claim about the
        // whole chain rather than about the binding layer alone.
        class RecordingActionSink final : public engine::IActionSink
        {
            std::vector<DeliveredAct> m_acts{};

            [[nodiscard]]
            static auto millis(MonotonicInstant::Duration duration) -> uint64
            {
                return static_cast<uint64>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(duration)
                        .count()
                );
            }

        public:
            [[nodiscard]]
            auto click(Point<ClientSpace>, ObservationLease const&) -> Status override
            {
                m_acts.emplace_back(DeliveredAct{.verb = "click"});
                return ok();
            }

            [[nodiscard]]
            auto pressKey(KeyName key, TargetGeneration) -> Status override
            {
                m_acts.emplace_back(DeliveredAct{.verb = "key", .key = key});
                return ok();
            }

            [[nodiscard]]
            auto scroll(int32 notches, ObservationLease const&) -> Status override
            {
                m_acts.emplace_back(
                    DeliveredAct{.verb = "scroll", .notches = notches}
                );
                return ok();
            }

            [[nodiscard]]
            auto longPress(
                Point<ClientSpace>,
                MonotonicInstant::Duration hold,
                ObservationLease const&
            ) -> Status override
            {
                m_acts.emplace_back(
                    DeliveredAct{
                        .verb       = "long_press",
                        .holdMillis = millis(hold),
                    }
                );
                return ok();
            }

            [[nodiscard]]
            auto drag(
                Point<ClientSpace>,
                Point<ClientSpace>,
                MonotonicInstant::Duration travel,
                ObservationLease const&
            ) -> Status override
            {
                m_acts.emplace_back(
                    DeliveredAct{
                        .verb       = "drag",
                        .holdMillis = millis(travel),
                    }
                );
                return ok();
            }

            [[nodiscard]]
            auto movePointer(Point<ClientSpace>, ObservationLease const&)
                -> Status override
            {
                m_acts.emplace_back(DeliveredAct{.verb = "pointer_move"});
                return ok();
            }

            [[nodiscard]] auto targetWorld() const noexcept -> TargetWorld override
            {
                return TargetWorld::Recorded;
            }

            [[nodiscard]]
            auto acts() const noexcept -> std::vector<DeliveredAct> const&
            {
                return m_acts;
            }
        };

        // Keeps every event type and every payload field, because the property
        // under test is what a reader of the finished stream can establish.
        class RecordingTraceSink final : public trace::ITraceSink
        {
            std::vector<trace::TraceEvent> m_events{};

        public:
            [[nodiscard]] auto append(trace::TraceEvent const& event) -> Status override
            {
                m_events.emplace_back(event);
                return ok();
            }

            [[nodiscard]]
            auto events() const noexcept -> std::vector<trace::TraceEvent> const&
            {
                return m_events;
            }
        };

        // One exploration world: the recorder, the engine session over a
        // synthetic frame, the context, and the VM booted exactly as
        // ExplorationSession boots one. It is assembled here rather than through
        // ExplorationSession because that type owns a trace FILE, and these
        // cases read the stream rather than a path.
        class ExplorationWorld final
        {
            // Declared first and held indirectly: the session, the context and
            // the VM all borrow it, so it outlives them and keeps one address.
            std::unique_ptr<trace::TraceRecorder> m_recorder{};

            // Borrows into the two sinks above, which the recorder and the
            // session own. They are read after a chunk has run and never after
            // this object dies.
            RecordingTraceSink*  m_pTrace{};
            RecordingActionSink* m_pActions{};

            // The VM borrows the context, so it is declared after it and dies
            // before it -- the ordering task/script-bindings.hpp requires.
            std::optional<TaskContext>    m_context{};
            std::optional<script::Engine> m_vm{};

        public:
            explicit ExplorationWorld(
                std::vector<std::string> publishedGlobals = explorationProjectGlobals(),
                bool installAnnotationSurface = true
            )
            {
                auto trace = std::make_unique<RecordingTraceSink>();
                m_pTrace   = trace.get();
                auto recorder = trace::TraceRecorder::create(
                    std::move(trace),
                    trace::TraceStreamSpec{
                        .sessionId           = "exploration-verbs",
                        .sessionManifestHash = manifestHash("exploration-verbs"),
                        .producer            = "annotation",
                    }
                );
                REQUIRE(recorder.has_value());
                m_recorder = std::make_unique<trace::TraceRecorder>(
                    *std::move(recorder)
                );

                auto actions = std::make_unique<RecordingActionSink>();
                m_pActions   = actions.get();
                auto session = engine::EngineSession::create(
                    std::make_unique<FrameSource>(syntheticFrame()),
                    std::move(actions),
                    *m_recorder,
                    engine::EngineSessionConfig{
                        .liveFingerprint         = fingerprint(),
                        .projectFingerprint      = fingerprint(),
                        .maximumPixelComparisons = k_comparisonBudget,
                        .recognitionTimeout      = std::chrono::seconds{1},
                    }
                );
                REQUIRE(session.has_value());
                m_context.emplace(*std::move(session), *m_recorder);

                auto vm = script::Engine::create(
                    script::EngineConfig{
                        .frameworkModules  = frameworkScriptModules(),
                        .installHostTables = scriptHostTableInstaller(),
                        .installPrivateCapabilities = installAnnotationSurface
                            ? annotationPrivateCapabilities(*m_context)
                            : script::PrivateCapabilityInstaller{},
                        .projectGlobals          = scriptProjectGlobals(),
                        .frameworkProjectGlobals = std::move(publishedGlobals),
                        .classifyRaisedError     = scriptRaisedErrorClassifier(),
                    }
                );
                REQUIRE(vm.has_value());
                m_vm = *std::move(vm);
            }

            ExplorationWorld(ExplorationWorld const&) = delete;
            ExplorationWorld(ExplorationWorld&&) = delete;
            auto operator=(ExplorationWorld const&) -> ExplorationWorld& = delete;
            auto operator=(ExplorationWorld&&) -> ExplorationWorld& = delete;

            ~ExplorationWorld() = default;

            // The constructor emplaces both optionals on every path that
            // produces an object, so no reachable state has either disengaged.
            // NOLINTBEGIN(bugprone-unchecked-optional-access)
            [[nodiscard]] auto run(std::string_view chunk) -> Result<script::ScriptValue>
            {
                return m_vm->runValue(chunk, "exploration-verbs-chunk");
            }

            [[nodiscard]] auto context() noexcept -> TaskContext& { return *m_context; }
            // NOLINTEND(bugprone-unchecked-optional-access)

            [[nodiscard]]
            auto acts() const noexcept -> std::vector<DeliveredAct> const&
            {
                return m_pActions->acts();
            }

            [[nodiscard]]
            auto events() const noexcept -> std::vector<trace::TraceEvent> const&
            {
                return m_pTrace->events();
            }
        };

        [[nodiscard]]
        auto eventTypes(ExplorationWorld const& world) -> std::vector<std::string>
        {
            auto types = std::vector<std::string>{};
            for (auto const& event : world.events())
            {
                types.emplace_back(event.eventType());
            }
            return types;
        }

        [[nodiscard]]
        auto lineNamed(ExplorationWorld const& world, std::string_view eventType)
            -> std::optional<trace::TraceEvent>
        {
            auto const found = std::ranges::find(
                world.events(),
                eventType,
                [](trace::TraceEvent const& event) -> std::string_view
                {
                    return event.eventType();
                }
            );
            if (found == world.events().end())
            {
                return std::nullopt;
            }
            return *found;
        }

        [[nodiscard]]
        auto textField(trace::TraceEvent const& event, std::string_view name)
            -> std::optional<std::string>
        {
            for (auto const& field : event.payload().fields)
            {
                if (field.name != name)
                {
                    continue;
                }
                if (auto const* p_text = std::get_if<std::string>(&field.value))
                {
                    return *p_text;
                }
            }
            return std::nullopt;
        }

        [[nodiscard]]
        auto numberField(trace::TraceEvent const& event, std::string_view name)
            -> std::optional<uint64>
        {
            for (auto const& field : event.payload().fields)
            {
                if (field.name != name)
                {
                    continue;
                }
                if (auto const* p_value = std::get_if<uint64>(&field.value))
                {
                    return *p_value;
                }
            }
            return std::nullopt;
        }
    }

    TEST_CASE("an exploration chunk delivers every act it names")
    {
        auto world = ExplorationWorld{};

        // One cycle per act, because each spends the cycle it was called on.
        auto const result = world.run(
            R"lua(
                explore.cycle(function(view) view:click_point(1, 0) end)
                explore.cycle(function(view) view:long_press(1, 0, 40) end)
                explore.cycle(function(view) view:drag(0, 0, 2, 0, 25) end)
                explore.cycle(function(view) view:move_pointer(2, 0) end)
                explore.cycle(function(view) view:scroll(-3) end)
                explore.cycle(function(view) view:key("ENTER") end)
                return "done"
            )lua"
        );
        REQUIRE(result.has_value());

        auto verbs = std::vector<std::string>{};
        for (auto const& act : world.acts())
        {
            verbs.emplace_back(act.verb);
        }
        CHECK(
            verbs
            == std::vector<std::string>{
                "click",
                "long_press",
                "drag",
                "pointer_move",
                "scroll",
                "key",
            }
        );
        CHECK(world.acts()[1].holdMillis == 40U);
        CHECK(world.acts()[2].holdMillis == 25U);
        CHECK(world.acts()[4].notches == -3);
    }

    // The key the chunk wrote must be the key the sink received. A verb that
    // delivered a neighbouring key would still satisfy every count above.
    TEST_CASE("a delivered key is the key the chunk named")
    {
        auto world = ExplorationWorld{};
        auto const result = world.run(
            R"lua(
                explore.cycle(function(view) view:key("F7") end)
                return "done"
            )lua"
        );
        REQUIRE(result.has_value());

        REQUIRE(world.acts().size() == 1U);
        REQUIRE(world.acts().front().key.has_value());
        CHECK(world.acts().front().key->value() == "F7");

        auto const line = lineNamed(world, "annotation.key_delivered");
        REQUIRE(line.has_value());
        CHECK(textField(*line, "key") == std::optional<std::string>{"F7"});
    }

    // KeyName is the single definition of which names exist, and this surface
    // does not open it. A refused name must also cost no frame.
    TEST_CASE("a key outside the accepted set is refused before the cycle is spent")
    {
        auto world = ExplorationWorld{};
        auto const result = world.run(
            R"lua(
                local spent = explore.cycle(function(view)
                    local ok = pcall(function() view:key("TAB") end)
                    if ok then return "delivered" end
                    view:key("ESC")
                    return "recovered"
                end)
                return spent
            )lua"
        );
        REQUIRE(result.has_value());
        REQUIRE(result->text() != nullptr);
        CHECK(*result->text() == "recovered");

        REQUIRE(world.acts().size() == 1U);
        REQUIRE(world.acts().front().key.has_value());
        CHECK(world.acts().front().key->value() == "ESC");
    }

    TEST_CASE("every delivered act leaves its own line in the trace")
    {
        auto world = ExplorationWorld{};
        auto const result = world.run(
            R"lua(
                explore.cycle(function(view) view:click_point(1, 0) end)
                explore.cycle(function(view) view:long_press(1, 0, 40) end)
                explore.cycle(function(view) view:drag(0, 0, 2, 0, 25) end)
                explore.cycle(function(view) view:move_pointer(2, 0) end)
                explore.cycle(function(view) view:scroll(-3) end)
                explore.cycle(function(view) view:key("ENTER") end)
                return "done"
            )lua"
        );
        REQUIRE(result.has_value());

        auto const types = eventTypes(world);
        auto const expected = std::vector<std::string>{
            "annotation.click_delivered",
            "annotation.long_press_delivered",
            "annotation.drag_delivered",
            "annotation.pointer_move_delivered",
            "annotation.scroll_delivered",
            "annotation.key_delivered",
        };
        for (auto const& line : expected)
        {
            CHECK_MESSAGE(
                std::ranges::contains(types, line),
                "no trace line records this act: ",
                line
            );
        }

        // The line carries what the CHUNK wrote, in frame pixels, which is what
        // the engine's own client-space line cannot answer.
        auto const drag = lineNamed(world, "annotation.drag_delivered");
        REQUIRE(drag.has_value());
        CHECK(numberField(*drag, "pixel_x") == std::optional<uint64>{0});
        CHECK(numberField(*drag, "end_pixel_x") == std::optional<uint64>{2});
        CHECK(numberField(*drag, "travel_millis") == std::optional<uint64>{25});
    }

    // A refused act writes no delivered line: an auditor must never find one for
    // something that did not happen.
    TEST_CASE("an act that was never delivered writes no delivered line")
    {
        auto world = ExplorationWorld{};
        auto const result = world.run(
            R"lua(
                explore.cycle(function(view)
                    if pcall(function() view:long_press(1, 0, 600000) end) then
                        error("the ceiling did not refuse")
                    end
                end)
                return "done"
            )lua"
        );
        REQUIRE(result.has_value());

        CHECK(world.acts().empty());
        CHECK_FALSE(
            std::ranges::contains(eventTypes(world), "annotation.long_press_delivered")
        );
    }

    // One frame delivers at most one input. The ledger enforces it, so the
    // second act on one cycle is refused with the frame already gone.
    TEST_CASE("a cycle delivers one act and then refuses")
    {
        auto world = ExplorationWorld{};
        auto const result = world.run(
            R"lua(
                explore.cycle(function(view)
                    view:click_point(1, 0)
                    if pcall(function() view:click_point(1, 0) end) then
                        error("the spent cycle delivered twice")
                    end
                end)
                return "done"
            )lua"
        );
        REQUIRE(result.has_value());
        CHECK(world.acts().size() == 1U);
    }

    // Holding a frame across a wait is the whole failure the verb exists to
    // stop, so the refusal is the verb rather than a precaution around it.
    TEST_CASE("a settle is refused while a cycle holds a frame")
    {
        auto world = ExplorationWorld{};
        auto const refused = world.run(
            R"lua(
                return explore.cycle(function()
                    local ok, err = pcall(function() explore.settle(1) end)
                    if ok then return "settled" end
                    return tostring(err.message)
                end)
            )lua"
        );
        REQUIRE(refused.has_value());
        REQUIRE(refused->text() != nullptr);
        CHECK(
            std::string_view{*refused->text()}.find("while a cycle is open")
            != std::string_view::npos
        );

        // And it is a wait rather than a no-op once no cycle is open.
        auto const taken = world.run("explore.settle(15) return \"waited\"");
        REQUIRE(taken.has_value());
    }

    // The environment split, half one: which module each environment publishes.
    // `explore` is in exactly one whitelist, and nothing else publishes it.
    TEST_CASE("only the exploration whitelist publishes the acting module")
    {
        auto const exploration = explorationProjectGlobals();
        CHECK(std::ranges::contains(exploration, std::string{"explore"}));

        CHECK_MESSAGE(
            !std::ranges::contains(frameworkProjectGlobals(), std::string{"explore"}),
            "the business whitelist publishes the exploration acting module"
        );
        CHECK_MESSAGE(
            !std::ranges::contains(runtimeProjectGlobals(), std::string{"explore"}),
            "the Runtime plugin whitelist publishes the exploration acting module"
        );
    }

    // The same half, as the VM actually enforces it. The annotation surface is
    // installed here on purpose: this VM is handed every acting primitive and is
    // still unable to name one, so the whitelist alone is load-bearing.
    TEST_CASE("a Runtime-whitelist VM cannot name the acting module")
    {
        auto world = ExplorationWorld{runtimeProjectGlobals()};
        auto const result = world.run(
            R"lua(
                if explore ~= nil then return "reachable" end
                if native ~= nil or uf_private ~= nil then return "surface leaked" end
                return "isolated"
            )lua"
        );
        REQUIRE(result.has_value());
        REQUIRE(result->text() != nullptr);
        CHECK_MESSAGE(
            *result->text() == std::string{"isolated"},
            "a plugin environment reached the exploration acting surface: ",
            *result->text()
        );
        CHECK(world.acts().empty());
    }

    // The environment split, half two, and it is independent of the whitelist:
    // the acting primitives live on a table only annotationPrivateCapabilities
    // builds, so a VM published `explore` but booted without that installer
    // holds the module and can still deliver nothing.
    //
    // What is asserted is the REFUSAL's wording as well as its existence. A VM
    // that reached an unbound primitive raises whatever the interpreter says
    // about indexing nothing, and a reader of that message cannot tell a
    // misconfigured environment from a typo; the module's own sentence names the
    // primitive and says this VM is not an exploration VM.
    TEST_CASE("the acting module is inert without the annotation private surface")
    {
        auto world = ExplorationWorld{explorationProjectGlobals(), false};
        auto const result = world.run(
            R"lua(
                if explore == nil then return "module absent" end
                local ok, err = pcall(function()
                    explore.cycle(function() end)
                end)
                if ok then return "delivered" end
                return tostring(err)
            )lua"
        );
        REQUIRE(result.has_value());
        REQUIRE(result->text() != nullptr);
        CHECK_MESSAGE(
            std::string_view{*result->text()}.find("does not provide")
                != std::string_view::npos,
            "an unbound primitive was not refused by the module's own guard: ",
            *result->text()
        );
        CHECK_MESSAGE(
            std::string_view{*result->text()}.find("explore_cycle_open")
                != std::string_view::npos,
            "the refusal does not name the primitive that is missing: ",
            *result->text()
        );
        CHECK(world.acts().empty());
    }
}
