#pragma once

#include <cstdint>
#include <source_location>
#include <string_view>

namespace umbra_flow::detail
{
    enum class ContractKind : std::uint8_t
    {
        Assertion,
        Check,
        Unreachable,
    };

    [[noreturn]]
    auto contractViolation(
        ContractKind kind,
        std::string_view expression,
        std::string_view message,
        std::source_location location
    ) noexcept -> void;
}

#if !defined(NDEBUG)
    #define UMBRA_FLOW_ASSERT(condition) \
        ((condition) \
            ? static_cast<void>(0) \
            : ::umbra_flow::detail::contractViolation( \
                ::umbra_flow::detail::ContractKind::Assertion, \
                #condition, \
                {}, \
                ::std::source_location::current() \
            ))
    #define UMBRA_FLOW_ASSERT_MSG(condition, message) \
        ((condition) \
            ? static_cast<void>(0) \
            : ::umbra_flow::detail::contractViolation( \
                ::umbra_flow::detail::ContractKind::Assertion, \
                #condition, \
                (message), \
                ::std::source_location::current() \
            ))
#else
    #define UMBRA_FLOW_ASSERT(condition) \
        static_cast<void>(sizeof(condition))
    #define UMBRA_FLOW_ASSERT_MSG(condition, message) \
        (static_cast<void>(sizeof(condition)), static_cast<void>(sizeof(message)))
#endif

#define UMBRA_FLOW_CHECK(condition) \
    ((condition) \
        ? static_cast<void>(0) \
        : ::umbra_flow::detail::contractViolation( \
            ::umbra_flow::detail::ContractKind::Check, \
            #condition, \
            {}, \
            ::std::source_location::current() \
        ))

#define UMBRA_FLOW_CHECK_MSG(condition, message) \
    ((condition) \
        ? static_cast<void>(0) \
        : ::umbra_flow::detail::contractViolation( \
            ::umbra_flow::detail::ContractKind::Check, \
            #condition, \
            (message), \
            ::std::source_location::current() \
        ))

#define UMBRA_FLOW_UNREACHABLE() \
    ::umbra_flow::detail::contractViolation( \
        ::umbra_flow::detail::ContractKind::Unreachable, \
        {}, \
        {}, \
        ::std::source_location::current() \
    )

#define UMBRA_FLOW_UNREACHABLE_MSG(message) \
    ::umbra_flow::detail::contractViolation( \
        ::umbra_flow::detail::ContractKind::Unreachable, \
        {}, \
        (message), \
        ::std::source_location::current() \
    )
