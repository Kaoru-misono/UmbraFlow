#include <trace/event.hpp>
#include <trace/recorder.hpp>
#include <trace/sink.hpp>
#include <trace/stream-validator.hpp>

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>

#include <domain/content-hash.hpp>

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace uf::trace
{
    namespace
    {
        [[nodiscard]] auto hashOf(char digit) -> ContentHash
        {
            auto const encoded = "sha256:" + std::string(64U, digit);
            auto const hash    = ContentHash::parse(encoded);
            REQUIRE(hash.has_value());
            return *hash;
        }

        [[nodiscard]]
        auto streamSpec(
            std::string sessionId = "session-1",
            char manifestDigit = 'a',
            std::string producer = "operator.host"
        ) -> TraceStreamSpec
        {
            return TraceStreamSpec{
                .sessionId           = std::move(sessionId),
                .sessionManifestHash = hashOf(manifestDigit),
                .producer            = std::move(producer),
            };
        }

        [[nodiscard]] auto eventSpec(std::string eventType) -> TraceEventSpec
        {
            return TraceEventSpec{
                .eventType = std::move(eventType),
                .audit     = AuditMetadata{
                    .actor = "operator.agent",
                },
            };
        }

        class CollectingSink final : public ITraceSink
        {
            std::vector<TraceEvent> m_events{};

        public:
            [[nodiscard]] auto append(TraceEvent const& event) -> Status override
            {
                m_events.emplace_back(event);
                return ok();
            }

            [[nodiscard]]
            auto events() const noexcept UF_LIFETIME_BOUND
                -> std::vector<TraceEvent> const&
            {
                return m_events;
            }
        };

        [[nodiscard]]
        auto record(
            TraceStreamSpec const& stream,
            std::vector<std::string> eventTypes
        ) -> std::vector<TraceEvent>
        {
            auto sink         = std::make_unique<CollectingSink>();
            auto sinkObserver = sink.get();
            auto recorder     = TraceRecorder::create(std::move(sink), stream);
            REQUIRE(recorder.has_value());
            for (auto& eventType : eventTypes)
            {
                REQUIRE(
                    recorder->emit(eventSpec(std::move(eventType))).has_value()
                );
            }
            return sinkObserver->events();
        }
    }

    TEST_CASE("validator accepts one contiguous fixed-identity stream")
    {
        auto const events = record(
            streamSpec(),
            {"host.first", "host.second", "host.third"}
        );
        auto validator = TraceStreamValidator{};
        for (auto const& event : events)
        {
            REQUIRE(validator.admit(event).has_value());
        }
    }

    TEST_CASE("validator rejects gaps and out-of-order events without advancing")
    {
        auto const events = record(
            streamSpec(),
            {"host.first", "host.second", "host.third"}
        );

        auto firstIsSecond = TraceStreamValidator{};
        CHECK_FALSE(firstIsSecond.admit(events[1]).has_value());
        REQUIRE(firstIsSecond.admit(events[0]).has_value());

        auto gap = TraceStreamValidator{};
        REQUIRE(gap.admit(events[0]).has_value());
        CHECK_FALSE(gap.admit(events[2]).has_value());
        REQUIRE(gap.admit(events[1]).has_value());
        REQUIRE(gap.admit(events[2]).has_value());
    }

    TEST_CASE("validator rejects mixed session, manifest, and producer identity")
    {
        auto const baseline = record(
            streamSpec(),
            {"host.baseline_first", "host.baseline_second"}
        );
        auto const alternatives = std::vector<TraceStreamSpec>{
            streamSpec("session-2", 'a', "operator.host"),
            streamSpec("session-1", 'c', "operator.host"),
            streamSpec("session-1", 'a', "operator.worker"),
        };

        for (auto const& alternative : alternatives)
        {
            auto const mixed = record(
                alternative,
                {"host.mixed_first", "host.mixed_second"}
            );
            auto validator = TraceStreamValidator{};
            REQUIRE(validator.admit(baseline[0]).has_value());
            CHECK_FALSE(validator.admit(mixed[1]).has_value());
            REQUIRE(validator.admit(baseline[1]).has_value());
        }
    }

    TEST_CASE("validator rejects a duplicated sequence")
    {
        auto const events = record(
            streamSpec(),
            {"host.first", "host.second"}
        );

        auto validator = TraceStreamValidator{};
        REQUIRE(validator.admit(events[0]).has_value());
        CHECK_FALSE(validator.admit(events[0]).has_value());
        REQUIRE(validator.admit(events[1]).has_value());
    }
}
