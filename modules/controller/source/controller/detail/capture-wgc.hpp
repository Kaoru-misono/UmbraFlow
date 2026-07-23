#pragma once

#include "controller/capture.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>
#include <domain/ids.hpp>

#include <utility>

namespace uf::controller_detail
{
    struct CaptureSize final
    {
        int32 m_width;
        int32 m_height;

        auto operator==(CaptureSize const&) const -> bool = default;
    };

    class FrameIdCounter final
    {
        uint64 m_next;

    public:
        constexpr explicit FrameIdCounter(uint64 next = 0) noexcept
            : m_next{next}
        {
        }

        [[nodiscard]] auto nextId() -> Result<FrameId>;
    };

    // A size change invalidates the geometry permanently so stale client transforms and
    // crop offsets cannot be reused before the owning capture session is rebuilt.
    class CaptureGeometryState final
    {
        uint32 m_expectedWidth;
        uint32 m_expectedHeight;
        bool   m_invalidated;

        constexpr CaptureGeometryState(
            uint32 expectedWidth,
            uint32 expectedHeight
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
            -> std::pair<uint32, uint32>
        {
            return {m_expectedWidth, m_expectedHeight};
        }

        [[nodiscard]]
        auto observeContentSize(
            CaptureSize contentSize
        ) -> Result<std::pair<uint32, uint32>>;

        [[nodiscard]]
        auto observeSurfaceSize(
            std::pair<uint32, uint32> confirmedContentSize,
            uint32 surfaceWidth,
            uint32 surfaceHeight
        ) -> Status;
    };

    [[nodiscard]]
    auto clientIntegerExtent(
        ClientGeometry const& client
    ) -> Result<std::pair<uint32, uint32>>;
}
