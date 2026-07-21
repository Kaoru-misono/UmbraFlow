#include "discovery.hpp"

#include "detail/discovery-logic.hpp"
#include "platform/windows-controller.hpp"

#include <core/error/contracts.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/text/unsafe/unicode-code-unit.hpp>
#include <core/text/utf8.hpp>
#include <core/types/integer.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <utility>

namespace
{
    constexpr auto g_replacementCodePoint = uf::uint32{0xFFFDU};
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
        int32 length
    ) -> std::string
    {
        if (length <= 0)
        {
            return {};
        }

        auto const convertedLength = checkedCast<std::size_t>(length);
        UF_CHECK(convertedLength.has_value());
        auto const end = std::min(*convertedLength, buffer.size());
        auto output = std::string{};
        output.reserve(end);

        for (auto index = std::size_t{0}; index < end; ++index)
        {
            auto const lead = uint32{
                text_unsafe::utf16CodeUnitValue(buffer[index])
            };
            auto codePoint = lead;

            if (lead >= 0xD800U && lead <= 0xDBFFU)
            {
                if (index + 1U < end)
                {
                    auto const trail = uint32{
                        text_unsafe::utf16CodeUnitValue(buffer[index + 1U])
                    };
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
                        codePoint = g_replacementCodePoint;
                    }
                }
                else
                {
                    codePoint = g_replacementCodePoint;
                }
            }
            else if (lead >= 0xDC00U && lead <= 0xDFFFU)
            {
                codePoint = g_replacementCodePoint;
            }

            appendUtf8Scalar(output, codePoint);
        }

        return output;
    }

    auto utf16BufferToPath(
        std::span<char16_t const> buffer,
        int32 length
    ) -> std::filesystem::path
    {
        auto const utf8 = utf16BufferToString(buffer, length);
        return std::filesystem::path{
            std::u8string{utf8.begin(), utf8.end()}
        };
    }
}
