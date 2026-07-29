#include <trace/event.hpp>
#include <trace/file-sink.hpp>
#include <trace/recorder.hpp>
#include <trace/sink.hpp>

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <annotation/recognition.hpp>
#include <annotation/resource.hpp>

#include <domain/error.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>

#include <vision/sad.hpp>

#include <doctest/doctest.h>

#include <charconv>
#include <cstddef>
#include <filesystem>
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
        auto resourceId(std::string_view value) -> annotation::ResourceId
        {
            auto const parsed = annotation::ResourceId::parse(value);
            REQUIRE(parsed.has_value());
            return *parsed;
        }

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
        // buffer. Failing only once is what lets a test read back what the
        // recorder stamped AFTER a lost event: the surviving line's own seq is the
        // only evidence that the counter advanced across the failure instead of
        // reusing the number the failed event was given.
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
        // build a StampedTraceEvent, so every test that needs a serialized line
        // goes through a recorder -- which is the property under test.
        class RecordedRun final
        {
            std::vector<std::string> m_lines{};
            TraceRecorder            m_recorder;

        public:
            RecordedRun()
                : m_recorder{
                      std::make_unique<CollectingTraceSink>(&m_lines),
                      k_runId,
                      k_generationId,
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
        // below depend on, so a reordering breaks here once rather than
        // everywhere.
        auto event = TraceEvent{
            .kind  = TraceEventKind::EngineActionFound,
            .frame = annotation::FrameIdentity{
                CaptureSessionId{uint64{7}},
                TargetGeneration::fromValue(3),
                FrameId{uint64{42}},
            },
            .page = TraceEvent::Page{
                .outcome = PageResolution::Resolved,
                .pageId  = annotation::PageId{
                    resourceId("11111111-2222-3333-4444-555555555555")
                },
                .scores  = {
                    TraceEvent::Page::Score{
                        .pageId    = annotation::PageId{
                            resourceId("11111111-2222-3333-4444-555555555555")
                        },
                        .candidate   = true,
                        .worstAnchor = annotation::RecognizerId{
                            resourceId("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")
                        },
                        .worstAnchorSad        = uint64{7},
                        .worstAnchorMaximumSad = uint64{255},
                    },
                    TraceEvent::Page::Score{
                        .pageId = annotation::PageId{
                            resourceId("22222222-3333-4444-5555-666666666666")
                        },
                        .candidate = false,
                    },
                },
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
                .recognizers = {"accept"},
                .pages       = {"home"},
            },
            .nativeCall = TraceEvent::NativeCall{
                .verb            = "click",
                .outcome         = NativeCallOutcome::Succeeded,
                .cycleOrdinal    = uint64{4},
                .hitCycleOrdinal = uint64{5},
            },
            .recognizerId = annotation::RecognizerId{
                resourceId("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")
            },
            .stopReason  = SadSearchStopReason::TimedOut,
            .runOutcome  = RunOutcome::Completed,
            .errorKind   = AutomationErrorKind::RecognitionFailed,
            .message     = std::string{"hello"},
            .clickClient = Point<ClientSpace>{128.0F, 64.0F},
        };

        auto constexpr expected = std::string_view{
            "{\"schema\":\"umbraflow-trace/v1\",\"kind\":\"engine.action_found\""
            ",\"seq\":1,\"runId\":7,\"generationId\":3"
            ",\"frameId\":42,\"sessionId\":7,\"targetGeneration\":3"
            ",\"pageOutcome\":\"Resolved\""
            ",\"pageId\":\"11111111-2222-3333-4444-555555555555\""
            ",\"pageScores\":["
            "{\"pageId\":\"11111111-2222-3333-4444-555555555555\",\"candidate\":true"
            ",\"worstAnchor\":\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\""
            ",\"worstAnchorSad\":7,\"worstAnchorMaximumSad\":255}"
            ",{\"pageId\":\"22222222-3333-4444-5555-666666666666\""
            ",\"candidate\":false}]"
            ",\"actionOutcome\":\"Found\",\"sadScore\":1234,\"maximumSad\":5000"
            ",\"matchedRect\":{\"x\":10,\"y\":20,\"width\":30,\"height\":40}"
            ",\"projectId\":\"personal.game\",\"taskName\":\"daily\""
            ",\"sourceHash\":\"abc123\",\"frameworkVersion\":\"0.1.0\""
            ",\"frameworkHash\":\"def456\",\"luauVersion\":\"6\",\"seed\":42"
            ",\"recognizers\":[\"accept\"],\"pages\":[\"home\"]"
            ",\"verb\":\"click\",\"cycleOrdinal\":4,\"hitCycleOrdinal\":5"
            ",\"outcome\":\"Succeeded\""
            ",\"recognizerId\":\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\""
            ",\"stopReason\":\"TimedOut\",\"runOutcome\":\"Completed\""
            ",\"errorKind\":\"recognition_failed\",\"message\":\"hello\""
            ",\"clickClientX\":128,\"clickClientY\":64}"
        };

        CHECK(goldenLine(event) == expected);
    }

    TEST_CASE("serializeTraceEvent emits only the stamp for a minimal event")
    {
        auto const event = TraceEvent{.kind = TraceEventKind::EngineActionAuthorized};

        auto constexpr expected = std::string_view{
            "{\"schema\":\"umbraflow-trace/v1\",\"kind\":\"engine.action_authorized\""
            ",\"seq\":1,\"runId\":7,\"generationId\":3}"
        };

        CHECK(goldenLine(event) == expected);
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
            "{\"schema\":\"umbraflow-trace/v1\",\"kind\":\"engine.action_rejected\""
            ",\"seq\":1,\"runId\":7,\"generationId\":3"
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
            "{\"schema\":\"umbraflow-trace/v1\",\"kind\":\"run.started\""
            ",\"seq\":1,\"runId\":7,\"generationId\":3"
            ",\"projectId\":\"personal.game\",\"taskName\":\"daily\""
            ",\"sourceHash\":\"abc123\",\"frameworkVersion\":\"0.1.0\""
            ",\"frameworkHash\":\"def456\",\"luauVersion\":\"6\",\"seed\":42}"
        };

        CHECK(goldenLine(event) == expected);
    }

    TEST_CASE("run.started distinguishes two runs of the same task on framework build")
    {
        // The whole point of stamping the framework version and bundle hash: a
        // task whose own bytes did not change, run against a rebuilt framework,
        // must not produce the same line. Before they were recorded, these two
        // events serialized identically.
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
        // The lists arrive unordered on purpose: an unordered container's
        // iteration order must never reach the wire (a determinism-ledger
        // constraint), so the serializer sorts them and the golden line is sorted
        // regardless of input order.
        auto const event = TraceEvent{
            .kind      = TraceEventKind::RunResourcesValidated,
            .resources = TraceEvent::Resources{
                .recognizers = {"battle", "accept", "daily"},
                .pages       = {"main", "home"},
            },
        };

        auto constexpr expected = std::string_view{
            "{\"schema\":\"umbraflow-trace/v1\",\"kind\":\"run.resources_validated\""
            ",\"seq\":1,\"runId\":7,\"generationId\":3"
            ",\"recognizers\":[\"accept\",\"battle\",\"daily\"]"
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
            "{\"schema\":\"umbraflow-trace/v1\",\"kind\":\"run.resources_validated\""
            ",\"seq\":1,\"runId\":7,\"generationId\":3"
            ",\"recognizers\":[],\"pages\":[]}"
        };

        CHECK(goldenLine(event) == expected);
    }

    TEST_CASE("serializeTraceEvent records every task.native_call outcome")
    {
        // Succeeded and Empty both completed; Empty is the Tier A completed miss,
        // and a failure names its kind with the snake_case spelling the script
        // itself saw.
        SUBCASE("succeeded")
        {
            // capture mints its own sequence, so it is the one verb whose line
            // carries no argument identity.
            auto const event = TraceEvent{
                .kind       = TraceEventKind::TaskNativeCall,
                .nativeCall = TraceEvent::NativeCall{
                    .verb    = "capture",
                    .outcome = NativeCallOutcome::Succeeded,
                },
            };

            CHECK(
                goldenLine(event)
                == "{\"schema\":\"umbraflow-trace/v1\",\"kind\":\"task.native_call\""
                   ",\"seq\":1,\"runId\":7,\"generationId\":3"
                   ",\"verb\":\"capture\",\"outcome\":\"Succeeded\"}"
            );
        }

        SUBCASE("empty")
        {
            auto const event = TraceEvent{
                .kind       = TraceEventKind::TaskNativeCall,
                .nativeCall = TraceEvent::NativeCall{
                    .verb         = "find",
                    .outcome      = NativeCallOutcome::Empty,
                    .cycleOrdinal = uint64{2},
                },
                .recognizerId = annotation::RecognizerId{
                    resourceId("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")
                },
            };

            CHECK(
                goldenLine(event)
                == "{\"schema\":\"umbraflow-trace/v1\",\"kind\":\"task.native_call\""
                   ",\"seq\":1,\"runId\":7,\"generationId\":3"
                   ",\"verb\":\"find\",\"cycleOrdinal\":2,\"outcome\":\"Empty\""
                   ",\"recognizerId\":\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\"}"
            );
        }

        SUBCASE("failed")
        {
            // The failure a host-side guard produces: the ledger rejects the
            // frame before the engine sees it, so this is the ONLY line the
            // failure writes -- and the two sequences on it are the only record
            // of which frames the script tried to use.
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
                == "{\"schema\":\"umbraflow-trace/v1\",\"kind\":\"task.native_call\""
                   ",\"seq\":1,\"runId\":7,\"generationId\":3"
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
                == "{\"schema\":\"umbraflow-trace/v1\",\"kind\":\"run.finished\""
                   ",\"seq\":1,\"runId\":7,\"generationId\":3"
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
                == "{\"schema\":\"umbraflow-trace/v1\",\"kind\":\"run.finished\""
                   ",\"seq\":1,\"runId\":7,\"generationId\":3"
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
                == "{\"schema\":\"umbraflow-trace/v1\",\"kind\":\"run.finished\""
                   ",\"seq\":1,\"runId\":7,\"generationId\":3"
                   ",\"runOutcome\":\"Cancelled\"}"
            );
        }
    }

    TEST_CASE("engine.observed pins its wire kind name and the frame join key")
    {
        // umbraflow-trace/v1 is a wire contract, so every kind name is pinned by
        // a full line somewhere. These two were the only ones no assertion
        // covered, which made a typo in either shippable in silence.
        auto const event = TraceEvent{
            .kind  = TraceEventKind::EngineObserved,
            .frame = annotation::FrameIdentity{
                CaptureSessionId{uint64{7}},
                TargetGeneration::fromValue(3),
                FrameId{uint64{17}},
            },
        };

        CHECK(
            goldenLine(event)
            == "{\"schema\":\"umbraflow-trace/v1\",\"kind\":\"engine.observed\""
               ",\"seq\":1,\"runId\":7,\"generationId\":3"
               ",\"frameId\":17,\"sessionId\":7,\"targetGeneration\":3}"
        );
    }

    TEST_CASE("engine.observation_invalidated pins its wire kind name")
    {
        auto const event = TraceEvent{
            .kind  = TraceEventKind::EngineObservationInvalidated,
            .frame = annotation::FrameIdentity{
                CaptureSessionId{uint64{7}},
                TargetGeneration::fromValue(3),
                FrameId{uint64{17}},
            },
        };

        CHECK(
            goldenLine(event)
            == "{\"schema\":\"umbraflow-trace/v1\""
               ",\"kind\":\"engine.observation_invalidated\""
               ",\"seq\":1,\"runId\":7,\"generationId\":3"
               ",\"frameId\":17,\"sessionId\":7,\"targetGeneration\":3}"
        );
    }

    TEST_CASE("engine.page_resolved keeps every outcome the old kinds distinguished")
    {
        // engine-trace/v1 spent three kinds (PageResolved, PageUnknown,
        // PageAmbiguous) plus the stage-blind RecognitionStopped and Failure on
        // this one step. All five survive as outcomes of one kind.
        auto const frame = annotation::FrameIdentity{
            CaptureSessionId{uint64{7}},
            TargetGeneration::fromValue(3),
            FrameId{uint64{17}},
        };
        auto constexpr prefix = std::string_view{
            "{\"schema\":\"umbraflow-trace/v1\",\"kind\":\"engine.page_resolved\""
            ",\"seq\":1,\"runId\":7,\"generationId\":3"
            ",\"frameId\":17,\"sessionId\":7,\"targetGeneration\":3"
        };

        SUBCASE("resolved carries the page id")
        {
            auto const event = TraceEvent{
                .kind  = TraceEventKind::EnginePageResolved,
                .frame = frame,
                .page  = TraceEvent::Page{
                    .outcome = PageResolution::Resolved,
                    .pageId  = annotation::PageId{
                        resourceId("11111111-2222-3333-4444-555555555555")
                    },
                },
            };

            CHECK(
                goldenLine(event)
                == std::string{prefix}
                    + ",\"pageOutcome\":\"Resolved\""
                      ",\"pageId\":\"11111111-2222-3333-4444-555555555555\"}"
            );
        }

        SUBCASE("unknown carries no page id")
        {
            auto const event = TraceEvent{
                .kind  = TraceEventKind::EnginePageResolved,
                .frame = frame,
                .page  = TraceEvent::Page{.outcome = PageResolution::Unknown},
            };

            CHECK(
                goldenLine(event)
                == std::string{prefix} + ",\"pageOutcome\":\"Unknown\"}"
            );
        }

        SUBCASE("unknown carries the anchor scores that explain it")
        {
            // "Why did my page not resolve" is the operator's first question, and
            // the outcome name alone cannot answer it. The evidence names the
            // page, that it was ruled out, and the required anchor that missed
            // together with the ceiling it was measured against.
            auto const event = TraceEvent{
                .kind  = TraceEventKind::EnginePageResolved,
                .frame = frame,
                .page  = TraceEvent::Page{
                    .outcome = PageResolution::Unknown,
                    .scores  = {
                        TraceEvent::Page::Score{
                            .pageId = annotation::PageId{
                                resourceId("11111111-2222-3333-4444-555555555555")
                            },
                            .candidate   = false,
                            .worstAnchor = annotation::RecognizerId{
                                resourceId("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")
                            },
                            .worstAnchorSad        = uint64{2},
                            .worstAnchorMaximumSad = uint64{255},
                        },
                    },
                },
            };

            CHECK(
                goldenLine(event)
                == std::string{prefix}
                    + ",\"pageOutcome\":\"Unknown\",\"pageScores\":["
                      "{\"pageId\":\"11111111-2222-3333-4444-555555555555\""
                      ",\"candidate\":false"
                      ",\"worstAnchor\":\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\""
                      ",\"worstAnchorSad\":2,\"worstAnchorMaximumSad\":255}]}"
            );
        }

        SUBCASE("ambiguous carries no page id")
        {
            auto const event = TraceEvent{
                .kind  = TraceEventKind::EnginePageResolved,
                .frame = frame,
                .page  = TraceEvent::Page{.outcome = PageResolution::Ambiguous},
            };

            CHECK(
                goldenLine(event)
                == std::string{prefix} + ",\"pageOutcome\":\"Ambiguous\"}"
            );
        }

        SUBCASE("stopped names the recognizer and the stop reason")
        {
            auto const event = TraceEvent{
                .kind  = TraceEventKind::EnginePageResolved,
                .frame = frame,
                .page         = TraceEvent::Page{.outcome = PageResolution::Stopped},
                .recognizerId = annotation::RecognizerId{
                    resourceId("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")
                },
                .stopReason   = SadSearchStopReason::ComparisonBudgetExhausted,
            };

            CHECK(
                goldenLine(event)
                == std::string{prefix}
                    + ",\"pageOutcome\":\"Stopped\""
                      ",\"recognizerId\":\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\""
                      ",\"stopReason\":\"ComparisonBudgetExhausted\"}"
            );
        }

        SUBCASE("failed names the error kind and message")
        {
            auto const event = TraceEvent{
                .kind  = TraceEventKind::EnginePageResolved,
                .frame = frame,
                .page      = TraceEvent::Page{.outcome = PageResolution::Failed},
                .errorKind = AutomationErrorKind::TargetCompatibilityUnverified,
                .message   = std::string{"boom"},
            };

            CHECK(
                goldenLine(event)
                == std::string{prefix}
                    + ",\"pageOutcome\":\"Failed\""
                      ",\"errorKind\":\"target_compatibility_unverified\""
                      ",\"message\":\"boom\"}"
            );
        }
    }

    TEST_CASE("engine.action_found keeps every outcome the old kinds distinguished")
    {
        auto const frame = annotation::FrameIdentity{
            CaptureSessionId{uint64{7}},
            TargetGeneration::fromValue(3),
            FrameId{uint64{17}},
        };
        auto const recognizer = annotation::RecognizerId{
            resourceId("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")
        };
        auto constexpr prefix = std::string_view{
            "{\"schema\":\"umbraflow-trace/v1\",\"kind\":\"engine.action_found\""
            ",\"seq\":1,\"runId\":7,\"generationId\":3"
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
                .recognizerId = recognizer,
            };

            CHECK(
                goldenLine(event)
                == std::string{prefix}
                    + ",\"actionOutcome\":\"Found\",\"sadScore\":1234"
                      ",\"maximumSad\":5000"
                      ",\"matchedRect\":{\"x\":10,\"y\":20,\"width\":30,\"height\":40}"
                      ",\"recognizerId\":\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\"}"
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
                .recognizerId = recognizer,
            };

            CHECK(
                goldenLine(event)
                == std::string{prefix}
                    + ",\"actionOutcome\":\"Absent\",\"sadScore\":9000"
                      ",\"maximumSad\":5000"
                      ",\"recognizerId\":\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\"}"
            );
        }
    }

    TEST_CASE("stripNonGoldenFields removes only the meta member")
    {
        auto constexpr withEarlyClock = std::string_view{
            "{\"schema\":\"umbraflow-trace/v1\",\"kind\":\"run.started\""
            ",\"seq\":1,\"meta\":{\"wallClock\":1000}}"
        };
        auto constexpr withLateClock = std::string_view{
            "{\"schema\":\"umbraflow-trace/v1\",\"kind\":\"run.started\""
            ",\"seq\":1,\"meta\":{\"wallClock\":999999}}"
        };
        auto constexpr withOtherSeq = std::string_view{
            "{\"schema\":\"umbraflow-trace/v1\",\"kind\":\"run.started\""
            ",\"seq\":2,\"meta\":{\"wallClock\":1000}}"
        };

        // Two lines that differ only inside meta are the same record.
        CHECK(stripNonGoldenFields(withEarlyClock) == stripNonGoldenFields(withLateClock));
        CHECK(
            stripNonGoldenFields(withEarlyClock)
            == "{\"schema\":\"umbraflow-trace/v1\",\"kind\":\"run.started\",\"seq\":1}"
        );

        // A difference anywhere else survives, so the helper cannot mask a real
        // divergence.
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
            CHECK(line.find("\"generationId\":3") != std::string::npos);
            CHECK(line.find("\"meta\":{\"wallClock\":") != std::string::npos);

            // A frozen clock, or one stubbed to zero, would satisfy every other
            // assertion in this suite: nothing else reads the value. Pin that it
            // is a real reading -- milliseconds since the Unix epoch, past
            // 2020-01-01T00:00:00Z. A lower bound rather than a window keeps the
            // case free of any dependence on how long the run takes.
            constexpr auto k_epoch2020Millis = int64{1'577'836'800'000};
            CHECK(wallClockOf(line) > k_epoch2020Millis);
        }
    }

    TEST_CASE("TraceRecorder surfaces a sink failure and still advances the sequence")
    {
        // The buffer outlives the sink, which the recorder owns, so it is declared
        // first.
        auto lines    = std::vector<std::string>{};
        auto recorder = TraceRecorder{
            std::make_unique<FailFirstTraceSink>(&lines),
            k_runId,
            k_generationId,
        };

        auto const first = recorder.emit(TraceEvent{.kind = TraceEventKind::RunStarted});
        REQUIRE_FALSE(first.has_value());
        CHECK(automationErrorKind(first.error()) == AutomationErrorKind::IoFailure);
        CHECK(lines.empty());

        // A lost event leaves a visible gap rather than a silently renumbered
        // stream: the event after the failure is stamped 2, so a reader sees that
        // one line is missing instead of reading a complete-looking stream. This
        // is the assertion the invariant in this case's name lives or dies on --
        // a recorder that advanced only on success would stamp 1 here.
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
            == "{\"schema\":\"umbraflow-trace/v1\",\"kind\":\"run.finished\""
               ",\"seq\":2,\"runId\":7,\"generationId\":3"
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
            == "{\"schema\":\"umbraflow-trace/v1\",\"kind\":\"run.started\""
               ",\"seq\":1,\"runId\":7,\"generationId\":3"
               ",\"projectId\":\"personal.game\",\"taskName\":\"daily\""
               ",\"sourceHash\":\"abc123\",\"frameworkVersion\":\"0.1.0\""
               ",\"frameworkHash\":\"def456\",\"luauVersion\":\"6\",\"seed\":42}"
        );
        CHECK(
            stripNonGoldenFields(lines[1])
            == "{\"schema\":\"umbraflow-trace/v1\",\"kind\":\"run.finished\""
               ",\"seq\":2,\"runId\":7,\"generationId\":3"
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
