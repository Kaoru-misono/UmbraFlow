#include <error-text.hpp>

#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <string>
#include <system_error>

namespace uf::input_agent
{
    TEST_CASE("automation error detail keeps the kind, context, and origin")
    {
        auto failure = fail(
            AutomationErrorKind::StaleObservation,
            "frame lease expired"
        );
        failure.error().addContext("click compensation failed");

        auto const formatted = formatAutomationError(failure.error());
        CHECK(formatted.starts_with("StaleObservation: frame lease expired"));
        CHECK(formatted.contains("| click compensation failed"));
        CHECK(formatted.contains("| at test-error-text.cpp:"));
    }

    TEST_CASE("automation error detail names the native category and value")
    {
        auto const native = std::error_code{5, std::system_category()};
        auto const failure = fail(
            AutomationErrorKind::IoFailure,
            "cannot write the trace",
            native
        );

        auto const formatted = formatAutomationError(failure.error());
        CHECK(formatted.contains(native.category().name()));
        CHECK(formatted.contains("5"));
        CHECK(formatted.contains(native.message()));
    }
}
