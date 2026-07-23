#pragma once

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>
#include <core/types/strong-id.hpp>
#include <core/types/strong-value.hpp>

#include <compare>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace uf
{
    namespace controller_detail
    {
        struct WindowHandleTag;
        struct ProcessIdTag;
        struct ProcessStartTimeTag;
        struct DpiTag;
    }

    using WindowHandle = StrongValue<controller_detail::WindowHandleTag, intptr>;
    using ProcessId = StrongId<controller_detail::ProcessIdTag, uint32>;
    using ProcessStartTime = StrongValue<
        controller_detail::ProcessStartTimeTag,
        uint64
    >;
    using Dpi = StrongValue<controller_detail::DpiTag, uint32>;

    class ClientSize final
    {
        uint32 m_width;
        uint32 m_height;

    public:
        constexpr ClientSize(uint32 width, uint32 height) noexcept
            : m_width{width}
            , m_height{height}
        {
        }

        auto operator<=>(ClientSize const&) const = default;

        [[nodiscard]] constexpr auto width() const noexcept -> uint32 { return m_width; }
        [[nodiscard]] constexpr auto height() const noexcept -> uint32 { return m_height; }
    };

    class TargetCandidate final
    {
        WindowHandle                         m_handle;
        ProcessId                            m_process;
        std::optional<ProcessStartTime>      m_processStartTime;
        std::optional<std::filesystem::path> m_executablePath;
        std::string                          m_windowClass;
        std::string                          m_title;
        ClientSize                           m_clientSize;
        Dpi                                  m_dpi;
        bool                                 m_isVisible;
        bool                                 m_isIconic;

    public:
        TargetCandidate(
            WindowHandle handle,
            ProcessId process,
            std::optional<ProcessStartTime> processStartTime,
            std::optional<std::filesystem::path> executablePath,
            std::string windowClass,
            std::string title,
            ClientSize clientSize,
            Dpi dpi,
            bool isVisible,
            bool isIconic
        );

        auto operator==(TargetCandidate const&) const -> bool = default;

        [[nodiscard]] auto handle() const noexcept -> WindowHandle;
        [[nodiscard]] auto process() const noexcept -> ProcessId;
        [[nodiscard]]
        auto processStartTime() const noexcept -> std::optional<ProcessStartTime>;
        [[nodiscard]]
        auto executablePath() const noexcept UF_LIFETIME_BOUND
            -> std::optional<std::filesystem::path> const&;
        [[nodiscard]]
        auto windowClass() const noexcept UF_LIFETIME_BOUND -> std::string const&;
        [[nodiscard]] auto title() const noexcept UF_LIFETIME_BOUND -> std::string const&;
        [[nodiscard]] auto clientSize() const noexcept -> ClientSize;
        [[nodiscard]] auto dpi() const noexcept -> Dpi;
        [[nodiscard]] auto isVisible() const noexcept -> bool;
        [[nodiscard]] auto isIconic() const noexcept -> bool;
    };

    [[nodiscard]] auto enumerateCandidates() -> Result<std::vector<TargetCandidate>>;
}
