#pragma once

#include "event.hpp"
#include "sink.hpp"

#include <core/error/result.hpp>

#include <filesystem>
#include <fstream>
#include <memory>

namespace uf::trace
{
    // An ITraceSink that appends one serialized JSONL line per event and flushes
    // after each write: evidence must survive a crash, so nothing buffers across
    // emits and a failed write or flush surfaces as an error Status. One run writes
    // one file through one sink.
    class FileTraceSink final : public ITraceSink
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
        ) -> Result<std::unique_ptr<ITraceSink>>;

        [[nodiscard]] auto emit(StampedTraceEvent const& event) -> Status override;
    };
}
