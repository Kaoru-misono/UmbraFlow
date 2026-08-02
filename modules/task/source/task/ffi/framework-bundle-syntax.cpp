#include <task/framework-bundle.hpp>

#include <core/error/result.hpp>

#include <domain/error.hpp>

#include <exception>
#include <string>
#include <string_view>

// Luau's Ast headers are third-party and do not build clean under /W4 /WX; a
// manifest-driven module has no CMakeLists to mark them external, so the
// includes are wrapped as modules/task's other ffi sources wrap theirs.
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
#include <Luau/Allocator.h>
#include <Luau/Ast.h>
#include <Luau/Lexer.h>
#include <Luau/Location.h>
#include <Luau/ParseOptions.h>
#include <Luau/ParseResult.h>
#include <Luau/Parser.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace uf::task
{
    namespace
    {
        [[nodiscard]]
        auto formatLocation(Luau::Location const& location) -> std::string
        {
            return "line " + std::to_string(location.begin.line + 1) + " column "
                + std::to_string(location.begin.column + 1);
        }
    }

    auto checkFrameworkModuleSyntax(
        std::string_view source,
        std::string_view chunkName
    ) -> Status
    {
        auto const chunk = std::string{chunkName};

        try
        {
            // parse collects recoverable syntax errors into result.errors and
            // catches its own fatal ParseError, returning a possibly-null root.
            // The pointer-and-length argument is the third-party signature; the
            // caller's view owns the storage for the whole call.
            auto allocator = Luau::Allocator{};
            auto names     = Luau::AstNameTable{allocator};
            auto options   = Luau::ParseOptions{};

            auto const result = Luau::Parser::parse(
                source.data(),
                source.size(),
                names,
                allocator,
                options
            );

            if (!result.errors.empty())
            {
                auto const& first = result.errors.front();
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "framework module '" + chunk + "' has a syntax error at "
                        + formatLocation(first.getLocation()) + ": "
                        + first.getMessage()
                );
            }
            if (result.root == nullptr)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "framework module '" + chunk + "' produced no syntax tree"
                );
            }

            return ok();
        }
        catch (std::exception const& error)
        {
            // The parser is third-party: an input that escapes its own error
            // collection fails closed rather than crossing this boundary.
            return fail(
                AutomationErrorKind::InternalInvariant,
                "framework module '" + chunk + "' could not be parsed: " + error.what()
            );
        }
    }
}
