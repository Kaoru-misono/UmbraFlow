#pragma once

#include <task/trace.hpp>

#include <core/error/result.hpp>

#include <filesystem>
#include <fstream>
#include <memory>

namespace uf::task
{
    // A TaskTraceSink that appends one serialized JSONL line per event and flushes
    // after each write. Trace evidence must survive a crash, so the sink never
    // buffers across emits: a failed write or flush is surfaced as an error Status
    // rather than lost silently. It mirrors cli::FileTraceSink, the engine trace's
    // file sink, so the two evidence streams are written with one discipline.
    class FileTaskTraceSink final : public TaskTraceSink
    {
        struct OpenTag final
        {
        };

        std::ofstream m_stream;

    public:
        FileTaskTraceSink(OpenTag, std::ofstream stream) noexcept;

        [[nodiscard]]
        static auto create(
            std::filesystem::path const& path
        ) -> Result<std::unique_ptr<TaskTraceSink>>;

        [[nodiscard]] auto emit(TaskTraceEvent const& event) -> Status override;
    };
}
