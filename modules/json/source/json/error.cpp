#include "error.hpp"

#include <core/error/contracts.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>

#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace uf::json
{
    namespace
    {
        // Offset by one so no kind encodes as zero, which std::error_code
        // reserves for "no error".
        [[nodiscard]]
        auto detailValue(ErrorKind kind) noexcept -> int
        {
            auto const underlying = checkedCast<int>(std::to_underlying(kind));
            UF_CHECK(underlying.has_value());
            auto const encoded = checkedAdd(*underlying, 1);
            UF_CHECK(encoded.has_value());
            return *encoded;
        }

        class Category final : public std::error_category
        {
        public:
            [[nodiscard]] auto name() const noexcept -> char const* override
            {
                return "uf.json";
            }

            [[nodiscard]] auto message(int value) const -> std::string override
            {
                for (auto const& entry : enumEntries<ErrorKind>())
                {
                    if (detailValue(entry.value) == value)
                    {
                        return std::string{entry.name};
                    }
                }

                return "UnknownJsonErrorKind";
            }
        };

        [[nodiscard]]
        auto category() noexcept -> std::error_category const&
        {
            static auto const s_category = Category{};
            return s_category;
        }
    }

    auto errorKind(Error const& error) noexcept -> std::optional<ErrorKind>
    {
        auto const code = error.detailCode();
        if (code.category() != category())
        {
            return std::nullopt;
        }

        for (auto const& entry : enumEntries<ErrorKind>())
        {
            if (detailValue(entry.value) == code.value())
            {
                return entry.value;
            }
        }

        return std::nullopt;
    }

    auto fail(ErrorKind kind, std::string message, std::source_location location)
        -> std::unexpected<Error>
    {
        return uf::fail(
            std::error_code{detailValue(kind), category()},
            std::move(message),
            {},
            location
        );
    }
}
