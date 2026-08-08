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

    TEST_CASE("selectCandidate takes the named window, not one that resembles it")
    {
        // The case a title substring could never decide: an unrelated window
        // whose title CONTAINS the target's, alongside the target itself.
        auto candidates = std::vector<TargetCandidate>{};
        candidates.emplace_back(makeCandidate(0x10, "searching Game for events", true, false));
        candidates.emplace_back(makeCandidate(0x20, "Game", true, false));

        auto const chosen = selectCandidate(candidates, WindowHandle{0x20});
        REQUIRE(chosen.has_value());
        CHECK(chosen->title() == "Game");
    }

    TEST_CASE("selectCandidate takes the named decoy's sibling, not the decoy")
    {
        // Every decoy is invisible and they share the real window's title, so
        // only the handle separates them.
        auto candidates = std::vector<TargetCandidate>{};
        candidates.emplace_back(makeCandidate(0x10, "Game_thread_0", false, false));
        candidates.emplace_back(makeCandidate(0x20, "Game", true, false));
        candidates.emplace_back(makeCandidate(0x30, "Game_thread_7", false, false));

        auto const chosen = selectCandidate(candidates, WindowHandle{0x20});
        REQUIRE(chosen.has_value());
        CHECK(chosen->handle() == WindowHandle{0x20});
    }

    TEST_CASE("selectCandidate reports a handle no window carries")
    {
        auto candidates = std::vector<TargetCandidate>{};
        candidates.emplace_back(makeCandidate(0x20, "Game", true, false));

        auto const chosen = selectCandidate(candidates, WindowHandle{0x99});
        REQUIRE_FALSE(chosen.has_value());
        CHECK(automationErrorKind(chosen.error()) == AutomationErrorKind::TargetUnavailable);
        CHECK(chosen.error().message().find("0x99") != std::string::npos);
    }

    TEST_CASE("selectCandidate refuses a minimized window and says which it was")
    {
        // Visible in the WS_VISIBLE sense and still uncapturable, which is the
        // distinction the message has to carry: the window is there.
        auto candidates = std::vector<TargetCandidate>{};
        candidates.emplace_back(makeCandidate(0x20, "Game", true, true));

        auto const chosen = selectCandidate(candidates, WindowHandle{0x20});
        REQUIRE_FALSE(chosen.has_value());
        CHECK(automationErrorKind(chosen.error()) == AutomationErrorKind::TargetUnavailable);
        CHECK(chosen.error().message().find("minimized") != std::string::npos);
    }

    TEST_CASE("selectCandidate refuses an invisible decoy by handle")
    {
        auto candidates = std::vector<TargetCandidate>{};
        candidates.emplace_back(makeCandidate(0x10, "Game_thread_0", false, false));

        auto const chosen = selectCandidate(candidates, WindowHandle{0x10});
        REQUIRE_FALSE(chosen.has_value());
        CHECK(automationErrorKind(chosen.error()) == AutomationErrorKind::TargetUnavailable);
        CHECK(chosen.error().message().find("not visible") != std::string::npos);
    }
}
