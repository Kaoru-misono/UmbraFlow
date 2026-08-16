#include "schema.hpp"

#include "error.hpp"
#include "value.hpp"

#include <core/error/contracts.hpp>
#include <core/types/integer.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <memory>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::json
{
    namespace
    {
        constexpr auto k_dialect = std::string_view{
            "https://json-schema.org/draft/2020-12/schema"
        };

        // Deeper than any $ref chain either corpus needs -- the longest is 4 --
        // and shallow enough that a schema whose $refs form a cycle is refused
        // instead of descending until the stack is gone.
        constexpr auto k_maximumEvaluationDepth = std::size_t{128};

        // 2^53, and the comparisons against it are >= rather than >. Every
        // integer of SMALLER magnitude has exactly one double, so an assertion
        // below this bound means the same thing to this evaluator as to a
        // reader of the schema text. At the bound itself the sharing has
        // already begun: 2^53 and 2^53+1 are one double, so a schema spelling
        // either is stored as the same number -- see requireExactNumericBound.
        constexpr auto k_exactIntegerLimit = 9007199254740992.0;

        // Every keyword this evaluator implements. Adding a name here without
        // adding its Keyword to the switch in applyKeyword does not compile:
        // /w44061 /w44062 with /WX, and -Wswitch-enum with -Werror, reject a
        // switch over this enum that omits an enumerator. That chain -- name to
        // table to enumerator to total switch -- is what makes "an unknown
        // keyword is refused" true of the code rather than of a comment.
        enum class Keyword : uint8
        {
            Comment,
            Defs,
            Id,
            Ref,
            Dialect,
            AdditionalProperties,
            AllOf,
            AnyOf,
            Const,
            Description,
            Else,
            Enum,
            Format,
            If,
            Items,
            MaxItems,
            MaxLength,
            MaxProperties,
            Maximum,
            MinItems,
            MinLength,
            MinProperties,
            Minimum,
            Not,
            OneOf,
            Pattern,
            PrefixItems,
            Properties,
            Required,
            Then,
            Title,
            Type,
            UniqueItems,
        };

        // What a keyword's value must be for the keyword to mean anything. An
        // evaluator that skips this reads `"required": "id"` as an empty name
        // list and `"minLength": "3"` as the bound zero, and both are checks
        // that cannot fail.
        enum class Shape : uint8
        {
            AnyValue,
            AnyArray,
            BooleanValue,
            CountBound,
            NumericBound,
            RegexPattern,
            Reference,
            RootAbsoluteUri,
            RootDialect,
            SchemaList,
            SchemaMap,
            StringValue,
            SubSchema,
            TypeNames,
            UniqueStringArray,
        };

        struct KeywordSpec final
        {
            std::string_view name{};
            Keyword          keyword{};
            Shape            shape{};
        };

        constexpr auto k_keywords = std::array{
            KeywordSpec{"$comment", Keyword::Comment, Shape::StringValue},
            KeywordSpec{"$defs", Keyword::Defs, Shape::SchemaMap},
            KeywordSpec{"$id", Keyword::Id, Shape::RootAbsoluteUri},
            KeywordSpec{"$ref", Keyword::Ref, Shape::Reference},
            KeywordSpec{"$schema", Keyword::Dialect, Shape::RootDialect},
            KeywordSpec{
                "additionalProperties",
                Keyword::AdditionalProperties,
                Shape::SubSchema,
            },
            KeywordSpec{"allOf", Keyword::AllOf, Shape::SchemaList},
            KeywordSpec{"anyOf", Keyword::AnyOf, Shape::SchemaList},
            KeywordSpec{"const", Keyword::Const, Shape::AnyValue},
            KeywordSpec{"description", Keyword::Description, Shape::StringValue},
            KeywordSpec{"else", Keyword::Else, Shape::SubSchema},
            KeywordSpec{"enum", Keyword::Enum, Shape::AnyArray},
            KeywordSpec{"format", Keyword::Format, Shape::StringValue},
            KeywordSpec{"if", Keyword::If, Shape::SubSchema},
            KeywordSpec{"items", Keyword::Items, Shape::SubSchema},
            KeywordSpec{"maxItems", Keyword::MaxItems, Shape::CountBound},
            KeywordSpec{"maxLength", Keyword::MaxLength, Shape::CountBound},
            KeywordSpec{"maxProperties", Keyword::MaxProperties, Shape::CountBound},
            KeywordSpec{"maximum", Keyword::Maximum, Shape::NumericBound},
            KeywordSpec{"minItems", Keyword::MinItems, Shape::CountBound},
            KeywordSpec{"minLength", Keyword::MinLength, Shape::CountBound},
            KeywordSpec{"minProperties", Keyword::MinProperties, Shape::CountBound},
            KeywordSpec{"minimum", Keyword::Minimum, Shape::NumericBound},
            KeywordSpec{"not", Keyword::Not, Shape::SubSchema},
            KeywordSpec{"oneOf", Keyword::OneOf, Shape::SchemaList},
            KeywordSpec{"pattern", Keyword::Pattern, Shape::RegexPattern},
            KeywordSpec{"prefixItems", Keyword::PrefixItems, Shape::SchemaList},
            KeywordSpec{"properties", Keyword::Properties, Shape::SchemaMap},
            KeywordSpec{"required", Keyword::Required, Shape::UniqueStringArray},
            KeywordSpec{"then", Keyword::Then, Shape::SubSchema},
            KeywordSpec{"title", Keyword::Title, Shape::StringValue},
            KeywordSpec{"type", Keyword::Type, Shape::TypeNames},
            KeywordSpec{"uniqueItems", Keyword::UniqueItems, Shape::BooleanValue},
        };

        [[nodiscard]]
        consteval auto keywordsAreSortedAndDistinct() -> bool
        {
            for (auto index = std::size_t{1}; index < k_keywords.size(); ++index)
            {
                // Both subscripts stay under k_keywords.size(), and this runs
                // only under constant evaluation, where a stray index is a
                // compile error.
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
                if (!(k_keywords[index - 1U].name < k_keywords[index].name))
                {
                    return false;
                }
            }
            for (auto left = std::size_t{0}; left < k_keywords.size(); ++left)
            {
                for (auto right = left + 1U; right < k_keywords.size(); ++right)
                {
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
                    if (k_keywords[left].keyword == k_keywords[right].keyword)
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        // implementedKeywords promises ASCII order, and one enumerator per
        // name is what lets the switch in applyKeyword stand for the table.
        static_assert(keywordsAreSortedAndDistinct());

        [[nodiscard]]
        consteval auto keywordNames()
            -> std::array<std::string_view, k_keywords.size()>
        {
            auto names = std::array<std::string_view, k_keywords.size()>{};
            for (auto index = std::size_t{0}; index < k_keywords.size(); ++index)
            {
                // Both subscripts stay under k_keywords.size(), and this runs
                // only under constant evaluation, where a stray index is a
                // compile error.
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
                names[index] = k_keywords[index].name;
            }
            return names;
        }

        constexpr auto k_keywordNames = keywordNames();

        enum class TypeName : uint8
        {
            Null,
            Boolean,
            Object,
            Array,
            String,
            Number,
            Integer,
        };

        struct TypeNameSpec final
        {
            std::string_view name{};
            TypeName         type{};
        };

        constexpr auto k_typeNames = std::array{
            TypeNameSpec{"array", TypeName::Array},
            TypeNameSpec{"boolean", TypeName::Boolean},
            TypeNameSpec{"integer", TypeName::Integer},
            TypeNameSpec{"null", TypeName::Null},
            TypeNameSpec{"number", TypeName::Number},
            TypeNameSpec{"object", TypeName::Object},
            TypeNameSpec{"string", TypeName::String},
        };

        [[nodiscard]]
        auto findKeyword(std::string_view name) -> KeywordSpec const*
        {
            auto const found = std::ranges::find(
                k_keywords,
                name,
                &KeywordSpec::name
            );
            return found == k_keywords.end() ? nullptr : &*found;
        }

        [[nodiscard]]
        auto findTypeName(std::string_view name) -> TypeNameSpec const*
        {
            auto const found = std::ranges::find(
                k_typeNames,
                name,
                &TypeNameSpec::name
            );
            return found == k_typeNames.end() ? nullptr : &*found;
        }

        [[nodiscard]]
        auto unsupported(std::string message) -> std::unexpected<Error>
        {
            return fail(ErrorKind::SchemaUnsupported, std::move(message));
        }

        [[nodiscard]]
        auto matchesType(Value const& instance, TypeName type) -> bool
        {
            switch (type)
            {
            case TypeName::Null: return instance.kind() == ValueKind::Null;
            case TypeName::Boolean: return instance.kind() == ValueKind::Boolean;
            case TypeName::Object: return instance.kind() == ValueKind::Object;
            case TypeName::Array: return instance.kind() == ValueKind::Array;
            case TypeName::String: return instance.kind() == ValueKind::String;
            case TypeName::Number: return instance.kind() == ValueKind::Number;
            case TypeName::Integer: return instance.isInteger();
            }

            UF_UNREACHABLE_MSG("Unknown json TypeName value");
        }

        // Code points rather than bytes, because minLength and maxLength count
        // characters and a UTF-8 byte count would silently refuse a short name
        // written in a non-Latin script.
        [[nodiscard]]
        auto codePointCount(std::string_view text) -> std::size_t
        {
            auto count = std::size_t{0};
            for (auto const character : text)
            {
                if ((static_cast<uint8>(character) & 0xC0U) != 0x80U)
                {
                    ++count;
                }
            }
            return count;
        }

        // ---------------------------------------------------------------
        // Patterns.
        //
        // JSON Schema reads `pattern` as ECMA-262 without the multiline flag,
        // where ^ asserts the start of the whole subject and $ its end. MSVC's
        // std::regex asserts them at every line boundary instead, whatever
        // syntax_option_type is passed: measured 2026-08-11, `^[0-9a-f]{4}$`
        // matched "zzzz\n0123". Left alone that is a false accept on exactly
        // the patterns this repository uses to pin hashes and identifiers, and
        // uf-chaos's evaluator carries it (contract/json-schema.cpp:292-312).
        //
        // The fix is to leave no anchor for the engine to misread. A pattern
        // with no anchor is unaffected by the deviation and is searched as
        // written. A pattern anchored at both ends has both anchors removed and
        // is matched against the whole subject, which is what the anchors meant.
        // Every other anchoring is refused when the schema compiles, because
        // this engine cannot answer for it -- and because none occurs: all 19
        // distinct patterns across both trees' 30 schemas are one of the two.
        // ---------------------------------------------------------------

        enum class PatternAnchoring : uint8
        {
            Unanchored,
            FullyAnchored,
        };

        struct CompiledPattern final
        {
            std::string      source{};
            PatternAnchoring anchoring{PatternAnchoring::Unanchored};
            std::regex       expression{};
        };

        struct PatternShape final
        {
            PatternAnchoring anchoring{PatternAnchoring::Unanchored};
            std::string_view effective{};
        };

        [[nodiscard]]
        auto anchoringOf(std::string_view pattern) -> Result<PatternShape>
        {
            auto anchors        = std::vector<std::size_t>{};
            auto topAlternation = false;
            auto inClass        = false;
            auto escaped        = false;
            auto depth          = std::size_t{0};

            for (auto at = std::size_t{0}; at < pattern.size(); ++at)
            {
                auto const character = pattern[at];
                if (escaped)
                {
                    escaped = false;
                    continue;
                }
                if (character == '\\')
                {
                    escaped = true;
                    continue;
                }
                if (inClass)
                {
                    inClass = character != ']';
                    continue;
                }

                switch (character)
                {
                case '[': inClass = true; break;
                case '(': ++depth; break;
                case ')':
                    if (depth > 0U)
                    {
                        --depth;
                    }
                    break;
                case '|': topAlternation = topAlternation || depth == 0U; break;
                case '^':
                case '$': anchors.emplace_back(at); break;
                default: break;
                }
            }

            if (anchors.empty())
            {
                return PatternShape{
                    .anchoring = PatternAnchoring::Unanchored,
                    .effective = pattern,
                };
            }
            if (
                anchors.size() == 2U
                && anchors.front() == 0U
                && pattern.front() == '^'
                && anchors.back() == pattern.size() - 1U
                && pattern.back() == '$'
                && !topAlternation
            )
            {
                return PatternShape{
                    .anchoring = PatternAnchoring::FullyAnchored,
                    .effective = pattern.substr(1U, pattern.size() - 2U),
                };
            }

            return unsupported(std::format(
                "a pattern anchors somewhere this evaluator cannot answer for, "
                "because the platform regex engine asserts ^ and $ at every line "
                "boundary rather than at the ends of the subject: {}",
                pattern
            ));
        }

        // ---------------------------------------------------------------
        // Reference resolution over a closed set of documents.
        // ---------------------------------------------------------------

        struct Resource final
        {
            std::string label{};
            std::string identity{};
            Value       root{};
        };

        [[nodiscard]]
        auto unescapePointerToken(std::string_view token) -> std::string
        {
            auto text = std::string{};
            for (auto at = std::size_t{0}; at < token.size(); ++at)
            {
                if (token[at] != '~' || at + 1U >= token.size())
                {
                    text.push_back(token[at]);
                    continue;
                }
                ++at;
                text.push_back(token[at] == '0' ? '~' : '/');
            }
            return text;
        }

        // RFC 3986 reference resolution, narrowed to the two forms that occur:
        // an absolute URI, and a relative reference naming a sibling of the
        // base. Everything else is refused rather than approximated, because a
        // base URI this evaluator resolves differently from the schema's author
        // would silently validate against the wrong document.
        [[nodiscard]]
        auto resolveIdentity(std::string_view base, std::string_view reference)
            -> Result<std::string>
        {
            auto const schemeEnd = reference.find(':');
            auto const slashAt   = reference.find('/');
            if (schemeEnd != std::string_view::npos
                && (slashAt == std::string_view::npos || schemeEnd < slashAt))
            {
                return std::string{reference};
            }
            if (reference.starts_with('/') || reference.contains("..")
                || reference.contains("./") || reference.contains('?'))
            {
                return unsupported(std::format(
                    "a $ref uses a relative form this evaluator does not resolve: "
                    "{}",
                    reference
                ));
            }
            if (base.empty())
            {
                return unsupported(std::format(
                    "a $ref is relative but its document declares no $id: {}",
                    reference
                ));
            }

            auto const lastSlash = base.rfind('/');
            if (lastSlash == std::string_view::npos)
            {
                return unsupported(std::format(
                    "a $ref is relative to a base with no path: {}",
                    base
                ));
            }

            return std::string{base.substr(0U, lastSlash + 1U)}
                + std::string{reference};
        }

        // The scheme and authority of an absolute URI, which is the whole of
        // what says who publishes it. Two identities sharing an origin are
        // published together, so a reference that misses inside a registered
        // origin names a sibling nobody handed to compile -- a different defect
        // from a reference to an origin this evaluator has never been given,
        // and one with a different repair.
        [[nodiscard]]
        auto originOf(std::string_view identity) -> std::string_view
        {
            auto const authorityAt = identity.find("//");
            if (authorityAt == std::string_view::npos)
            {
                return identity;
            }
            auto const pathAt = identity.find('/', authorityAt + 2U);
            return pathAt == std::string_view::npos
                ? identity
                : identity.substr(0U, pathAt);
        }

        struct Target final
        {
            std::size_t resource{0};
            // A borrow into that resource's tree, which the Schema owns for as
            // long as any evaluation using this target can run.
            Value const* p_node{nullptr};
        };

        [[nodiscard]]
        auto resolveReference(
            std::span<Resource const> resources,
            std::size_t from,
            std::string_view reference
        ) -> Result<Target>
        {
            auto const hashAt = reference.find('#');
            if (reference.find('#', hashAt == std::string_view::npos ? 0U : hashAt + 1U)
                != std::string_view::npos)
            {
                return unsupported(
                    std::format("a $ref carries more than one fragment: {}", reference)
                );
            }

            auto const documentPart = reference.substr(0U, hashAt);
            auto const fragment     = hashAt == std::string_view::npos
                    ? std::string_view{}
                    : reference.substr(hashAt + 1U);

            auto resource = from;
            if (!documentPart.empty())
            {
                UF_TRY_VALUE(
                    identity,
                    resolveIdentity(resources[from].identity, documentPart)
                );
                auto const found = std::ranges::find(
                    resources,
                    identity,
                    &Resource::identity
                );
                if (found == resources.end())
                {
                    auto const origin  = originOf(identity);
                    auto const sibling = std::ranges::any_of(
                        resources,
                        [origin](Resource const& resource)
                        {
                            return !resource.identity.empty()
                                && originOf(resource.identity) == origin;
                        }
                    );
                    if (sibling)
                    {
                        return unsupported(std::format(
                            "a $ref names a document outside the set this schema "
                            "was compiled from: {}",
                            identity
                        ));
                    }
                    return unsupported(std::format(
                        "a $ref names a remote document, and this evaluator "
                        "fetches nothing: {}",
                        identity
                    ));
                }
                resource = static_cast<std::size_t>(found - resources.begin());
            }

            auto const* p_node = &resources[resource].root;
            if (fragment.empty())
            {
                return Target{.resource = resource, .p_node = p_node};
            }
            if (!fragment.starts_with('/'))
            {
                return unsupported(std::format(
                    "a $ref names an anchor rather than a JSON pointer, which this "
                    "evaluator does not implement: {}",
                    reference
                ));
            }

            auto rest = fragment.substr(1U);
            while (true)
            {
                auto const slashAt = rest.find('/');
                auto const token   = unescapePointerToken(
                    slashAt == std::string_view::npos ? rest : rest.substr(0U, slashAt)
                );
                if (p_node->kind() != ValueKind::Object)
                {
                    return unsupported(std::format(
                        "a $ref walks through something that is not an object: {}",
                        reference
                    ));
                }

                auto const* const p_next = p_node->find(token);
                if (p_next == nullptr)
                {
                    return unsupported(std::format(
                        "a $ref has no target in the document it names: {}",
                        reference
                    ));
                }

                p_node = p_next;
                if (slashAt == std::string_view::npos)
                {
                    return Target{.resource = resource, .p_node = p_node};
                }
                rest = rest.substr(slashAt + 1U);
            }
        }

        // ---------------------------------------------------------------
        // Compilation: the keyword boundary and every keyword's value shape.
        // ---------------------------------------------------------------

        [[nodiscard]]
        auto requireExactNumericBound(std::string_view keyword, Value const& value)
            -> Status
        {
            if (value.kind() != ValueKind::Number)
            {
                return unsupported(std::format("{} must be a number", keyword));
            }
            // A non-integer bound is as exact as the instances it judges: both
            // literals travel through the same double. Two integers of larger
            // magnitude can share one, so a bound written there admits or
            // refuses a value it does not name.
            if (value.isInteger() && std::abs(value.number()) >= k_exactIntegerLimit)
            {
                return unsupported(std::format(
                    "{} is an integer beyond 2^53, which no double distinguishes "
                    "from its neighbours; RFC 8785 makes double the number model of "
                    "every document this evaluator judges, so the bound cannot be "
                    "enforced as written",
                    keyword
                ));
            }
            return ok();
        }

        [[nodiscard]]
        auto requireCountBound(std::string_view keyword, Value const& value) -> Status
        {
            if (!value.isInteger() || value.number() < 0.0
                || value.number() >= k_exactIntegerLimit)
            {
                return unsupported(std::format(
                    "{} must be a non-negative integer below 2^53",
                    keyword
                ));
            }
            return ok();
        }

        [[nodiscard]]
        auto requireTypeNames(Value const& value) -> Status
        {
            auto const requireOne = [](Value const& name) -> Status
            {
                if (name.kind() != ValueKind::String)
                {
                    return unsupported("every type must be named by a string");
                }
                if (findTypeName(name.string()) == nullptr)
                {
                    return unsupported(
                        std::format("'{}' is not a JSON type name", name.string())
                    );
                }
                return ok();
            };

            if (value.kind() == ValueKind::String)
            {
                return requireOne(value);
            }
            if (value.kind() != ValueKind::Array || value.items().empty())
            {
                return unsupported(
                    "type must be a type name or a non-empty array of them"
                );
            }
            for (auto const& name : value.items())
            {
                UF_TRY(requireOne(name));
            }
            return ok();
        }

        [[nodiscard]]
        auto requireUniqueStringArray(std::string_view keyword, Value const& value)
            -> Status
        {
            if (value.kind() != ValueKind::Array)
            {
                return unsupported(std::format("{} must be an array", keyword));
            }
            for (auto left = std::size_t{0}; left < value.items().size(); ++left)
            {
                if (value.items()[left].kind() != ValueKind::String)
                {
                    return unsupported(
                        std::format("every {} entry must be a string", keyword)
                    );
                }
                for (auto right = left + 1U; right < value.items().size(); ++right)
                {
                    if (value.items()[left] == value.items()[right])
                    {
                        return unsupported(std::format(
                            "{} names '{}' twice",
                            keyword,
                            value.items()[left].string()
                        ));
                    }
                }
            }
            return ok();
        }

        class Compiler final
        {
            std::vector<Resource>        m_resources{};
            std::vector<CompiledPattern> m_patterns{};

        public:
            [[nodiscard]] auto resources() && -> std::vector<Resource>
            {
                return std::move(m_resources);
            }

            [[nodiscard]] auto patterns() && -> std::vector<CompiledPattern>
            {
                return std::move(m_patterns);
            }

            [[nodiscard]] auto addResource(Schema::Document const& document) -> Status;
            [[nodiscard]] auto checkEveryResource() -> Status;

        private:
            [[nodiscard]]
            auto checkNode(std::size_t resource, Value const& node, bool isRoot)
                -> Status;

            [[nodiscard]]
            auto checkValue(
                std::size_t resource,
                Shape shape,
                std::string_view keyword,
                Value const& value,
                bool isRoot
            ) -> Status;

            [[nodiscard]] auto addPattern(std::string_view pattern) -> Status;
        };

        auto Compiler::addResource(Schema::Document const& document) -> Status
        {
            UF_TRY_VALUE_CONTEXT(
                root,
                parse(document.exactBytes),
                std::string{document.label}
            );

            auto identity = std::string{};
            if (auto const* const p_id = root.find("$id"); p_id != nullptr)
            {
                if (p_id->kind() != ValueKind::String
                    || !p_id->string().contains(':'))
                {
                    return unsupported(std::format(
                        "{}: $id must be an absolute URI",
                        document.label
                    ));
                }
                identity = std::string{p_id->string()};
            }

            auto const clashes = std::ranges::any_of(
                m_resources,
                [&identity](Resource const& resource)
                {
                    return !identity.empty() && resource.identity == identity;
                }
            );
            if (clashes)
            {
                return unsupported(std::format(
                    "two documents in this set declare the same $id: {}",
                    identity
                ));
            }

            m_resources.emplace_back(
                std::string{document.label},
                std::move(identity),
                std::move(root)
            );
            return ok();
        }

        auto Compiler::addPattern(std::string_view pattern) -> Status
        {
            auto const known = std::ranges::any_of(
                m_patterns,
                [pattern](CompiledPattern const& entry)
                {
                    return entry.source == pattern;
                }
            );
            if (known)
            {
                return ok();
            }

            UF_TRY_VALUE(shape, anchoringOf(pattern));
            try
            {
                m_patterns.emplace_back(CompiledPattern{
                    .source    = std::string{pattern},
                    .anchoring = shape.anchoring,
                    .expression = std::regex{
                        std::string{shape.effective},
                        std::regex::ECMAScript,
                    },
                });
            }
            catch (std::regex_error const& error)
            {
                return unsupported(std::format(
                    "a pattern this evaluator cannot compile: {} ({})",
                    pattern,
                    error.what()
                ));
            }
            return ok();
        }

        auto Compiler::checkValue(
            std::size_t resource,
            Shape shape,
            std::string_view keyword,
            Value const& value,
            bool isRoot
        ) -> Status
        {
            switch (shape)
            {
            case Shape::AnyValue: return ok();
            case Shape::AnyArray:
                if (value.kind() != ValueKind::Array)
                {
                    return unsupported(std::format("{} must be an array", keyword));
                }
                return ok();
            case Shape::BooleanValue:
                if (value.kind() != ValueKind::Boolean)
                {
                    return unsupported(std::format("{} must be a boolean", keyword));
                }
                return ok();
            case Shape::CountBound: return requireCountBound(keyword, value);
            case Shape::NumericBound: return requireExactNumericBound(keyword, value);
            case Shape::RegexPattern:
                if (value.kind() != ValueKind::String)
                {
                    return unsupported(std::format("{} must be a string", keyword));
                }
                return addPattern(value.string());
            case Shape::Reference:
                if (value.kind() != ValueKind::String)
                {
                    return unsupported(std::format("{} must be a string", keyword));
                }
                UF_TRY(resolveReference(m_resources, resource, value.string()));
                return ok();
            case Shape::RootAbsoluteUri:
                // Checked in addResource for the root, where it sets the base a
                // relative $ref resolves against. Anywhere else it would move
                // that base under this evaluator's feet.
                if (!isRoot)
                {
                    return unsupported(
                        "$id below a document's root declares a base URI this "
                        "evaluator does not implement"
                    );
                }
                return ok();
            case Shape::RootDialect:
                if (!isRoot)
                {
                    return unsupported(
                        "$schema below a document's root declares a second dialect, "
                        "which this evaluator does not implement"
                    );
                }
                if (value.kind() != ValueKind::String || value.string() != k_dialect)
                {
                    return unsupported(std::format(
                        "$schema must be exactly {}",
                        k_dialect
                    ));
                }
                return ok();
            case Shape::SchemaList:
                if (value.kind() != ValueKind::Array || value.items().empty())
                {
                    return unsupported(std::format(
                        "{} must be a non-empty array of schemas",
                        keyword
                    ));
                }
                for (auto const& item : value.items())
                {
                    UF_TRY(checkNode(resource, item, false));
                }
                return ok();
            case Shape::SchemaMap:
                if (value.kind() != ValueKind::Object)
                {
                    return unsupported(
                        std::format("{} must be an object of schemas", keyword)
                    );
                }
                for (auto const& member : value.members())
                {
                    UF_TRY(checkNode(resource, member.second, false));
                }
                return ok();
            case Shape::StringValue:
                if (value.kind() != ValueKind::String)
                {
                    return unsupported(std::format("{} must be a string", keyword));
                }
                return ok();
            case Shape::SubSchema: return checkNode(resource, value, false);
            case Shape::TypeNames: return requireTypeNames(value);
            case Shape::UniqueStringArray:
                return requireUniqueStringArray(keyword, value);
            }

            UF_UNREACHABLE_MSG("Unknown json schema Shape value");
        }

        auto Compiler::checkNode(std::size_t resource, Value const& node, bool isRoot)
            -> Status
        {
            if (node.kind() == ValueKind::Boolean)
            {
                return ok();
            }
            if (node.kind() != ValueKind::Object)
            {
                return unsupported("a schema must be an object or a boolean");
            }

            for (auto const& member : node.members())
            {
                auto const* const p_spec = findKeyword(member.first);
                if (p_spec == nullptr)
                {
                    return unsupported(std::format(
                        "this evaluator does not implement the schema keyword '{}', "
                        "so a schema carrying it would be silently weaker than it "
                        "reads",
                        member.first
                    ));
                }

                UF_TRY(checkValue(
                    resource,
                    p_spec->shape,
                    p_spec->name,
                    member.second,
                    isRoot
                ));
            }
            return ok();
        }

        auto Compiler::checkEveryResource() -> Status
        {
            for (auto index = std::size_t{0}; index < m_resources.size(); ++index)
            {
                UF_TRY_CONTEXT(
                    checkNode(index, m_resources[index].root, true),
                    m_resources[index].label
                );
            }
            return ok();
        }
    }

    // -------------------------------------------------------------------
    // Evaluation.
    // -------------------------------------------------------------------

    struct Schema::State final
    {
        std::vector<Resource>        resources{};
        std::vector<CompiledPattern> patterns{};
    };

    namespace
    {
        // Both spans and the label view name storage the Schema owns through a
        // shared_ptr the caller of validate holds for the whole call, so an
        // Evaluator never outlives what it borrows.
        class Evaluator final
        {
            std::span<Resource const>        m_resources;
            std::span<CompiledPattern const> m_patterns;
            std::string_view                 m_label;

        public:
            Evaluator(
                std::span<Resource const> resources,
                std::span<CompiledPattern const> patterns
            ) noexcept
                : m_resources{resources}
                , m_patterns{patterns}
                , m_label{resources.front().label}
            {
            }

            [[nodiscard]]
            auto evaluate(
                std::size_t resource,
                Value const& schema,
                Value const& instance,
                std::string const& path,
                std::size_t depth
            ) const -> Status;

        private:
            // DocumentRejected unless a keyword demands its own kind: the
            // additionalProperties: false refusal is a closure violation, and
            // combinators that decide how many branches matched carry that
            // kind through when the only failures were closures.
            [[nodiscard]]
            auto refuseAt(
                std::string const& path,
                std::string_view keyword,
                std::string_view detail,
                ErrorKind kind = ErrorKind::DocumentRejected
            ) const -> std::unexpected<Error>
            {
                return fail(
                    kind,
                    std::format(
                        "{} refused {}: {} {}",
                        m_label,
                        path.empty() ? std::string_view{"the document"}
                                     : std::string_view{path},
                        keyword,
                        detail
                    )
                );
            }

            [[nodiscard]] auto matchesPattern(
                std::string_view pattern,
                std::string_view text
            ) const -> bool;

            // Whether a branch of anyOf, oneOf, not, if or contains merely
            // failed to match. A SchemaUnsupported failure -- the evaluation
            // depth limit is the only one that can still arise here, because
            // compile resolved every $ref and compiled every pattern -- is not
            // a non-match and must propagate, or a $ref cycle would silently
            // turn into "this branch did not apply". A closure refusal is a
            // non-match like any other rejection: inside a states oneOf, the
            // wait branch of a ui_action state refuses the ui_action member as
            // undeclared, and that branch must still lose to the branch that
            // does match.
            [[nodiscard]] static auto isNonMatch(Status const& outcome) -> bool
            {
                if (outcome.has_value())
                {
                    return false;
                }
                auto const kind = errorKind(outcome.error());
                return kind == ErrorKind::DocumentRejected
                    || kind == ErrorKind::DocumentClosureRejected;
            }

            [[nodiscard]]
            auto applyKeyword(
                std::size_t resource,
                Keyword keyword,
                Value const& schema,
                Value const& value,
                Value const& instance,
                std::string const& path,
                std::size_t depth
            ) const -> Status;

            [[nodiscard]]
            auto applyProperties(
                std::size_t resource,
                Value const& value,
                Value const& instance,
                std::string const& path,
                std::size_t depth
            ) const -> Status;

            [[nodiscard]]
            auto applyAdditionalProperties(
                std::size_t resource,
                Value const& schema,
                Value const& value,
                Value const& instance,
                std::string const& path,
                std::size_t depth
            ) const -> Status;

            [[nodiscard]]
            auto applyItems(
                std::size_t resource,
                Value const& schema,
                Value const& value,
                Value const& instance,
                std::string const& path,
                std::size_t depth
            ) const -> Status;
        };

        auto Evaluator::matchesPattern(
            std::string_view pattern,
            std::string_view text
        ) const -> bool
        {
            auto const found = std::ranges::find(
                m_patterns,
                pattern,
                &CompiledPattern::source
            );
            // compile visited every pattern in every resource, so a pattern
            // reachable here is one it compiled.
            UF_CHECK(found != m_patterns.end());

            switch (found->anchoring)
            {
            case PatternAnchoring::Unanchored:
                return std::regex_search(text.begin(), text.end(), found->expression);
            case PatternAnchoring::FullyAnchored:
                return std::regex_match(text.begin(), text.end(), found->expression);
            }

            UF_UNREACHABLE_MSG("Unknown json schema PatternAnchoring value");
        }

        auto Evaluator::applyProperties(
            std::size_t resource,
            Value const& value,
            Value const& instance,
            std::string const& path,
            std::size_t depth
        ) const -> Status
        {
            for (auto const& member : instance.members())
            {
                auto const* const p_schema = value.find(member.first);
                if (p_schema == nullptr)
                {
                    continue;
                }
                UF_TRY(evaluate(
                    resource,
                    *p_schema,
                    member.second,
                    path.empty() ? member.first
                                 : std::format("{}.{}", path, member.first),
                    depth + 1U
                ));
            }
            return ok();
        }

        auto Evaluator::applyAdditionalProperties(
            std::size_t resource,
            Value const& schema,
            Value const& value,
            Value const& instance,
            std::string const& path,
            std::size_t depth
        ) const -> Status
        {
            auto const* const p_properties = schema.find("properties");
            for (auto const& member : instance.members())
            {
                if (p_properties != nullptr
                    && p_properties->find(member.first) != nullptr)
                {
                    continue;
                }
                if (value.kind() == ValueKind::Boolean && !value.boolean())
                {
                    return refuseAt(
                        path,
                        "additionalProperties",
                        std::format(
                            "carries '{}', which this closed object does not declare",
                            member.first
                        ),
                        ErrorKind::DocumentClosureRejected
                    );
                }
                UF_TRY(evaluate(
                    resource,
                    value,
                    member.second,
                    path.empty() ? member.first
                                 : std::format("{}.{}", path, member.first),
                    depth + 1U
                ));
            }
            return ok();
        }

        auto Evaluator::applyItems(
            std::size_t resource,
            Value const& schema,
            Value const& value,
            Value const& instance,
            std::string const& path,
            std::size_t depth
        ) const -> Status
        {
            auto const* const p_prefix = schema.find("prefixItems");
            auto const        skipped  = p_prefix == nullptr
                       ? std::size_t{0}
                       : std::min(p_prefix->items().size(), instance.items().size());

            for (auto index = skipped; index < instance.items().size(); ++index)
            {
                UF_TRY(evaluate(
                    resource,
                    value,
                    instance.items()[index],
                    std::format("{}[{}]", path, index),
                    depth + 1U
                ));
            }
            return ok();
        }

        auto Evaluator::applyKeyword(
            std::size_t resource,
            Keyword keyword,
            Value const& schema,
            Value const& value,
            Value const& instance,
            std::string const& path,
            std::size_t depth
        ) const -> Status
        {
            switch (keyword)
            {
            // Identity and annotation. The 2020-12 default vocabulary set gives
            // these no assertion behaviour at all, and that includes format:
            // asserting it requires the Format-Assertion vocabulary, which
            // neither this evaluator nor tools/annotate/contracts.py enables. A
            // schema's "format": "date-time" therefore constrains nothing here,
            // exactly as it constrains nothing in the Python that validates the
            // same documents today. Turning it on is a decision for both
            // spellings at once, never for one.
            case Keyword::Comment:
            case Keyword::Dialect:
            case Keyword::Description:
            case Keyword::Format:
            case Keyword::Id:
            case Keyword::Title: return ok();

            // A container of subschemas that applies only where a $ref names
            // one of them.
            case Keyword::Defs: return ok();

            // Applied by Keyword::If, which is the only thing that gives them
            // meaning; 2020-12 section 10.2.2 makes both inert without it.
            case Keyword::Else:
            case Keyword::Then: return ok();

            case Keyword::Ref:
            {
                UF_TRY_VALUE(
                    target,
                    resolveReference(m_resources, resource, value.string())
                );
                return evaluate(
                    target.resource,
                    *target.p_node,
                    instance,
                    path,
                    depth + 1U
                );
            }

            case Keyword::Type:
            {
                if (value.kind() == ValueKind::String)
                {
                    auto const* const p_type = findTypeName(value.string());
                    UF_CHECK(p_type != nullptr);
                    if (!matchesType(instance, p_type->type))
                    {
                        return refuseAt(path, "type", "is not one this schema admits");
                    }
                    return ok();
                }
                for (auto const& name : value.items())
                {
                    auto const* const p_type = findTypeName(name.string());
                    UF_CHECK(p_type != nullptr);
                    if (matchesType(instance, p_type->type))
                    {
                        return ok();
                    }
                }
                return refuseAt(path, "type", "is not one this schema admits");
            }

            case Keyword::Const:
                if (!(instance == value))
                {
                    return refuseAt(path, "const", "is not the required value");
                }
                return ok();

            case Keyword::Enum:
                for (auto const& option : value.items())
                {
                    if (instance == option)
                    {
                        return ok();
                    }
                }
                return refuseAt(path, "enum", "is outside the closed vocabulary");

            case Keyword::Minimum:
                if (instance.kind() == ValueKind::Number
                    && instance.number() < value.number())
                {
                    return refuseAt(path, "minimum", "is below the bound");
                }
                return ok();

            case Keyword::Maximum:
                if (instance.kind() == ValueKind::Number
                    && instance.number() > value.number())
                {
                    return refuseAt(path, "maximum", "is above the bound");
                }
                return ok();

            case Keyword::MinLength:
                if (instance.kind() == ValueKind::String
                    && static_cast<double>(codePointCount(instance.string()))
                        < value.number())
                {
                    return refuseAt(path, "minLength", "is shorter than allowed");
                }
                return ok();

            case Keyword::MaxLength:
                if (instance.kind() == ValueKind::String
                    && static_cast<double>(codePointCount(instance.string()))
                        > value.number())
                {
                    return refuseAt(path, "maxLength", "is longer than allowed");
                }
                return ok();

            case Keyword::Pattern:
                if (instance.kind() == ValueKind::String
                    && !matchesPattern(value.string(), instance.string()))
                {
                    return refuseAt(path, "pattern", "does not match");
                }
                return ok();

            case Keyword::MinItems:
                if (instance.kind() == ValueKind::Array
                    && static_cast<double>(instance.items().size()) < value.number())
                {
                    return refuseAt(path, "minItems", "carries too few items");
                }
                return ok();

            case Keyword::MaxItems:
                if (instance.kind() == ValueKind::Array
                    && static_cast<double>(instance.items().size()) > value.number())
                {
                    return refuseAt(path, "maxItems", "carries too many items");
                }
                return ok();

            case Keyword::UniqueItems:
            {
                if (instance.kind() != ValueKind::Array || !value.boolean())
                {
                    return ok();
                }
                auto const items = instance.items();
                for (auto left = std::size_t{0}; left < items.size(); ++left)
                {
                    for (auto right = left + 1U; right < items.size(); ++right)
                    {
                        if (items[left] == items[right])
                        {
                            return refuseAt(path, "uniqueItems", "repeats an item");
                        }
                    }
                }
                return ok();
            }

            case Keyword::MinProperties:
                if (instance.kind() == ValueKind::Object
                    && static_cast<double>(instance.members().size()) < value.number())
                {
                    return refuseAt(path, "minProperties", "carries too few members");
                }
                return ok();

            case Keyword::MaxProperties:
                if (instance.kind() == ValueKind::Object
                    && static_cast<double>(instance.members().size()) > value.number())
                {
                    return refuseAt(path, "maxProperties", "carries too many members");
                }
                return ok();

            case Keyword::Required:
                if (instance.kind() != ValueKind::Object)
                {
                    return ok();
                }
                for (auto const& name : value.items())
                {
                    if (instance.find(name.string()) == nullptr)
                    {
                        return refuseAt(
                            path,
                            "required",
                            std::format("has no member '{}'", name.string())
                        );
                    }
                }
                return ok();

            case Keyword::Properties:
                if (instance.kind() != ValueKind::Object)
                {
                    return ok();
                }
                return applyProperties(resource, value, instance, path, depth);

            case Keyword::AdditionalProperties:
                if (instance.kind() != ValueKind::Object)
                {
                    return ok();
                }
                return applyAdditionalProperties(
                    resource,
                    schema,
                    value,
                    instance,
                    path,
                    depth
                );

            case Keyword::PrefixItems:
            {
                if (instance.kind() != ValueKind::Array)
                {
                    return ok();
                }
                auto const paired =
                    std::min(value.items().size(), instance.items().size());
                for (auto index = std::size_t{0}; index < paired; ++index)
                {
                    UF_TRY(evaluate(
                        resource,
                        value.items()[index],
                        instance.items()[index],
                        std::format("{}[{}]", path, index),
                        depth + 1U
                    ));
                }
                return ok();
            }

            case Keyword::Items:
                if (instance.kind() != ValueKind::Array)
                {
                    return ok();
                }
                return applyItems(resource, schema, value, instance, path, depth);

            case Keyword::AllOf:
                for (auto const& branch : value.items())
                {
                    UF_TRY(evaluate(resource, branch, instance, path, depth + 1U));
                }
                return ok();

            case Keyword::AnyOf:
            {
                // A branch that failed only by closure is still a non-match,
                // but it is the only evidence of what made every branch fail.
                // When nothing matched and every branch's refusal was a
                // closure one, the outcome carries that kind so a caller that
                // distinguishes closure violations does not lose the one the
                // instance actually committed. A shape refusal from any
                // branch keeps the refusal ordinary: the instance broke a
                // rule no closure site gets to excuse, which is what a
                // closure kind would mislabel.
                auto onlyClosureRefusals = true;
                for (auto const& branch : value.items())
                {
                    auto outcome = evaluate(resource, branch, instance, path, depth + 1U);
                    if (outcome.has_value())
                    {
                        return ok();
                    }
                    if (!isNonMatch(outcome))
                    {
                        return std::unexpected{std::move(outcome).error()};
                    }
                    if (errorKind(outcome.error()) != ErrorKind::DocumentClosureRejected)
                    {
                        onlyClosureRefusals = false;
                    }
                }
                if (onlyClosureRefusals)
                {
                    return refuseAt(
                        path,
                        "anyOf",
                        "satisfies no branch; every branch refused the closed-object rule",
                        ErrorKind::DocumentClosureRejected
                    );
                }
                return refuseAt(path, "anyOf", "satisfies no branch");
            }

            case Keyword::OneOf:
            {
                auto matches             = std::size_t{0};
                auto onlyClosureRefusals = true;
                for (auto const& branch : value.items())
                {
                    auto outcome = evaluate(resource, branch, instance, path, depth + 1U);
                    if (outcome.has_value())
                    {
                        ++matches;
                        continue;
                    }
                    if (!isNonMatch(outcome))
                    {
                        return std::unexpected{std::move(outcome).error()};
                    }
                    if (errorKind(outcome.error()) != ErrorKind::DocumentClosureRejected)
                    {
                        onlyClosureRefusals = false;
                    }
                }
                // A refusal with matches == 0 and nothing but closure refusals
                // behind it is a closure violation wearing a combinator's
                // message: every branch refused the instance on an
                // additionalProperties site, so the closure kind says which
                // rule the instance actually broke. Any shape refusal among
                // the branches, or two or more matches, keeps the rejection
                // ordinary.
                if (matches == 0U && onlyClosureRefusals)
                {
                    return refuseAt(
                        path,
                        "oneOf",
                        "satisfies no branch; every branch refused the closed-object rule",
                        ErrorKind::DocumentClosureRejected
                    );
                }
                if (matches != 1U)
                {
                    return refuseAt(
                        path,
                        "oneOf",
                        std::format("satisfies {} branches rather than one", matches)
                    );
                }
                return ok();
            }

            case Keyword::Not:
            {
                auto outcome = evaluate(resource, value, instance, path, depth + 1U);
                if (outcome.has_value())
                {
                    return refuseAt(path, "not", "satisfies a forbidden schema");
                }
                if (!isNonMatch(outcome))
                {
                    return std::unexpected{std::move(outcome).error()};
                }
                return ok();
            }

            case Keyword::If:
            {
                auto outcome = evaluate(resource, value, instance, path, depth + 1U);
                if (!outcome.has_value() && !isNonMatch(outcome))
                {
                    return std::unexpected{std::move(outcome).error()};
                }

                auto const* const p_branch = outcome.has_value()
                    ? schema.find("then")
                    : schema.find("else");
                if (p_branch == nullptr)
                {
                    return ok();
                }
                return evaluate(resource, *p_branch, instance, path, depth + 1U);
            }
            }

            UF_UNREACHABLE_MSG("Unknown json schema Keyword value");
        }

        auto Evaluator::evaluate(
            std::size_t resource,
            Value const& schema,
            Value const& instance,
            std::string const& path,
            std::size_t depth
        ) const -> Status
        {
            if (depth > k_maximumEvaluationDepth)
            {
                return unsupported(std::format(
                    "{}: a schema recursed deeper than this evaluator allows, which "
                    "a cycle among its $refs is the only way to reach",
                    m_label
                ));
            }
            if (schema.kind() == ValueKind::Boolean)
            {
                if (schema.boolean())
                {
                    return ok();
                }
                return refuseAt(path, "false", "is refused by a schema that admits nothing");
            }

            for (auto const& member : schema.members())
            {
                auto const* const p_spec = findKeyword(member.first);
                // compile refused every name outside the table before this
                // schema existed.
                UF_CHECK(p_spec != nullptr);

                UF_TRY(applyKeyword(
                    resource,
                    p_spec->keyword,
                    schema,
                    member.second,
                    instance,
                    path,
                    depth
                ));
            }
            return ok();
        }
    }

    Schema::Schema(std::shared_ptr<State const> p_state) noexcept
        : m_state{std::move(p_state)}
    {
    }

    auto Schema::compile(
        Document const& document,
        std::span<Document const> referencedDocuments
    ) -> Result<Schema>
    {
        auto compiler = Compiler{};
        UF_TRY(compiler.addResource(document));
        for (auto const& referenced : referencedDocuments)
        {
            UF_TRY(compiler.addResource(referenced));
        }
        UF_TRY(compiler.checkEveryResource());

        auto state      = State{};
        state.resources = std::move(compiler).resources();
        // Not a use after move: the two rvalue-qualified accessors move out
        // disjoint members, and neither reads the other's.
        // NOLINTNEXTLINE(bugprone-use-after-move)
        state.patterns  = std::move(compiler).patterns();
        return Schema{std::make_shared<State const>(std::move(state))};
    }

    auto Schema::validate(Value const& instance) const -> Status
    {
        auto const evaluator = Evaluator{m_state->resources, m_state->patterns};
        return evaluator.evaluate(
            0U,
            m_state->resources.front().root,
            instance,
            std::string{},
            0U
        );
    }

    auto Schema::hasDefinition(std::string_view name) const -> bool
    {
        auto const* const p_defs = m_state->resources.front().root.find("$defs");
        return p_defs != nullptr && p_defs->find(name) != nullptr;
    }

    auto Schema::validateDefinition(std::string_view name, Value const& instance) const
        -> Status
    {
        auto const& root         = m_state->resources.front().root;
        auto const* const p_defs = root.find("$defs");
        auto const* const p_schema =
            p_defs == nullptr ? nullptr : p_defs->find(name);
        if (p_schema == nullptr)
        {
            return unsupported(std::format(
                "{} declares no subschema named '{}'",
                m_state->resources.front().label,
                name
            ));
        }

        auto const evaluator = Evaluator{m_state->resources, m_state->patterns};
        return evaluator.evaluate(0U, *p_schema, instance, std::string{}, 0U);
    }

    auto Schema::implementedKeywords() -> std::span<std::string_view const>
    {
        return k_keywordNames;
    }
}
