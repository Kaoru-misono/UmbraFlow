#include <core/concurrency/synchronized.hpp>
#include <core/control/control-flow.hpp>
#include <core/types/enum-reflection.hpp>
#include <core/types/flags.hpp>
#include <core/types/non-zero.hpp>
#include <core/utility/scope-exit.hpp>
#include <core/utility/variant-match.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace
{
    enum class Permission : std::uint8_t
    {
        Read = 1,
        Write = 2,
        Execute = 4
    };
}

namespace test_types
{
    enum class BuildState : std::uint8_t
    {
        Idle = 1,
        Running = 4,
        Failed = 9
    };
}

UMBRA_FLOW_REFLECT_ENUM(
    test_types::BuildState,
    test_types::BuildState::Idle,
    test_types::BuildState::Running,
    test_types::BuildState::Failed
);

static_assert(umbra_flow::ReflectedEnum<test_types::BuildState>);

TEST_CASE("variant matching requires one explicit handler per state")
{
    auto state = std::variant<int, std::string>{std::string{"ready"}};
    auto const size = umbra_flow::matchVariant(
        state,
        [](int value) -> std::size_t { return static_cast<std::size_t>(value); },
        [](std::string const& value) -> std::size_t { return value.size(); }
    );

    CHECK(size == 5);
}

TEST_CASE("enum reflection round-trips sparse values and rejects unknown names")
{
    auto const name = umbra_flow::enumName(test_types::BuildState::Running);
    if (!name.has_value())
    {
        FAIL("The reflected enum value did not have a name");
        return;
    }
    CHECK(*name == "Running");

    auto const value = umbra_flow::enumFromName<test_types::BuildState>("Failed");
    if (!value.has_value())
    {
        FAIL("A known enum name did not produce a value");
        return;
    }
    CHECK(*value == test_types::BuildState::Failed);

    CHECK_FALSE(umbra_flow::enumName(static_cast<test_types::BuildState>(2)).has_value());
    CHECK_FALSE(umbra_flow::enumFromName<test_types::BuildState>("running").has_value());
    CHECK(umbra_flow::enumEntries<test_types::BuildState>().size() == 3);
}

TEST_CASE("control flow distinguishes early exit and carries its value")
{
    auto flow = umbra_flow::ControlFlow<int>{umbra_flow::Break<int>{42}};

    CHECK(umbra_flow::isBreak(flow));
    CHECK_FALSE(umbra_flow::isContinue(flow));
    CHECK(
        umbra_flow::matchVariant(
            flow,
            [](umbra_flow::Continue<>) -> int { return 0; },
            [](umbra_flow::Break<int> stop) -> int { return stop.value; }
        )
        == 42
    );
}

TEST_CASE("non-zero values and flags preserve their declared invariants")
{
    auto const zero = umbra_flow::NonZero<int>::create(0);
    auto const divisor = umbra_flow::NonZero<int>::create(-3);

    CHECK_FALSE(zero.has_value());
    if (!divisor.has_value())
    {
        FAIL("NonZero::create rejected a non-zero value");
        return;
    }
    CHECK(divisor->value() == -3);

    auto permissions = umbra_flow::Flags<Permission>{Permission::Read, Permission::Write};
    CHECK(permissions.containsAll(umbra_flow::Flags<Permission>{Permission::Read}));
    CHECK_FALSE(permissions.containsAny(umbra_flow::Flags<Permission>{Permission::Execute}));

    permissions.remove(umbra_flow::Flags<Permission>{Permission::Write});
    CHECK(permissions == umbra_flow::Flags<Permission>{Permission::Read});
}

TEST_CASE("scope exit executes exactly while it remains armed")
{
    auto executions = 0;
    {
        [[maybe_unused]] auto cleanup = umbra_flow::scopeExit([&executions]() noexcept
        {
            ++executions;
        });
    }

    {
        auto cleanup = umbra_flow::scopeExit([&executions]() noexcept
        {
            ++executions;
        });
        cleanup.release();
    }

    CHECK(executions == 1);
}

TEST_CASE("synchronized operations preserve every concurrent update")
{
    auto counter = umbra_flow::Synchronized<int>{0};
    {
        auto workers = std::vector<std::jthread>{};
        for (auto worker = 0; worker < 4; ++worker)
        {
            workers.emplace_back([&counter]
            {
                for (auto iteration = 0; iteration < 1'000; ++iteration)
                {
                    counter.withLock([](int& value) -> void
                    {
                        ++value;
                    });
                }
            });
        }
    }

    auto const value = counter.withLock([](int const& current) -> int
    {
        return current;
    });
    CHECK(value == 4'000);
}
