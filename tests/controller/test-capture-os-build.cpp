#include <controller/detail/capture-os-build.hpp>

#include <core/types/integer.hpp>

#include <doctest/doctest.h>

#include <array>

TEST_CASE("cursor capture gate tracks Windows 10 build 19041")
{
    struct Case final
    {
        uf::uint32 build{};
        bool supported{};
    };

    for (
        auto const& testCase : std::array{
            Case{19'040, false},
            Case{19'041, true},
            Case{19'042, true},
        }
    )
    {
        CAPTURE(testCase.build);
        CAPTURE(testCase.supported);
        CHECK(
            uf::controller_detail::cursorCaptureSupported(testCase.build)
            == testCase.supported
        );
    }
}

TEST_CASE("borderless gate tracks build 20348")
{
    struct Case final
    {
        uf::uint32 build{};
        bool supported{};
    };

    for (
        auto const& testCase : std::array{
            Case{20'347, false},
            Case{20'348, true},
            Case{20'349, true},
        }
    )
    {
        CAPTURE(testCase.build);
        CAPTURE(testCase.supported);
        CHECK(
            uf::controller_detail::borderlessSupported(testCase.build)
            == testCase.supported
        );
    }
}
