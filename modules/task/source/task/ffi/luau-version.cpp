#include <task/runtime-version.hpp>

#include <format>
#include <string>

// Luau's headers are third-party and do not build clean under /W4 /WX; a
// manifest-driven module has no CMakeLists to mark them external, so the
// include is wrapped as modules/task's other ffi sources wrap theirs.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wold-style-cast"
// GCC has no -Weverything, so every warning these headers trip has to be named.
// Luau's containers take constructor parameters named after the members they
// initialise, which is what -Wshadow objects to.
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#include <Luau/BytecodeBuilder.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace uf::task
{
    auto luauRuntimeVersion() -> std::string
    {
        // The bytecode format number that governs whether a compiled task stays
        // reproducible; widened from uint8 only so it formats as a number.
        auto const version = static_cast<int>(Luau::BytecodeBuilder::getVersion());
        return std::format("luau-bytecode-{}", version);
    }
}
