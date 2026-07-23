#include "content-hash.hpp"

#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/safety/checked-access.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>

namespace uf::annotation
{
    namespace
    {
        constexpr auto g_sha256Prefix = std::string_view{"sha256:"};
        constexpr auto g_hexDigits = std::string_view{"0123456789abcdef"};
        constexpr auto g_sha256BlockBytes = std::size_t{64};
        constexpr auto g_sha256LengthBytes = std::size_t{8};
        constexpr auto g_sha256DigestBytes = std::size_t{32};

        constexpr auto g_initialState = std::array<uint32, 8>{
            0x6A09E667U,
            0xBB67AE85U,
            0x3C6EF372U,
            0xA54FF53AU,
            0x510E527FU,
            0x9B05688CU,
            0x1F83D9ABU,
            0x5BE0CD19U,
        };

        constexpr auto g_roundConstants = std::array<uint32, 64>{
            0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U,
            0x3956C25BU, 0x59F111F1U, 0x923F82A4U, 0xAB1C5ED5U,
            0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U,
            0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U, 0xC19BF174U,
            0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU,
            0x2DE92C6FU, 0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU,
            0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U,
            0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U,
            0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU, 0x53380D13U,
            0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U,
            0xA2BFE8A1U, 0xA81A664BU, 0xC24B8B70U, 0xC76C51A3U,
            0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U,
            0x19A4C116U, 0x1E376C08U, 0x2748774CU, 0x34B0BCB5U,
            0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
            0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U,
            0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U,
        };

        [[nodiscard]]
        constexpr auto lowerHexValue(char value) noexcept -> uint8
        {
            if (value >= '0' && value <= '9')
            {
                return static_cast<uint8>(value - '0');
            }
            if (value >= 'a' && value <= 'f')
            {
                return static_cast<uint8>(value - 'a' + 10);
            }
            return uint8{0xFF};
        }

        [[nodiscard]]
        constexpr auto choose(uint32 x, uint32 y, uint32 z) noexcept -> uint32
        {
            return (x & y) ^ (~x & z);
        }

        [[nodiscard]]
        constexpr auto majority(uint32 x, uint32 y, uint32 z) noexcept -> uint32
        {
            return (x & y) ^ (x & z) ^ (y & z);
        }

        [[nodiscard]]
        constexpr auto bigSigma0(uint32 value) noexcept -> uint32
        {
            return std::rotr(value, 2) ^ std::rotr(value, 13) ^ std::rotr(value, 22);
        }

        [[nodiscard]]
        constexpr auto bigSigma1(uint32 value) noexcept -> uint32
        {
            return std::rotr(value, 6) ^ std::rotr(value, 11) ^ std::rotr(value, 25);
        }

        [[nodiscard]]
        constexpr auto smallSigma0(uint32 value) noexcept -> uint32
        {
            return std::rotr(value, 7) ^ std::rotr(value, 18) ^ (value >> 3U);
        }

        [[nodiscard]]
        constexpr auto smallSigma1(uint32 value) noexcept -> uint32
        {
            return std::rotr(value, 17) ^ std::rotr(value, 19) ^ (value >> 10U);
        }

        [[nodiscard]]
        auto readBigEndianWord(
            std::span<std::byte const> block,
            std::size_t offset
        ) noexcept -> uint32
        {
            return (
                std::to_integer<uint32>(checkedAt(block, offset)) << 24U
                | std::to_integer<uint32>(checkedAt(block, offset + 1U)) << 16U
                | std::to_integer<uint32>(checkedAt(block, offset + 2U)) << 8U
                | std::to_integer<uint32>(checkedAt(block, offset + 3U))
            );
        }

        auto processBlock(
            std::array<uint32, 8>& state,
            std::span<std::byte const> block
        ) noexcept -> void
        {
            UF_CHECK(block.size() == g_sha256BlockBytes);

            auto schedule = std::array<uint32, 64>{};
            for (auto index = std::size_t{0}; index < 16U; ++index)
            {
                checkedAt(schedule, index) = readBigEndianWord(block, index * 4U);
            }
            for (auto index = std::size_t{16}; index < schedule.size(); ++index)
            {
                checkedAt(schedule, index) = (
                    smallSigma1(checkedAt(schedule, index - 2U))
                    + checkedAt(schedule, index - 7U)
                    + smallSigma0(checkedAt(schedule, index - 15U))
                    + checkedAt(schedule, index - 16U)
                );
            }

            auto a = checkedAt(state, 0);
            auto b = checkedAt(state, 1);
            auto c = checkedAt(state, 2);
            auto d = checkedAt(state, 3);
            auto e = checkedAt(state, 4);
            auto f = checkedAt(state, 5);
            auto g = checkedAt(state, 6);
            auto h = checkedAt(state, 7);

            for (auto index = std::size_t{0}; index < schedule.size(); ++index)
            {
                auto const first = (
                    h
                    + bigSigma1(e)
                    + choose(e, f, g)
                    + checkedAt(g_roundConstants, index)
                    + checkedAt(schedule, index)
                );
                auto const second = bigSigma0(a) + majority(a, b, c);
                h = g;
                g = f;
                f = e;
                e = d + first;
                d = c;
                c = b;
                b = a;
                a = first + second;
            }

            checkedAt(state, 0) += a;
            checkedAt(state, 1) += b;
            checkedAt(state, 2) += c;
            checkedAt(state, 3) += d;
            checkedAt(state, 4) += e;
            checkedAt(state, 5) += f;
            checkedAt(state, 6) += g;
            checkedAt(state, 7) += h;
        }
    }

    auto ContentHash::parse(std::string_view value) -> Result<ContentHash>
    {
        auto constexpr encodedSize = g_sha256Prefix.size() + g_sha256DigestBytes * 2U;
        if (
            value.size() != encodedSize
            || !value.starts_with(g_sha256Prefix)
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "content hash must be canonical sha256 followed by 64 lowercase hex digits"
            );
        }

        auto bytes = std::array<uint8, g_sha256DigestBytes>{};
        for (auto index = std::size_t{0}; index < bytes.size(); ++index)
        {
            auto const high = lowerHexValue(checkedAt(value, g_sha256Prefix.size() + index * 2U));
            auto const low = lowerHexValue(checkedAt(value, g_sha256Prefix.size() + index * 2U + 1U));
            if (high == uint8{0xFF} || low == uint8{0xFF})
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "content hash contains a non-canonical hexadecimal digit"
                );
            }
            checkedAt(bytes, index) = static_cast<uint8>((high << 4U) | low);
        }
        return ContentHash{bytes};
    }

    auto ContentHash::hex() const -> std::string
    {
        auto result = std::string{};
        result.reserve(g_sha256DigestBytes * 2U);
        for (auto const byte : m_bytes)
        {
            result.push_back(checkedAt(g_hexDigits, byte >> 4U));
            result.push_back(checkedAt(g_hexDigits, byte & uint8{0x0F}));
        }
        return result;
    }

    auto ContentHash::toString() const -> std::string
    {
        auto result = std::string{g_sha256Prefix};
        result += hex();
        return result;
    }

    auto ContentHash::bytes() const noexcept -> std::span<uint8 const>
    {
        return m_bytes;
    }

    auto sha256(std::span<std::byte const> bytes) -> Result<ContentHash>
    {
        auto const byteLength = checkedCast<uint64>(bytes.size());
        auto const bitLength = byteLength
            ? checkedMultiply(*byteLength, uint64{8})
            : std::optional<uint64>{};
        if (!bitLength)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "content is too large for SHA-256 length encoding"
            );
        }

        auto state = g_initialState;
        auto offset = std::size_t{0};
        while (bytes.size() - offset >= g_sha256BlockBytes)
        {
            processBlock(state, bytes.subspan(offset, g_sha256BlockBytes));
            offset += g_sha256BlockBytes;
        }

        auto tail = std::array<std::byte, g_sha256BlockBytes * 2U>{};
        auto const remainder = bytes.subspan(offset);
        std::ranges::copy(remainder, tail.begin());
        checkedAt(tail, remainder.size()) = std::byte{0x80};
        auto const paddedSize = remainder.size() < 56U
            ? g_sha256BlockBytes
            : g_sha256BlockBytes * 2U;
        for (auto index = std::size_t{0}; index < g_sha256LengthBytes; ++index)
        {
            checkedAt(tail, paddedSize - 1U - index) = static_cast<std::byte>(
                *bitLength >> static_cast<uint32>(index * 8U)
            );
        }
        processBlock(state, std::span<std::byte const>{tail}.first(g_sha256BlockBytes));
        if (paddedSize == g_sha256BlockBytes * 2U)
        {
            processBlock(state, std::span<std::byte const>{tail}.last(g_sha256BlockBytes));
        }

        auto digest = std::array<uint8, g_sha256DigestBytes>{};
        for (auto index = std::size_t{0}; index < state.size(); ++index)
        {
            auto const word = checkedAt(state, index);
            checkedAt(digest, index * 4U) = static_cast<uint8>(word >> 24U);
            checkedAt(digest, index * 4U + 1U) = static_cast<uint8>(word >> 16U);
            checkedAt(digest, index * 4U + 2U) = static_cast<uint8>(word >> 8U);
            checkedAt(digest, index * 4U + 3U) = static_cast<uint8>(word);
        }
        return ContentHash{digest};
    }
}
