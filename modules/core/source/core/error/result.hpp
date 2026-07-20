#pragma once

#include "error.hpp"

#include <expected>
#include <source_location>
#include <string>
#include <utility>

namespace umbra_flow
{
    template <typename Value>
    using Result = std::expected<Value, Error>;

    using Status = Result<void>;

    [[nodiscard]]
    inline auto fail(
        ErrorCode code,
        std::string message,
        std::int64_t nativeCode = 0,
        std::source_location location = std::source_location::current()
    ) -> std::unexpected<Error>
    {
        return std::unexpected{
            Error{code, std::move(message), nativeCode, location}
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

#define UMBRA_FLOW_TRY(expression) \
    do \
    { \
        auto cppTemplateResult = (expression); \
        if (!cppTemplateResult) \
        { \
            return ::std::unexpected{::std::move(cppTemplateResult).error()}; \
        } \
    } while (false)

#define UMBRA_FLOW_TRY_CONTEXT(expression, context) \
    do \
    { \
        auto cppTemplateResult = (expression); \
        if (!cppTemplateResult) \
        { \
            auto cppTemplateError = ::std::move(cppTemplateResult).error(); \
            cppTemplateError.addContext(context); \
            return ::std::unexpected{::std::move(cppTemplateError)}; \
        } \
    } while (false)

#define UMBRA_FLOW_DETAIL_CONCAT_INNER(left, right) left##right
#define UMBRA_FLOW_DETAIL_CONCAT(left, right) UMBRA_FLOW_DETAIL_CONCAT_INNER(left, right)

// Declaration-style value propagation must be used as a standalone statement
// inside a braced block so the extracted value remains in the caller's scope.
#define UMBRA_FLOW_DETAIL_TRY_VALUE_IMPL(resultName, valueName, expression) \
    auto resultName = (expression); \
    if (!resultName) \
    { \
        return ::std::unexpected{::std::move(resultName).error()}; \
    } \
    auto valueName = *::std::move(resultName)

#define UMBRA_FLOW_DETAIL_TRY_VALUE_CONTEXT_IMPL(resultName, valueName, expression, context) \
    auto resultName = (expression); \
    if (!resultName) \
    { \
        auto cppTemplateError = ::std::move(resultName).error(); \
        cppTemplateError.addContext(context); \
        return ::std::unexpected{::std::move(cppTemplateError)}; \
    } \
    auto valueName = *::std::move(resultName)

#define UMBRA_FLOW_TRY_VALUE(valueName, expression) \
    UMBRA_FLOW_DETAIL_TRY_VALUE_IMPL( \
        UMBRA_FLOW_DETAIL_CONCAT(cppTemplateResult_, valueName), \
        valueName, \
        expression \
    )

#define UMBRA_FLOW_TRY_VALUE_CONTEXT(valueName, expression, context) \
    UMBRA_FLOW_DETAIL_TRY_VALUE_CONTEXT_IMPL( \
        UMBRA_FLOW_DETAIL_CONCAT(cppTemplateResult_, valueName), \
        valueName, \
        expression, \
        context \
    )
