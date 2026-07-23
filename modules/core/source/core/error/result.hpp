#pragma once

#include "error.hpp"

#include <expected>
#include <source_location>
#include <string>
#include <system_error>
#include <utility>

namespace uf
{
    template <typename Value>
    using Result = std::expected<Value, Error>;

    using Status = Result<void>;

    [[nodiscard]]
    inline auto fail(
        std::error_code detailCode,
        std::string message,
        std::error_code nativeCode = {},
        std::source_location location = std::source_location::current()
    ) -> std::unexpected<Error>
    {
        return std::unexpected{
            Error{detailCode, std::move(message), nativeCode, location}
        };
    }

    [[nodiscard]] inline auto ok() -> Status { return {}; }

    template <typename Value>
    [[nodiscard]]
    auto withContext(Result<Value> result, std::string context) -> Result<Value>
    {
        if (!result)
        {
            result.error().addContext(std::move(context));
        }

        return result;
    }
}

// The result is held by value. Binding it to auto&& instead would only extend
// the lifetime of a prvalue, so an expression yielding a reference into a
// temporary would leave the macro reading freed storage; holding the value
// makes that unrepresentable. Error is move-only, so an lvalue operand must be
// moved in by the caller.
#define UF_TRY(expression) \
    do \
    { \
        auto ufResult = (expression); \
        if (!ufResult) \
        { \
            return ::std::unexpected{::std::move(ufResult).error()}; \
        } \
    } while (false)

#define UF_TRY_CONTEXT(expression, context) \
    do \
    { \
        auto ufResult = (expression); \
        if (!ufResult) \
        { \
            auto ufError = ::std::move(ufResult).error(); \
            ufError.addContext(context); \
            return ::std::unexpected{::std::move(ufError)}; \
        } \
    } while (false)

#define UF_DETAIL_CONCAT_INNER(left, right) left##right
#define UF_DETAIL_CONCAT(left, right) UF_DETAIL_CONCAT_INNER(left, right)

// Declaration-style value propagation must be used as a standalone statement
// inside a braced block so the extracted value remains in the caller's scope.
#define UF_DETAIL_TRY_VALUE_IMPL(resultName, valueName, expression) \
    auto resultName = (expression); \
    if (!resultName) \
    { \
        return ::std::unexpected{::std::move(resultName).error()}; \
    } \
    auto valueName = *::std::move(resultName)

#define UF_DETAIL_TRY_VALUE_CONTEXT_IMPL(resultName, valueName, expression, context) \
    auto resultName = (expression); \
    if (!resultName) \
    { \
        auto ufError = ::std::move(resultName).error(); \
        ufError.addContext(context); \
        return ::std::unexpected{::std::move(ufError)}; \
    } \
    auto valueName = *::std::move(resultName)

#define UF_TRY_VALUE(valueName, expression) \
    UF_DETAIL_TRY_VALUE_IMPL( \
        UF_DETAIL_CONCAT(ufResult, __LINE__), \
        valueName, \
        expression \
    )

#define UF_TRY_VALUE_CONTEXT(valueName, expression, context) \
    UF_DETAIL_TRY_VALUE_CONTEXT_IMPL( \
        UF_DETAIL_CONCAT(ufResult, __LINE__), \
        valueName, \
        expression, \
        context \
    )
