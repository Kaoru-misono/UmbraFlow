#include "error.hpp"

#include <core/error/contracts.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>

#include <string>
#include <utility>

namespace uf
{
    namespace
    {
        [[nodiscard]]
        auto automationErrorDetailValue(AutomationErrorKind kind) noexcept -> int
        {
            auto const underlying = checkedCast<int>(std::to_underlying(kind));
            UF_CHECK(underlying.has_value());
            auto const encoded = checkedAdd(*underlying, 1);
            UF_CHECK(encoded.has_value());
            return *encoded;
        }

        class AutomationErrorCategory final : public std::error_category
        {
        public:
            [[nodiscard]] auto name() const noexcept -> char const* override
            {
                return "uf.automation";
            }

            [[nodiscard]] auto message(int value) const -> std::string override
            {
                for (auto const& entry : enumEntries<AutomationErrorKind>())
                {
                    if (automationErrorDetailValue(entry.value) == value)
                    {
                        return std::string{entry.name};
                    }
                }

                return "UnknownAutomationErrorKind";
            }
        };

        [[nodiscard]]
        auto automationErrorCategory() noexcept -> std::error_category const&
        {
            static auto const s_category = AutomationErrorCategory{};
            return s_category;
        }

        [[nodiscard]]
        auto automationErrorDetailCode(AutomationErrorKind kind) noexcept
            -> std::error_code
        {
            return std::error_code{
                automationErrorDetailValue(kind),
                automationErrorCategory()
            };
        }
    }

    auto systemErrorCode(uint32 value) noexcept -> std::error_code
    {
        // SAFETY: since C++20 an out-of-range unsigned-to-signed conversion is
        // defined as the unique value congruent modulo 2^N, so this wraps
        // rather than being implementation-defined, and std::system_category()
        // interprets the int through the same platform mapping the value came
        // from. A checked cast would reject exactly the high-bit codes worth
        // keeping.
        return std::error_code{static_cast<int>(value), std::system_category()};
    }

    auto failureResponse(AutomationErrorKind kind) noexcept -> FailureResponse
    {
        switch (kind)
        {
        case AutomationErrorKind::Cancelled: return FailureResponse::Cancelled;
        case AutomationErrorKind::CaptureStalled: return FailureResponse::Retry;
        case AutomationErrorKind::StaleObservation: return FailureResponse::Retry;
        case AutomationErrorKind::RecognitionFailed: return FailureResponse::StepFailed;
        case AutomationErrorKind::ActionRejected: return FailureResponse::StepFailed;
        case AutomationErrorKind::Timeout: return FailureResponse::Abort;
        case AutomationErrorKind::InvalidResource: return FailureResponse::Abort;
        case AutomationErrorKind::UnsupportedCapability: return FailureResponse::Abort;
        case AutomationErrorKind::TargetCompatibilityUnverified: return FailureResponse::Abort;
        case AutomationErrorKind::TargetUnavailable: return FailureResponse::Abort;
        case AutomationErrorKind::CaptureUnavailable: return FailureResponse::Abort;
        case AutomationErrorKind::ControllerDisconnected: return FailureResponse::Abort;
        case AutomationErrorKind::InternalInvariant: return FailureResponse::Abort;
        case AutomationErrorKind::IoFailure: return FailureResponse::Abort;
        case AutomationErrorKind::ExternalFailure: return FailureResponse::Abort;
        }

        UF_UNREACHABLE_MSG("Unknown AutomationErrorKind value");
    }

    auto failureResponse(Error const& error) noexcept -> FailureResponse
    {
        auto const kind = automationErrorKind(error);
        if (!kind)
        {
            return FailureResponse::Abort;
        }

        return failureResponse(*kind);
    }

    auto automationErrorKind(Error const& error) noexcept -> std::optional<AutomationErrorKind>
    {
        auto const detailCode = error.detailCode();
        if (detailCode.category() != automationErrorCategory())
        {
            return std::nullopt;
        }

        for (auto const& entry : enumEntries<AutomationErrorKind>())
        {
            if (automationErrorDetailValue(entry.value) == detailCode.value())
            {
                return entry.value;
            }
        }

        return std::nullopt;
    }

    auto fail(
        AutomationErrorKind kind,
        std::string message,
        std::error_code nativeCode,
        std::source_location location
    ) -> std::unexpected<Error>
    {
        return uf::fail(
            automationErrorDetailCode(kind),
            std::move(message),
            nativeCode,
            location
        );
    }
}
