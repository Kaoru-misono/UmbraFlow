#pragma once

#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>
#include <domain/frame.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace uf::m0_demo
{
    struct LogLine final
    {
        uint64 m_elapsedNanoseconds{};
        std::optional<uint32> m_loopIndex{};
        std::string m_phase;
        std::string m_event;
        std::optional<uint64> m_frameId{};
        std::optional<uint64> m_targetGeneration{};
        std::optional<uint64> m_confidence{};
        std::optional<bool> m_leaseOk{};
        std::string m_outcome{"info"};
        std::string m_detail{};

        LogLine(std::string phase, std::string event);

        [[nodiscard]] auto loopIndex(uint32 loopIndex) && -> LogLine;
        [[nodiscard]] auto frame(Frame const& frame) && -> LogLine;
        [[nodiscard]] auto confidence(uint64 confidence) && -> LogLine;
        [[nodiscard]] auto leaseOk(bool leaseOk) && -> LogLine;
        [[nodiscard]] auto outcome(std::string outcome) && -> LogLine;
        [[nodiscard]] auto detail(std::string detail) && -> LogLine;
    };

    [[nodiscard]] auto serializeLine(LogLine const& line) -> std::string;
    [[nodiscard]] auto formatAutomationError(Error const& error) -> std::string;

    class IJsonlSink
    {
    public:
        IJsonlSink() = default;
        IJsonlSink(IJsonlSink const&) = delete;
        auto operator=(IJsonlSink const&) -> IJsonlSink& = delete;
        virtual ~IJsonlSink() = default;

        [[nodiscard]]
        virtual auto writeLine(std::string_view line) -> std::error_code = 0;
        [[nodiscard]] virtual auto flush() -> std::error_code = 0;
    };

    class JsonlLog final
    {
        MonotonicInstant m_origin;
        std::unique_ptr<IJsonlSink> m_sink;

    public:
        explicit JsonlLog(std::unique_ptr<IJsonlSink> p_sink) noexcept;
        JsonlLog(JsonlLog const&) = delete;
        auto operator=(JsonlLog const&) -> JsonlLog& = delete;
        JsonlLog(JsonlLog&&) noexcept = default;
        auto operator=(JsonlLog&&) noexcept -> JsonlLog& = default;
        ~JsonlLog() = default;

        [[nodiscard]]
        static auto create(
            std::optional<std::filesystem::path> const& path
        ) -> Result<JsonlLog>;

        [[nodiscard]] auto write(LogLine line) -> Status;
        [[nodiscard]] auto flush() -> Status;
    };
}
