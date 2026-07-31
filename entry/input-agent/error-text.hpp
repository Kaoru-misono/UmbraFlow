#pragma once

#include <core/error/result.hpp>

#include <string>

namespace uf::input_agent
{
    // One human-readable line for a failure that is about to leave the process,
    // whether as a results line, a log detail, or a message on stderr. It is
    // deliberately not a JSON concern: the kind, the context frames, the native
    // code and the origin are what a reader needs, and every caller embeds the
    // string in its own envelope.
    [[nodiscard]] auto formatAutomationError(Error const& error) -> std::string;
}
