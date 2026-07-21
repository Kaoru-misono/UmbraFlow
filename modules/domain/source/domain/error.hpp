#pragma once

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/enum-reflection.hpp>

#include <cstdint>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <system_error>

namespace uf
{
    enum class AutomationErrorKind : std::uint8_t
    {
        Cancelled,
        Timeout,
        InvalidResource,
        UnsupportedCapability,
        TargetCompatibilityUnverified,
        TargetUnavailable,
        CaptureUnavailable,
        CaptureStalled,
        RecognitionFailed,
        StaleObservation,
        ActionRejected,
        ControllerDisconnected,
        InternalInvariant,
    };

    class AutomationError final
    {
        AutomationErrorKind m_kind;
        std::string m_message;

    public:
        AutomationError(AutomationErrorKind kind, std::string message);

        auto operator==(AutomationError const&) const -> bool = default;

        [[nodiscard]] auto kind() const noexcept -> AutomationErrorKind;
        [[nodiscard]] auto message() const noexcept UF_LIFETIME_BOUND -> std::string_view;

        [[nodiscard]] static auto staleObservation(std::string message) -> AutomationError;
        [[nodiscard]] static auto actionRejected(std::string message) -> AutomationError;
        [[nodiscard]] static auto internalInvariant(std::string message) -> AutomationError;
    };

    [[nodiscard]] auto toString(AutomationError const& error) -> std::string;

    [[nodiscard]]
    auto genericErrorCode(AutomationErrorKind kind) noexcept -> ErrorCode;

    [[nodiscard]]
    auto automationErrorDetailCode(AutomationErrorKind kind) noexcept -> std::error_code;

    [[nodiscard]]
    auto automationErrorKind(Error const& error) noexcept -> std::optional<AutomationErrorKind>;

    [[nodiscard]]
    auto fail(
        AutomationErrorKind kind,
        std::string message,
        std::source_location location = std::source_location::current()
    ) -> std::unexpected<Error>;
}

UF_REFLECT_ENUM(
    uf::AutomationErrorKind,
    uf::AutomationErrorKind::Cancelled,
    uf::AutomationErrorKind::Timeout,
    uf::AutomationErrorKind::InvalidResource,
    uf::AutomationErrorKind::UnsupportedCapability,
    uf::AutomationErrorKind::TargetCompatibilityUnverified,
    uf::AutomationErrorKind::TargetUnavailable,
    uf::AutomationErrorKind::CaptureUnavailable,
    uf::AutomationErrorKind::CaptureStalled,
    uf::AutomationErrorKind::RecognitionFailed,
    uf::AutomationErrorKind::StaleObservation,
    uf::AutomationErrorKind::ActionRejected,
    uf::AutomationErrorKind::ControllerDisconnected,
    uf::AutomationErrorKind::InternalInvariant
);
