#include <task/task-loader.hpp>

#include <format>
#include <string>

// Luau's Bytecode header is third-party and does not build clean under the
// project's /W4 /WX profile; a manifest-driven module has no CMakeLists to mark
// it external, so wrap the include exactly as modules/task's other ffi sources
// do. Only the bytecode version accessor is needed here -- no VM, no parser.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wold-style-cast"
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
        // The bytecode target version the compiler would emit for a task: the
        // stable format number that governs whether a compiled task stays
        // reproducible. static_cast widens the uint8 version for formatting; no
        // pointer, aliasing, or lifetime concern arises, so no SAFETY note is due.
        auto const version = static_cast<int>(Luau::BytecodeBuilder::getVersion());
        return std::format("luau-bytecode-{}", version);
    }
}
