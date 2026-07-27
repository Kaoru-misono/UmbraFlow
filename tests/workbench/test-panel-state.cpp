#include <panel-state.hpp>

#include <core/types/integer.hpp>

#include <doctest/doctest.h>

#include <format>
#include <string>

namespace uf::workbench
{
    namespace
    {
        TEST_CASE("the disk line carries the severity word between stamp and text")
        {
            CHECK(logSeverityWord(LogSeverity::Info) == "INFO");
            CHECK(logSeverityWord(LogSeverity::Warning) == "WARN");
            CHECK(logSeverityWord(LogSeverity::Error) == "ERROR");

            CHECK(
                formatLogLine(LogSeverity::Error, "2026-07-27T00:00:00Z", "boom")
                == "2026-07-27T00:00:00Z  ERROR  boom"
            );
            CHECK(
                formatLogLine(LogSeverity::Info, "2026-07-27T00:00:00Z", "done")
                == "2026-07-27T00:00:00Z  INFO  done"
            );
        }

        TEST_CASE("a reported outcome carries its severity through the mirror seam")
        {
            auto ui = PanelUiState{};

            ui.report(LogSeverity::Warning, "placed but does not match");
            auto const event = ui.captureLogEvent("T1");
            REQUIRE(event.has_value());
            CHECK(event->severity == LogSeverity::Warning);
            CHECK(event->timestamp == "T1");
            CHECK(event->message == "placed but does not match");
            REQUIRE(ui.logEvents.size() == 1U);
            CHECK(ui.logEvents.back().severity == LogSeverity::Warning);
        }

        TEST_CASE("the mirror collapses a consecutive duplicate")
        {
            auto ui = PanelUiState{};

            ui.report(LogSeverity::Info, "saved and generated");
            CHECK(ui.captureLogEvent("T1").has_value());

            // Same message reported again, even at a later timestamp, adds no
            // second entry.
            ui.report(LogSeverity::Info, "saved and generated");
            CHECK_FALSE(ui.captureLogEvent("T2").has_value());
            CHECK(ui.logEvents.size() == 1U);

            // A different message breaks the run and is recorded.
            ui.report(LogSeverity::Error, "save failed: disk full");
            CHECK(ui.captureLogEvent("T3").has_value());
            CHECK(ui.logEvents.size() == 2U);

            // An empty status is never mirrored.
            ui.report(LogSeverity::Info, "");
            CHECK_FALSE(ui.captureLogEvent("T4").has_value());
            CHECK(ui.logEvents.size() == 2U);
        }

        TEST_CASE("the event history is bounded, dropping the oldest first")
        {
            auto ui = PanelUiState{};

            auto const overfill = PanelUiState::k_logEventCapacity + 50U;
            for (auto index = std::size_t{0}; index < overfill; ++index)
            {
                ui.report(LogSeverity::Info, std::format("event {}", index));
                CHECK(ui.captureLogEvent("T").has_value());
            }

            CHECK(ui.logEvents.size() == PanelUiState::k_logEventCapacity);
            // The first 50 aged out, so the front is event 50 and the back is
            // the most recent.
            CHECK(ui.logEvents.front().message == "event 50");
            CHECK(
                ui.logEvents.back().message
                == std::format("event {}", overfill - 1U)
            );

            ui.clearLog();
            CHECK(ui.logEvents.empty());
        }
    }
}
