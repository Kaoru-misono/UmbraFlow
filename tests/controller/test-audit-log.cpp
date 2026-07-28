#include <controller/detail/audit-log-access.hpp>
#include <controller/input.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <string_view>

TEST_CASE("runtime forbidden list matches the Rust guard names")
{
    CHECK(uf::k_forbiddenBackgroundApis.size() == 6U);
    for (auto const name : std::array<std::string_view, 6>{
        "SetForegroundWindow",
        "SetFocus",
        "SendInput",
        "mouse_event",
        "keybd_event",
        "SetCursorPos",
    })
    {
        CHECK(std::ranges::find(uf::k_forbiddenBackgroundApis, name) != uf::k_forbiddenBackgroundApis.end());
    }
}

TEST_CASE("audit log appends one record per delivery")
{
    auto log = uf::AuditLog{};
    CHECK(log.empty());
    uf::controller_detail::AuditLogAccess::record(
        log,
        uf::WindowHandle{0x1234},
        0x0201U,
        0x0001U,
        0x00C8'0064
    );
    uf::controller_detail::AuditLogAccess::record(
        log,
        uf::WindowHandle{0x1234},
        0x0202U,
        0x0000U,
        0x00C8'0064
    );

    REQUIRE(log.size() == 2U);
    CHECK(log.records()[0].target == 0x1234U);
    CHECK(log.records()[0].message == 0x0201U);
    CHECK(log.records()[0].wParam == 0x0001U);
    CHECK(log.records()[1].message == 0x0202U);
}
