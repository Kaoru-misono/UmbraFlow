#include "error.hpp"

#include "contracts.hpp"

#include <sstream>
#include <utility>

namespace umbra_flow
{
    Error::Error(
        ErrorCode code,
        std::string message,
        std::int64_t nativeCode,
        std::source_location location
    )
        : m_code{code}
        , m_message{std::move(message)}
        , m_nativeCode{nativeCode}
        , m_location{location}
    {
    }

    auto Error::code() const noexcept -> ErrorCode { return m_code; }
    auto Error::message() const noexcept -> std::string_view { return m_message; }
    auto Error::nativeCode() const noexcept -> std::int64_t { return m_nativeCode; }
    auto Error::location() const noexcept -> std::source_location { return m_location; }
    auto Error::context() const noexcept -> std::span<std::string const>
    {
        return std::span<std::string const>{m_context};
    }

    auto Error::addContext(std::string context) -> Error&
    {
        m_context.emplace_back(std::move(context));
        return *this;
    }

    auto errorCodeName(ErrorCode code) noexcept -> std::string_view
    {
        switch (code)
        {
        case ErrorCode::Cancelled: return "Cancelled";
        case ErrorCode::Timeout: return "Timeout";
        case ErrorCode::InvalidArgument: return "InvalidArgument";
        case ErrorCode::FailedPrecondition: return "FailedPrecondition";
        case ErrorCode::NotFound: return "NotFound";
        case ErrorCode::AlreadyExists: return "AlreadyExists";
        case ErrorCode::PermissionDenied: return "PermissionDenied";
        case ErrorCode::ResourceExhausted: return "ResourceExhausted";
        case ErrorCode::Unsupported: return "Unsupported";
        case ErrorCode::Io: return "Io";
        case ErrorCode::External: return "External";
        case ErrorCode::Internal: return "Internal";
        }

        UMBRA_FLOW_UNREACHABLE_MSG("Unknown ErrorCode value");
    }

    auto toString(Error const& error) -> std::string
    {
        auto stream = std::ostringstream{};
        auto const location = error.location();

        stream << '[' << errorCodeName(error.code()) << "] ";
        stream << error.message();
        stream << " at " << location.file_name() << ':' << location.line();
        stream << " in " << location.function_name();

        if (error.nativeCode() != 0)
        {
            stream << " (native code " << error.nativeCode() << ')';
        }

        for (auto const& context : error.context())
        {
            stream << "\n  context: " << context;
        }

        return stream.str();
    }
}
