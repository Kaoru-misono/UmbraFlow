#include "discovery.hpp"

#include "detail/discovery-logic.hpp"
#include "platform/windows-controller.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <utility>

namespace
{
    constexpr auto replacementCodePoint = std::uint32_t{0xFFFDU};

    auto appendUtf8(std::string& output, std::uint32_t codePoint) -> void
    {
        if (codePoint <= 0x7FU)
        {
            output.push_back(static_cast<char>(codePoint));
            return;
        }

        if (codePoint <= 0x7FFU)
        {
            output.push_back(static_cast<char>(0xC0U | (codePoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
            return;
        }

        if (codePoint <= 0xFFFFU)
        {
            output.push_back(static_cast<char>(0xE0U | (codePoint >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
            return;
        }

        output.push_back(static_cast<char>(0xF0U | (codePoint >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    }
}

namespace uf
{
    TargetCandidate::TargetCandidate(
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
    )
        : m_handle{handle}
        , m_process{process}
        , m_processStartTime{processStartTime}
        , m_executablePath{std::move(executablePath)}
        , m_windowClass{std::move(windowClass)}
        , m_title{std::move(title)}
        , m_clientSize{clientSize}
        , m_dpi{dpi}
        , m_isVisible{isVisible}
        , m_isIconic{isIconic}
    {
    }

    auto TargetCandidate::handle() const noexcept -> WindowHandle { return m_handle; }
    auto TargetCandidate::process() const noexcept -> ProcessId { return m_process; }
    auto TargetCandidate::processStartTime() const noexcept
        -> std::optional<ProcessStartTime>
    {
        return m_processStartTime;
    }
    auto TargetCandidate::executablePath() const noexcept
        -> std::optional<std::filesystem::path> const&
    {
        return m_executablePath;
    }
    auto TargetCandidate::windowClass() const noexcept -> std::string const&
    {
        return m_windowClass;
    }
    auto TargetCandidate::title() const noexcept -> std::string const& { return m_title; }
    auto TargetCandidate::clientSize() const noexcept -> ClientSize { return m_clientSize; }
    auto TargetCandidate::dpi() const noexcept -> Dpi { return m_dpi; }
    auto TargetCandidate::isVisible() const noexcept -> bool { return m_isVisible; }
    auto TargetCandidate::isIconic() const noexcept -> bool { return m_isIconic; }

    auto enumerateCandidates() -> Result<std::vector<TargetCandidate>>
    {
        return controller_platform::enumerateCandidates();
    }
}

namespace uf::controller_detail
{
    auto utf16BufferToString(
        std::span<char16_t const> buffer,
        std::int32_t length
    ) -> std::string
    {
        if (length <= 0)
        {
            return {};
        }

        auto const end = std::min(static_cast<std::size_t>(length), buffer.size());
        auto output = std::string{};
        output.reserve(end);

        for (auto index = std::size_t{0}; index < end; ++index)
        {
            auto const lead = static_cast<std::uint32_t>(buffer[index]);
            auto codePoint = lead;

            if (lead >= 0xD800U && lead <= 0xDBFFU)
            {
                if (index + 1U < end)
                {
                    auto const trail = static_cast<std::uint32_t>(buffer[index + 1U]);
                    if (trail >= 0xDC00U && trail <= 0xDFFFU)
                    {
                        codePoint = (
                            0x10000U
                            + ((lead - 0xD800U) << 10U)
                            + (trail - 0xDC00U)
                        );
                        ++index;
                    }
                    else
                    {
                        codePoint = replacementCodePoint;
                    }
                }
                else
                {
                    codePoint = replacementCodePoint;
                }
            }
            else if (lead >= 0xDC00U && lead <= 0xDFFFU)
            {
                codePoint = replacementCodePoint;
            }

            appendUtf8(output, codePoint);
        }

        return output;
    }

    auto utf16BufferToPath(
        std::span<char16_t const> buffer,
        std::int32_t length
    ) -> std::filesystem::path
    {
        auto const utf8 = utf16BufferToString(buffer, length);
        return std::filesystem::path{
            std::u8string{utf8.begin(), utf8.end()}
        };
    }
}
