#pragma once

#include "core/safety/annotations.hpp"

#include <memory>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace uf
{
    // The diagnostic payload is stored out of line so that Error stays one
    // pointer wide. Result<T> overlays T with Error, so every Result-returning
    // function pays sizeof(Error) on its success path, while the allocation
    // this costs is only paid when a failure is actually constructed.
    class Error final
    {
        struct Payload final
        {
            std::error_code          m_detailCode{};
            std::error_code          m_nativeCode{};
            std::string              m_message{};
            std::source_location     m_location{};
            std::vector<std::string> m_context{};
        };

        std::unique_ptr<Payload> m_payload;

        explicit Error(Payload payload);

        [[nodiscard]] auto payload() const noexcept UF_LIFETIME_BOUND -> Payload const&;

    public:
        // Classification lives in the detail code's category, so every error
        // names the vocabulary it belongs to. There is deliberately no second,
        // coarser code here: a module that needs one registers its own category
        // and its own classifier over it, rather than restating the same fact
        // less precisely at this layer.
        // nativeCode carries the originating operating-system or library code
        // together with its category, so a reader can tell a Win32 code from an
        // errno without knowing which subsystem produced the failure.
        Error(
            std::error_code detailCode,
            std::string message,
            std::error_code nativeCode = {},
            std::source_location location = std::source_location::current()
        );

        // An error has exactly one owner. Duplicating one is always deliberate
        // and goes through clone(), so a propagated error can never be left
        // aliased by the frame it came from.
        //
        // A moved-from Error owns no payload. Every observer below, and
        // addContext, then fails a release-active contract check and terminates
        // rather than reading freed state, so a moved-from Error may only be
        // destroyed or assigned to. Because the payload is heap-allocated its
        // address is stable, so a view obtained from message() or context()
        // stays valid across a move of the owning Error and is invalidated only
        // by that payload's destruction or by addContext.
        Error(Error const&) = delete;
        auto operator=(Error const&) -> Error& = delete;
        Error(Error&&) noexcept = default;
        auto operator=(Error&&) noexcept -> Error& = default;
        ~Error() = default;

        [[nodiscard]] auto clone() const -> Error;

        [[nodiscard]] auto detailCode() const noexcept -> std::error_code;
        [[nodiscard]] auto message() const noexcept UF_LIFETIME_BOUND -> std::string_view;
        [[nodiscard]] auto nativeCode() const noexcept -> std::error_code;
        [[nodiscard]] auto location() const noexcept -> std::source_location;
        // SAFETY: The returned span is invalidated by any subsequent addContext() call.
        [[nodiscard]]
        auto context() const noexcept UF_LIFETIME_BOUND -> std::span<std::string const>;

        auto addContext(std::string context) UF_LIFETIME_BOUND -> Error&;
    };

    [[nodiscard]] auto toString(Error const& error) -> std::string;
}
