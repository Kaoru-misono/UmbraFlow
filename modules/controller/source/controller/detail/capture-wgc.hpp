#pragma once

#include "controller/capture.hpp"

#include <core/error/result.hpp>
#include <domain/ids.hpp>

#include <cstdint>
#include <utility>

namespace uf::controller_detail
{
    struct CaptureSize final
    {
        std::int32_t m_width;
        std::int32_t m_height;

        auto operator==(CaptureSize const&) const -> bool = default;
    };

    class FrameIdCounter final
    {
        std::uint64_t m_next;

    public:
        constexpr explicit FrameIdCounter(std::uint64_t next = 0) noexcept
            : m_next{next}
        {
        }

        [[nodiscard]] auto nextId() -> Result<FrameId>;
    };

    // A size change invalidates the geometry permanently so stale client transforms and
    // crop offsets cannot be reused before the owning capture session is rebuilt.
    class CaptureGeometryState final
    {
        std::uint32_t m_expectedWidth;
        std::uint32_t m_expectedHeight;
        bool m_invalidated;

        constexpr CaptureGeometryState(
            std::uint32_t expectedWidth,
            std::uint32_t expectedHeight
        ) noexcept
            : m_expectedWidth{expectedWidth}
            , m_expectedHeight{expectedHeight}
            , m_invalidated{false}
        {
        }

    public:
        [[nodiscard]]
        static auto create(CaptureSize initialSize) -> Result<CaptureGeometryState>;

        [[nodiscard]] auto ensureActive() const -> Status;

        [[nodiscard]]
        constexpr auto expectedSize() const noexcept
            -> std::pair<std::uint32_t, std::uint32_t>
        {
            return {m_expectedWidth, m_expectedHeight};
        }

        [[nodiscard]]
        auto observeContentSize(
            CaptureSize contentSize
        ) -> Result<std::pair<std::uint32_t, std::uint32_t>>;

        [[nodiscard]]
        auto observeSurfaceSize(
            std::pair<std::uint32_t, std::uint32_t> confirmedContentSize,
            std::uint32_t surfaceWidth,
            std::uint32_t surfaceHeight
        ) -> Status;
    };

    [[nodiscard]]
    auto clientIntegerExtent(
        ClientGeometry const& client
    ) -> Result<std::pair<std::uint32_t, std::uint32_t>>;
}
