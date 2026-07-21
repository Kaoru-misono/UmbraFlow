#include <controller/detail/target-logic.hpp>
#include <controller/target.hpp>

#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf
{
    namespace
    {
        [[nodiscard]]
        auto startTime(std::optional<uint64> value) -> std::optional<ProcessStartTime>
        {
            if (!value)
            {
                return std::nullopt;
            }
            return ProcessStartTime{*value};
        }

        [[nodiscard]]
        auto candidate(
            intptr handle,
            uint32 process,
            std::optional<uint64> start,
            std::string_view windowClass,
            std::string_view title,
            ClientSize clientSize
        ) -> TargetCandidate
        {
            return TargetCandidate{
                WindowHandle{handle},
                ProcessId{process},
                startTime(start),
                std::nullopt,
                std::string{windowClass},
                std::string{title},
                clientSize,
                Dpi{96},
                true,
                false
            };
        }

        [[nodiscard]]
        auto identity(
            intptr handle,
            uint32 process,
            std::optional<uint64> start,
            ClientSize clientSize
        ) -> TargetIdentity
        {
            return TargetIdentity{
                WindowHandle{handle},
                ProcessId{process},
                startTime(start),
                clientSize
            };
        }

        [[nodiscard]]
        auto automationKind(Error const& error) -> AutomationErrorKind
        {
            auto const kind = automationErrorKind(error);
            if (!kind.has_value())
            {
                FAIL("The error did not contain an automation error kind");
                return AutomationErrorKind::InternalInvariant;
            }
            return *kind;
        }

        [[nodiscard]]
        auto baseTarget() -> ResolvedTarget
        {
            auto const candidates = std::vector{
                candidate(0x10, 100, 1, "A", "one", ClientSize{800, 600})
            };
            auto result = resolveTarget(candidates, TargetSelector{});
            REQUIRE(result.has_value());
            return *std::move(result);
        }
    }

    TEST_CASE("a unique candidate resolves at the initial generation")
    {
        auto const candidates = std::vector{
            candidate(0x10, 100, 1, "GameWindow", "Play", ClientSize{800, 600})
        };
        auto const resolved = resolveTarget(candidates, TargetSelector{});

        REQUIRE(resolved.has_value());
        CHECK(resolved->windowHandle() == WindowHandle{0x10});
        CHECK(resolved->clientSize() == ClientSize{800, 600});
        CHECK(resolved->currentGeneration() == TargetGeneration{});
    }

    TEST_CASE("explicit process and window-handle filters select their match")
    {
        auto const candidates = std::vector{
            candidate(0x10, 100, 1, "A", "one", ClientSize{800, 600}),
            candidate(0x20, 200, 2, "B", "two", ClientSize{640, 480}),
        };

        auto const byProcess = resolveTarget(
            candidates,
            TargetSelector{}.withProcess(ProcessId{200})
        );
        REQUIRE(byProcess.has_value());
        CHECK(byProcess->identity().process() == ProcessId{200});

        auto const byHandle = resolveTarget(
            candidates,
            TargetSelector{}.withWindowHandle(WindowHandle{0x10})
        );
        REQUIRE(byHandle.has_value());
        CHECK(byHandle->windowHandle() == WindowHandle{0x10});
    }

    TEST_CASE("combined process and window filters disambiguate repeated identities")
    {
        auto const candidates = std::vector{
            candidate(0x10, 100, 1, "A", "one", ClientSize{800, 600}),
            candidate(0x20, 100, 1, "A", "one", ClientSize{800, 600}),
            candidate(0x30, 200, 2, "B", "two", ClientSize{640, 480}),
        };
        auto const selector = TargetSelector{}
            .withProcess(ProcessId{100})
            .withWindowHandle(WindowHandle{0x20});
        auto const resolved = resolveTarget(candidates, selector);

        REQUIRE(resolved.has_value());
        CHECK(resolved->windowHandle() == WindowHandle{0x20});
    }

    TEST_CASE("selector string filters are exact and case-sensitive")
    {
        auto const candidateValue = candidate(
            0x10,
            100,
            1,
            "GameWindow",
            "Play",
            ClientSize{800, 600}
        );

        CHECK(
            TargetSelector{}
                .withWindowClass("GameWindow")
                .withTitle("Play")
                .matches(candidateValue)
        );
        CHECK_FALSE(TargetSelector{}.withWindowClass("gamewindow").matches(candidateValue));
        CHECK_FALSE(TargetSelector{}.withTitle("play").matches(candidateValue));
    }

    TEST_CASE("ambiguous and missing matches fail with target-unavailable context")
    {
        auto const candidates = std::vector{
            candidate(0x10, 100, 1, "Shared", "one", ClientSize{800, 600}),
            candidate(0x20, 100, 1, "Shared", "two", ClientSize{800, 600}),
        };
        auto const ambiguous = resolveTarget(candidates, TargetSelector{});

        REQUIRE_FALSE(ambiguous.has_value());
        CHECK(automationKind(ambiguous.error()) == AutomationErrorKind::TargetUnavailable);
        CHECK(ambiguous.error().message().contains("0x10"));
        CHECK(ambiguous.error().message().contains("0x20"));
        CHECK(matchingCandidates(candidates, TargetSelector{}).size() == 2);

        auto const missingSelectors = std::array{
            TargetSelector{}.withWindowHandle(WindowHandle{0xDEAD}),
            TargetSelector{}.withProcess(ProcessId{999}),
            TargetSelector{}.withWindowClass("Nonexistent"),
        };
        for (auto const& selector : missingSelectors)
        {
            auto const missing = resolveTarget(candidates, selector);
            REQUIRE_FALSE(missing.has_value());
            CHECK(automationKind(missing.error()) == AutomationErrorKind::TargetUnavailable);
        }
    }

    TEST_CASE("selector and candidate diagnostics use Rust debug-string escaping")
    {
        auto windowClass = std::string{"Class"};
        windowClass.push_back('"');
        windowClass.push_back('\\');
        windowClass.push_back('\x02');

        auto title = std::string{"line1\nline2"};
        title.push_back('\0');
        title.push_back('\t');
        title.push_back('\r');
        title.push_back('"');
        title.push_back('\\');
        title.push_back('\x01');

        auto const emptyCandidates = std::vector<TargetCandidate>{};
        auto const missing = resolveTarget(
            emptyCandidates,
            TargetSelector{}.withWindowClass(windowClass).withTitle(title)
        );
        REQUIRE_FALSE(missing.has_value());
        CHECK(
            missing.error().message()
            == R"(no window matched selector class="Class\"\\\u{2}" title="line1\nline2\0\t\r\"\\\u{1}")"
        );

        auto const ambiguousCandidates = std::vector{
            candidate(0x10, 100, 1, windowClass, title, ClientSize{800, 600}),
            candidate(0x20, 200, 2, "plain", "plain", ClientSize{640, 480}),
        };
        auto const ambiguous = resolveTarget(
            ambiguousCandidates,
            TargetSelector{}
        );
        REQUIRE_FALSE(ambiguous.has_value());
        CHECK(
            ambiguous.error().message()
            == R"(2 windows matched selector (none); disambiguate with --pid or --hwnd: [pid=100 hwnd=0x10 class="Class\"\\\u{2}" title="line1\nline2\0\t\r\"\\\u{1}"] [pid=200 hwnd=0x20 class="plain" title="plain"])"
        );
    }

    TEST_CASE("diagnostic window handles use unsigned pointer-width hexadecimal")
    {
        auto constexpr negativeHandle = intptr{-1};
        auto const ambiguousCandidates = std::vector{
            candidate(negativeHandle, 100, 1, "A", "one", ClientSize{800, 600}),
            candidate(0x20, 200, 2, "B", "two", ClientSize{640, 480}),
        };
        auto const ambiguous = resolveTarget(
            ambiguousCandidates,
            TargetSelector{}
        );
        REQUIRE_FALSE(ambiguous.has_value());

        auto const emptyCandidates = std::vector<TargetCandidate>{};
        auto const missing = resolveTarget(
            emptyCandidates,
            TargetSelector{}.withWindowHandle(WindowHandle{negativeHandle})
        );
        REQUIRE_FALSE(missing.has_value());

        auto const conflictingCandidates = std::vector{
            candidate(negativeHandle, 200, 2, "B", "two", ClientSize{640, 480})
        };
        auto const conflicting = resolveTarget(
            conflictingCandidates,
            TargetSelector{}
                .withProcess(ProcessId{100})
                .withWindowHandle(WindowHandle{negativeHandle})
        );
        REQUIRE_FALSE(conflicting.has_value());

        auto expectedHandle = std::string{"0x"};
        expectedHandle.append(sizeof(uintptr) * 2U, 'f');
        auto const messages = std::array{
            std::string{ambiguous.error().message()},
            std::string{missing.error().message()},
            std::string{conflicting.error().message()},
        };
        for (auto const& message : messages)
        {
            CHECK(message.contains(expectedHandle));
            CHECK_FALSE(message.contains("-0x1"));
        }
    }

    TEST_CASE("an empty candidate set is target unavailable")
    {
        auto const candidates = std::vector<TargetCandidate>{};
        auto const result = resolveTarget(candidates, TargetSelector{});

        REQUIRE_FALSE(result.has_value());
        CHECK(automationKind(result.error()) == AutomationErrorKind::TargetUnavailable);
    }

    TEST_CASE("inconsistent process and window filters are rejected explicitly")
    {
        auto const candidates = std::vector{
            candidate(0x20, 200, 2, "B", "two", ClientSize{640, 480})
        };
        auto const selector = TargetSelector{}
            .withProcess(ProcessId{100})
            .withWindowHandle(WindowHandle{0x20});
        auto const result = resolveTarget(candidates, selector);

        REQUIRE_FALSE(result.has_value());
        CHECK(automationKind(result.error()) == AutomationErrorKind::TargetUnavailable);
        CHECK(result.error().message().contains("belongs to pid 200"));
    }

    TEST_CASE("each identity change advances target generation exactly once")
    {
        auto const observations = std::array{
            identity(0x11, 100, 1, ClientSize{800, 600}),
            identity(0x10, 100, 1, ClientSize{1024, 768}),
            identity(0x10, 100, 2, ClientSize{800, 600}),
        };
        for (auto const& observation : observations)
        {
            auto target = baseTarget();
            auto const before = target.currentGeneration();
            auto const outcome = target.applyRevalidation(observation);
            auto const expectedGeneration = before.next();

            REQUIRE(outcome.has_value());
            REQUIRE(expectedGeneration.has_value());
            CHECK(*outcome == RevalidateOutcome::GenerationBumped);
            CHECK(target.currentGeneration() == *expectedGeneration);
        }
    }

    TEST_CASE("unchanged revalidation preserves target generation")
    {
        auto target = baseTarget();
        auto const outcome = target.applyRevalidation(
            identity(0x10, 100, 1, ClientSize{800, 600})
        );

        REQUIRE(outcome.has_value());
        CHECK(*outcome == RevalidateOutcome::Unchanged);
        CHECK(target.currentGeneration() == TargetGeneration{});
    }

    TEST_CASE("lost revalidation invalidates once without generation churn")
    {
        auto target = baseTarget();
        auto const outcome = target.applyRevalidation(std::nullopt);
        auto const invalidatedGeneration = TargetGeneration{}.next();

        REQUIRE(outcome.has_value());
        REQUIRE(invalidatedGeneration.has_value());
        CHECK(*outcome == RevalidateOutcome::Lost);
        CHECK(target.currentGeneration() == *invalidatedGeneration);
        CHECK(target.requiresReresolution());

        auto const repeated = target.applyRevalidation(std::nullopt);
        REQUIRE(repeated.has_value());
        CHECK(*repeated == RevalidateOutcome::Lost);
        CHECK(target.currentGeneration() == *invalidatedGeneration);

        auto const lostError = errorOnLost(*outcome);
        REQUIRE_FALSE(lostError.has_value());
        CHECK(automationKind(lostError.error()) == AutomationErrorKind::ControllerDisconnected);

        auto const unchanged = errorOnLost(RevalidateOutcome::Unchanged);
        REQUIRE(unchanged.has_value());
        CHECK(*unchanged == RevalidateOutcome::Unchanged);
    }

    TEST_CASE("unreadable process start time fails closed and stays latched")
    {
        auto const candidates = std::vector{
            candidate(0x10, 100, std::nullopt, "A", "one", ClientSize{800, 600})
        };
        auto targetResult = resolveTarget(candidates, TargetSelector{});
        auto const invalidatedGeneration = TargetGeneration{}.next();
        REQUIRE(targetResult.has_value());
        REQUIRE(invalidatedGeneration.has_value());
        auto target = *std::move(targetResult);

        for (auto iteration = 0; iteration < 3; ++iteration)
        {
            auto const outcome = target.applyRevalidation(
                identity(0x10, 100, std::nullopt, ClientSize{800, 600})
            );
            REQUIRE(outcome.has_value());
            CHECK(*outcome == RevalidateOutcome::InstanceUnconfirmed);
            CHECK(target.currentGeneration() == *invalidatedGeneration);
        }
        CHECK(target.requiresReresolution());

        auto const replacement = std::vector{
            candidate(0x20, 101, 2, "B", "two", ClientSize{640, 480})
        };
        auto const status = target.reResolve(replacement, TargetSelector{});
        REQUIRE(status.has_value());
        CHECK(target.currentGeneration() == *invalidatedGeneration);
        CHECK(target.identity().process() == ProcessId{101});
        CHECK_FALSE(target.requiresReresolution());
    }

    TEST_CASE("explicit re-resolution of a confirmed target advances generation")
    {
        auto target = baseTarget();
        auto const replacement = std::vector{
            candidate(0x20, 100, 1, "A", "one", ClientSize{800, 600})
        };
        auto const status = target.reResolve(replacement, TargetSelector{});
        auto const expectedGeneration = TargetGeneration{}.next();

        REQUIRE(status.has_value());
        REQUIRE(expectedGeneration.has_value());
        CHECK(target.currentGeneration() == *expectedGeneration);
        CHECK(target.windowHandle() == WindowHandle{0x20});
        CHECK_FALSE(target.requiresReresolution());
    }

    TEST_CASE("process-id reuse with a new start time advances generation")
    {
        auto const candidates = std::vector{
            candidate(0x10, 100, 111, "A", "one", ClientSize{800, 600})
        };
        auto targetResult = resolveTarget(candidates, TargetSelector{});
        REQUIRE(targetResult.has_value());
        auto target = *std::move(targetResult);

        auto const outcome = target.applyRevalidation(
            identity(0x10, 100, 222, ClientSize{800, 600})
        );
        REQUIRE(outcome.has_value());
        CHECK(*outcome == RevalidateOutcome::GenerationBumped);
    }

    TEST_CASE("losing a previously readable start time invalidates continuity")
    {
        auto const candidates = std::vector{
            candidate(0x10, 100, 111, "A", "one", ClientSize{800, 600})
        };
        auto targetResult = resolveTarget(candidates, TargetSelector{});
        auto const expectedGeneration = TargetGeneration{}.next();
        REQUIRE(targetResult.has_value());
        REQUIRE(expectedGeneration.has_value());
        auto target = *std::move(targetResult);

        auto const outcome = target.applyRevalidation(
            identity(0x10, 100, std::nullopt, ClientSize{800, 600})
        );
        REQUIRE(outcome.has_value());
        CHECK(*outcome == RevalidateOutcome::InstanceUnconfirmed);
        CHECK(target.currentGeneration() == *expectedGeneration);
        CHECK(target.requiresReresolution());
    }

    TEST_CASE("process-instance comparison covers every identity state")
    {
        auto const process = ProcessId{100};
        auto const otherProcess = ProcessId{101};
        auto const first = std::optional{ProcessStartTime{1}};
        auto const second = std::optional{ProcessStartTime{2}};

        CHECK(
            controller_detail::compareProcessInstance(
                process,
                first,
                otherProcess,
                first
            ) == controller_detail::ProcessInstanceMatch::Different
        );
        CHECK(
            controller_detail::compareProcessInstance(
                process,
                first,
                process,
                first
            ) == controller_detail::ProcessInstanceMatch::Same
        );
        CHECK(
            controller_detail::compareProcessInstance(
                process,
                first,
                process,
                second
            ) == controller_detail::ProcessInstanceMatch::Different
        );

        auto const unconfirmed = std::array{
            std::pair{first, std::optional<ProcessStartTime>{}},
            std::pair{std::optional<ProcessStartTime>{}, first},
            std::pair{
                std::optional<ProcessStartTime>{},
                std::optional<ProcessStartTime>{}
            },
        };
        for (auto const& [tracked, observed] : unconfirmed)
        {
            CHECK(
                controller_detail::compareProcessInstance(
                    process,
                    tracked,
                    process,
                    observed
                ) == controller_detail::ProcessInstanceMatch::Unconfirmed
            );
        }
    }
}
