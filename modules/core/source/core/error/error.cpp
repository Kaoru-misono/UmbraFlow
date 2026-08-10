#include "error.hpp"

#include "contracts.hpp"

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
}
