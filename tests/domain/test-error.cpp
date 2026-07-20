#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <array>
#include <string_view>

TEST_CASE("automation error display includes its kind and message")
{
    auto const error = uf::AutomationError::staleObservation("lease expired");

    CHECK(error.kind() == uf::AutomationErrorKind::StaleObservation);
    CHECK(uf::toString(error) == "StaleObservation: lease expired");
}

TEST_CASE("all automation error kinds survive the generic result channel")
{
    struct ErrorCase final
    {
        uf::AutomationErrorKind m_kind;
        std::string_view m_name;
        uf::ErrorCode m_genericCode;
    };

    auto const cases = std::array{
        ErrorCase{
            uf::AutomationErrorKind::Cancelled,
            "Cancelled",
            uf::ErrorCode::Cancelled
        },
        ErrorCase{
            uf::AutomationErrorKind::Timeout,
            "Timeout",
            uf::ErrorCode::Timeout
        },
        ErrorCase{
            uf::AutomationErrorKind::InvalidResource,
            "InvalidResource",
            uf::ErrorCode::InvalidArgument
        },
        ErrorCase{
            uf::AutomationErrorKind::UnsupportedCapability,
            "UnsupportedCapability",
            uf::ErrorCode::Unsupported
        },
        ErrorCase{
            uf::AutomationErrorKind::TargetCompatibilityUnverified,
            "TargetCompatibilityUnverified",
            uf::ErrorCode::FailedPrecondition
        },
        ErrorCase{
            uf::AutomationErrorKind::TargetUnavailable,
            "TargetUnavailable",
            uf::ErrorCode::NotFound
        },
        ErrorCase{
            uf::AutomationErrorKind::CaptureUnavailable,
            "CaptureUnavailable",
            uf::ErrorCode::External
        },
        ErrorCase{
            uf::AutomationErrorKind::CaptureStalled,
            "CaptureStalled",
            uf::ErrorCode::Timeout
        },
        ErrorCase{
            uf::AutomationErrorKind::RecognitionFailed,
            "RecognitionFailed",
            uf::ErrorCode::External
        },
        ErrorCase{
            uf::AutomationErrorKind::StaleObservation,
            "StaleObservation",
            uf::ErrorCode::FailedPrecondition
        },
        ErrorCase{
            uf::AutomationErrorKind::ActionRejected,
            "ActionRejected",
            uf::ErrorCode::FailedPrecondition
        },
        ErrorCase{
            uf::AutomationErrorKind::ControllerDisconnected,
            "ControllerDisconnected",
            uf::ErrorCode::External
        },
        ErrorCase{
            uf::AutomationErrorKind::InternalInvariant,
            "InternalInvariant",
            uf::ErrorCode::Internal
        },
    };
    for (auto const& testCase : cases)
    {
        auto result = uf::Result<int>{
            uf::fail(testCase.m_kind, "domain failure")
        };

        REQUIRE_FALSE(result.has_value());
        auto const kind = uf::automationErrorKind(result.error());
        REQUIRE(kind.has_value());
        CHECK(*kind == testCase.m_kind);
        CHECK(result.error().code() == testCase.m_genericCode);
        CHECK(result.error().detailCode().message() == testCase.m_name);
    }
}

TEST_CASE("generic errors do not impersonate automation errors")
{
    auto result = uf::Result<int>{
        uf::fail(uf::ErrorCode::Internal, "generic failure")
    };

    REQUIRE_FALSE(result.has_value());
    CHECK_FALSE(uf::automationErrorKind(result.error()).has_value());
}
