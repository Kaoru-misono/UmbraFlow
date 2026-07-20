#pragma once

#include <cstdint>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace umbra_flow
{
    enum class ErrorCode : std::uint8_t
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
        ErrorCode m_code;
        std::string m_message;
        std::int64_t m_nativeCode;
        std::source_location m_location;
        std::vector<std::string> m_context{};

    public:
        Error(
            ErrorCode code,
            std::string message,
            std::int64_t nativeCode = 0,
            std::source_location location = std::source_location::current()
        );

        [[nodiscard]] auto code() const noexcept -> ErrorCode;
        [[nodiscard]] auto message() const noexcept -> std::string_view;
        [[nodiscard]] auto nativeCode() const noexcept -> std::int64_t;
        [[nodiscard]] auto location() const noexcept -> std::source_location;
        [[nodiscard]] auto context() const noexcept -> std::span<std::string const>;

        auto addContext(std::string context) -> Error&;
    };

    [[nodiscard]] auto errorCodeName(ErrorCode code) noexcept -> std::string_view;
    [[nodiscard]] auto toString(Error const& error) -> std::string;
}
