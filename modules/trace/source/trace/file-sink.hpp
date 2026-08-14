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
        // NOT noexcept: std::ofstream's move constructor is not noexcept -- it
        // moves a basic_filebuf, which carries a locale and a buffer -- so
        // promising noexcept here promised something the standard library does
        // not, and a throw from that move would have terminated. createNew is
        // the only caller and already returns a Result.
        FileTraceSink(OpenTag, std::ofstream stream);

        [[nodiscard]]
        static auto createNew(
            std::filesystem::path const& path
        ) -> Result<std::unique_ptr<ITraceSink>>;

        [[nodiscard]] auto append(TraceEvent const& event) -> Status override;
    };
}
