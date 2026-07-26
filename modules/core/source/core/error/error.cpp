#include "error.hpp"

#include "contracts.hpp"

#include <sstream>
#include <utility>

namespace uf
{
    Error::Error(
        std::error_code detailCode,
        std::string message,
        std::error_code nativeCode,
        std::source_location location
    )
        : Error{
            Payload{
                .detailCode = detailCode,
                .nativeCode = nativeCode,
                .message    = std::move(message),
                .location   = location,
            }
        }
    {
    }

    Error::Error(Payload payload)
        : m_payload{std::make_unique<Payload>(std::move(payload))}
    {
    }

    auto Error::payload() const noexcept -> Error::Payload const&
    {
        // A live error always owns its payload; only a moved-from error does
        // not, and reading one is a caller defect rather than a failure mode.
        UF_CHECK(m_payload != nullptr);
        return *m_payload;
    }

    auto Error::clone() const -> Error { return Error{Payload{payload()}}; }

    auto Error::detailCode() const noexcept -> std::error_code
    {
        return payload().detailCode;
    }
    auto Error::message() const noexcept -> std::string_view
    {
        return payload().message;
    }
    auto Error::nativeCode() const noexcept -> std::error_code
    {
        return payload().nativeCode;
    }
    auto Error::location() const noexcept -> std::source_location
    {
        return payload().location;
    }
    auto Error::context() const noexcept -> std::span<std::string const>
    {
        return std::span<std::string const>{payload().context};
    }

    auto Error::addContext(std::string context) -> Error&
    {
        UF_CHECK(m_payload != nullptr);
        m_payload->context.emplace_back(std::move(context));
        return *this;
    }

    auto toString(Error const& error) -> std::string
    {
        auto stream = std::ostringstream{};
        auto const location = error.location();
        auto const detailCode = error.detailCode();

        stream << '[' << detailCode.category().name();
        stream << ':' << detailCode.message() << "] ";
        stream << error.message();
        stream << " at " << location.file_name() << ':' << location.line();
        stream << " in " << location.function_name();

        if (auto const nativeCode = error.nativeCode(); nativeCode)
        {
            stream << " (" << nativeCode.category().name();
            stream << ' ' << nativeCode.value();
            stream << ": " << nativeCode.message() << ')';
        }

        for (auto const& context : error.context())
        {
            stream << "\n  context: " << context;
        }

        return stream.str();
    }
}
