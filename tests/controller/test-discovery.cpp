#include <controller/detail/discovery-logic.hpp>
#include <controller/discovery.hpp>
#include <controller/platform/windows-controller.hpp>

#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <array>
#include <filesystem>
#include <system_error>

namespace uf
{
    namespace
    {
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
    }

    TEST_CASE("window handles preserve their pointer-sized value")
    {
        auto const handle = WindowHandle{intptr{0x1234}};

        CHECK(handle.value() == intptr{0x1234});
    }

    TEST_CASE("invalid live window queries preserve Win32 failures")
    {
        auto const process = controller_platform::windowProcess(WindowHandle{0});
        REQUIRE_FALSE(process.has_value());
        CHECK(automationKind(process.error()) == AutomationErrorKind::TargetUnavailable);
        CHECK(process.error().nativeCode().value() != 0);
        CHECK(process.error().nativeCode().category() == std::system_category());

        auto const clientSize = controller_platform::windowClientSize(
            WindowHandle{0}
        );
        REQUIRE_FALSE(clientSize.has_value());
        CHECK(
            automationKind(clientSize.error())
            == AutomationErrorKind::TargetUnavailable
        );
        CHECK(clientSize.error().nativeCode().value() != 0);
        CHECK(
            clientSize.error().nativeCode().category() == std::system_category()
        );
    }

    TEST_CASE("an exited or invalid process is best-effort metadata absence")
    {
        auto const startTime = controller_platform::processStartTime(
            ProcessId{0}
        );

        REQUIRE(startTime.has_value());
        CHECK_FALSE(startTime->has_value());
    }

    TEST_CASE("file time packs high and low halves")
    {
        auto const ticks = controller_detail::fileTimeToTicks(
            0x0123'4567U,
            0x89AB'CDEFU
        );

        CHECK(ticks == 0x0123'4567'89AB'CDEFULL);
    }

    TEST_CASE("UTF-16 conversion handles empty input and clamps an overlong length")
    {
        auto const letter = std::array{u'a'};
        CHECK(controller_detail::utf16BufferToString(letter, 0).empty());
        CHECK(controller_detail::utf16BufferToString(letter, -1).empty());

        auto const greeting = std::array{u'h', u'i'};
        CHECK(controller_detail::utf16BufferToString(greeting, 99) == "hi");
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
        auto const decoded = controller_detail::utf16BufferToPath(
            malformedPath,
            8
        );

        CHECK(decoded == std::filesystem::path{L"C:\\\uFFFD.exe"});
    }
}
