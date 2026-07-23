#pragma once

#include "core/safety/annotations.hpp"

#include <core/types/integer.hpp>

#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace uf
{
    enum class ErrorCode : uint8
    {
        Cancelled,
        Timeout,
        InvalidArgument,
        FailedPrecondition,
        NotFound,
        AlreadyExists,
        PermissionDenied,
        ResourceExhausted,
        Unsupported,
        Io,
        External,
        Internal,
    };

    class Error final
    {
        ErrorCode                m_code;
        std::error_code          m_detailCode;
        std::string              m_message;
        int64                    m_nativeCode;
        std::source_location     m_location;
        std::vector<std::string> m_context{};

    public:
        Error(
            ErrorCode code,
            std::string message,
            int64 nativeCode = 0,
            std::source_location location = std::source_location::current()
        );
        Error(
            ErrorCode code,
            std::error_code detailCode,
            std::string message,
            int64 nativeCode = 0,
            std::source_location location = std::source_location::current()
        );

        [[nodiscard]] auto code() const noexcept -> ErrorCode;
        [[nodiscard]] auto detailCode() const noexcept -> std::error_code;
        [[nodiscard]] auto message() const noexcept UF_LIFETIME_BOUND -> std::string_view;
        [[nodiscard]] auto nativeCode() const noexcept -> int64;
        [[nodiscard]] auto location() const noexcept -> std::source_location;
        // SAFETY: The returned span is invalidated by any subsequent addContext() call.
        [[nodiscard]]
        auto context() const noexcept UF_LIFETIME_BOUND -> std::span<std::string const>;

        auto addContext(std::string context) UF_LIFETIME_BOUND -> Error&;
    };

    [[nodiscard]] auto errorCodeName(ErrorCode code) noexcept -> std::string_view;
    [[nodiscard]] auto toString(Error const& error) -> std::string;
}
