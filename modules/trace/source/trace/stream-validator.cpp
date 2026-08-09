#include "stream-validator.hpp"

#include "event.hpp"

#include <core/error/result.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/types/integer.hpp>

#include <string>
#include <system_error>
#include <utility>

namespace uf::trace
{
    namespace
    {
        [[nodiscard]] auto invalidStream(std::string message) -> Status
        {
            return fail(
                std::make_error_code(std::errc::invalid_argument),
                std::move(message)
            );
        }
    }

    auto TraceStreamValidator::admit(TraceEvent const& event) -> Status
    {
        if (!m_sessionId.has_value())
        {
            if (event.sequence() != 1U)
            {
                return invalidStream("a trace stream must begin at sequence 1");
            }

            m_sessionId           = event.sessionId();
            m_sessionManifestHash = event.sessionManifestHash();
            m_producer            = event.producer();
            m_lastSequence        = event.sequence();
            return ok();
        }

        if (event.sessionId() != *m_sessionId)
        {
            return invalidStream("trace session_id changed within one stream");
        }
        if (event.sessionManifestHash() != *m_sessionManifestHash)
        {
            return invalidStream(
                "trace session_manifest_hash changed within one stream"
            );
        }
        if (event.producer() != *m_producer)
        {
            return invalidStream("trace producer changed within one stream");
        }

        auto const expected = checkedAdd(m_lastSequence, uint64{1});
        if (!expected.has_value() || event.sequence() != *expected)
        {
            return invalidStream(
                "trace sequence is duplicated, missing, or out of order"
            );
        }
        m_lastSequence = event.sequence();
        return ok();
    }
}
