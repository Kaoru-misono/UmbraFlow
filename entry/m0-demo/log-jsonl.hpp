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
        uint64                elapsedNanoseconds{};
        std::optional<uint32> loopIndex{};
        std::string           phase;
        std::string           event;
        std::optional<uint64> frameId{};
        std::optional<uint64> targetGeneration{};
        std::optional<uint64> confidence{};
        std::optional<bool>   leaseOk{};
        std::string           outcome{"info"};
        std::string           detail{};

        LogLine(std::string phase, std::string event);

        [[nodiscard]] auto withLoopIndex(uint32 loopIndexValue) && -> LogLine;
        [[nodiscard]] auto withFrame(Frame const& frame) && -> LogLine;
        [[nodiscard]] auto withConfidence(uint64 confidence) && -> LogLine;
        [[nodiscard]] auto withLeaseOk(bool leaseOk) && -> LogLine;
        [[nodiscard]] auto withOutcome(std::string outcome) && -> LogLine;
        [[nodiscard]] auto withDetail(std::string detail) && -> LogLine;
    };

    [[nodiscard]] auto serializeLine(LogLine const& line) -> std::string;

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
        MonotonicInstant            m_origin;
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
