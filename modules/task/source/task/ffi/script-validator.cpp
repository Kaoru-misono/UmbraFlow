#include <task/script-validator.hpp>

#include <task/capability-surface.hpp>

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/types/enum-reflection.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <cstring>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

// Luau's Ast headers are third-party and do not build clean under the project's
// /W4 /WX profile; a manifest-driven module has no CMakeLists to mark them
// external, so wrap the includes exactly as modules/task's other ffi source and
// modules/script's ffi layer do. Only the parser and AST are needed here -- no
// VM -- so the compiler/VM headers stay out of this translation unit.
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
        // The canonical script-visible root object. Every reference to it must be
        // one of the three approved literal accesses or a verb method call; any
        // other contact is rejected (annotation-design 4).
        constexpr auto k_namespace = "umbra";

        // The two resource sub-namespaces that carry named handles, plus the
        // error-kind constant table. errors is validated the same way but is host
        // vocabulary rather than a project resource, so a reference to it is
        // approved without entering the resource report.
        constexpr auto k_recognizersTable = "recognizers";
        constexpr auto k_pagesTable       = "pages";
        constexpr auto k_errorsTable      = "errors";

        // Luau's base library binds `_G` to the global table itself. It is not a
        // resource path but a reflexive handle to the whole global environment, so
        // any chain rooted at it (_G.umbra, _G['umbra'], rawget(_G, 'umbra')) would
        // reach the umbra namespace WITHOUT rooting at the literal umbra global the
        // rest of this validator keys on. It is therefore rejected outright, the
        // pre-VM twin of installSandbox niling `_G` on the task thread.
        constexpr auto k_globalEnv = "_G";

        [[nodiscard]]
        auto isNamespace(Luau::AstName name) -> bool
        {
            return name == k_namespace;
        }

        [[nodiscard]]
        auto isGlobalEnv(Luau::AstName name) -> bool
        {
            return name == k_globalEnv;
        }

        // An AstName's interned C string as a view, empty when the name is absent.
        // The view borrows the parser's arena and is used only within one parse.
        [[nodiscard]]
        auto nameView(Luau::AstName name) -> std::string_view
        {
            return name.value != nullptr ? std::string_view{name.value}
                                         : std::string_view{};
        }

        // Formats a Luau source location as a 1-based "line L column C" for a
        // human-facing message. Luau positions are 0-based, so both add one.
        [[nodiscard]]
        auto formatLocation(Luau::Location const& location) -> std::string
        {
            return "line " + std::to_string(location.begin.line + 1) + " column "
                + std::to_string(location.begin.column + 1);
        }

        // Follows a pure dot/colon member-access chain from `expr` down to its
        // base and reports whether that base is the global umbra. Any non
        // AstExprIndexName link -- a parenthesised group, a call, a computed
        // index -- breaks the "direct literal" chain, so only an unbroken member
        // chain over the global umbra roots at the namespace. This is what makes
        // (umbra).recognizers.x and umbra.recognizers[x].y fall through to their
        // bare-global or computed-index rejection rather than being mistaken for
        // an approved access.
        [[nodiscard]]
        auto rootsAtNamespace(Luau::AstExpr* expr) -> bool
        {
            Luau::AstExpr* current = expr;
            while (auto* const index = current->as<Luau::AstExprIndexName>())
            {
                current = index->expr;
            }
            auto* const global = current->as<Luau::AstExprGlobal>();
            return global != nullptr && isNamespace(global->name);
        }

        // True when `node` is a umbra:<verb>(...) method call: a self-call whose
        // callee is a colon index directly on the global umbra. This is the verb
        // form, kept distinct from the two-level resource literal; the verb name
        // itself is not resolved here (an unknown verb fails at runtime, and the
        // bound verb set is not this validator's concern).
        [[nodiscard]]
        auto isNamespaceVerbCall(Luau::AstExprCall* node) -> bool
        {
            if (!node->self)
            {
                return false;
            }
            auto* const callee = node->func->as<Luau::AstExprIndexName>();
            if (callee == nullptr || callee->op != ':')
            {
                return false;
            }
            auto* const global = callee->expr->as<Luau::AstExprGlobal>();
            return global != nullptr && isNamespace(global->name);
        }

        // Walks one script's AST and enumerates every umbra resource reference,
        // rejecting the first contact with the umbra namespace that is not an
        // approved two-level literal access or a verb method call. An
        // umbra.errors.<kind> literal is checked the same way but contributes no
        // resource, since the kinds are host vocabulary. Latches the
        // first violation and turns every later visit into a no-op, so the
        // reported location is the earliest offending one and traversal order is
        // the message's tie-break. NOT reused across scripts: one visitor per
        // parse.
        class ResourceVisitor final : public Luau::AstVisitor
        {
            std::unordered_set<std::string_view> m_recognizerNames{};
            std::unordered_set<std::string_view> m_pageNames{};

            std::unordered_set<std::string> m_referencedRecognizers{};
            std::unordered_set<std::string> m_referencedPages{};

            std::optional<std::string> m_failure{};

        public:
            explicit ResourceVisitor(CapabilitySurface const& surface)
            {
                for (auto const& spec : surface.recognizers())
                {
                    m_recognizerNames.insert(spec.name);
                }
                for (auto const& spec : surface.pages())
                {
                    m_pageNames.insert(spec.name);
                }
            }

            [[nodiscard]]
            auto failure() const -> std::optional<std::string> const&
            {
                return m_failure;
            }

            // Moves the enumerated references into a deterministic report: both
            // lists deduplicated (via the sets) and sorted, so identical scripts
            // yield identical reports regardless of reference order.
            [[nodiscard]]
            auto takeReport() -> ScriptResourceReport
            {
                auto report = ScriptResourceReport{};
                report.recognizers.assign(
                    std::make_move_iterator(m_referencedRecognizers.begin()),
                    std::make_move_iterator(m_referencedRecognizers.end())
                );
                report.pages.assign(
                    std::make_move_iterator(m_referencedPages.begin()),
                    std::make_move_iterator(m_referencedPages.end())
                );
                std::sort(report.recognizers.begin(), report.recognizers.end());
                std::sort(report.pages.begin(), report.pages.end());
                return report;
            }

            auto visit(Luau::AstExprGlobal* node) -> bool override
            {
                if (m_failure.has_value())
                {
                    return false;
                }
                if (isNamespace(node->name))
                {
                    // A bare umbra global reached by recursion is never approved:
                    // every approved form consumes its umbra root before descent
                    // could reach it. So this is an alias, an argument, a return,
                    // or a traversal target -- all rejected.
                    recordFailure(
                        node->location,
                        "the umbra namespace may be used only as "
                        "umbra.recognizers.<name>, umbra.pages.<name>, "
                        "umbra.errors.<kind>, or umbra:<verb>(...); it cannot be "
                        "aliased, indexed dynamically, iterated, passed, or "
                        "returned"
                    );
                    return false;
                }
                if (isGlobalEnv(node->name))
                {
                    // `_G` is the reflexive global-table handle. Every chain built
                    // on it (_G.umbra:capture(), _G['umbra'], rawget(_G, 'umbra'))
                    // roots here, never at the umbra global the other checks key on,
                    // so it would otherwise sail through as unrelated code. Reject
                    // the handle itself: a task script has no legitimate use for the
                    // raw global environment.
                    recordFailure(
                        node->location,
                        "the raw global environment '_G' is not accessible from a "
                        "task script; it is an alias door to the umbra namespace "
                        "and every other global, so reach the capability surface "
                        "only through umbra.recognizers.<name>, "
                        "umbra.pages.<name>, umbra.errors.<kind>, or "
                        "umbra:<verb>(...)"
                    );
                    return false;
                }
                return true;
            }

            auto visit(Luau::AstExprIndexName* node) -> bool override
            {
                if (m_failure.has_value())
                {
                    return false;
                }
                if (!rootsAtNamespace(node))
                {
                    // Not umbra-rooted (e.g. frame:find, page.field on a local):
                    // keep walking so nested umbra references are still checked.
                    return true;
                }
                // This is the outermost umbra-rooted member chain. Classify it as
                // the one approved two-level literal or reject it, then stop
                // descending (return false) so its umbra root is never revisited
                // by the bare-global net above.
                classifyResourceAccess(node);
                return false;
            }

            auto visit(Luau::AstExprCall* node) -> bool override
            {
                if (m_failure.has_value())
                {
                    return false;
                }
                if (isNamespaceVerbCall(node))
                {
                    // umbra:<verb>(...) is approved. Skip the callee (its umbra
                    // root is consumed here) but still walk the arguments, where
                    // umbra.recognizers.<name> literals and disallowed umbra
                    // touches both live.
                    for (Luau::AstExpr* const arg : node->args)
                    {
                        arg->visit(this);
                    }
                    return false;
                }
                return true;
            }

        private:
            void recordFailure(Luau::Location const& location, std::string message)
            {
                if (m_failure.has_value())
                {
                    return;
                }
                m_failure = formatLocation(location) + ": " + std::move(message);
            }

            // Classifies an umbra-rooted member-access chain whose outermost node
            // is `node`. The only approved shape is exactly two dot levels:
            // umbra . (recognizers|pages|errors) . <name>. Everything else -- a
            // one-level field (umbra.recognizers as a value, umbra.foo), a deeper
            // chain (umbra.recognizers.x.y), or an unknown sub-namespace -- is
            // rejected.
            void classifyResourceAccess(Luau::AstExprIndexName* node)
            {
                if (node->op == '.')
                {
                    if (auto* const mid = node->expr->as<Luau::AstExprIndexName>())
                    {
                        auto* const root = mid->expr->as<Luau::AstExprGlobal>();
                        if (mid->op == '.' && root != nullptr
                            && isNamespace(root->name))
                        {
                            std::string_view const table = nameView(mid->index);
                            std::string_view const leaf  = nameView(node->index);
                            if (table == k_recognizersTable)
                            {
                                resolveRecognizer(leaf, node->location);
                                return;
                            }
                            if (table == k_pagesTable)
                            {
                                resolvePage(leaf, node->location);
                                return;
                            }
                            if (table == k_errorsTable)
                            {
                                resolveErrorKind(leaf, node->location);
                                return;
                            }
                            recordFailure(
                                mid->location,
                                "'umbra." + std::string{table}
                                    + "' is not a capability namespace; only "
                                      "umbra.recognizers, umbra.pages and "
                                      "umbra.errors exist"
                            );
                            return;
                        }
                    }
                }
                recordFailure(
                    node->location,
                    "'" + std::string{k_namespace}
                        + "' is only accessible as the two-level literals "
                          "umbra.recognizers.<name>, umbra.pages.<name> and "
                          "umbra.errors.<kind>; this access has the wrong shape"
                );
            }

            // Resolves a recognizer or page leaf `name` against the surface's
            // exposed set, recording the reference on success and a precise
            // missing-resource failure otherwise. Split so the pages branch reuses
            // it; see classifyResourceAccess for the dispatch.
            void resolveRecognizer(std::string_view name, Luau::Location const& location)
            {
                if (m_recognizerNames.contains(name))
                {
                    m_referencedRecognizers.insert(std::string{name});
                    return;
                }
                recordFailure(
                    location,
                    "no recognizer named '" + std::string{name}
                        + "' is exposed under umbra.recognizers"
                );
            }

            void resolvePage(std::string_view name, Luau::Location const& location)
            {
                if (m_pageNames.contains(name))
                {
                    m_referencedPages.insert(std::string{name});
                    return;
                }
                recordFailure(
                    location,
                    "no page named '" + std::string{name}
                        + "' is exposed under umbra.pages"
                );
            }

            // Resolves an error-kind leaf against AutomationErrorKind's single
            // wire spelling, which is exactly what umbra.errors is keyed by. A
            // misspelling would otherwise be a nil that makes every comparison
            // against it silently false, so it is closed here for the same reason
            // a missing recognizer is. Nothing is recorded on success: the kinds
            // are host vocabulary, fixed for the binary, and the report enumerates
            // the project resources a run depends on.
            void resolveErrorKind(std::string_view name, Luau::Location const& location)
            {
                for (auto const& entry : enumEntries<AutomationErrorKind>())
                {
                    if (automationErrorWireName(entry.value) == name)
                    {
                        return;
                    }
                }
                recordFailure(
                    location,
                    "no error kind named '" + std::string{name}
                        + "' is exposed under umbra.errors"
                );
            }
        };
    }

    auto validateScriptResources(
        std::string_view source,
        std::string_view chunkName,
        CapabilitySurface const& surface
    ) -> Result<ScriptResourceReport>
    {
        auto const chunk = std::string{chunkName};

        try
        {
            // The Ast allocator, name table, and default parse options; parse
            // collects recoverable syntax errors into result.errors and catches
            // its own fatal ParseError, returning a possibly-null root.
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
                    AutomationErrorKind::InvalidResource,
                    "task script '" + chunk + "' has a syntax error at "
                        + formatLocation(first.getLocation()) + ": "
                        + first.getMessage()
                );
            }
            if (result.root == nullptr)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "task script '" + chunk + "' produced no syntax tree"
                );
            }

            auto visitor = ResourceVisitor{surface};
            result.root->visit(&visitor);

            if (auto const& failure = visitor.failure())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "task script '" + chunk
                        + "' has a disallowed umbra reference: " + *failure
                );
            }
            return visitor.takeReport();
        }
        catch (std::exception const& error)
        {
            // The parser is third-party: a pathological input that escapes its
            // own error collection still fails closed as an invalid resource
            // rather than propagating an exception across this boundary.
            return fail(
                AutomationErrorKind::InvalidResource,
                "task script '" + chunk + "' could not be parsed: " + error.what()
            );
        }
    }
}
