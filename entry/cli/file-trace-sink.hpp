#pragma once

#include <core/error/result.hpp>

#include <engine/ports.hpp>
#include <engine/trace.hpp>

#include <filesystem>
#include <fstream>
#include <memory>

namespace uf::cli
{
    // A TraceSink that appends one serialized JSONL line per event and flushes
    // after each write. Trace evidence must survive a crash, so the sink never
    // buffers across emits: a failed write or flush is surfaced as an error
    // Status rather than lost silently.
    class FileTraceSink final : public engine::TraceSink
    {
        struct OpenTag final
        {
        };

        std::ofstream m_stream;

    public:
        FileTraceSink(OpenTag, std::ofstream stream) noexcept;

        [[nodiscard]]
        static auto create(
            std::filesystem::path const& path
        ) -> Result<std::unique_ptr<engine::TraceSink>>;

        [[nodiscard]] auto emit(engine::TraceEvent const& event) -> Status override;
    };
}
