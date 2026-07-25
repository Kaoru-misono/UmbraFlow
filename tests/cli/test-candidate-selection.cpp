#include <candidate-selection.hpp>

#include <controller/discovery.hpp>
#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uf::cli
{
    namespace
    {
        [[nodiscard]]
        auto makeCandidate(
            intptr handle,
            std::string_view title,
            bool isVisible,
            bool isIconic
        ) -> TargetCandidate
        {
            return TargetCandidate{
                WindowHandle{handle},
                ProcessId{100},
                std::nullopt,
                std::nullopt,
                std::string{"GameClass"},
                std::string{title},
                ClientSize{1600, 900},
                Dpi{96},
                isVisible,
                isIconic
            };
        }
    }

    TEST_CASE("selectCandidate ignores invisible decoy windows around the real one")
    {
        // A shield spawns many invisible decoys whose titles are the real title
        // plus a suffix; only the real window is visible.
        auto candidates = std::vector<TargetCandidate>{};
        candidates.emplace_back(makeCandidate(0x10, "Game_thread_0", false, false));
        candidates.emplace_back(makeCandidate(0x20, "Game", true, false));
        candidates.emplace_back(makeCandidate(0x30, "Game_thread_0", false, false));
        candidates.emplace_back(makeCandidate(0x40, "Game_thread_7", false, false));

        auto const chosen = selectCandidate(candidates, "Game");
        REQUIRE(chosen.has_value());
        CHECK(chosen->handle() == WindowHandle{0x20});
    }

    TEST_CASE("selectCandidate reports no match when every title match is invisible")
    {
        auto candidates = std::vector<TargetCandidate>{};
        candidates.emplace_back(makeCandidate(0x10, "Game_thread_0", false, false));
        candidates.emplace_back(makeCandidate(0x20, "Game_thread_1", false, false));

        auto const chosen = selectCandidate(candidates, "Game");
        REQUIRE_FALSE(chosen.has_value());
        CHECK(automationErrorKind(chosen.error()) == AutomationErrorKind::TargetUnavailable);
        CHECK(chosen.error().message() == "no visible window title contains \"Game\"");
    }

    TEST_CASE("selectCandidate excludes a minimized window")
    {
        // A minimized window is visible in the WS_VISIBLE sense but cannot be
        // captured, so it must not be selected.
        auto candidates = std::vector<TargetCandidate>{};
        candidates.emplace_back(makeCandidate(0x20, "Game", true, true));

        auto const chosen = selectCandidate(candidates, "Game");
        REQUIRE_FALSE(chosen.has_value());
        CHECK(automationErrorKind(chosen.error()) == AutomationErrorKind::TargetUnavailable);
    }

    TEST_CASE("selectCandidate refuses to guess between multiple visible matches")
    {
        auto candidates = std::vector<TargetCandidate>{};
        candidates.emplace_back(makeCandidate(0x20, "Game One", true, false));
        candidates.emplace_back(makeCandidate(0x30, "Game Two", true, false));

        auto const chosen = selectCandidate(candidates, "Game");
        REQUIRE_FALSE(chosen.has_value());
        CHECK(automationErrorKind(chosen.error()) == AutomationErrorKind::TargetUnavailable);
        CHECK(
            chosen.error().message()
            == "selector \"Game\" matches 2 visible windows; refine it: "
               "\"Game One\", \"Game Two\""
        );
    }

    TEST_CASE("selectCandidate reports no match when no title contains the selector")
    {
        auto candidates = std::vector<TargetCandidate>{};
        candidates.emplace_back(makeCandidate(0x20, "Other", true, false));

        auto const chosen = selectCandidate(candidates, "Game");
        REQUIRE_FALSE(chosen.has_value());
        CHECK(automationErrorKind(chosen.error()) == AutomationErrorKind::TargetUnavailable);
    }
}
