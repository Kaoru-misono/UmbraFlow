#include "log-jsonl.hpp"

#include "json-string.hpp"

#include <core/error/contracts.hpp>
#include <core/types/integer.hpp>
#include <domain/error.hpp>
#include <domain/time.hpp>

#include <cerrno>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace uf::m0_demo
{
    namespace
    {
        using JsonlSinkResult = Result<std::unique_ptr<IJsonlSink>>;

        [[nodiscard]]
        auto currentIoError() -> std::error_code
        {
            if (errno != 0)
            {
                return std::error_code{errno, std::generic_category()};
            }
            return std::make_error_code(std::io_errc::stream);
        }

        [[nodiscard]]
        auto configureStdoutForBinaryJsonl() -> std::error_code
        {
#if defined(_WIN32)
            errno = 0;
            if (_setmode(_fileno(stdout), _O_BINARY) == -1)
            {
                return currentIoError();
            }
#endif
            return {};
        }

        template <typename Value>
        auto appendOptionalNumber(
            std::string& output,
            std::optional<Value> value
        ) -> void
        {
            output += value ? std::to_string(*value) : "null";
        }

        auto appendOptionalBoolean(
            std::string& output,
            std::optional<bool> value
        ) -> void
        {
            if (!value)
            {
                output += "null";
                return;
            }
            output += *value ? "true" : "false";
        }

        [[nodiscard]]
        auto serializeLineUnchecked(LogLine const& line) -> std::string
        {
            auto output = std::string{"{\"elapsed_ns\":"};
            output += std::to_string(line.m_elapsedNanoseconds);
            output += ",\"loop_idx\":";
            appendOptionalNumber(output, line.m_loopIndex);
            output += ",\"phase\":";
            output += escapeJsonString(line.m_phase);
            output += ",\"event\":";
            output += escapeJsonString(line.m_event);
            output += ",\"frame_id\":";
            appendOptionalNumber(output, line.m_frameId);
            output += ",\"target_generation\":";
            appendOptionalNumber(output, line.m_targetGeneration);
            output += ",\"confidence\":";
            appendOptionalNumber(output, line.m_confidence);
            output += ",\"lease_ok\":";
            appendOptionalBoolean(output, line.m_leaseOk);
            output += ",\"outcome\":";
            output += escapeJsonString(line.m_outcome);
            output += ",\"detail\":";
            output += escapeJsonString(line.m_detail);
            output += '}';
            return output;
        }

        class StdoutJsonlSink final : public IJsonlSink
        {
            struct ConfiguredTag final
            {
            };

        public:
            explicit StdoutJsonlSink(ConfiguredTag) noexcept
            {
            }

            [[nodiscard]]
            static auto create() -> JsonlSinkResult
            {
                if (auto const error = configureStdoutForBinaryJsonl(); error)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "cannot configure stdout log: " + error.message()
                    );
                }

                auto p_sink = std::make_unique<StdoutJsonlSink>(ConfiguredTag{});
                return std::unique_ptr<IJsonlSink>{std::move(p_sink)};
            }

            auto writeLine(std::string_view line) -> std::error_code override
            {
                errno = 0;
                std::cout << line << '\n';
                return std::cout ? std::error_code{} : currentIoError();
            }

            auto flush() -> std::error_code override
            {
                errno = 0;
                std::cout.flush();
                return std::cout ? std::error_code{} : currentIoError();
            }
        };

        class FileJsonlSink final : public IJsonlSink
        {
            struct OpenTag final
            {
            };

            std::ofstream m_stream;

        public:
            explicit FileJsonlSink(OpenTag, std::ofstream stream)
                : m_stream{std::move(stream)}
            {
                UF_CHECK(m_stream.is_open());
            }

            [[nodiscard]]
            static auto create(
                std::filesystem::path const& path
            ) -> JsonlSinkResult
            {
                auto stream = std::ofstream{};
                errno = 0;
                stream.open(path, std::ios::binary | std::ios::trunc);
                if (!stream.is_open())
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "cannot open log file "
                            + path.string()
                            + ": "
                            + currentIoError().message()
                    );
                }

                auto p_sink = std::make_unique<FileJsonlSink>(
                    OpenTag{},
                    std::move(stream)
                );
                return std::unique_ptr<IJsonlSink>{std::move(p_sink)};
            }

            auto writeLine(std::string_view line) -> std::error_code override
            {
                errno = 0;
                m_stream << line << '\n';
                return m_stream ? std::error_code{} : currentIoError();
            }

            auto flush() -> std::error_code override
            {
                errno = 0;
                m_stream.flush();
                return m_stream ? std::error_code{} : currentIoError();
            }
        };

        [[nodiscard]]
        auto logError(
            std::string_view operation,
            std::error_code error
        ) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "JSONL " + std::string{operation} + " failed: " + error.message()
            );
        }
    }

    LogLine::LogLine(std::string phase, std::string event)
        : m_phase{std::move(phase)}
        , m_event{std::move(event)}
    {
    }

    auto LogLine::loopIndex(uint32 loopIndex) && -> LogLine
    {
        m_loopIndex = loopIndex;
        return std::move(*this);
    }

    auto LogLine::frame(Frame const& frameValue) && -> LogLine
    {
        m_frameId = frameValue.id().value();
        m_targetGeneration = frameValue.targetGeneration().value();
        return std::move(*this);
    }

    auto LogLine::confidence(uint64 confidenceValue) && -> LogLine
    {
        m_confidence = confidenceValue;
        return std::move(*this);
    }

    auto LogLine::leaseOk(bool leaseOkValue) && -> LogLine
    {
        m_leaseOk = leaseOkValue;
        return std::move(*this);
    }

    auto LogLine::outcome(std::string outcomeValue) && -> LogLine
    {
        m_outcome = std::move(outcomeValue);
        return std::move(*this);
    }

    auto LogLine::detail(std::string detailValue) && -> LogLine
    {
        m_detail = std::move(detailValue);
        return std::move(*this);
    }

    auto serializeLine(LogLine const& line) -> std::string
    {
        try
        {
            return serializeLineUnchecked(line);
        }
        catch (std::exception const& error)
        {
            return
                "{\"event\":\"log_serialize_error\",\"detail\":"
                + escapeJsonString(error.what())
                + '}';
        }
    }

    auto formatAutomationError(Error const& error) -> std::string
    {
        auto name = std::optional<std::string_view>{};
        if (auto const kind = automationErrorKind(error); kind)
        {
            name = enumName(*kind);
        }
        return std::string{name.value_or("UnknownAutomationErrorKind")}
            + ": "
            + std::string{error.message()};
    }

    JsonlLog::JsonlLog(std::unique_ptr<IJsonlSink> p_sink) noexcept
        : m_origin{MonotonicInstant::now()}
        , m_sink{std::move(p_sink)}
    {
        UF_CHECK(m_sink != nullptr);
    }

    auto JsonlLog::create(
        std::optional<std::filesystem::path> const& path
    ) -> Result<JsonlLog>
    {
        if (!path)
        {
            UF_TRY_VALUE(p_sink, StdoutJsonlSink::create());
            return JsonlLog{std::move(p_sink)};
        }

        UF_TRY_VALUE(p_sink, FileJsonlSink::create(*path));
        return JsonlLog{std::move(p_sink)};
    }

    auto JsonlLog::write(LogLine line) -> Status
    {
        line.m_elapsedNanoseconds = elapsedNanosecondsSince(
            MonotonicInstant::now(),
            m_origin
        );
        auto const error = m_sink->writeLine(serializeLine(line));
        if (error)
        {
            return logError("write", error);
        }
        return ok();
    }

    auto JsonlLog::flush() -> Status
    {
        auto const error = m_sink->flush();
        if (error)
        {
            return logError("flush", error);
        }
        return ok();
    }
}
