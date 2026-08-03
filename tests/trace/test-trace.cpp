#include <trace/event.hpp>
#include <trace/file-sink.hpp>
#include <trace/recorder.hpp>
#include <trace/sink.hpp>

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>

#include <vision/sad.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace uf::trace
{
    namespace
    {
        constexpr auto k_runId        = TaskRunId{7};
        constexpr auto k_generationId = GenerationId{3};

        [[nodiscard]]
        auto pixelRect(uint32 x, uint32 y, uint32 width, uint32 height) -> PixelRect
        {
            auto const rect = PixelRect::create(x, y, width, height);
            REQUIRE(rect.has_value());
            return *rect;
        }

        // Serializes every emit into a caller-owned buffer. The buffer outlives
        // the sink, which the recorder owns: RecordedRun declares it first.
        class CollectingTraceSink final : public ITraceSink
        {
            std::vector<std::string>* m_lines;

        public:
            explicit CollectingTraceSink(std::vector<std::string>* lines) noexcept
                : m_lines{lines}
            {
            }

            [[nodiscard]] auto emit(StampedTraceEvent const& event) -> Status override
            {
                m_lines->emplace_back(serializeTraceEvent(event));
                return ok();
            }
        };

        // Fails the first emit and records every later one into a caller-owned
        // buffer. Failing only once lets a test read what the recorder stamped
        // AFTER a lost event: the surviving seq is the only evidence the counter
        // advanced across the failure instead of reusing the lost number.
        class FailFirstTraceSink final : public ITraceSink
        {
            std::vector<std::string>* m_lines;
            bool                      m_failed{false};

        public:
            explicit FailFirstTraceSink(std::vector<std::string>* lines) noexcept
                : m_lines{lines}
            {
            }

            [[nodiscard]] auto emit(StampedTraceEvent const& event) -> Status override
            {
                if (!m_failed)
                {
                    m_failed = true;
                    return fail(
                        AutomationErrorKind::IoFailure,
                        "sink deliberately failing"
                    );
                }
                m_lines->emplace_back(serializeTraceEvent(event));
                return ok();
            }
        };

        // One recorder over a collecting sink. Nothing outside modules/trace can
        // build a StampedTraceEvent, so every serialized line goes through one.
        class RecordedRun final
        {
            std::vector<std::string> m_lines{};
            TraceRecorder            m_recorder;

        public:
            explicit RecordedRun(FrontEnd frontEnd = FrontEnd::Task)
                : m_recorder{
                      std::make_unique<CollectingTraceSink>(&m_lines),
                      k_runId,
                      k_generationId,
                      frontEnd,
                  }
            {
            }

            [[nodiscard]] auto emit(TraceEvent const& event) -> Status
            {
                return m_recorder.emit(event);
            }

            [[nodiscard]]
            auto lines() const noexcept UF_LIFETIME_BOUND
                -> std::vector<std::string> const&
            {
                return m_lines;
            }
        };

        // The golden form of one event: emitted through a fresh recorder, so the
        // sequence is always 1, then stripped of the non-golden `meta` member.
        [[nodiscard]]
        auto goldenLine(TraceEvent const& event) -> std::string
        {
            auto run = RecordedRun{};
            REQUIRE(run.emit(event).has_value());
            REQUIRE(run.lines().size() == 1U);
            return stripNonGoldenFields(run.lines().back());
        }

        // The wall clock recorded in one serialized line's non-golden meta member.
        [[nodiscard]]
        auto wallClockOf(std::string_view line) -> int64
        {
            auto constexpr member = std::string_view{"\"meta\":{\"wallClock\":"};
            auto const start = line.find(member);
            REQUIRE(start != std::string_view::npos);

            auto const digits = line.substr(start + member.size());
            auto value        = int64{0};
            auto const parsed = std::from_chars(
                digits.data(),
                digits.data() + digits.size(),
                value
            );
            REQUIRE(parsed.ec == std::errc{});
            return value;
        }

        [[nodiscard]]
        auto readLines(std::filesystem::path const& path) -> std::vector<std::string>
        {
            auto stream = std::ifstream{path, std::ios::binary};
            REQUIRE(stream.is_open());
            auto lines = std::vector<std::string>{};
            auto line  = std::string{};
            while (std::getline(stream, line))
            {
                lines.emplace_back(line);
            }
            return lines;
        }

        [[nodiscard]]
        auto uniqueTracePath() -> std::filesystem::path
        {
            static auto counter = 0;
            ++counter;
            return std::filesystem::temp_directory_path()
                / ("uf-trace-" + std::to_string(counter) + ".jsonl");
        }
    }

    TEST_CASE("serializeTraceEvent emits every populated group in schema order")
    {
        // Deliberately synthetic: no real event carries a page group, an action
        // group and a click at once. It pins the field order the golden lines
        // below depend on, so a reordering breaks here once rather than everywhere.
        auto event = TraceEvent{
            .kind  = TraceEventKind::EngineActionFound,
            .frame = FrameIdentity{
                CaptureSessionId{uint64{7}},
                TargetGeneration::fromValue(3),
                FrameId{uint64{42}},
            },
            .action = TraceEvent::Action{
                .outcome     = ActionSearch::Found,
                .sadScore    = uint64{1234},
                .maximumSad  = uint64{5000},
                .matchedRect = pixelRect(10, 20, 30, 40),
            },
            .run = TraceEvent::Run{
                .projectId        = "personal.game",
                .taskName         = "daily",
                .sourceHash       = "abc123",
                .frameworkVersion = "0.1.0",
                .frameworkHash    = "def456",
                .luauVersion      = "6",
                .seed             = uint64{42},
            },
            .resources = TraceEvent::Resources{
                .elements = {"accept"},
                .pages    = {"home"},
            },
            .nativeCall = TraceEvent::NativeCall{
                .verb            = "click",
                .outcome         = NativeCallOutcome::Succeeded,
                .cycleOrdinal    = uint64{4},
                .hitCycleOrdinal = uint64{5},
            },
            .templateHash = std::string{"sha256:abcdef"},
            .stopReason   = SadSearchStopReason::TimedOut,
            .runOutcome   = RunOutcome::Completed,
            .errorKind    = AutomationErrorKind::RecognitionIncomplete,
            .message      = std::string{"hello"},
            .clickClient  = Point<ClientSpace>{128.0F, 64.0F},
        };

        auto constexpr expected = std::string_view{
            "{\"schema\":\"umbraflow-trace/v3\",\"kind\":\"engine.action_found\""
            ",\"seq\":1,\"runId\":7,\"generationId\":3,\"frontEnd\":\"task\""
            ",\"frameId\":42,\"sessionId\":7,\"targetGeneration\":3"
            ",\"actionOutcome\":\"Found\",\"sadScore\":1234,\"maximumSad\":5000"
            ",\"matchedRect\":{\"x\":10,\"y\":20,\"width\":30,\"height\":40}"
            ",\"projectId\":\"personal.game\",\"taskName\":\"daily\""
            ",\"sourceHash\":\"abc123\",\"frameworkVersion\":\"0.1.0\""
            ",\"frameworkHash\":\"def456\",\"luauVersion\":\"6\",\"seed\":42"
            ",\"elements\":[\"accept\"],\"pages\":[\"home\"]"
            ",\"verb\":\"click\",\"cycleOrdinal\":4,\"hitCycleOrdinal\":5"
            ",\"outcome\":\"Succeeded\""
            ",\"templateHash\":\"sha256:abcdef\""
            ",\"stopReason\":\"TimedOut\",\"runOutcome\":\"Completed\""
            ",\"errorKind\":\"recognition_incomplete\",\"message\":\"hello\""
            ",\"clickClientX\":128,\"clickClientY\":64}"
        };

        CHECK(goldenLine(event) == expected);
    }

    TEST_CASE("serializeTraceEvent emits only the stamp for a minimal event")
    {
        auto const event = TraceEvent{.kind = TraceEventKind::EngineActionAuthorized};

        auto constexpr expected = std::string_view{
            "{\"schema\":\"umbraflow-trace/v3\",\"kind\":\"engine.action_authorized\""
            ",\"seq\":1,\"runId\":7,\"generationId\":3,\"frontEnd\":\"task\"}"
        };

        CHECK(goldenLine(event) == expected);
    }

    TEST_CASE("every front-end reaches the wire under a spelling of its own")
    {
        // The stamp is the only thing on a line that says who produced it, so two
        // front-ends sharing a spelling would merge two streams a reader has to
        // tell apart. The table is exhaustive by construction: frontEndWireName
        // switches with no default, so an unspelled value fails to compile.
        auto const spellings = std::array<std::pair<FrontEnd, std::string_view>, 3>{
            {
                {FrontEnd::Task, "task"},
                {FrontEnd::Operator, "operator"},
                {FrontEnd::Annotation, "annotation"},
            }
        };

        auto seen = std::vector<std::string_view>{};
        for (auto const& [frontEnd, spelling] : spellings)
        {
            CAPTURE(spelling);
            CHECK(frontEndWireName(frontEnd) == spelling);
            CHECK(std::ranges::find(seen, spelling) == seen.end());
            seen.emplace_back(spelling);

            auto run = RecordedRun{frontEnd};
            REQUIRE(
                run.emit(
                       TraceEvent{
                           .kind = TraceEventKind::EngineActionAuthorized,
                       }
                )
                    .has_value()
            );
            REQUIRE(run.lines().size() == 1U);
            CHECK(
                run.lines().back().contains(
                    std::format("\"frontEnd\":\"{}\"", spelling)
                )
            );
        }
    }

    TEST_CASE("serializeTraceEvent escapes quotes, backslashes, and control bytes")
    {
        auto message = std::string{"a\"b\\c\n"};
        message.push_back(static_cast<char>(0x01));

        auto const event = TraceEvent{
            .kind    = TraceEventKind::EngineActionRejected,
            .message = message,
        };

        auto constexpr expected = std::string_view{
            "{\"schema\":\"umbraflow-trace/v3\",\"kind\":\"engine.action_rejected\""
            ",\"seq\":1,\"runId\":7,\"generationId\":3,\"frontEnd\":\"task\""
            ",\"message\":\"a\\\"b\\\\c\\n\\u0001\"}"
        };

        CHECK(goldenLine(event) == expected);
    }

    TEST_CASE("serializeTraceEvent emits a full run.started in schema order")
    {
        auto const event = TraceEvent{
            .kind = TraceEventKind::RunStarted,
            .run  = TraceEvent::Run{
                .projectId        = "personal.game",
                .taskName         = "daily",
                .sourceHash       = "abc123",
                .frameworkVersion = "0.1.0",
                .frameworkHash    = "def456",
                .luauVersion      = "6",
                .seed             = uint64{42},
            },
        };

        auto constexpr expected = std::string_view{
            "{\"schema\":\"umbraflow-trace/v3\",\"kind\":\"run.started\""
            ",\"seq\":1,\"runId\":7,\"generationId\":3,\"frontEnd\":\"task\""
            ",\"projectId\":\"personal.game\",\"taskName\":\"daily\""
            ",\"sourceHash\":\"abc123\",\"frameworkVersion\":\"0.1.0\""
            ",\"frameworkHash\":\"def456\",\"luauVersion\":\"6\",\"seed\":42}"
        };

        CHECK(goldenLine(event) == expected);
    }

    TEST_CASE("run.started distinguishes two runs of the same task on framework build")
    {
        // Why the framework version and bundle hash are stamped: a task whose own
        // bytes did not change, run against a rebuilt framework, must not produce
        // the same line.
        auto run = TraceEvent::Run{
            .projectId        = "personal.game",
            .taskName         = "daily",
            .sourceHash       = "abc123",
            .frameworkVersion = "0.1.0",
            .frameworkHash    = "def456",
            .luauVersion      = "6",
            .seed             = uint64{42},
        };
        auto const first = goldenLine(
            TraceEvent{.kind = TraceEventKind::RunStarted, .run = run}
        );

        run.frameworkHash = "999999";
        auto const second = goldenLine(
            TraceEvent{.kind = TraceEventKind::RunStarted, .run = run}
        );

        CHECK(first != second);
    }

    TEST_CASE("serializeTraceEvent sorts the resource name lists before emitting")
    {
        // The lists arrive unordered on purpose: an unordered container's iteration
        // order must never reach the wire (a determinism-ledger constraint).
        auto const event = TraceEvent{
            .kind      = TraceEventKind::RunResourcesValidated,
            .resources = TraceEvent::Resources{
                .elements = {"battle", "accept", "daily"},
                .pages    = {"main", "home"},
            },
        };

        auto constexpr expected = std::string_view{
            "{\"schema\":\"umbraflow-trace/v3\",\"kind\":\"run.resources_validated\""
            ",\"seq\":1,\"runId\":7,\"generationId\":3,\"frontEnd\":\"task\""
            ",\"elements\":[\"accept\",\"battle\",\"daily\"]"
            ",\"pages\":[\"home\",\"main\"]}"
        };

        CHECK(goldenLine(event) == expected);
    }

    TEST_CASE("serializeTraceEvent emits an empty resource list as an empty array")
    {
        auto const event = TraceEvent{
            .kind      = TraceEventKind::RunResourcesValidated,
            .resources = TraceEvent::Resources{},
        };

        auto constexpr expected = std::string_view{
            "{\"schema\":\"umbraflow-trace/v3\",\"kind\":\"run.resources_validated\""
            ",\"seq\":1,\"runId\":7,\"generationId\":3,\"frontEnd\":\"task\""
            ",\"elements\":[],\"pages\":[]}"
        };

        CHECK(goldenLine(event) == expected);
    }

    TEST_CASE("serializeTraceEvent records every task.native_call outcome")
    {
        // Succeeded and Empty both completed -- Empty is the Tier A completed miss
        // -- and a failure names its kind in the snake_case spelling the script saw.
        SUBCASE("succeeded")
        {
            // capture mints its own sequence, so its line carries no argument id.
            auto const event = TraceEvent{
                .kind       = TraceEventKind::TaskNativeCall,
                .nativeCall = TraceEvent::NativeCall{
                    .verb    = "capture",
                    .outcome = NativeCallOutcome::Succeeded,
                },
            };

            CHECK(
                goldenLine(event)
                == "{\"schema\":\"umbraflow-trace/v3\",\"kind\":\"task.native_call\""
                   ",\"seq\":1,\"runId\":7,\"generationId\":3,\"frontEnd\":\"task\""
                   ",\"verb\":\"capture\",\"outcome\":\"Succeeded\"}"
            );
        }

        SUBCASE("empty")
        {
            auto const event = TraceEvent{
                .kind       = TraceEventKind::TaskNativeCall,
                .nativeCall = TraceEvent::NativeCall{
                    .verb         = "cycle_match",
                    .outcome      = NativeCallOutcome::Empty,
                    .cycleOrdinal = uint64{2},
                },
                .templateHash = std::string{"sha256:abcdef"},
            };

            CHECK(
                goldenLine(event)
                == "{\"schema\":\"umbraflow-trace/v3\",\"kind\":\"task.native_call\""
                   ",\"seq\":1,\"runId\":7,\"generationId\":3,\"frontEnd\":\"task\""
                   ",\"verb\":\"cycle_match\",\"cycleOrdinal\":2"
                   ",\"outcome\":\"Empty\",\"templateHash\":\"sha256:abcdef\"}"
            );
        }

        SUBCASE("failed")
        {
            // The failure a host-side guard produces: the ledger rejects the frame
            // before the engine sees it, so this is the ONLY line the failure
            // writes, and its two sequences the only record of the frames used.
            auto const event = TraceEvent{
                .kind       = TraceEventKind::TaskNativeCall,
                .nativeCall = TraceEvent::NativeCall{
                    .verb            = "click",
                    .outcome         = NativeCallOutcome::Failed,
                    .cycleOrdinal    = uint64{3},
                    .hitCycleOrdinal = uint64{4},
                },
                .errorKind  = AutomationErrorKind::StaleObservation,
            };

            CHECK(
                goldenLine(event)
                == "{\"schema\":\"umbraflow-trace/v3\",\"kind\":\"task.native_call\""
                   ",\"seq\":1,\"runId\":7,\"generationId\":3,\"frontEnd\":\"task\""
                   ",\"verb\":\"click\",\"cycleOrdinal\":3,\"hitCycleOrdinal\":4"
                   ",\"outcome\":\"Failed\",\"errorKind\":\"stale_observation\"}"
            );
        }
    }

    TEST_CASE("serializeTraceEvent records every run.finished outcome")
    {
        SUBCASE("completed")
        {
            auto const event = TraceEvent{
                .kind       = TraceEventKind::RunFinished,
                .runOutcome = RunOutcome::Completed,
            };

            CHECK(
                goldenLine(event)
                == "{\"schema\":\"umbraflow-trace/v3\",\"kind\":\"run.finished\""
                   ",\"seq\":1,\"runId\":7,\"generationId\":3,\"frontEnd\":\"task\""
                   ",\"runOutcome\":\"Completed\"}"
            );
        }

        SUBCASE("failed with its error kind")
        {
            auto const event = TraceEvent{
                .kind       = TraceEventKind::RunFinished,
                .runOutcome = RunOutcome::Failed,
                .errorKind  = AutomationErrorKind::Timeout,
            };

            CHECK(
                goldenLine(event)
                == "{\"schema\":\"umbraflow-trace/v3\",\"kind\":\"run.finished\""
                   ",\"seq\":1,\"runId\":7,\"generationId\":3,\"frontEnd\":\"task\""
                   ",\"runOutcome\":\"Failed\",\"errorKind\":\"timeout\"}"
            );
        }

        SUBCASE("cancelled")
        {
            auto const event = TraceEvent{
                .kind       = TraceEventKind::RunFinished,
                .runOutcome = RunOutcome::Cancelled,
            };

            CHECK(
                goldenLine(event)
                == "{\"schema\":\"umbraflow-trace/v3\",\"kind\":\"run.finished\""
                   ",\"seq\":1,\"runId\":7,\"generationId\":3,\"frontEnd\":\"task\""
                   ",\"runOutcome\":\"Cancelled\"}"
            );
        }
    }

    TEST_CASE("engine.observed pins its wire kind name and the frame join key")
    {
        // umbraflow-trace/v3 is a wire contract, so every kind name is pinned by a
        // full line somewhere; otherwise a typo in one ships in silence.
        auto const event = TraceEvent{
            .kind  = TraceEventKind::EngineObserved,
            .frame = FrameIdentity{
                CaptureSessionId{uint64{7}},
                TargetGeneration::fromValue(3),
                FrameId{uint64{17}},
            },
        };

        CHECK(
            goldenLine(event)
            == "{\"schema\":\"umbraflow-trace/v3\",\"kind\":\"engine.observed\""
               ",\"seq\":1,\"runId\":7,\"generationId\":3,\"frontEnd\":\"task\""
               ",\"frameId\":17,\"sessionId\":7,\"targetGeneration\":3}"
        );
    }

    TEST_CASE("engine.observation_invalidated pins its wire kind name")
    {
        auto const event = TraceEvent{
            .kind  = TraceEventKind::EngineObservationInvalidated,
            .frame = FrameIdentity{
                CaptureSessionId{uint64{7}},
                TargetGeneration::fromValue(3),
                FrameId{uint64{17}},
            },
        };

        CHECK(
            goldenLine(event)
            == "{\"schema\":\"umbraflow-trace/v3\""
               ",\"kind\":\"engine.observation_invalidated\""
               ",\"seq\":1,\"runId\":7,\"generationId\":3,\"frontEnd\":\"task\""
               ",\"frameId\":17,\"sessionId\":7,\"targetGeneration\":3}"
        );
    }

    TEST_CASE("engine.scroll_delivered pins its wire kind name and its delta")
    {
        // The delta reaches the wire as a signed literal because a reader sums and
        // compares it. Both directions are pinned: a sign dropped between the verb
        // and this line would turn every scroll up into a scroll down unnoticed.
        auto event = TraceEvent{
            .kind  = TraceEventKind::EngineScrollDelivered,
            .frame = FrameIdentity{
                CaptureSessionId{uint64{7}},
                TargetGeneration::fromValue(3),
                FrameId{uint64{17}},
            },
        };
        event.wheelNotches = int32{-2};

        CHECK(
            goldenLine(event)
            == "{\"schema\":\"umbraflow-trace/v3\""
               ",\"kind\":\"engine.scroll_delivered\""
               ",\"seq\":1,\"runId\":7,\"generationId\":3,\"frontEnd\":\"task\""
               ",\"frameId\":17,\"sessionId\":7,\"targetGeneration\":3"
               ",\"wheelNotches\":-2}"
        );

        event.wheelNotches = int32{3};
        CHECK(goldenLine(event).find("\"wheelNotches\":3}") != std::string::npos);
    }

    TEST_CASE("engine.pointer_move_delivered pins its wire kind name and its point")
    {
        // Its own kind and not an engine.action_delivered at the same coordinate:
        // a reader summing delivered clicks would otherwise count a message that
        // pressed nothing. The point rides the member the click and the long press
        // already share, so a consumer joining a move to the scroll it primed
        // reads one spelling rather than three.
        auto event = TraceEvent{
            .kind  = TraceEventKind::EnginePointerMoveDelivered,
            .frame = FrameIdentity{
                CaptureSessionId{uint64{7}},
                TargetGeneration::fromValue(3),
                FrameId{uint64{17}},
            },
        };
        event.clickClient = Point<ClientSpace>{4.0F, 2.0F};

        CHECK(
            goldenLine(event)
            == "{\"schema\":\"umbraflow-trace/v3\""
               ",\"kind\":\"engine.pointer_move_delivered\""
               ",\"seq\":1,\"runId\":7,\"generationId\":3,\"frontEnd\":\"task\""
               ",\"frameId\":17,\"sessionId\":7,\"targetGeneration\":3"
               ",\"clickClientX\":4,\"clickClientY\":2}"
        );
    }

    TEST_CASE("engine.action_found keeps every outcome the old kinds distinguished")
    {
        auto const frame = FrameIdentity{
            CaptureSessionId{uint64{7}},
            TargetGeneration::fromValue(3),
            FrameId{uint64{17}},
        };
        auto constexpr prefix = std::string_view{
            "{\"schema\":\"umbraflow-trace/v3\",\"kind\":\"engine.action_found\""
            ",\"seq\":1,\"runId\":7,\"generationId\":3,\"frontEnd\":\"task\""
            ",\"frameId\":17,\"sessionId\":7,\"targetGeneration\":3"
        };

        SUBCASE("found carries the scores and the matched rect")
        {
            auto const event = TraceEvent{
                .kind  = TraceEventKind::EngineActionFound,
                .frame = frame,
                .action = TraceEvent::Action{
                    .outcome     = ActionSearch::Found,
                    .sadScore    = uint64{1234},
                    .maximumSad  = uint64{5000},
                    .matchedRect = pixelRect(10, 20, 30, 40),
                },
            };

            CHECK(
                goldenLine(event)
                == std::string{prefix}
                    + ",\"actionOutcome\":\"Found\",\"sadScore\":1234"
                      ",\"maximumSad\":5000"
                      ",\"matchedRect\":{\"x\":10,\"y\":20,\"width\":30,\"height\":40}}"
            );
        }

        SUBCASE("absent keeps the scores that prove the search ran")
        {
            auto const event = TraceEvent{
                .kind  = TraceEventKind::EngineActionFound,
                .frame = frame,
                .action = TraceEvent::Action{
                    .outcome    = ActionSearch::Absent,
                    .sadScore   = uint64{9000},
                    .maximumSad = uint64{5000},
                },
            };

            CHECK(
                goldenLine(event)
                == std::string{prefix}
                    + ",\"actionOutcome\":\"Absent\",\"sadScore\":9000"
                      ",\"maximumSad\":5000}"
            );
        }
    }

    TEST_CASE("stripNonGoldenFields removes only the meta member")
    {
        auto constexpr withEarlyClock = std::string_view{
            "{\"schema\":\"umbraflow-trace/v3\",\"kind\":\"run.started\""
            ",\"seq\":1,\"meta\":{\"wallClock\":1000}}"
        };
        auto constexpr withLateClock = std::string_view{
            "{\"schema\":\"umbraflow-trace/v3\",\"kind\":\"run.started\""
            ",\"seq\":1,\"meta\":{\"wallClock\":999999}}"
        };
        auto constexpr withOtherSeq = std::string_view{
            "{\"schema\":\"umbraflow-trace/v3\",\"kind\":\"run.started\""
            ",\"seq\":2,\"meta\":{\"wallClock\":1000}}"
        };

        // Two lines that differ only inside meta are the same record.
        CHECK(stripNonGoldenFields(withEarlyClock) == stripNonGoldenFields(withLateClock));
        CHECK(
            stripNonGoldenFields(withEarlyClock)
            == "{\"schema\":\"umbraflow-trace/v3\",\"kind\":\"run.started\",\"seq\":1}"
        );

        // A difference anywhere else survives, so the helper cannot mask one.
        CHECK(stripNonGoldenFields(withEarlyClock) != stripNonGoldenFields(withOtherSeq));

        // A leading meta drops its trailing comma instead, and a line without a
        // meta member is returned unchanged.
        CHECK(
            stripNonGoldenFields("{\"meta\":{\"wallClock\":1},\"seq\":1}")
            == "{\"seq\":1}"
        );
        CHECK(stripNonGoldenFields("{\"seq\":1}") == "{\"seq\":1}");
        CHECK(stripNonGoldenFields("not json") == "not json");

        // A `meta` spelling inside a string value is not a top-level member and
        // must survive, so a message quoting the schema is not corrupted.
        CHECK(
            stripNonGoldenFields("{\"message\":\"\\\"meta\\\":1\",\"seq\":1}")
            == "{\"message\":\"\\\"meta\\\":1\",\"seq\":1}"
        );
    }

    TEST_CASE("TraceRecorder stamps a monotonic sequence and the run identity")
    {
        auto run = RecordedRun{};
        REQUIRE(run.emit(TraceEvent{.kind = TraceEventKind::RunStarted}).has_value());
        REQUIRE(run.emit(TraceEvent{.kind = TraceEventKind::EngineObserved}).has_value());
        REQUIRE(run.emit(TraceEvent{.kind = TraceEventKind::RunFinished}).has_value());

        REQUIRE(run.lines().size() == 3U);
        for (auto index = std::size_t{0}; index < run.lines().size(); ++index)
        {
            auto const& line = run.lines()[index];
            CHECK(line.find("\"seq\":" + std::to_string(index + 1U)) != std::string::npos);
            CHECK(line.find("\"runId\":7") != std::string::npos);
            CHECK(line.find("\"generationId\":3,\"frontEnd\":\"task\"") != std::string::npos);
            CHECK(line.find("\"meta\":{\"wallClock\":") != std::string::npos);

            // A frozen clock, or one stubbed to zero, satisfies every other
            // assertion in this suite: nothing else reads the value. A lower bound
            // rather than a window keeps the case independent of the run's length.
            constexpr auto k_epoch2020Millis = int64{1'577'836'800'000};
            CHECK(wallClockOf(line) > k_epoch2020Millis);
        }
    }

    TEST_CASE("TraceRecorder surfaces a sink failure and still advances the sequence")
    {
        // The buffer outlives the sink the recorder owns, so it is declared first.
        auto lines    = std::vector<std::string>{};
        auto recorder = TraceRecorder{
            std::make_unique<FailFirstTraceSink>(&lines),
            k_runId,
            k_generationId,
            FrontEnd::Task,
        };

        auto const first = recorder.emit(TraceEvent{.kind = TraceEventKind::RunStarted});
        REQUIRE_FALSE(first.has_value());
        CHECK(automationErrorKind(first.error()) == AutomationErrorKind::IoFailure);
        CHECK(lines.empty());

        // A lost event leaves a visible gap rather than a silently renumbered
        // stream: the event after the failure is stamped 2, so a reader sees one
        // line missing. A recorder that advanced only on success would stamp 1.
        REQUIRE(
            recorder
                .emit(
                    TraceEvent{
                        .kind       = TraceEventKind::RunFinished,
                        .runOutcome = RunOutcome::Completed,
                    }
                )
                .has_value()
        );
        REQUIRE(lines.size() == 1U);
        CHECK(
            stripNonGoldenFields(lines.front())
            == "{\"schema\":\"umbraflow-trace/v3\",\"kind\":\"run.finished\""
               ",\"seq\":2,\"runId\":7,\"generationId\":3,\"frontEnd\":\"task\""
               ",\"runOutcome\":\"Completed\"}"
        );
    }

    TEST_CASE("FileTraceSink appends one stamped JSONL line per emit")
    {
        auto const path = uniqueTracePath();
        std::filesystem::remove(path);

        {
            auto sink = FileTraceSink::create(path);
            REQUIRE(sink.has_value());
            auto recorder = TraceRecorder{
                *std::move(sink),
                k_runId,
                k_generationId,
                FrontEnd::Task,
            };
            CHECK(
                recorder
                    .emit(
                        TraceEvent{
                            .kind = TraceEventKind::RunStarted,
                            .run  = TraceEvent::Run{
                                .projectId        = "personal.game",
                                .taskName         = "daily",
                                .sourceHash       = "abc123",
                                .frameworkVersion = "0.1.0",
                                .frameworkHash    = "def456",
                                .luauVersion      = "6",
                                .seed             = uint64{42},
                            },
                        }
                    )
                    .has_value()
            );
            CHECK(
                recorder
                    .emit(
                        TraceEvent{
                            .kind       = TraceEventKind::RunFinished,
                            .runOutcome = RunOutcome::Completed,
                        }
                    )
                    .has_value()
            );
        }

        auto const lines = readLines(path);
        REQUIRE(lines.size() == 2U);
        CHECK(
            stripNonGoldenFields(lines[0])
            == "{\"schema\":\"umbraflow-trace/v3\",\"kind\":\"run.started\""
               ",\"seq\":1,\"runId\":7,\"generationId\":3,\"frontEnd\":\"task\""
               ",\"projectId\":\"personal.game\",\"taskName\":\"daily\""
               ",\"sourceHash\":\"abc123\",\"frameworkVersion\":\"0.1.0\""
               ",\"frameworkHash\":\"def456\",\"luauVersion\":\"6\",\"seed\":42}"
        );
        CHECK(
            stripNonGoldenFields(lines[1])
            == "{\"schema\":\"umbraflow-trace/v3\",\"kind\":\"run.finished\""
               ",\"seq\":2,\"runId\":7,\"generationId\":3,\"frontEnd\":\"task\""
               ",\"runOutcome\":\"Completed\"}"
        );

        std::filesystem::remove(path);
    }

    TEST_CASE("FileTraceSink reports an unopenable trace path as an error Status")
    {
        auto const path = std::filesystem::temp_directory_path()
            / "uf-trace-missing-dir"
            / "trace.jsonl";
        std::filesystem::remove_all(path.parent_path());

        auto const sink = FileTraceSink::create(path);
        REQUIRE_FALSE(sink.has_value());
        CHECK(automationErrorKind(sink.error()) == AutomationErrorKind::IoFailure);
    }
}
