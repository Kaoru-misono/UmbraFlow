#pragma once

#include "event.hpp"
#include "sink.hpp"

#include <core/error/result.hpp>

#include <filesystem>
#include <fstream>
#include <memory>

namespace uf::trace
{
    // Owns one new JSONL stream and appends one flushed line per event. Existing
    // non-empty files are rejected; this sink never truncates prior evidence and
    // does not scan old JSON to resume a stream.
    class FileTraceSink final : public ITraceSink
    {
        struct OpenTag final
        {
        };

        std::ofstream m_stream;

    public:
        FileTraceSink(OpenTag, std::ofstream stream) noexcept;

        [[nodiscard]]
        static auto createNew(
            std::filesystem::path const& path
        ) -> Result<std::unique_ptr<ITraceSink>>;

        [[nodiscard]] auto append(TraceEvent const& event) -> Status override;
    };
}
