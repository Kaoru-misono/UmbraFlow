#include <core/concurrency/synchronized.hpp>
#include <core/control/control-flow.hpp>
#include <core/types/enum-reflection.hpp>
#include <core/types/flags.hpp>
#include <core/types/integer.hpp>
#include <core/types/non-zero.hpp>
#include <core/utility/scope-exit.hpp>
#include <core/utility/variant-match.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace uf
{
    namespace
    {
        enum class Permission : uint8
        {
            Read = 1,
            Write = 2,
            Execute = 4
        };
    }

    namespace test_types
    {
        enum class BuildState : uint8
        {
            Idle = 1,
            Running = 4,
            Failed = 9
        };
    }
}

UF_REFLECT_ENUM(
    uf::test_types::BuildState,
    uf::test_types::BuildState::Idle,
    uf::test_types::BuildState::Running,
    uf::test_types::BuildState::Failed
);

static_assert(uf::ReflectedEnum<uf::test_types::BuildState>);

namespace uf
{
    TEST_CASE("variant matching requires one explicit handler per state")
    {
        auto state = std::variant<int, std::string>{std::string{"ready"}};
        auto const size = matchVariant(
            state,
            [](int) -> std::size_t { return 0; },
            [](std::string const& value) -> std::size_t { return value.size(); }
        );

        CHECK(size == 5);
    }

    TEST_CASE("enum reflection round-trips sparse values and rejects unknown names")
    {
        auto const name = enumName(test_types::BuildState::Running);
        if (!name.has_value())
        {
            FAIL("The reflected enum value did not have a name");
            return;
        }
        CHECK(*name == "Running");

        auto const value = enumFromName<test_types::BuildState>("Failed");
        if (!value.has_value())
        {
            FAIL("A known enum name did not produce a value");
            return;
        }
        CHECK(*value == test_types::BuildState::Failed);

        CHECK_FALSE(enumName(static_cast<test_types::BuildState>(2)).has_value());
        CHECK_FALSE(enumFromName<test_types::BuildState>("running").has_value());
        CHECK(enumEntries<test_types::BuildState>().size() == 3);
    }

    TEST_CASE("control flow distinguishes early exit and carries its value")
    {
        auto flow = ControlFlow<int>{Break<int>{42}};

        CHECK(isBreak(flow));
        CHECK_FALSE(isContinue(flow));
        CHECK(
            matchVariant(
                flow,
                [](Continue<>) -> int { return 0; },
                [](Break<int> stop) -> int { return stop.value; }
            )
            == 42
        );
    }

    TEST_CASE("non-zero values and flags preserve their declared invariants")
    {
        auto const zero = NonZero<int>::create(0);
        auto const divisor = NonZero<int>::create(-3);

        CHECK_FALSE(zero.has_value());
        if (!divisor.has_value())
        {
            FAIL("NonZero::create rejected a non-zero value");
            return;
        }
        CHECK(divisor->value() == -3);

        auto permissions = Flags<Permission>{Permission::Read, Permission::Write};
        CHECK(permissions.containsAll(Flags<Permission>{Permission::Read}));
        CHECK_FALSE(permissions.containsAny(Flags<Permission>{Permission::Execute}));

        permissions.remove(Flags<Permission>{Permission::Write});
        CHECK(permissions == Flags<Permission>{Permission::Read});
    }

    TEST_CASE("scope exit executes exactly while it remains armed")
    {
        auto executions = std::make_shared<std::atomic<int>>(0);
        {
            [[maybe_unused]] auto cleanup = scopeExit(
                [executions]() noexcept -> void
                {
                    executions->fetch_add(1, std::memory_order_relaxed);
                }
            );
        }

        {
            auto cleanup = scopeExit(
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
        auto counter = std::make_shared<Synchronized<int>>(0);
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

    TEST_CASE("synchronized default construction value-initializes scalars")
    {
        Synchronized<int> defaultValue;
        auto const initialized = defaultValue.withLock(
            [](int value) noexcept -> int
            {
                return value;
            }
        );
        CHECK(initialized == 0);
    }

    TEST_CASE("synchronized in-place construction uses parenthesized overload resolution")
    {
        auto repeatedValues = Synchronized<std::vector<int>>{
            std::in_place,
            std::vector<int>::size_type{3},
            7
        };
        auto const values = repeatedValues.withLock(
            [](std::vector<int> const& value) -> std::vector<int>
            {
                return value;
            }
        );
        CHECK(values == std::vector<int>{7, 7, 7});
    }
}
