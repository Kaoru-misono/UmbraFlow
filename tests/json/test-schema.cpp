#include "repository-path.hpp"

#include <json/error.hpp>
#include <json/schema.hpp>
#include <json/value.hpp>

#include <core/safety/checked-access.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::json
{
    namespace
    {
        [[nodiscard]] auto compiled(std::string_view bytes) -> Schema
        {
            auto schema = Schema::compile(
                Schema::Document{.label = "under-test", .exactBytes = bytes}
            );
            auto const why = schema.has_value()
                ? std::string{}
                : std::string{schema.error().message()};
            INFO(why);
            REQUIRE(schema.has_value());
            return *std::move(schema);
        }

        // Whether compile refused, and refused as a schema problem rather than
        // by mistaking it for a document problem.
        [[nodiscard]] auto compileRefuses(std::string_view bytes) -> bool
        {
            auto const schema = Schema::compile(
                Schema::Document{.label = "under-test", .exactBytes = bytes}
            );
            return !schema.has_value()
                && errorKind(schema.error()) == ErrorKind::SchemaUnsupported;
        }

        [[nodiscard]]
        auto accepts(Schema const& schema, std::string_view instance) -> bool
        {
            auto const value = parse(instance);
            REQUIRE(value.has_value());
            return schema.validate(*value).has_value();
        }

        [[nodiscard]]
        auto rejects(Schema const& schema, std::string_view instance) -> bool
        {
            auto const value = parse(instance);
            REQUIRE(value.has_value());
            auto const outcome = schema.validate(*value);
            return !outcome.has_value()
                && errorKind(outcome.error()) == ErrorKind::DocumentRejected;
        }

        struct KeywordCase final
        {
            std::string_view keyword{};
            std::string_view schema{};
            std::string_view accepted{};
            std::string_view refused{};
        };

        auto checkKeywordCases(std::span<KeywordCase const> cases) -> void
        {
            for (auto const& entry : cases)
            {
                CAPTURE(entry.keyword);
                auto const schema = compiled(entry.schema);
                CHECK(accepts(schema, entry.accepted));
                CHECK(rejects(schema, entry.refused));
            }
        }
    }

    TEST_CASE("json::Schema states exactly the keywords it implements")
    {
        constexpr auto k_expected = std::array{
            std::string_view{"$comment"},
            std::string_view{"$defs"},
            std::string_view{"$id"},
            std::string_view{"$ref"},
            std::string_view{"$schema"},
            std::string_view{"additionalProperties"},
            std::string_view{"allOf"},
            std::string_view{"anyOf"},
            std::string_view{"const"},
            std::string_view{"description"},
            std::string_view{"else"},
            std::string_view{"enum"},
            std::string_view{"format"},
            std::string_view{"if"},
            std::string_view{"items"},
            std::string_view{"maxItems"},
            std::string_view{"maxLength"},
            std::string_view{"maxProperties"},
            std::string_view{"maximum"},
            std::string_view{"minItems"},
            std::string_view{"minLength"},
            std::string_view{"minProperties"},
            std::string_view{"minimum"},
            std::string_view{"not"},
            std::string_view{"oneOf"},
            std::string_view{"pattern"},
            std::string_view{"prefixItems"},
            std::string_view{"properties"},
            std::string_view{"required"},
            std::string_view{"then"},
            std::string_view{"title"},
            std::string_view{"type"},
            std::string_view{"uniqueItems"},
        };

        auto const implemented = Schema::implementedKeywords();
        REQUIRE(implemented.size() == k_expected.size());
        for (auto index = std::size_t{0}; index < k_expected.size(); ++index)
        {
            CHECK(implemented[index] == checkedAt(k_expected, index));
        }
    }

    // The property this module exists for: a keyword nobody implemented is a
    // refusal, never a skip. Each name below is a real Draft 2020-12 keyword
    // outside the implemented set, so accepting one would validate a document
    // against a constraint that was never applied.
    TEST_CASE("json::Schema refuses a keyword it does not implement")
    {
        constexpr auto k_unimplemented = std::array{
            std::string_view{"patternProperties"},
            std::string_view{"propertyNames"},
            std::string_view{"contains"},
            std::string_view{"minContains"},
            std::string_view{"maxContains"},
            std::string_view{"dependentSchemas"},
            std::string_view{"dependentRequired"},
            std::string_view{"unevaluatedProperties"},
            std::string_view{"unevaluatedItems"},
            std::string_view{"exclusiveMinimum"},
            std::string_view{"exclusiveMaximum"},
            std::string_view{"multipleOf"},
            std::string_view{"contentEncoding"},
            std::string_view{"contentMediaType"},
            std::string_view{"$anchor"},
            std::string_view{"$dynamicRef"},
            std::string_view{"$vocabulary"},
            std::string_view{"definitions"},
            std::string_view{"default"},
            std::string_view{"deprecated"},
            std::string_view{"examples"},
            std::string_view{"readOnly"},
            std::string_view{"mispelledType"},
        };

        for (auto const keyword : k_unimplemented)
        {
            CAPTURE(keyword);
            auto document = std::string{
                R"({"$schema":"https://json-schema.org/draft/2020-12/schema",")"
            };
            document += keyword;
            document += R"(":{}})";
            CHECK(compileRefuses(document));
        }

        // Refusal reaches every position a schema can occupy, not only the root.
        CHECK(compileRefuses(R"({"properties":{"a":{"multipleOf":2}}})"));
        CHECK(compileRefuses(R"({"$defs":{"A":{"multipleOf":2}}})"));
        CHECK(compileRefuses(R"({"items":{"multipleOf":2}})"));
        CHECK(compileRefuses(R"({"allOf":[{"multipleOf":2}]})"));
        CHECK(compileRefuses(R"({"if":{"multipleOf":2},"then":true})"));
        CHECK(compileRefuses(R"({"not":{"multipleOf":2}})"));
    }

    // The same defect one level down: a keyword whose value has the wrong shape
    // reads as an empty list or a zero bound, and the check silently cannot
    // fail. uf-chaos's evaluator accepts every line below.
    TEST_CASE("json::Schema refuses a keyword whose value has the wrong shape")
    {
        CHECK(compileRefuses(R"({"required":"id"})"));
        CHECK(compileRefuses(R"({"required":["a","a"]})"));
        CHECK(compileRefuses(R"({"required":[1]})"));
        CHECK(compileRefuses(R"({"minLength":"3"})"));
        CHECK(compileRefuses(R"({"minLength":-1})"));
        CHECK(compileRefuses(R"({"minLength":1.5})"));
        CHECK(compileRefuses(R"({"maxItems":null})"));
        CHECK(compileRefuses(R"({"uniqueItems":"true"})"));
        CHECK(compileRefuses(R"({"type":"integar"})"));
        CHECK(compileRefuses(R"({"type":[]})"));
        CHECK(compileRefuses(R"({"type":1})"));
        CHECK(compileRefuses(R"({"enum":"a"})"));
        CHECK(compileRefuses(R"({"properties":[]})"));
        CHECK(compileRefuses(R"({"allOf":{}})"));
        CHECK(compileRefuses(R"({"allOf":[]})"));
        CHECK(compileRefuses(R"({"items":1})"));
        CHECK(compileRefuses(R"({"pattern":1})"));
        CHECK(compileRefuses(R"({"pattern":"("})"));
        CHECK(compileRefuses(R"({"title":1})"));
        CHECK(compileRefuses(R"({"$ref":1})"));
        CHECK(compileRefuses(R"({"$schema":"https://json-schema.org/draft-07/schema#"})"));
        CHECK(compileRefuses(R"({"$id":"not-absolute"})"));
        CHECK(compileRefuses(R"({"properties":{"a":{"$id":"https://x.test/a"}}})"));
        CHECK(compileRefuses(R"({"properties":{"a":{"$schema":"https://json-schema.org/draft/2020-12/schema"}}})"));
        CHECK(compileRefuses(R"([])"));
        CHECK(compileRefuses(R"({"properties":{"a":1}})"));
    }

    TEST_CASE("json::Schema applies every assertion keyword")
    {
        constexpr auto k_cases = std::array{
            KeywordCase{"type", R"({"type":"integer"})", "1", "1.5"},
            KeywordCase{"type union", R"({"type":["string","null"]})", "null", "1"},
            KeywordCase{"const", R"({"const":{"a":1}})", R"({"a":1})", R"({"a":2})"},
            KeywordCase{"enum", R"({"enum":["a","b"]})", R"("b")", R"("c")"},
            KeywordCase{"minimum", R"({"minimum":3})", "3", "2"},
            KeywordCase{"maximum", R"({"maximum":3})", "3", "4"},
            KeywordCase{"minLength", R"({"minLength":2})", R"("ab")", R"("a")"},
            KeywordCase{"maxLength", R"({"maxLength":2})", R"("ab")", R"("abc")"},
            KeywordCase{"pattern", R"({"pattern":"^a+$"})", R"("aa")", R"("ab")"},
            KeywordCase{"minItems", R"({"minItems":2})", "[1,2]", "[1]"},
            KeywordCase{"maxItems", R"({"maxItems":2})", "[1,2]", "[1,2,3]"},
            KeywordCase{"uniqueItems", R"({"uniqueItems":true})", "[1,2]", "[1,1]"},
            KeywordCase{"minProperties", R"({"minProperties":1})", R"({"a":1})", "{}"},
            KeywordCase{"maxProperties", R"({"maxProperties":1})", R"({"a":1})", R"({"a":1,"b":2})"},
            KeywordCase{"required", R"({"required":["a"]})", R"({"a":1})", R"({"b":1})"},
        };

        checkKeywordCases(k_cases);
    }

    TEST_CASE("json::Schema applies every applicator keyword")
    {
        constexpr auto k_cases = std::array{
            KeywordCase{
                "properties",
                R"({"properties":{"a":{"type":"integer"}}})",
                R"({"a":1})",
                R"({"a":"x"})",
            },
            KeywordCase{
                "additionalProperties",
                R"({"properties":{"a":true},"additionalProperties":false})",
                R"({"a":1})",
                R"({"b":1})",
            },
            KeywordCase{
                "items",
                R"({"items":{"type":"integer"}})",
                "[1,2]",
                "[1,\"x\"]",
            },
            KeywordCase{
                "prefixItems",
                R"({"prefixItems":[{"type":"integer"},{"type":"string"}]})",
                R"([1,"a"])",
                R"([1,2])",
            },
            KeywordCase{
                "prefixItems with items",
                R"({"prefixItems":[{"type":"integer"}],"items":{"type":"boolean"}})",
                "[1,true]",
                "[1,2]",
            },
            KeywordCase{
                "allOf",
                R"({"allOf":[{"type":"integer"},{"minimum":3}]})",
                "3",
                "2",
            },
            KeywordCase{
                "anyOf",
                R"({"anyOf":[{"type":"integer"},{"type":"string"}]})",
                R"("a")",
                "null",
            },
            KeywordCase{
                "oneOf",
                R"({"oneOf":[{"type":"integer"},{"type":"string"}]})",
                "1",
                "null",
            },
            KeywordCase{"not", R"({"not":{"type":"string"}})", "1", R"("a")"},
            KeywordCase{
                "if then",
                R"({"if":{"required":["k"]},"then":{"required":["x"]}})",
                R"({"k":1,"x":1})",
                R"({"k":1})",
            },
            KeywordCase{
                "if else",
                R"({"if":{"required":["k"]},"then":true,"else":{"required":["y"]}})",
                R"({"y":1})",
                R"({"z":1})",
            },
            KeywordCase{
                "$ref",
                R"({"$defs":{"Id":{"type":"string"}},"$ref":"#/$defs/Id"})",
                R"("a")",
                "1",
            },
            KeywordCase{"false schema", R"({"not":true})", "", ""},
        };

        // The last row is a placeholder for the boolean-schema case, checked
        // below where both instances are the same value.
        checkKeywordCases(std::span{k_cases}.first(k_cases.size() - 1U));

        auto const closed = compiled(R"({"properties":{"a":false}})");
        CHECK(accepts(closed, R"({"b":1})"));
        CHECK(rejects(closed, R"({"a":1})"));

        auto const open = compiled(R"({"properties":{"a":true}})");
        CHECK(accepts(open, R"({"a":1})"));
    }

    // oneOf demands exactly one branch, which is the only combinator whose
    // refusal a "some branch matched" reading would lose.
    TEST_CASE("json::Schema refuses a value that satisfies two oneOf branches")
    {
        auto const schema = compiled(R"({"oneOf":[{"type":"integer"},{"minimum":3}]})");
        CHECK(accepts(schema, "1"));
        // 4 satisfies both branches; 2.5 satisfies neither. A string satisfies
        // the minimum branch alone, because a numeric bound says nothing about
        // a non-number, so it is accepted.
        CHECK(rejects(schema, "4"));
        CHECK(rejects(schema, "2.5"));
        CHECK(accepts(schema, R"("a")"));
    }

    // 2020-12 section 10.2.2 makes then and else inert without if, and the
    // default vocabulary set gives title, description, $comment and format no
    // assertion behaviour at all. Each line states a keyword that constrains
    // nothing, so that turning one into an assertion has to be a decision
    // rather than a side effect.
    TEST_CASE("json::Schema treats annotations and orphaned branches as inert")
    {
        CHECK(accepts(compiled(R"({"format":"date-time"})"), R"("not a date")"));
        CHECK(accepts(compiled(R"({"title":"t","description":"d","$comment":"c"})"), "1"));
        CHECK(accepts(compiled(R"({"then":{"required":["x"]}})"), "{}"));
        CHECK(accepts(compiled(R"({"else":{"required":["x"]}})"), "{}"));
        // $defs holds subschemas that apply only where a $ref names one.
        CHECK(accepts(compiled(R"({"$defs":{"A":{"type":"integer"}}})"), R"("a")"));
        // $schema and $id are checked when the schema compiles and assert
        // nothing about an instance; without this line nothing exercises them
        // at evaluation time at all.
        CHECK(accepts(
            compiled(
                R"({"$schema":"https://json-schema.org/draft/2020-12/schema",)"
                R"("$id":"https://x.test/inert.json"})"
            ),
            "1"
        ));
    }

    TEST_CASE("json::Schema resolves $ref only inside the documents it was given")
    {
        constexpr auto k_other = std::string_view{
            R"({"$id":"https://x.test/schema/other.json",)"
            R"("$defs":{"Id":{"type":"string","minLength":2}}})"
        };
        constexpr auto k_root = std::string_view{
            R"({"$id":"https://x.test/schema/root.json",)"
            R"("properties":{"id":{"$ref":"other.json#/$defs/Id"}}})"
        };

        auto const others = std::array{
            Schema::Document{.label = "other", .exactBytes = k_other},
        };
        auto schema = Schema::compile(
            Schema::Document{.label = "root", .exactBytes = k_root},
            others
        );
        REQUIRE(schema.has_value());
        CHECK(accepts(*schema, R"({"id":"ab"})"));
        CHECK(rejects(*schema, R"({"id":"a"})"));
        CHECK(rejects(*schema, R"({"id":1})"));

        // Without the other document in the set, the same reference is refused
        // rather than resolved somewhere else. It names an origin the set does
        // carry, so the repair is to widen the set.
        auto const outside = Schema::compile(
            Schema::Document{.label = "root", .exactBytes = k_root}
        );
        REQUIRE_FALSE(outside.has_value());
        CHECK(std::string{outside.error().message()}.contains(
            "outside the set this schema was compiled from"
        ));

        // A reference to an origin no document in the set publishes is the
        // other refusal, and it is reported apart from the one above because no
        // widening of the set repairs it: this evaluator fetches nothing.
        constexpr auto k_fetching = std::string_view{
            R"({"$id":"https://x.test/schema/root.json",)"
            R"("$ref":"https://elsewhere.test/s.json"})"
        };
        auto const remote = Schema::compile(
            Schema::Document{.label = "remote", .exactBytes = k_fetching},
            others
        );
        REQUIRE_FALSE(remote.has_value());
        CHECK(std::string{remote.error().message()}.contains(
            "a remote document, and this evaluator fetches nothing"
        ));

        // A same-document reference to a name that is not there, an anchor
        // rather than a pointer, and a reference into an array, are all refused.
        auto const missing = Schema::compile(Schema::Document{
            .label      = "missing",
            .exactBytes = R"({"$ref":"#/$defs/Missing"})",
        });
        REQUIRE_FALSE(missing.has_value());
        CHECK(std::string{missing.error().message()}.contains(
            "no target in the document it names"
        ));
        CHECK(compileRefuses(R"({"$defs":{"A":true},"$ref":"#A"})"));
        CHECK(compileRefuses(R"({"$defs":[true],"$ref":"#/$defs/0"})"));
    }

    TEST_CASE("json::Schema applies a recursive $ref and stops on a cycle")
    {
        auto const recursive = compiled(
            R"({"type":"object","properties":{"next":{"$ref":"#"}},)"
            R"("additionalProperties":false})"
        );
        CHECK(accepts(recursive, "{}"));
        CHECK(accepts(recursive, R"({"next":{"next":{}}})"));
        CHECK(rejects(recursive, R"({"other":1})"));

        // A $ref cycle with nothing between the hops cannot be caught when the
        // schema compiles -- each hop resolves -- so it is caught by the
        // evaluation depth limit, and reported as a schema problem rather than
        // as the document failing to match.
        auto const cyclic = compiled(R"({"$defs":{"A":{"$ref":"#/$defs/A"}},"$ref":"#/$defs/A"})");
        auto const value = parse("1");
        REQUIRE(value.has_value());
        auto const outcome = cyclic.validate(*value);
        REQUIRE_FALSE(outcome.has_value());
        CHECK(errorKind(outcome.error()) == ErrorKind::SchemaUnsupported);
    }

    // A branch that failed because the schema is unusable is not a branch that
    // did not match. Reading it as one would let anyOf swallow the cycle above.
    TEST_CASE("json::Schema propagates a schema failure through a combinator")
    {
        auto const schema = compiled(
            R"({"$defs":{"A":{"$ref":"#/$defs/A"}},)"
            R"("anyOf":[{"$ref":"#/$defs/A"},{"type":"integer"}]})"
        );
        auto const value = parse("1");
        REQUIRE(value.has_value());
        auto const outcome = schema.validate(*value);
        REQUIRE_FALSE(outcome.has_value());
        CHECK(errorKind(outcome.error()) == ErrorKind::SchemaUnsupported);
    }

    // MSVC's std::regex asserts ^ and $ at every line boundary whatever
    // syntax_option_type it is handed, so `^[0-9a-f]{4}$` matches
    // "zzzz\n0123" -- a false accept on exactly the patterns this repository
    // uses to pin hashes. Confirmed against V8, which is the engine ECMA-262
    // and therefore JSON Schema describe: /^[0-9a-f]{4}$/.test("zzzz\n0123")
    // is false.
    TEST_CASE("json::Schema anchors a pattern at the ends of the whole string")
    {
        auto const schema = compiled(R"({"type":"string","pattern":"^[0-9a-f]{4}$"})");
        CHECK(accepts(schema, R"("0123")"));
        CHECK(rejects(schema, R"("zzzz\n0123")"));
        CHECK(rejects(schema, R"("0123\nzzzz")"));
        CHECK(rejects(schema, R"("0123\n")"));
        CHECK(rejects(schema, R"("\n0123")"));

        // An unanchored pattern is unaffected by the deviation and keeps
        // searching the whole string.
        auto const unanchored = compiled(R"({"type":"string","pattern":"bc"})");
        CHECK(accepts(unanchored, R"("abcd")"));
        CHECK(rejects(unanchored, R"("abd")"));

        // Anchoring this engine cannot answer for is refused when the schema
        // compiles rather than answered wrongly.
        CHECK(compileRefuses(R"({"pattern":"^abc"})"));
        CHECK(compileRefuses(R"({"pattern":"abc$"})"));
        CHECK(compileRefuses(R"({"pattern":"^a|b$"})"));
        CHECK(compileRefuses(R"({"pattern":"^a$|^b$"})"));

        // ^ inside a character class is negation, not an anchor, and an escaped
        // one is a literal; neither may be mistaken for anchoring.
        CHECK(compiled(R"({"pattern":"[^a]"})").validate(*parse(R"("b")")).has_value());
        CHECK(compileRefuses(R"({"pattern":"^[^a]"})"));
    }

    // The number model is IEEE-754 double because RFC 8785 makes it so. A bound
    // beyond 2^53 shares its double with its neighbours, so it cannot be
    // enforced as written and is refused rather than approximated.
    TEST_CASE("json::Schema refuses a numeric bound it cannot enforce exactly")
    {
        CHECK(compileRefuses(R"({"maximum":18446744073709551615})"));
        CHECK(compileRefuses(R"({"minimum":-9223372036854775808})"));
        CHECK(compileRefuses(R"({"maximum":9223372036854775807})"));
        // 2^53 itself is already shared: it and 2^53+1 are one double, so a
        // schema spelling either is stored as the same number.
        CHECK(compileRefuses(R"({"maximum":9007199254740993})"));
        CHECK(compileRefuses(R"({"maximum":9007199254740992})"));

        CHECK(Schema::compile(
                  Schema::Document{
                      .label      = "the largest bound every literal below it owns",
                      .exactBytes = R"({"maximum":9007199254740991})",
                  }
        )
                  .has_value());
        CHECK(Schema::compile(
                  Schema::Document{
                      .label      = "a fraction is as exact as the instance",
                      .exactBytes = R"({"minimum":0.1})",
                  }
        )
                  .has_value());
    }

    TEST_CASE("json::Schema validates one named subschema of a document")
    {
        auto const schema = compiled(
            R"({"$defs":{"A":{"type":"integer"},"B":{"type":"string"}}})"
        );
        CHECK(schema.hasDefinition("A"));
        CHECK_FALSE(schema.hasDefinition("C"));

        auto const number = parse("1");
        REQUIRE(number.has_value());
        CHECK(schema.validateDefinition("A", *number).has_value());
        CHECK_FALSE(schema.validateDefinition("B", *number).has_value());

        auto const missing = schema.validateDefinition("C", *number);
        REQUIRE_FALSE(missing.has_value());
        CHECK(errorKind(missing.error()) == ErrorKind::SchemaUnsupported);
    }

    // This repository's own schemas, compiled by the evaluator that is meant
    // to apply them. Four require context: trace carries numeric bounds this
    // evaluator cannot represent exactly, while workspace, Fact and Collection
    // Fact name documents that must be present in their closed reference set.
    TEST_CASE("json::Schema compiles this repository's schemas")
    {
        constexpr auto k_knownFile =
            std::string_view{"schema/umbraflow-trace-v2.schema.json"};
        constexpr auto k_outOfRange =
            std::string_view{"umbraflow-trace-v2.schema.json"};
        constexpr auto k_crossDocument =
            std::string_view{"umbraflow-annotation-workspace-v2.schema.json"};
        constexpr auto k_fact =
            std::string_view{"umbraflow-fact-v1.schema.json"};
        constexpr auto k_collectionFact =
            std::string_view{"umbraflow-collection-fact-v1.schema.json"};

        auto const root = repositoryRoot(k_knownFile);
        REQUIRE_FALSE(root.empty());

        auto sources = std::vector<std::pair<std::string, std::string>>{};
        for (auto const& entry : std::filesystem::directory_iterator{root / "schema"})
        {
            if (entry.path().extension() != ".json")
            {
                continue;
            }
            auto stream = std::ifstream{entry.path(), std::ios::binary};
            auto buffer = std::ostringstream{};
            buffer << stream.rdbuf();
            sources.emplace_back(entry.path().filename().string(), buffer.str());
        }
        REQUIRE(sources.size() == 11U);

        auto compiledCount = std::size_t{0};
        for (auto const& source : sources)
        {
            CAPTURE(source.first);
            auto schema = Schema::compile(Schema::Document{
                .label      = source.first,
                .exactBytes = source.second,
            });

            if (
                source.first == k_outOfRange
                || source.first == k_crossDocument
                || source.first == k_fact
                || source.first == k_collectionFact
            )
            {
                REQUIRE_FALSE(schema.has_value());
                CHECK(errorKind(schema.error()) == ErrorKind::SchemaUnsupported);
                auto const message = std::string{schema.error().message()};
                auto const expected = source.first == k_outOfRange
                    ? std::string_view{"2^53"}
                    : std::string_view{"outside the set this schema was compiled from"};
                CHECK(message.contains(expected));
                continue;
            }

            if (!schema.has_value())
            {
                FAIL_CHECK(
                    source.first << ": " << std::string{schema.error().message()}
                );
                continue;
            }
            ++compiledCount;
        }
        CHECK(compiledCount == 7U);

        // The cross-document schema compiles once its two siblings are in the
        // set, which is the whole of what the closed world buys.
        auto referenced = std::vector<Schema::Document>{};
        auto primary    = std::string_view{};
        for (auto const& source : sources)
        {
            if (source.first == k_crossDocument)
            {
                primary = source.second;
                continue;
            }
            if (source.first == "umbraflow-runtime-v2.schema.json"
                || source.first == "umbraflow-runtime-artifact-v1.schema.json")
            {
                referenced.emplace_back(Schema::Document{
                    .label      = source.first,
                    .exactBytes = source.second,
                });
            }
        }
        REQUIRE(referenced.size() == 2U);

        auto const workspace = Schema::compile(
            Schema::Document{.label = k_crossDocument, .exactBytes = primary},
            referenced
        );
        auto const why = workspace.has_value()
            ? std::string{}
            : std::string{workspace.error().message()};
        INFO(why);
        CHECK(workspace.has_value());

        auto factReferences  = std::vector<Schema::Document>{};
        auto factBytes       = std::string_view{};
        auto collectionBytes = std::string_view{};
        for (auto const& source : sources)
        {
            if (source.first == k_fact)
            {
                factBytes = source.second;
            }
            else if (source.first == k_collectionFact)
            {
                collectionBytes = source.second;
            }
            else if (source.first == "umbraflow-fact-provenance-v1.schema.json")
            {
                factReferences.emplace_back(Schema::Document{
                    .label      = source.first,
                    .exactBytes = source.second,
                });
            }
        }
        REQUIRE(factReferences.size() == 1U);

        // The published Fact family, closed one document at a time: Fact needs
        // provenance, and Collection Fact needs both.
        auto const fact = Schema::compile(
            Schema::Document{.label = k_fact, .exactBytes = factBytes},
            factReferences
        );
        auto const whyFact = fact.has_value()
            ? std::string{}
            : std::string{fact.error().message()};
        INFO(whyFact);
        REQUIRE(fact.has_value());

        factReferences.emplace_back(Schema::Document{
            .label      = k_fact,
            .exactBytes = factBytes,
        });
        auto const collection = Schema::compile(
            Schema::Document{
                .label      = k_collectionFact,
                .exactBytes = collectionBytes,
            },
            factReferences
        );
        auto const whyCollection = collection.has_value()
            ? std::string{}
            : std::string{collection.error().message()};
        INFO(whyCollection);
        REQUIRE(collection.has_value());
    }
}
