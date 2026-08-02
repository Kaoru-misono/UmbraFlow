#include <task/script-validator.hpp>

#include <task/page-model-file.hpp>

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

// Luau's Ast headers are third-party and do not build clean under /W4 /WX; a
// manifest-driven module has no CMakeLists to mark them external, so the
// includes are wrapped as modules/script's ffi layer wraps its own.
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
        // The canonical script-visible root. It carries data alone, so every
        // reference must be one of the approved two-level literal accesses and
        // any other contact is rejected (annotation-design 4). There is no verb
        // form: the capability surface lives in the trusted framework's closure,
        // so `uf:anything(...)` names nothing and is rejected here rather than
        // left to fail as a runtime nil call.
        constexpr auto k_namespace = "uf";

        // The resource sub-namespaces that carry named handles, plus the
        // error-kind constants. errors is validated the same way but is host
        // vocabulary, so a reference to it never enters the resource report.
        constexpr auto k_elementsTable = "elements";
        constexpr auto k_pagesTable       = "pages";
        constexpr auto k_errorsTable      = "errors";

        // `_G` is the reflexive handle to the whole global environment, so any
        // chain rooted at it (_G.uf, rawget(_G, 'uf')) reaches the uf namespace
        // without rooting at the literal uf global the rest of this validator
        // keys on. Rejected outright, the pre-VM twin of installSandbox niling
        // `_G` on the task thread.
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

        // Whether a pure dot/colon member-access chain from `expr` bottoms out at
        // the global uf. Any other link -- a parenthesised group, a call, a
        // computed index -- breaks the chain, which is what makes (uf).elements.x
        // and uf.elements[x].y fall through to their own rejections rather than
        // being mistaken for an approved access.
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

        // Walks one script's AST and enumerates every uf resource reference,
        // rejecting the first contact that is not an approved two-level literal
        // access. The first violation latches and every later visit is a no-op,
        // so the reported location is the earliest offending one. One visitor per
        // parse; never reused across scripts.
        class ResourceVisitor final : public Luau::AstVisitor
        {
            std::unordered_set<std::string_view> m_elementNames{};
            std::unordered_set<std::string_view> m_pageNames{};

            std::unordered_set<std::string> m_referencedElements{};
            std::unordered_set<std::string> m_referencedPages{};

            std::optional<std::string> m_failure{};

        public:
            // The views borrow `model`'s strings; the visitor is built, used and
            // dropped inside validateScriptResources, which keeps them alive.
            explicit ResourceVisitor(PageModelFacts const& model)
            {
                for (auto const& name : model.elementNames)
                {
                    m_elementNames.insert(name);
                }
                for (auto const& name : model.pageNames)
                {
                    m_pageNames.insert(name);
                }
            }

            [[nodiscard]]
            auto failure() const -> std::optional<std::string> const&
            {
                return m_failure;
            }

            // Deduplicated and sorted, so identical scripts yield identical
            // reports regardless of reference order.
            [[nodiscard]]
            auto takeReport() -> ScriptResourceReport
            {
                auto report = ScriptResourceReport{};
                report.elements.assign(
                    std::make_move_iterator(m_referencedElements.begin()),
                    std::make_move_iterator(m_referencedElements.end())
                );
                report.pages.assign(
                    std::make_move_iterator(m_referencedPages.begin()),
                    std::make_move_iterator(m_referencedPages.end())
                );
                std::sort(report.elements.begin(), report.elements.end());
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
                    // Every approved form consumes its uf root before descent
                    // could reach it, so a bare uf global here is an alias, an
                    // argument, a return or a traversal target.
                    recordFailure(
                        node->location,
                        "the uf namespace may be used only as "
                        "uf.elements.<name>, uf.pages.<name> or "
                        "uf.errors.<kind>; it cannot be aliased, indexed "
                        "dynamically, iterated, passed, or returned"
                    );
                    return false;
                }
                if (isGlobalEnv(node->name))
                {
                    // Every chain built on `_G` roots here rather than at the uf
                    // global the other checks key on; see k_globalEnv.
                    recordFailure(
                        node->location,
                        "the raw global environment '_G' is not accessible from a "
                        "task script; it is an alias door to the uf namespace and "
                        "every other global, so reach the named resources only "
                        "through uf.elements.<name>, uf.pages.<name> and "
                        "uf.errors.<kind>"
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
                    // Not uf-rooted (e.g. frame:find, page.field on a local):
                    // keep walking so nested uf references are still checked.
                    return true;
                }
                // The outermost uf-rooted chain: classify or reject it, then
                // stop descending so its uf root never reaches the bare-global
                // net above.
                classifyResourceAccess(node);
                return false;
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

            // Classifies a uf-rooted member-access chain. The only approved
            // shape is exactly two dot levels, uf.(elements|pages|errors).<name>;
            // a one-level field, a deeper chain, a colon index and an unknown
            // sub-namespace are all rejected.
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
                            if (table == k_elementsTable)
                            {
                                resolveElement(leaf, node->location);
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
                                "'uf." + std::string{table}
                                    + "' is not a capability namespace; only "
                                      "uf.elements, uf.pages and uf.errors "
                                      "exist"
                            );
                            return;
                        }
                    }
                }
                recordFailure(
                    node->location,
                    "'" + std::string{k_namespace}
                        + "' is only accessible as the two-level literals "
                          "uf.elements.<name>, uf.pages.<name> and "
                          "uf.errors.<kind>; this access has the wrong shape"
                );
            }

            // Resolves an element leaf against the set the project file
            // declares, recording the reference or a missing-resource failure.
            void resolveElement(std::string_view name, Luau::Location const& location)
            {
                if (m_elementNames.contains(name))
                {
                    m_referencedElements.insert(std::string{name});
                    return;
                }
                recordFailure(
                    location,
                    "this project's page model declares no element named '"
                        + std::string{name} + "'"
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
                    "this project's page model declares no page named '"
                        + std::string{name} + "'"
                );
            }

            // Resolves an error-kind leaf against the wire spelling uf.errors is
            // keyed by. A misspelling would otherwise be a nil that makes every
            // comparison against it silently false. Nothing is recorded: the
            // kinds are host vocabulary, not project resources.
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
                        + "' is exposed under uf.errors"
                );
            }
        };
    }

    auto validateScriptResources(
        std::string_view source,
        std::string_view chunkName,
        PageModelFacts const& model
    ) -> Result<ScriptResourceReport>
    {
        auto const chunk = std::string{chunkName};

        try
        {
            // parse collects recoverable syntax errors into result.errors and
            // catches its own fatal ParseError, returning a possibly-null root.
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

            auto visitor = ResourceVisitor{model};
            result.root->visit(&visitor);

            if (auto const& failure = visitor.failure())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "task script '" + chunk
                        + "' has a disallowed uf reference: " + *failure
                );
            }
            return visitor.takeReport();
        }
        catch (std::exception const& error)
        {
            // The parser is third-party: an input that escapes its own error
            // collection fails closed rather than crossing this boundary.
            return fail(
                AutomationErrorKind::InvalidResource,
                "task script '" + chunk + "' could not be parsed: " + error.what()
            );
        }
    }
}
