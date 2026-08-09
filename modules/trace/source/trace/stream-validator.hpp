#pragma once

#include "event.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <optional>
#include <string>

namespace uf::trace
{
    // Incrementally validates an in-memory event stream without parsing or
    // replaying JSON. Rejected events do not advance validator state.
    class TraceStreamValidator final
    {
        std::optional<std::string> m_sessionId{};
        std::optional<ContentHash> m_sessionManifestHash{};
        std::optional<std::string> m_producer{};
        uint64                     m_lastSequence{};

    public:
        TraceStreamValidator() = default;

        [[nodiscard]] auto admit(TraceEvent const& event) -> Status;
    };
}
