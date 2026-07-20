#include "log-jsonl.hpp"

#include <core/error/contracts.hpp>
#include <domain/error.hpp>
#include <domain/time.hpp>

#include <cerrno>
#include <cstdio>
#include <cstdint>
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

namespace
{
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

    [[nodiscard]]
    auto escapedJsonString(std::string_view value) -> std::string
    {
        auto output = std::string{"\""};
        output.reserve(value.size() + 2U);
        auto constexpr hex = std::string_view{"0123456789abcdef"};
        for (auto const character : value)
        {
            auto const byte = static_cast<unsigned char>(character);
            switch (byte)
            {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (byte < 0x20U)
                {
                    output += "\\u00";
                    output += hex[byte >> 4U];
                    output += hex[byte & 0x0FU];
                }
                else
                {
                    output += static_cast<char>(byte);
                }
                break;
            }
        }
        output += '"';
        return output;
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
    auto serializeLineUnchecked(uf::m0_demo::LogLine const& line) -> std::string
    {
        auto output = std::string{"{\"elapsed_ns\":"};
        output += std::to_string(line.m_elapsedNanoseconds);
        output += ",\"loop_idx\":";
        appendOptionalNumber(output, line.m_loopIndex);
        output += ",\"phase\":";
        output += escapedJsonString(line.m_phase);
        output += ",\"event\":";
        output += escapedJsonString(line.m_event);
        output += ",\"frame_id\":";
        appendOptionalNumber(output, line.m_frameId);
        output += ",\"target_generation\":";
        appendOptionalNumber(output, line.m_targetGeneration);
        output += ",\"confidence\":";
        appendOptionalNumber(output, line.m_confidence);
        output += ",\"lease_ok\":";
        appendOptionalBoolean(output, line.m_leaseOk);
        output += ",\"outcome\":";
        output += escapedJsonString(line.m_outcome);
        output += ",\"detail\":";
        output += escapedJsonString(line.m_detail);
        output += '}';
        return output;
    }

    class StdoutJsonlSink final : public uf::m0_demo::IJsonlSink
    {
        std::error_code m_initializationError{configureStdoutForBinaryJsonl()};

    public:
        [[nodiscard]]
        auto initializationError() const noexcept -> std::error_code
        {
            return m_initializationError;
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

    class FileJsonlSink final : public uf::m0_demo::IJsonlSink
    {
        std::ofstream m_stream;
        std::error_code m_openError{};

    public:
        explicit FileJsonlSink(std::filesystem::path const& path)
        {
            errno = 0;
            m_stream.open(path, std::ios::binary | std::ios::trunc);
            if (!m_stream.is_open())
            {
                m_openError = currentIoError();
            }
        }

        [[nodiscard]] auto openError() const noexcept -> std::error_code { return m_openError; }

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
    ) -> std::unexpected<uf::Error>
    {
        return uf::fail(
            uf::AutomationErrorKind::InvalidResource,
            "JSONL " + std::string{operation} + " failed: " + error.message()
        );
    }
}

namespace uf::m0_demo
{
    LogLine::LogLine(std::string phase, std::string event)
        : m_phase{std::move(phase)}
        , m_event{std::move(event)}
    {
    }

    auto LogLine::loopIndex(std::uint32_t loopIndex) && -> LogLine
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

    auto LogLine::confidence(std::uint64_t confidenceValue) && -> LogLine
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
                + escapedJsonString(error.what())
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
            auto sink = std::make_unique<StdoutJsonlSink>();
            if (auto const error = sink->initializationError(); error)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "cannot configure stdout log: " + error.message()
                );
            }
            return JsonlLog{std::move(sink)};
        }

        auto sink = std::make_unique<FileJsonlSink>(*path);
        if (auto const error = sink->openError(); error)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "cannot open log file " + path->string() + ": " + error.message()
            );
        }
        return JsonlLog{std::move(sink)};
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
