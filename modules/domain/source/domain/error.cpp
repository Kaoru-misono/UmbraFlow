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
                    if (automationErrorDetailValue(entry.m_value) == value)
                    {
                        return std::string{entry.m_name};
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
    }

    AutomationError::AutomationError(AutomationErrorKind kind, std::string message)
        : m_kind{kind}
        , m_message{std::move(message)}
    {
    }

    auto AutomationError::kind() const noexcept -> AutomationErrorKind { return m_kind; }
    auto AutomationError::message() const noexcept -> std::string_view { return m_message; }

    auto AutomationError::staleObservation(std::string message) -> AutomationError
    {
        return AutomationError{AutomationErrorKind::StaleObservation, std::move(message)};
    }

    auto AutomationError::actionRejected(std::string message) -> AutomationError
    {
        return AutomationError{AutomationErrorKind::ActionRejected, std::move(message)};
    }

    auto AutomationError::internalInvariant(std::string message) -> AutomationError
    {
        return AutomationError{AutomationErrorKind::InternalInvariant, std::move(message)};
    }

    auto toString(AutomationError const& error) -> std::string
    {
        auto const name = enumName(error.kind());
        UF_CHECK(name.has_value());
        return std::string{*name} + ": " + std::string{error.message()};
    }

    auto genericErrorCode(AutomationErrorKind kind) noexcept -> ErrorCode
    {
        switch (kind)
        {
        case AutomationErrorKind::Cancelled: return ErrorCode::Cancelled;
        case AutomationErrorKind::Timeout: return ErrorCode::Timeout;
        case AutomationErrorKind::InvalidResource: return ErrorCode::InvalidArgument;
        case AutomationErrorKind::UnsupportedCapability: return ErrorCode::Unsupported;
        case AutomationErrorKind::TargetCompatibilityUnverified:
            return ErrorCode::FailedPrecondition;
        case AutomationErrorKind::TargetUnavailable: return ErrorCode::NotFound;
        case AutomationErrorKind::CaptureUnavailable: return ErrorCode::External;
        case AutomationErrorKind::CaptureStalled: return ErrorCode::Timeout;
        case AutomationErrorKind::RecognitionFailed: return ErrorCode::External;
        case AutomationErrorKind::StaleObservation: return ErrorCode::FailedPrecondition;
        case AutomationErrorKind::ActionRejected: return ErrorCode::FailedPrecondition;
        case AutomationErrorKind::ControllerDisconnected: return ErrorCode::External;
        case AutomationErrorKind::InternalInvariant: return ErrorCode::Internal;
        }

        UF_UNREACHABLE_MSG("Unknown AutomationErrorKind value");
    }

    auto automationErrorDetailCode(AutomationErrorKind kind) noexcept -> std::error_code
    {
        return std::error_code{
            automationErrorDetailValue(kind),
            automationErrorCategory()
        };
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
            if (automationErrorDetailValue(entry.m_value) == detailCode.value())
            {
                return entry.m_value;
            }
        }

        return std::nullopt;
    }

    auto fail(
        AutomationErrorKind kind,
        std::string message,
        std::source_location location
    ) -> std::unexpected<Error>
    {
        return uf::fail(
            genericErrorCode(kind),
            automationErrorDetailCode(kind),
            std::move(message),
            0,
            location
        );
    }
}
