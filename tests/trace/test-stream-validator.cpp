#include <trace/event.hpp>
#include <trace/recorder.hpp>
#include <trace/sink.hpp>
#include <trace/stream-validator.hpp>

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/ids.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// The stream protocol every run's evidence must obey, driven through a real
// TraceRecorder because that is the only way any emitter reaches it -- the
// validator is not separately reachable, exactly as the sequence counter is not.
//
// Every case pairs a refusal with a control that would fail if the rule were
// vacuous: a well-formed sequence in the same shape must be accepted, and a
// refused event must leave no line and no sequence number behind. "Rejected at
// the request boundary, never silently truncated" is only meaningful if the
// second half is asserted.
namespace uf::trace
{
    namespace
    {
        constexpr auto k_runId        = TaskRunId{11};
        constexpr auto k_generationId = GenerationId{4};

        class CollectingSink final : public ITraceSink
        {
            std::vector<StampedTraceEvent>* m_events;

        public:
            explicit CollectingSink(
                std::vector<StampedTraceEvent>* p_events
            ) noexcept
                : m_events{p_events}
            {
            }

            [[nodiscard]]
            auto emit(StampedTraceEvent const& event) -> Status override
            {
                m_events->emplace_back(event);
                return ok();
            }
        };

        // One run's stream. The buffer is declared before the recorder that owns
        // the sink borrowing it, and the type is non-movable, so the borrow stays
        // valid for the whole case.
        class Stream final
        {
            std::vector<StampedTraceEvent> m_events{};
            TraceRecorder                  m_recorder;

        public:
            explicit Stream(FrontEnd frontEnd = FrontEnd::Task)
                : m_recorder{
                      std::make_unique<CollectingSink>(&m_events),
                      k_runId,
                      k_generationId,
                      frontEnd,
                  }
            {
            }

            Stream(Stream const&) = delete;
            Stream(Stream&&) = delete;
            auto operator=(Stream const&) -> Stream& = delete;
            auto operator=(Stream&&) -> Stream& = delete;

            ~Stream() = default;

            [[nodiscard]] auto emit(TraceEvent const& event) -> Status
            {
                return m_recorder.emit(event);
            }

            [[nodiscard]] auto requireScopesClosed() const -> Status
            {
                return m_recorder.requireScopesClosed();
            }

            [[nodiscard]]
            auto events() const noexcept UF_LIFETIME_BOUND
                -> std::vector<StampedTraceEvent> const&
            {
                return m_events;
            }
        };

        [[nodiscard]]
        auto plain(TraceEventKind kind) -> TraceEvent
        {
            return TraceEvent{.kind = kind};
        }

        [[nodiscard]]
        auto scoped(TraceEventKind kind, std::string label) -> TraceEvent
        {
            return TraceEvent{
                .kind      = kind,
                .framework = TraceEvent::Framework{.label = std::move(label)},
            };
        }

        [[nodiscard]]
        auto retryAttempt(uint64 attempt, uint64 attempts) -> TraceEvent
        {
            return TraceEvent{
                .kind      = TraceEventKind::FrameworkRetryAttempt,
                .framework = TraceEvent::Framework{
                    .attempt  = attempt,
                    .attempts = attempts,
                },
            };
        }

        [[nodiscard]]
        auto nativeCall() -> TraceEvent
        {
            return TraceEvent{
                .kind       = TraceEventKind::TaskNativeCall,
                .nativeCall = TraceEvent::NativeCall{
                    .verb    = "cycle_open",
                    .outcome = NativeCallOutcome::Succeeded,
                },
            };
        }

        [[nodiscard]]
        auto refusedKind(Status const& status) -> AutomationErrorKind
        {
            REQUIRE_FALSE(status.has_value());
            auto const kind = automationErrorKind(status.error());
            REQUIRE(kind.has_value());
            return *kind;
        }

        TEST_CASE("only the task stream may hold a framework event")
        {
            // The rule is stated against FrontEnd::Task, so a front-end added
            // later inherits the refusal instead of needing to be listed. This
            // pins that for the third one: an annotation session runs no Luau
            // framework at all, so a framework.* line on its stream could only be
            // a host bug attributing task structure to it.
            auto annotation = Stream{FrontEnd::Annotation};
            CHECK(
                refusedKind(
                    annotation.emit(
                        scoped(TraceEventKind::FrameworkStepStarted, "daily")
                    )
                )
                == AutomationErrorKind::InternalInvariant
            );
            // The refusal leaves nothing behind: no line, and no sequence spent.
            CHECK(annotation.events().empty());

            // The control, on the same stream: everything the host itself
            // authors is still admitted, so the refusal is about the event kind
            // and not about the stream having stopped accepting anything.
            REQUIRE(annotation.emit(plain(TraceEventKind::RunStarted)).has_value());
            REQUIRE(annotation.emit(nativeCall()).has_value());
            REQUIRE(annotation.events().size() == 2U);
            CHECK(annotation.events().front().sequence() == 1U);

            auto task = Stream{FrontEnd::Task};
            CHECK(
                task.emit(scoped(TraceEventKind::FrameworkStepStarted, "daily"))
                    .has_value()
            );
        }

        TEST_CASE("a run bracket opens once and accepts nothing after it closes")
        {
            auto stream = Stream{};

            REQUIRE(stream.emit(plain(TraceEventKind::RunStarted)).has_value());
            CHECK(
                refusedKind(stream.emit(plain(TraceEventKind::RunStarted)))
                == AutomationErrorKind::InternalInvariant
            );

            // The control: the refusal above is about the SECOND run.started and
            // not about the stream having stopped accepting anything.
            REQUIRE(stream.emit(nativeCall()).has_value());
            REQUIRE(
                stream.emit(scoped(TraceEventKind::FrameworkStepStarted, "live"))
                    .has_value()
            );
            REQUIRE(
                stream.emit(scoped(TraceEventKind::FrameworkStepFinished, "live"))
                    .has_value()
            );

            REQUIRE(stream.emit(plain(TraceEventKind::RunFinished)).has_value());

            CHECK(
                refusedKind(
                    stream.emit(scoped(TraceEventKind::FrameworkStepStarted, "late"))
                )
                == AutomationErrorKind::InternalInvariant
            );
            CHECK(
                refusedKind(stream.emit(nativeCall()))
                == AutomationErrorKind::InternalInvariant
            );
            CHECK(
                refusedKind(stream.emit(plain(TraceEventKind::RunFinished)))
                == AutomationErrorKind::InternalInvariant
            );

            // Five lines were accepted and every refusal wrote none, so the
            // sequence has no number spent on a line that was never written.
            REQUIRE(stream.events().size() == 5U);
            CHECK(stream.events().back().sequence() == 5U);
        }

        TEST_CASE("steps nest strictly and a finish names the innermost open one")
        {
            auto stream = Stream{};

            REQUIRE(
                stream.emit(scoped(TraceEventKind::FrameworkStepStarted, "outer"))
                    .has_value()
            );
            REQUIRE(
                stream.emit(scoped(TraceEventKind::FrameworkStepStarted, "inner"))
                    .has_value()
            );

            // Closing the outer step around a still-open inner one would make
            // every line stamped since then a lie about where it happened.
            CHECK(
                refusedKind(
                    stream.emit(scoped(TraceEventKind::FrameworkStepFinished, "outer"))
                )
                == AutomationErrorKind::InternalInvariant
            );

            // The control: the same two finishes in the right order are accepted,
            // so the refusal is about the ORDER and not about finishing at all.
            REQUIRE(
                stream.emit(scoped(TraceEventKind::FrameworkStepFinished, "inner"))
                    .has_value()
            );
            REQUIRE(
                stream.emit(scoped(TraceEventKind::FrameworkStepFinished, "outer"))
                    .has_value()
            );
            CHECK(
                refusedKind(
                    stream.emit(scoped(TraceEventKind::FrameworkStepFinished, "outer"))
                )
                == AutomationErrorKind::InternalInvariant
            );
        }

        TEST_CASE("step nesting stops at the host's hard depth ceiling")
        {
            auto stream = Stream{};

            for (auto depth = std::size_t{0}; depth < k_maxScopeDepth; ++depth)
            {
                auto const status = stream.emit(
                    scoped(TraceEventKind::FrameworkStepStarted, std::to_string(depth))
                );
                REQUIRE(status.has_value());
            }

            // One past the ceiling. It is the project's own nesting, so it is a
            // refused request rather than a framework bug.
            CHECK(
                refusedKind(
                    stream.emit(scoped(TraceEventKind::FrameworkStepStarted, "over"))
                )
                == AutomationErrorKind::InvalidResource
            );

            // Nothing opened: the innermost step is still the last accepted one,
            // which the stamp on the next line reports.
            REQUIRE(stream.emit(nativeCall()).has_value());
            auto const& steps = stream.events().back().openSteps();
            REQUIRE(steps.size() == k_maxScopeDepth);
            CHECK(steps.back() == std::to_string(k_maxScopeDepth - 1U));
        }

        TEST_CASE("a run may not end with a framework scope still open")
        {
            SUBCASE("an open step")
            {
                auto stream = Stream{};
                REQUIRE(stream.requireScopesClosed().has_value());

                REQUIRE(
                    stream.emit(scoped(TraceEventKind::FrameworkStepStarted, "daily"))
                        .has_value()
                );
                CHECK(
                    refusedKind(stream.requireScopesClosed())
                    == AutomationErrorKind::InternalInvariant
                );

                REQUIRE(
                    stream.emit(scoped(TraceEventKind::FrameworkStepFinished, "daily"))
                        .has_value()
                );
                CHECK(stream.requireScopesClosed().has_value());
            }

            SUBCASE("an interrupt still being handled")
            {
                auto stream = Stream{};
                REQUIRE(
                    stream
                        .emit(scoped(TraceEventKind::FrameworkInterruptMatched, "popup"))
                        .has_value()
                );
                CHECK(
                    refusedKind(stream.requireScopesClosed())
                    == AutomationErrorKind::InternalInvariant
                );

                REQUIRE(
                    stream
                        .emit(scoped(TraceEventKind::FrameworkInterruptHandled, "popup"))
                        .has_value()
                );
                CHECK(stream.requireScopesClosed().has_value());
            }
        }

        TEST_CASE("a retry attempt stays inside the total its policy declared")
        {
            SUBCASE("beyond the declared attempts")
            {
                auto stream = Stream{};
                REQUIRE(stream.emit(retryAttempt(1, 3)).has_value());
                REQUIRE(stream.emit(retryAttempt(2, 3)).has_value());
                REQUIRE(stream.emit(retryAttempt(3, 3)).has_value());
                CHECK(
                    refusedKind(stream.emit(retryAttempt(4, 3)))
                    == AutomationErrorKind::InternalInvariant
                );
            }

            SUBCASE("skipping a number inside one scope")
            {
                auto stream = Stream{};
                REQUIRE(stream.emit(retryAttempt(1, 3)).has_value());
                CHECK(
                    refusedKind(stream.emit(retryAttempt(3, 3)))
                    == AutomationErrorKind::InternalInvariant
                );
            }

            SUBCASE("nested retry scopes each count for themselves")
            {
                // The control that keeps the two refusals above from being a rule
                // against retrying at all: an inner policy that gives up and an
                // outer one that then takes its next attempt is a legal sequence
                // whose numbers do not read monotonically end to end.
                auto stream = Stream{};
                REQUIRE(stream.emit(retryAttempt(1, 3)).has_value());
                REQUIRE(stream.emit(retryAttempt(1, 2)).has_value());
                REQUIRE(stream.emit(retryAttempt(2, 2)).has_value());
                REQUIRE(stream.emit(retryAttempt(2, 3)).has_value());
                REQUIRE(stream.emit(retryAttempt(3, 3)).has_value());
            }
        }

        TEST_CASE("an interrupt is closed only by the match that opened it")
        {
            auto stream = Stream{};

            CHECK(
                refusedKind(
                    stream.emit(
                        scoped(TraceEventKind::FrameworkInterruptHandled, "popup")
                    )
                )
                == AutomationErrorKind::InternalInvariant
            );

            REQUIRE(
                stream.emit(scoped(TraceEventKind::FrameworkInterruptMatched, "popup"))
                    .has_value()
            );

            // A different id cannot close this match, and the same id cannot open
            // a second one inside it.
            CHECK(
                refusedKind(
                    stream.emit(
                        scoped(TraceEventKind::FrameworkInterruptHandled, "other")
                    )
                )
                == AutomationErrorKind::InternalInvariant
            );
            CHECK(
                refusedKind(
                    stream.emit(
                        scoped(TraceEventKind::FrameworkInterruptMatched, "popup")
                    )
                )
                == AutomationErrorKind::InternalInvariant
            );

            // The control: the matching close is accepted, and so is a match that
            // ran out of budget rather than being handled.
            REQUIRE(
                stream.emit(scoped(TraceEventKind::FrameworkInterruptHandled, "popup"))
                    .has_value()
            );
            REQUIRE(
                stream.emit(scoped(TraceEventKind::FrameworkInterruptMatched, "popup"))
                    .has_value()
            );
            REQUIRE(
                stream
                    .emit(scoped(TraceEventKind::FrameworkInterruptExhausted, "popup"))
                    .has_value()
            );
        }

        TEST_CASE("an over-budget label is refused whole, never truncated")
        {
            auto stream = Stream{};

            // A name one byte past the ceiling.
            auto const tooLong = std::string(k_maxScopeLabelBytes + 1U, 'a');
            CHECK(
                refusedKind(
                    stream.emit(scoped(TraceEventKind::FrameworkStepStarted, tooLong))
                )
                == AutomationErrorKind::InvalidResource
            );

            // A control byte, which a reader's terminal would act on rather than
            // print, and a byte sequence that is not UTF-8 at all, which would
            // make the trace file itself ill-formed.
            CHECK(
                refusedKind(
                    stream.emit(
                        scoped(TraceEventKind::FrameworkStepStarted, "wait\nfor home")
                    )
                )
                == AutomationErrorKind::InvalidResource
            );
            CHECK(
                refusedKind(
                    stream.emit(
                        scoped(TraceEventKind::FrameworkStepStarted, "\xC3\x28")
                    )
                )
                == AutomationErrorKind::InvalidResource
            );
            CHECK(
                refusedKind(
                    stream.emit(scoped(TraceEventKind::FrameworkStepStarted, ""))
                )
                == AutomationErrorKind::InvalidResource
            );

            // The control, and the reason the rule is UTF-8 rather than ASCII:
            // this project's tasks are written in Chinese as often as in English,
            // so a non-ASCII step name must be ordinary rather than refused.
            REQUIRE(
                stream
                    .emit(
                        scoped(TraceEventKind::FrameworkStepStarted, "\xE6\x97\xA5\xE5\xB8\xB8")
                    )
                    .has_value()
            );

            // Nothing was truncated into the stream: exactly one step opened, and
            // it is the one that was accepted whole.
            REQUIRE(stream.events().size() == 1U);
            REQUIRE(stream.emit(nativeCall()).has_value());
            auto const& steps = stream.events().back().openSteps();
            REQUIRE(steps.size() == 1U);
            CHECK(steps.front() == "\xE6\x97\xA5\xE5\xB8\xB8");
        }

        TEST_CASE("the open step path has a total budget of its own")
        {
            // Each name is legal and the depth stays far inside the ceiling, so
            // the only thing that can refuse the last one is the TOTAL payload
            // budget -- which is the point of having one: the path is stamped on
            // every line written while those steps are open, so what matters is
            // their sum rather than any single name.
            auto stream = Stream{};

            auto opened = std::size_t{0};
            auto label  = char{'a'};
            while (
                stream
                    .emit(
                        scoped(
                            TraceEventKind::FrameworkStepStarted,
                            std::string(k_maxScopeLabelBytes, label)
                        )
                    )
                    .has_value()
            )
            {
                ++opened;
                ++label;
                REQUIRE(opened < k_maxScopeDepth);
            }

            CHECK(opened > 1U);
            CHECK(opened < k_maxScopeDepth);

            // The refusal left the path exactly as it was.
            REQUIRE(stream.emit(nativeCall()).has_value());
            CHECK(stream.events().back().openSteps().size() == opened);
        }

        TEST_CASE("every line carries the step scope that was open when it was written")
        {
            auto stream = Stream{};

            REQUIRE(stream.emit(nativeCall()).has_value());
            CHECK(stream.events().back().openSteps().empty());

            REQUIRE(
                stream.emit(scoped(TraceEventKind::FrameworkStepStarted, "outer"))
                    .has_value()
            );
            // A step's own start reports the scope it opened INSIDE; the step it
            // opens is named by the event itself.
            CHECK(stream.events().back().openSteps().empty());

            REQUIRE(
                stream.emit(scoped(TraceEventKind::FrameworkStepStarted, "inner"))
                    .has_value()
            );
            REQUIRE(stream.emit(nativeCall()).has_value());
            auto const& steps = stream.events().back().openSteps();
            REQUIRE(steps.size() == 2U);
            CHECK(steps[0] == "outer");
            CHECK(steps[1] == "inner");

            // And it reaches the wire in that order.
            CHECK(
                serializeTraceEvent(stream.events().back())
                    .contains(R"("steps":["outer","inner"])")
            );
        }
    }
}
