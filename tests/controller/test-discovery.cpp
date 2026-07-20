#include <controller/detail/discovery-logic.hpp>
#include <controller/discovery.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <filesystem>

TEST_CASE("window handles preserve their pointer-sized value")
{
    auto const handle = uf::WindowHandle{std::intptr_t{0x1234}};

    CHECK(handle.value() == std::intptr_t{0x1234});
}

TEST_CASE("client size reports axes and allows zero")
{
    auto const empty = uf::ClientSize{0, 0};
    CHECK(empty.width() == 0);
    CHECK(empty.height() == 0);

    auto const sized = uf::ClientSize{1600, 900};
    CHECK(sized.width() == 1600);
    CHECK(sized.height() == 900);
}

TEST_CASE("file time packs high and low halves")
{
    auto const ticks = uf::controller_detail::fileTimeToTicks(
        0x0123'4567U,
        0x89AB'CDEFU
    );

    CHECK(ticks == 0x0123'4567'89AB'CDEFULL);
}

TEST_CASE("UTF-16 conversion handles empty input and clamps an overlong length")
{
    auto const letter = std::array{u'a'};
    CHECK(uf::controller_detail::utf16BufferToString(letter, 0).empty());
    CHECK(uf::controller_detail::utf16BufferToString(letter, -1).empty());

    auto const greeting = std::array{u'h', u'i'};
    CHECK(uf::controller_detail::utf16BufferToString(greeting, 99) == "hi");
}

TEST_CASE("executable paths decode malformed UTF-16 lossily")
{
    auto const malformedPath = std::array{
        u'C',
        u':',
        u'\\',
        char16_t{0xD800U},
        u'.',
        u'e',
        u'x',
        u'e',
    };
    auto const decoded = uf::controller_detail::utf16BufferToPath(
        malformedPath,
        8
    );

    CHECK(decoded == std::filesystem::path{L"C:\\\uFFFD.exe"});
}
