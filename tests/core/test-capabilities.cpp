#include <core/concurrency/synchronized.hpp>
#include <core/control/control-flow.hpp>
#include <core/types/enum-reflection.hpp>
#include <core/types/flags.hpp>
#include <core/types/non-zero.hpp>
#include <core/utility/scope-exit.hpp>
#include <core/utility/variant-match.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
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

UF_REFLECT_ENUM(
    test_types::BuildState,
    test_types::BuildState::Idle,
    test_types::BuildState::Running,
    test_types::BuildState::Failed
);

static_assert(uf::ReflectedEnum<test_types::BuildState>);

TEST_CASE("variant matching requires one explicit handler per state")
{
    auto state = std::variant<int, std::string>{std::string{"ready"}};
    auto const size = uf::matchVariant(
        state,
        [](int) -> std::size_t { return 0; },
        [](std::string const& value) -> std::size_t { return value.size(); }
    );

    CHECK(size == 5);
}

TEST_CASE("enum reflection round-trips sparse values and rejects unknown names")
{
    auto const name = uf::enumName(test_types::BuildState::Running);
    if (!name.has_value())
    {
        FAIL("The reflected enum value did not have a name");
        return;
    }
    CHECK(*name == "Running");

    auto const value = uf::enumFromName<test_types::BuildState>("Failed");
    if (!value.has_value())
    {
        FAIL("A known enum name did not produce a value");
        return;
    }
    CHECK(*value == test_types::BuildState::Failed);

    CHECK_FALSE(uf::enumName(static_cast<test_types::BuildState>(2)).has_value());
    CHECK_FALSE(uf::enumFromName<test_types::BuildState>("running").has_value());
    CHECK(uf::enumEntries<test_types::BuildState>().size() == 3);
}

TEST_CASE("control flow distinguishes early exit and carries its value")
{
    auto flow = uf::ControlFlow<int>{uf::Break<int>{42}};

    CHECK(uf::isBreak(flow));
    CHECK_FALSE(uf::isContinue(flow));
    CHECK(
        uf::matchVariant(
            flow,
            [](uf::Continue<>) -> int { return 0; },
            [](uf::Break<int> stop) -> int { return stop.value; }
        )
        == 42
    );
}

TEST_CASE("non-zero values and flags preserve their declared invariants")
{
    auto const zero = uf::NonZero<int>::create(0);
    auto const divisor = uf::NonZero<int>::create(-3);

    CHECK_FALSE(zero.has_value());
    if (!divisor.has_value())
    {
        FAIL("NonZero::create rejected a non-zero value");
        return;
    }
    CHECK(divisor->value() == -3);

    auto permissions = uf::Flags<Permission>{Permission::Read, Permission::Write};
    CHECK(permissions.containsAll(uf::Flags<Permission>{Permission::Read}));
    CHECK_FALSE(permissions.containsAny(uf::Flags<Permission>{Permission::Execute}));

    permissions.remove(uf::Flags<Permission>{Permission::Write});
    CHECK(permissions == uf::Flags<Permission>{Permission::Read});
}

TEST_CASE("scope exit executes exactly while it remains armed")
{
    auto executions = std::make_shared<std::atomic<int>>(0);
    {
        [[maybe_unused]] auto cleanup = uf::scopeExit(
            [executions]() noexcept -> void
            {
                executions->fetch_add(1, std::memory_order_relaxed);
            }
        );
    }

    {
        auto cleanup = uf::scopeExit(
            [executions]() noexcept -> void
            {
                executions->fetch_add(1, std::memory_order_relaxed);
            }
        );
        cleanup.release();
    }

    CHECK(executions->load(std::memory_order_relaxed) == 1);
}

TEST_CASE("synchronized operations preserve every concurrent update")
{
    auto counter = std::make_shared<uf::Synchronized<int>>(0);
    {
        auto workers = std::vector<std::jthread>{};
        for (auto worker = 0; worker < 4; ++worker)
        {
            workers.emplace_back(
                [counter]() -> void
                {
                    for (auto iteration = 0; iteration < 1'000; ++iteration)
                    {
                        counter->withLock(
                            [](int& value) -> void
                            {
                                ++value;
                            }
                        );
                    }
                }
            );
        }
    }

    auto const value = counter->withLock(
        [](int const& current) -> int
        {
            return current;
        }
    );
    CHECK(value == 4'000);
}
