#pragma once

#include <core/types/integer.hpp>

#include <source_location>
#include <string_view>

namespace uf::detail
{
    enum class ContractKind : uint8
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
    #define UF_ASSERT(condition) \
        ( \
            (condition) \
                ? static_cast<void>(0) \
                : ::uf::detail::contractViolation( \
                    ::uf::detail::ContractKind::Assertion, \
                    #condition, \
                    {}, \
                    ::std::source_location::current() \
                ) \
        )
    #define UF_ASSERT_MSG(condition, message) \
        ( \
            (condition) \
                ? static_cast<void>(0) \
                : ::uf::detail::contractViolation( \
                    ::uf::detail::ContractKind::Assertion, \
                    #condition, \
                    (message), \
                    ::std::source_location::current() \
                ) \
        )
#else
    #define UF_ASSERT(condition) \
        static_cast<void>(sizeof(condition))
    #define UF_ASSERT_MSG(condition, message) \
        ( \
            static_cast<void>(sizeof(condition)), \
            static_cast<void>(sizeof(message)) \
        )
#endif

#define UF_CHECK(condition) \
    ( \
        (condition) \
            ? static_cast<void>(0) \
            : ::uf::detail::contractViolation( \
                ::uf::detail::ContractKind::Check, \
                #condition, \
                {}, \
                ::std::source_location::current() \
            ) \
    )

#define UF_CHECK_MSG(condition, message) \
    ( \
        (condition) \
            ? static_cast<void>(0) \
            : ::uf::detail::contractViolation( \
                ::uf::detail::ContractKind::Check, \
                #condition, \
                (message), \
                ::std::source_location::current() \
            ) \
    )

#define UF_UNREACHABLE() \
    ::uf::detail::contractViolation( \
        ::uf::detail::ContractKind::Unreachable, \
        {}, \
        {}, \
        ::std::source_location::current() \
    )

#define UF_UNREACHABLE_MSG(message) \
    ::uf::detail::contractViolation( \
        ::uf::detail::ContractKind::Unreachable, \
        {}, \
        (message), \
        ::std::source_location::current() \
    )
