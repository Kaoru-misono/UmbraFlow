#pragma once

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uf::annotation::detail
{
    class CanonicalTomlReader final
    {
        std::string m_documentName;
        std::string m_text;
        std::size_t m_offset{};
        std::size_t m_line{1};

        [[nodiscard]]
        auto lineAtOffset() const -> Result<std::string>;

        [[nodiscard]]
        auto fieldValue(
            std::string_view line,
            std::string_view key
        ) const -> Result<std::string>;

        [[nodiscard]]
        auto parseTomlString(std::string_view encoded) const -> Result<std::string>;

        [[nodiscard]]
        auto parseUnsigned64(std::string_view encoded) const -> Result<uint64>;

        [[nodiscard]]
        auto parseUnsigned32Array(
            std::string_view encoded
        ) const -> Result<std::vector<uint32>>;

        [[nodiscard]]
        auto parseStringArray(
            std::string_view encoded
        ) const -> Result<std::vector<std::string>>;

    public:
        CanonicalTomlReader(
            std::string documentName,
            std::string text
        ) noexcept;

        [[nodiscard]]
        auto invalid(std::string message) const -> std::unexpected<Error>;

        [[nodiscard]] auto eof() const noexcept -> bool;
        [[nodiscard]] auto line() const noexcept -> std::size_t;

        [[nodiscard]] auto take() -> Result<std::string>;

        [[nodiscard]] auto expect(std::string_view expected) -> Status;

        [[nodiscard]]
        auto takeStringField(std::string_view key) -> Result<std::string>;

        [[nodiscard]]
        auto takeUnsigned32Field(std::string_view key) -> Result<uint32>;

        [[nodiscard]]
        auto takeUnsigned64Field(std::string_view key) -> Result<uint64>;

        [[nodiscard]]
        auto takeUnsigned32ArrayField(
            std::string_view key
        ) -> Result<std::vector<uint32>>;

        [[nodiscard]]
        auto takeStringArrayField(
            std::string_view key
        ) -> Result<std::vector<std::string>>;

        [[nodiscard]]
        auto nextIsField(std::string_view key) const -> Result<bool>;
    };

    auto appendTomlString(
        std::string& output,
        std::string_view value
    ) -> void;

    auto appendStringField(
        std::string& output,
        std::string_view key,
        std::string_view value
    ) -> void;

    auto appendUnsigned32Array(
        std::string& output,
        std::span<uint32 const> values
    ) -> void;
}
