#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <array>
#include <string_view>
#include <system_error>

TEST_CASE("all automation error kinds survive the generic result channel")
{
    struct ErrorCase final
    {
        uf::AutomationErrorKind m_kind{};
        std::string_view        m_name{};
        uf::FailureResponse     m_response{};
    };

    auto const cases = std::array{
        ErrorCase{
            uf::AutomationErrorKind::Cancelled,
            "Cancelled",
            uf::FailureResponse::Cancelled
        },
        ErrorCase{
            uf::AutomationErrorKind::Timeout,
            "Timeout",
            uf::FailureResponse::Abort
        },
        ErrorCase{
            uf::AutomationErrorKind::InvalidResource,
            "InvalidResource",
            uf::FailureResponse::Abort
        },
        ErrorCase{
            uf::AutomationErrorKind::UnsupportedCapability,
            "UnsupportedCapability",
            uf::FailureResponse::Abort
        },
        ErrorCase{
            uf::AutomationErrorKind::TargetCompatibilityUnverified,
            "TargetCompatibilityUnverified",
            uf::FailureResponse::Abort
        },
        ErrorCase{
            uf::AutomationErrorKind::TargetUnavailable,
            "TargetUnavailable",
            uf::FailureResponse::Abort
        },
        ErrorCase{
            uf::AutomationErrorKind::CaptureUnavailable,
            "CaptureUnavailable",
            uf::FailureResponse::Abort
        },
        ErrorCase{
            uf::AutomationErrorKind::CaptureStalled,
            "CaptureStalled",
            uf::FailureResponse::Retry
        },
        ErrorCase{
            uf::AutomationErrorKind::RecognitionFailed,
            "RecognitionFailed",
            uf::FailureResponse::StepFailed
        },
        ErrorCase{
            uf::AutomationErrorKind::StaleObservation,
            "StaleObservation",
            uf::FailureResponse::Retry
        },
        ErrorCase{
            uf::AutomationErrorKind::ActionRejected,
            "ActionRejected",
            uf::FailureResponse::StepFailed
        },
        ErrorCase{
            uf::AutomationErrorKind::ControllerDisconnected,
            "ControllerDisconnected",
            uf::FailureResponse::Abort
        },
        ErrorCase{
            uf::AutomationErrorKind::InternalInvariant,
            "InternalInvariant",
            uf::FailureResponse::Abort
        },
        ErrorCase{
            uf::AutomationErrorKind::IoFailure,
            "IoFailure",
            uf::FailureResponse::Abort
        },
        ErrorCase{
            uf::AutomationErrorKind::ExternalFailure,
            "ExternalFailure",
            uf::FailureResponse::Abort
        },
    };
    CHECK(cases.size() == uf::enumEntries<uf::AutomationErrorKind>().size());

    for (auto const& testCase : cases)
    {
        auto result = uf::Result<int>{
            uf::fail(testCase.m_kind, "domain failure")
        };

        REQUIRE_FALSE(result.has_value());
        auto const kind = uf::automationErrorKind(result.error());
        REQUIRE(kind.has_value());
        CHECK(kind == testCase.m_kind);
        CHECK(result.error().detailCode());
        CHECK(result.error().detailCode().message() == testCase.m_name);
        CHECK(uf::failureResponse(result.error()) == testCase.m_response);
    }
}

TEST_CASE("system error codes keep operating-system values with the high bit set")
{
    // An HRESULT-shaped GetLastError result exceeds INT_MAX. A checked cast
    // rejects it and yields an empty code, silently dropping the structured
    // cause; this pins the wrapping conversion that keeps it.
    auto const wide = uf::systemErrorCode(0x8007'0005U);
    CHECK(wide.value() == static_cast<int>(0x8007'0005U));
    CHECK(wide.category() == std::system_category());

    auto const ordinary = uf::systemErrorCode(5U);
    CHECK(ordinary.value() == 5);
    CHECK(ordinary.category() == std::system_category());
}

TEST_CASE("generic errors do not impersonate automation errors")
{
    auto result = uf::Result<int>{
        uf::fail(std::make_error_code(std::errc::io_error), "generic failure")
    };

    REQUIRE_FALSE(result.has_value());
    CHECK_FALSE(uf::automationErrorKind(result.error()).has_value());
    CHECK(uf::failureResponse(result.error()) == uf::FailureResponse::Abort);
}
