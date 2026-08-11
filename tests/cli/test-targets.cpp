#include <cli/targets.hpp>

#include <core/types/integer.hpp>

#include <doctest/doctest.h>

#include <array>
#include <string>

namespace uf::cli
{
    TEST_CASE("formatTargetListings says an empty desktop rather than printing nothing")
    {
        CHECK(formatTargetListings({}) == "no visible window on this desktop\n");
    }

    TEST_CASE("formatTargetListings leads with the handle --hwnd takes")
    {
        auto const listings = std::array{
            TargetListing{
                .handle       = 0x504f2,
                .windowClass  = "GLFW30",
                .title        = "A Game",
                .clientWidth  = 1600,
                .clientHeight = 900,
                .dpi          = 96,
                .isIconic     = false,
            },
        };

        auto const text = formatTargetListings(listings);
        CHECK(text.starts_with("0x504f2 "));
        CHECK(text.find("GLFW30") != std::string::npos);
        CHECK(text.find("1600x900") != std::string::npos);
        CHECK(text.find("96 dpi") != std::string::npos);
        // Last, because it is the one field that contains spaces.
        CHECK(text.ends_with("A Game\n"));
    }

    TEST_CASE("formatTargetListings marks a minimized window instead of dropping it")
    {
        auto const listings = std::array{
            TargetListing{
                .handle       = 0x1,
                .windowClass  = "Shell",
                .title        = "Down",
                .clientWidth  = 0,
                .clientHeight = 0,
                .dpi          = 96,
                .isIconic     = true,
            },
            TargetListing{
                .handle       = 0x2,
                .windowClass  = "Shell",
                .title        = "Up",
                .clientWidth  = 800,
                .clientHeight = 600,
                .dpi          = 96,
                .isIconic     = false,
            },
        };

        auto const text = formatTargetListings(listings);
        CHECK(text.find("minimized") != std::string::npos);
        CHECK(text.find("Down\n") != std::string::npos);
        CHECK(text.find("Up\n") != std::string::npos);
    }
}
