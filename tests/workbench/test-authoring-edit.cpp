#include "../annotation/test-helpers.hpp"
#include "authoring-fixture.hpp"
#include "colour-key-fixture.hpp"

#include <authoring-edit.hpp>

#include <annotation/authoring-compiler.hpp>
#include <annotation/authoring-document.hpp>
#include <annotation/content-hash.hpp>
#include <annotation/resource.hpp>

#include <core/types/integer.hpp>

#include <image/png.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace uf::workbench
{
    namespace
    {
        constexpr auto k_sourceId     = "00000000-0000-0000-0000-000000000201";
        constexpr auto k_anchorId     = "00000000-0000-0000-0000-000000000001";
        constexpr auto k_actionId     = "00000000-0000-0000-0000-000000000002";
        constexpr auto k_awayId       = "00000000-0000-0000-0000-000000000003";
        constexpr auto k_sharedId     = "00000000-0000-0000-0000-000000000004";
        constexpr auto k_pageId       = "00000000-0000-0000-0000-000000000101";
        constexpr auto k_secondPageId = "00000000-0000-0000-0000-000000000102";
        constexpr auto k_regressionId = "00000000-0000-0000-0000-000000000301";

        [[nodiscard]]
        auto document() -> annotation::AuthoringDocument
        {
            auto const fingerprint = annotation::test::fingerprint(8, 8, 96, 96);
            auto const sourceId    = annotation::test::sourceId(k_sourceId);
            auto const anchorId    = annotation::test::elementId(k_anchorId);
            auto const actionId    = annotation::test::elementId(k_actionId);
            auto const pageId      = annotation::test::pageId(k_pageId);
            auto const sourceHash = annotation::sha256(
                std::span<std::byte const>{}
            );
            REQUIRE(sourceHash.has_value());
            auto source = annotation::AuthoringSource::create(
                annotation::AuthoringSourceSpec{
                    .id          = sourceId,
                    .contentHash = *sourceHash,
                    .fingerprint = fingerprint,
                    .provenance  = annotation::ImportedSourceProvenance{},
                }
            );
            REQUIRE(source.has_value());

            auto const click = annotation::TemplateOffset::create(1, 1, 2, 2);
            REQUIRE(click.has_value());
            auto constexpr classification = (
                annotation::RegressionClassification::Positive
            );
            auto created = annotation::AuthoringDocument::create(
                annotation::test::projectId(),
                fingerprint,
                {*source},
                {
                    test::markElement(
                        fingerprint,
                        anchorId,
                        "home_marker",
                        sourceId,
                        annotation::test::pixelRect(0, 0, 2, 2),
                        annotation::test::pixelRect(0, 0, 4, 4)
                    ),
                    test::clickableElement(
                        fingerprint,
                        actionId,
                        "daily_button",
                        sourceId,
                        annotation::test::pixelRect(4, 4, 2, 2),
                        annotation::test::pixelRect(3, 3, 4, 4),
                        *click
                    ),
                },
                {annotation::test::page(pageId, "home")},
                {
                    annotation::test::reference(
                        pageId,
                        anchorId,
                        annotation::test::identifiesAs()
                    ),
                    annotation::test::reference(
                        pageId,
                        actionId,
                        annotation::test::interacts()
                    ),
                },
                {
                    annotation::RegressionCase{
                        annotation::RegressionSpec{
                            .id = annotation::test::regressionId(
                                k_regressionId
                            ),
                            .sourceId       = sourceId,
                            .classification = classification,
                            .expectation = annotation::ResolvedRegression{
                                .pageId = pageId,
                            },
                        }
                    },
                }
            );
            REQUIRE(created.has_value());
            return *std::move(created);
        }

        // Covers the draft alternatives that document() leaves at their zero
        // value: WGC provenance, a non-Positive classification, an unresolved
        // expectation, and a non-empty forbidden set. Without these a draft
        // round-trip that silently dropped the field would still compare equal.
        [[nodiscard]]
        auto appearanceDocument() -> annotation::AuthoringDocument
        {
            auto const fingerprint = annotation::test::fingerprint(8, 8, 96, 96);
            auto const sourceId    = annotation::test::sourceId(k_sourceId);
            auto const anchorId    = annotation::test::elementId(k_anchorId);
            auto const awayId      = annotation::test::elementId(k_awayId);
            auto const pageId      = annotation::test::pageId(k_pageId);
            auto const sourceHash = annotation::sha256(
                std::span<std::byte const>{}
            );
            REQUIRE(sourceHash.has_value());
            auto source = annotation::AuthoringSource::create(
                annotation::AuthoringSourceSpec{
                    .id          = sourceId,
                    .contentHash = *sourceHash,
                    .fingerprint = fingerprint,
                    .provenance  = annotation::WgcSourceProvenance{
                        .targetGeneration = TargetGeneration::fromValue(7),
                        .capturedAt       = "2026-07-23T09:15:00+09:00",
                    },
                }
            );
            REQUIRE(source.has_value());

            auto constexpr classification = (
                annotation::RegressionClassification::Negative
            );
            auto created = annotation::AuthoringDocument::create(
                annotation::test::projectId(),
                fingerprint,
                {*source},
                {
                    test::markElement(
                        fingerprint,
                        anchorId,
                        "home_marker",
                        sourceId,
                        annotation::test::pixelRect(0, 0, 2, 2),
                        annotation::test::pixelRect(0, 0, 4, 4)
                    ),
                    test::markElement(
                        fingerprint,
                        awayId,
                        "away_marker",
                        sourceId,
                        annotation::test::pixelRect(4, 4, 2, 2),
                        annotation::test::pixelRect(3, 3, 4, 4)
                    ),
                },
                {annotation::test::page(pageId, "home")},
                {
                    annotation::test::reference(
                        pageId,
                        anchorId,
                        annotation::test::identifiesAs()
                    ),
                    annotation::test::reference(
                        pageId,
                        awayId,
                        annotation::test::identifiesAs(
                            annotation::SignatureRole::Forbidden
                        )
                    ),
                },
                {
                    annotation::RegressionCase{
                        annotation::RegressionSpec{
                            .id = annotation::test::regressionId(
                                k_regressionId
                            ),
                            .sourceId       = sourceId,
                            .classification = classification,
                            .expectation    = annotation::UnknownRegression{},
                        }
                    },
                }
            );
            REQUIRE(created.has_value());
            return *std::move(created);
        }

        // Two pages whose anchors differ, so a conversion that must pick a page
        // to authorize can be observed picking the element's own page rather
        // than the first one. "battle" survives losing its required anchor
        // because it still forbids one.
        [[nodiscard]]
        auto twoPageDocument() -> annotation::AuthoringDocument
        {
            auto const fingerprint = annotation::test::fingerprint(8, 8, 96, 96);
            auto const sourceId    = annotation::test::sourceId(k_sourceId);
            auto const anchorId    = annotation::test::elementId(k_anchorId);
            auto const awayId      = annotation::test::elementId(k_awayId);
            auto const homeId      = annotation::test::pageId(k_pageId);
            auto const battleId    = annotation::test::pageId(k_secondPageId);
            auto const sourceHash  = annotation::sha256(
                std::span<std::byte const>{}
            );
            REQUIRE(sourceHash.has_value());
            auto source = annotation::AuthoringSource::create(
                annotation::AuthoringSourceSpec{
                    .id          = sourceId,
                    .contentHash = *sourceHash,
                    .fingerprint = fingerprint,
                    .provenance  = annotation::ImportedSourceProvenance{},
                }
            );
            REQUIRE(source.has_value());

            auto created = annotation::AuthoringDocument::create(
                annotation::test::projectId(),
                fingerprint,
                {*source},
                {
                    test::markElement(
                        fingerprint,
                        anchorId,
                        "home_marker",
                        sourceId,
                        annotation::test::pixelRect(0, 0, 2, 2),
                        annotation::test::pixelRect(0, 0, 4, 4)
                    ),
                    test::markElement(
                        fingerprint,
                        awayId,
                        "battle_marker",
                        sourceId,
                        annotation::test::pixelRect(4, 4, 2, 2),
                        annotation::test::pixelRect(3, 3, 4, 4)
                    ),
                },
                {
                    annotation::test::page(homeId, "home"),
                    annotation::test::page(battleId, "battle"),
                },
                {
                    annotation::test::reference(
                        homeId,
                        anchorId,
                        annotation::test::identifiesAs()
                    ),
                    annotation::test::reference(
                        battleId,
                        awayId,
                        annotation::test::identifiesAs()
                    ),
                    annotation::test::reference(
                        battleId,
                        anchorId,
                        annotation::test::identifiesAs(
                            annotation::SignatureRole::Forbidden
                        ),
                        annotation::Holding::Referenced
                    ),
                },
                {}
            );
            REQUIRE(created.has_value());
            return *std::move(created);
        }

        [[nodiscard]]
        auto elementName(
            AuthoringEditHistory const& history,
            std::size_t index
        ) -> std::string
        {
            return history.draft().elements.at(index).name;
        }

        // Reads an element out of a draft by identity rather than by position,
        // so a test states which element it means instead of depending on the
        // catalog's ordering.
        [[nodiscard]]
        auto elementIn(
            AuthoringDraft const& draft,
            annotation::ElementId id
        ) -> EditableElement
        {
            auto const found = std::ranges::find(
                draft.elements,
                id,
                &EditableElement::id
            );
            REQUIRE(found != draft.elements.end());
            return *found;
        }

        // Which way one page's reference to one element points, when it points
        // at all. The signature is derived from the references now, so this is
        // how a test reads back what used to be two vectors on the page.
        [[nodiscard]]
        auto signatureRoleIn(
            AuthoringDraft const& draft,
            annotation::PageId pageId,
            annotation::ElementId elementId
        ) -> std::optional<annotation::SignatureRole>
        {
            auto const found = std::ranges::find_if(
                draft.references,
                [pageId, elementId](EditableReference const& reference)
                {
                    return reference.pageId == pageId
                        && reference.elementId == elementId;
                }
            );
            if (
                found == draft.references.end()
                || !found->exercised.identify.has_value()
            )
            {
                return std::nullopt;
            }
            return found->exercised.identify->role;
        }
    }

    TEST_CASE("a fresh name avoids element and page names alike")
    {
        // The catalog compares a page name against element names too, so a
        // fresh name checked against one kind still collides.
        auto draft = makeAuthoringDraft(document());
        draft.pages.at(0).name       = "thing_1";
        draft.elements.at(0).name = "thing_2";

        CHECK(freshAuthoringName(draft, "thing") == "thing_3");
        CHECK(freshAuthoringName(draft, "other") == "other_1");
    }

    TEST_CASE("a page created from a screen anchors, requires, and expects it")
    {
        // None of the three can be authored on its own: an empty signature is
        // rejected, and an anchor joins a page only through a signature.
        auto const sourceId  = annotation::test::sourceId(k_sourceId);
        auto const pageId    = annotation::test::pageId(k_secondPageId);
        auto const anchorId  = annotation::test::elementId(k_awayId);
        auto const rect      = annotation::test::pixelRect(0, 0, 2, 2);
        auto const roi       = annotation::test::pixelRect(0, 0, 4, 4);

        auto const created = createPageFromSource(
            makeAuthoringDraft(document()),
            NewPageSpec{
                .pageId   = pageId,
                .anchorId = anchorId,
                .regressionId = annotation::test::regressionId(
                    "00000000-0000-0000-0000-000000000302"
                ),
                .sourceId     = sourceId,
                .templateRect = rect,
                .searchRoi    = roi,
                .similarityBasisPoints = 9'000U,
            }
        );
        REQUIRE(created.has_value());

        auto const anchor = elementIn(created->draft, anchorId);
        CHECK(anchor.capabilities.identify.has_value());
        REQUIRE(anchor.appearances.size() == 1U);
        CHECK(anchor.appearances.front().sourceId == sourceId);

        // The page's signature is derived from the reference, which is why the
        // page and the reference cannot be authored one at a time.
        CHECK(
            signatureRoleIn(created->draft, pageId, anchorId)
            == annotation::SignatureRole::Required
        );

        // The regression case is the only statement that this screen is that
        // page; the anchor's source is a different claim.
        auto const expectation = annotation::RegressionExpectation{
            annotation::ResolvedRegression{.pageId = pageId},
        };
        auto const recorded = std::ranges::any_of(
            created->draft.regressions,
            [&](EditableRegression const& regression)
            {
                return regression.sourceId == sourceId
                    && regression.expectation == expectation;
            }
        );
        CHECK(recorded);

        // The whole edit has to be a document the catalog accepts.
        CHECK(buildAuthoringDocument(created->draft).has_value());
    }

    TEST_CASE("creating a page from a screen outside the draft is refused")
    {
        auto const created = createPageFromSource(
            makeAuthoringDraft(document()),
            NewPageSpec{
                .pageId   = annotation::test::pageId(k_secondPageId),
                .anchorId = annotation::test::elementId(k_awayId),
                .regressionId = annotation::test::regressionId(
                    "00000000-0000-0000-0000-000000000302"
                ),
                .sourceId = annotation::test::sourceId(
                    "00000000-0000-0000-0000-000000000202"
                ),
                .templateRect = annotation::test::pixelRect(0, 0, 2, 2),
                .searchRoi    = annotation::test::pixelRect(0, 0, 4, 4),
                .similarityBasisPoints = 9'000U,
            }
        );
        CHECK_FALSE(created.has_value());
    }

    TEST_CASE("a page member is typed and linked by the group it joins")
    {
        auto const sourceId = annotation::test::sourceId(k_sourceId);
        auto const pageId   = annotation::test::pageId(k_pageId);
        auto const newId    = annotation::test::elementId(k_awayId);

        auto const spec = [&](PageMemberKind kind)
        {
            return PageMemberSpec{
                .elementId    = newId,
                .pageId       = pageId,
                .sourceId     = sourceId,
                .templateRect = annotation::test::pixelRect(0, 0, 2, 2),
                .searchRoi    = annotation::test::pixelRect(0, 0, 4, 4),
                .similarityBasisPoints = 9'000U,
                .kind                  = kind,
            };
        };

        SUBCASE("a mark enters the page signature and authorizes nothing")
        {
            auto const added = addPageMember(
                makeAuthoringDraft(document()),
                spec(PageMemberKind::Anchor)
            );
            REQUIRE(added.has_value());

            auto const element = elementIn(added->draft, newId);
            CHECK(element.capabilities.identify.has_value());
            CHECK(
                signatureRoleIn(added->draft, pageId, newId)
                == annotation::SignatureRole::Required
            );
            CHECK(buildAuthoringDocument(added->draft).has_value());
        }

        SUBCASE("an action target is authorized and holds no signature role")
        {
            auto const added = addPageMember(
                makeAuthoringDraft(document()),
                spec(PageMemberKind::ActionTarget)
            );
            REQUIRE(added.has_value());

            auto const element = elementIn(added->draft, newId);
            CHECK(element.capabilities.interact.has_value());
            CHECK(
                std::ranges::contains(pagesReferencing(added->draft, newId), pageId)
            );
            CHECK_FALSE(signatureRoleIn(added->draft, pageId, newId).has_value());
            CHECK(buildAuthoringDocument(added->draft).has_value());
        }

        SUBCASE("an info region is placed and holds no signature role")
        {
            auto const added = addPageMember(
                makeAuthoringDraft(document()),
                spec(PageMemberKind::InfoRegion)
            );
            REQUIRE(added.has_value());

            auto const element = elementIn(added->draft, newId);
            CHECK(element.capabilities.read.has_value());
            CHECK(
                std::ranges::contains(pagesReferencing(added->draft, newId), pageId)
            );
            CHECK_FALSE(signatureRoleIn(added->draft, pageId, newId).has_value());
            CHECK(buildAuthoringDocument(added->draft).has_value());
        }
    }

    TEST_CASE("duplicating an element mints a distinct, valid copy")
    {
        auto const actionId = annotation::test::elementId(k_actionId);
        auto const newId    = annotation::test::elementId(k_awayId);
        auto const pageId   = annotation::test::pageId(k_pageId);

        auto const original = elementIn(makeAuthoringDraft(document()), actionId);

        auto const duplicated = duplicateElement(
            makeAuthoringDraft(document()),
            DuplicateElementSpec{
                .sourceElementId = actionId,
                .newElementId    = newId,
            }
        );
        REQUIRE(duplicated.has_value());

        // A genuinely new element: a fresh id and a distinct, unique name.
        auto const copy = elementIn(duplicated->draft, newId);
        CHECK(copy.id == newId);
        CHECK(copy.name != original.name);
        CHECK(copy.name == duplicated->name);
        CHECK(
            copy.capabilities.interact.has_value()
            == original.capabilities.interact.has_value()
        );
        REQUIRE(copy.appearances.size() == original.appearances.size());
        REQUIRE(copy.appearances.size() == 1U);
        CHECK(
            copy.appearances.front().templateRect
            == original.appearances.front().templateRect
        );
        CHECK(
            copy.appearances.front().similarityBasisPoints
            == original.appearances.front().similarityBasisPoints
        );
        REQUIRE(copy.capabilities.interact.has_value());
        REQUIRE(original.capabilities.interact.has_value());
        auto const copyClick     = copy.capabilities.interact->clickOffset;
        auto const originalClick = original.capabilities.interact->clickOffset;
        REQUIRE(copyClick.has_value());
        REQUIRE(originalClick.has_value());
        CHECK(copyClick->x == originalClick->x);
        CHECK(copyClick->y == originalClick->y);

        // The copy inherits the original's references, so the clickable element
        // stays valid (something clickable must be reachable somewhere), and the
        // original is untouched.
        CHECK(
            std::ranges::contains(pagesReferencing(duplicated->draft, newId), pageId)
        );
        CHECK(elementIn(duplicated->draft, actionId).name == original.name);
        CHECK(buildAuthoringDocument(duplicated->draft).has_value());
    }

    TEST_CASE("an applied duplicate is removed by one undo")
    {
        auto const actionId = annotation::test::elementId(k_actionId);
        auto const newId    = annotation::test::elementId(k_awayId);

        auto history = AuthoringEditHistory{document()};
        auto const before = history.document().catalog().elements().size();

        auto const duplicated = duplicateElement(
            history.draft(),
            DuplicateElementSpec{
                .sourceElementId = actionId,
                .newElementId    = newId,
            }
        );
        REQUIRE(duplicated.has_value());

        auto const applied = history.apply(duplicated->draft);
        REQUIRE(applied.has_value());
        CHECK(*applied);
        CHECK(history.document().catalog().elements().size() == before + 1U);

        REQUIRE(history.undo());
        CHECK(history.document().catalog().elements().size() == before);
    }

    TEST_CASE("duplicating an element outside the draft is refused")
    {
        auto const duplicated = duplicateElement(
            makeAuthoringDraft(document()),
            DuplicateElementSpec{
                .sourceElementId = annotation::test::elementId(k_sharedId),
                .newElementId    = annotation::test::elementId(k_awayId),
            }
        );
        CHECK_FALSE(duplicated.has_value());
    }

    TEST_CASE("a second page borrows the same element with a range of its own")
    {
        auto const actionId = annotation::test::elementId(k_actionId);
        auto const homeId   = annotation::test::pageId(k_pageId);
        auto const pageId   = annotation::test::pageId(k_secondPageId);
        auto const roi      = annotation::test::pixelRect(2, 2, 6, 6);

        auto draft = makeAuthoringDraft(twoPageDocument());
        // twoPageDocument has nothing clickable, so give it the element being
        // borrowed, owned by the home page.
        draft.elements.emplace_back(
            EditableElement{
                .id   = actionId,
                .name = "back",
                .capabilities = EditableCapabilities{
                    .interact = EditableInteract{},
                },
                .searchRoi = annotation::test::pixelRect(3, 3, 4, 4),
                .appearances  = {
                    EditableAppearance{
                        .name         = "default",
                        .sourceId     = annotation::test::sourceId(k_sourceId),
                        .templateRect = annotation::test::pixelRect(4, 4, 2, 2),
                        .similarityBasisPoints = 9'000U,
                    },
                },
            }
        );
        draft.references.emplace_back(
            EditableReference{
                .pageId    = homeId,
                .elementId = actionId,
                .holding   = annotation::Holding::Owned,
                .exercised = EditableExercised{
                    .interact = annotation::ExercisedInteract{},
                },
            }
        );

        auto const referenced = referenceElementOnPage(
            std::move(draft),
            ReferenceElementSpec{
                .elementId = actionId,
                .pageId    = pageId,
                .searchRoi = roi,
            }
        );
        REQUIRE(referenced.has_value());

        // No copy is minted: the same element gains a second reference carrying
        // its own search region, and that reference is a borrow rather than a
        // second claim of ownership -- which the old flag could not say.
        CHECK(pagesReferencing(referenced->draft, actionId).size() == 2U);
        auto const borrowed = std::ranges::find_if(
            referenced->draft.references,
            [&](EditableReference const& reference)
            {
                return reference.elementId == actionId
                    && reference.pageId == pageId;
            }
        );
        REQUIRE(borrowed != referenced->draft.references.end());
        CHECK(borrowed->searchRoi == roi);
        CHECK(borrowed->holding == annotation::Holding::Referenced);
        CHECK(buildAuthoringDocument(referenced->draft).has_value());

        auto removed = removeReferenceFromPage(
            std::move(referenced->draft),
            actionId,
            pageId
        );
        REQUIRE(removed.has_value());
        auto const remaining = pagesReferencing(*removed, actionId);
        REQUIRE(remaining.size() == 1U);
        CHECK(remaining.front() == homeId);
        CHECK(buildAuthoringDocument(*removed).has_value());
    }

    TEST_CASE("a second page may not claim to own an element as well")
    {
        // Owned is the author saying these pixels are one page's own, and two
        // pages claiming it is the contradiction the reuse flag could hold
        // without anything noticing. Building the draft is where it is caught.
        auto const actionId = annotation::test::elementId(k_actionId);

        auto draft = makeAuthoringDraft(document());
        draft.pages.emplace_back(
            EditablePage{
                .id   = annotation::test::pageId(k_secondPageId),
                .name = "battle",
            }
        );
        draft.references.emplace_back(
            EditableReference{
                .pageId    = annotation::test::pageId(k_secondPageId),
                .elementId = annotation::test::elementId(k_anchorId),
                .holding   = annotation::Holding::Referenced,
                .exercised = EditableExercised{
                    .identify = annotation::ExercisedIdentify{
                        .role = annotation::SignatureRole::Forbidden,
                    },
                },
            }
        );
        draft.references.emplace_back(
            EditableReference{
                .pageId    = annotation::test::pageId(k_secondPageId),
                .elementId = actionId,
                .holding   = annotation::Holding::Owned,
                .exercised = EditableExercised{
                    .interact = annotation::ExercisedInteract{},
                },
            }
        );
        CHECK_FALSE(buildAuthoringDocument(draft).has_value());

        // The control: the same draft with the second page borrowing instead of
        // owning is accepted, so the refusal is about the second owner and not
        // about the second reference.
        draft.references.back().holding = annotation::Holding::Referenced;
        CHECK(buildAuthoringDocument(draft).has_value());
    }

    TEST_CASE("reference withdrawal preserves page and interactive closure rules")
    {
        auto const actionId = annotation::test::elementId(k_actionId);
        auto const pageId   = annotation::test::pageId(k_pageId);

        auto const lastInteract = removeReferenceFromPage(
            makeAuthoringDraft(document()),
            actionId,
            pageId
        );
        CHECK_FALSE(lastInteract.has_value());

        auto const onlyMark = removeReferenceFromPage(
            makeAuthoringDraft(document()),
            annotation::test::elementId(k_anchorId),
            pageId
        );
        CHECK_FALSE(onlyMark.has_value());
    }

    TEST_CASE("referencing an element on a page that already has it is refused")
    {
        auto const actionId = annotation::test::elementId(k_actionId);
        auto const pageId   = annotation::test::pageId(k_pageId);

        auto const referenced = referenceElementOnPage(
            makeAuthoringDraft(document()),
            ReferenceElementSpec{
                .elementId = actionId,
                .pageId    = pageId,
                .searchRoi = annotation::test::pixelRect(0, 0, 8, 8),
            }
        );
        CHECK_FALSE(referenced.has_value());
    }

    TEST_CASE("referencing an element that only identifies is refused")
    {
        // Pixels that can only be evidence join a page through its signature,
        // which is a different verb.
        auto const referenced = referenceElementOnPage(
            makeAuthoringDraft(document()),
            ReferenceElementSpec{
                .elementId = annotation::test::elementId(k_anchorId),
                .pageId    = annotation::test::pageId(k_pageId),
                .searchRoi = annotation::test::pixelRect(0, 0, 8, 8),
            }
        );
        CHECK_FALSE(referenced.has_value());
    }

    TEST_CASE("one mark required by one page and forbidden by another stays one element")
    {
        // The case the capability model exists for. Before a page could
        // reference an existing element for identify, "page A requires this
        // mark, page B forbids the same pixels" had exactly one route: draw a
        // second rectangle over the first. Two ids, two templates, two searches
        // a cycle -- the duplication this model deletes.
        auto const markId   = annotation::test::elementId(k_anchorId);
        auto const homeId   = annotation::test::pageId(k_pageId);
        auto const battleId = annotation::test::pageId(k_secondPageId);
        auto const onBattle = [markId, battleId](
                                  EditableReference const& reference
                              )
        {
            return reference.pageId == battleId && reference.elementId == markId;
        };

        // twoPageDocument already holds the relationship, so withdraw it and
        // let the function under test be what puts it back.
        auto draft = makeAuthoringDraft(twoPageDocument());
        std::erase_if(draft.references, onBattle);
        auto const elements = draft.elements.size();

        auto const referenced = referenceElementOnPage(
            std::move(draft),
            ReferenceElementSpec{
                .elementId = markId,
                .pageId    = battleId,
                .exercised = EditableExercised{
                    .identify = annotation::ExercisedIdentify{
                        .role = annotation::SignatureRole::Forbidden,
                    },
                },
            }
        );
        REQUIRE(referenced.has_value());

        // The assertion the whole verb is for: no second element over the same
        // pixels. One patch, one id, two pages pointing opposite ways at it.
        CHECK(referenced->draft.elements.size() == elements);
        CHECK(pagesReferencing(referenced->draft, markId).size() == 2U);

        auto const forbidding = std::ranges::find_if(
            referenced->draft.references,
            onBattle
        );
        REQUIRE(forbidding != referenced->draft.references.end());
        CHECK(forbidding->holding == annotation::Holding::Referenced);
        REQUIRE(forbidding->exercised.identify.has_value());
        CHECK(
            forbidding->exercised.identify->role
            == annotation::SignatureRole::Forbidden
        );
        // The anchor pass reads the element's own region, so the reference
        // refines none -- the one combination the catalog refuses outright.
        CHECK_FALSE(forbidding->searchRoi.has_value());

        auto const built = buildAuthoringDocument(referenced->draft);
        REQUIRE(built.has_value());
        CHECK(built->elements().size() == elements);

        auto const* p_home = built->catalog().findPage(homeId);
        REQUIRE(p_home != nullptr);
        CHECK(std::ranges::contains(p_home->required(), markId));
        auto const* p_battle = built->catalog().findPage(battleId);
        REQUIRE(p_battle != nullptr);
        CHECK(std::ranges::contains(p_battle->forbidden(), markId));

        // Asking twice is refused by name rather than writing a second
        // reference the catalog would then reject as a duplicate.
        auto const again = referenceElementOnPage(
            referenced->draft,
            ReferenceElementSpec{
                .elementId = markId,
                .pageId    = battleId,
                .exercised = EditableExercised{
                    .identify = annotation::ExercisedIdentify{},
                },
            }
        );
        CHECK_FALSE(again.has_value());
    }

    TEST_CASE("one element, two pages, one of them also identified by it")
    {
        // Section 2.4 of the capability plan, which the editing layer could
        // state in neither of its two halves before: one verb minted interact
        // and read and refused identify, the other minted identify alone and
        // refused a page that already referenced the element.
        //
        //   Page "home"   -> { interact }
        //   Page "battle" -> { interact, identify }
        //
        // The element has to declare both, because a reference may only
        // exercise what the element declares -- the plan's own block omits
        // identify from the element, and no page could exercise it then.
        auto const backId   = annotation::test::elementId(k_actionId);
        auto const homeId   = annotation::test::pageId(k_pageId);
        auto const battleId = annotation::test::pageId(k_secondPageId);

        auto draft = makeAuthoringDraft(twoPageDocument());
        draft.elements.emplace_back(
            EditableElement{
                .id   = backId,
                .name = "back",
                .capabilities = EditableCapabilities{
                    .identify = annotation::Identify{},
                    .interact = EditableInteract{},
                },
                .searchRoi = annotation::test::pixelRect(3, 3, 4, 4),
                .appearances  = {
                    EditableAppearance{
                        .name         = "on_dark",
                        .sourceId     = annotation::test::sourceId(k_sourceId),
                        .templateRect = annotation::test::pixelRect(4, 4, 2, 2),
                        .similarityBasisPoints = 9'000U,
                    },
                },
            }
        );
        draft.references.emplace_back(
            EditableReference{
                .pageId    = homeId,
                .elementId = backId,
                .holding   = annotation::Holding::Owned,
                .exercised = EditableExercised{
                    .interact = annotation::ExercisedInteract{},
                },
            }
        );
        auto const elements = draft.elements.size();

        auto const referenced = referenceElementOnPage(
            std::move(draft),
            ReferenceElementSpec{
                .elementId = backId,
                .pageId    = battleId,
                .exercised = EditableExercised{
                    .identify = annotation::ExercisedIdentify{
                        .role = annotation::SignatureRole::Required,
                    },
                    .interact = annotation::ExercisedInteract{},
                },
            }
        );
        REQUIRE(referenced.has_value());

        auto const borrowed = std::ranges::find_if(
            referenced->draft.references,
            [backId, battleId](EditableReference const& reference)
            {
                return reference.pageId == battleId
                    && reference.elementId == backId;
            }
        );
        REQUIRE(borrowed != referenced->draft.references.end());

        // One row carrying both uses. Two rows over one element are not a
        // second way to say this: the catalog refuses a page that references
        // the same element twice.
        CHECK(borrowed->exercised.identify.has_value());
        CHECK(borrowed->exercised.interact.has_value());
        CHECK(borrowed->holding == annotation::Holding::Referenced);
        CHECK_FALSE(borrowed->searchRoi.has_value());

        auto const built = buildAuthoringDocument(referenced->draft);
        REQUIRE(built.has_value());

        // Same pixels, same id, two uses. The signature derives from the
        // reference that exercises identify, so battle is identified by the
        // back button and home -- which only clicks it -- is not.
        CHECK(built->elements().size() == elements);
        auto const* p_battle = built->catalog().findPage(battleId);
        REQUIRE(p_battle != nullptr);
        CHECK(std::ranges::contains(p_battle->required(), backId));
        auto const* p_home = built->catalog().findPage(homeId);
        REQUIRE(p_home != nullptr);
        CHECK_FALSE(std::ranges::contains(p_home->required(), backId));
        CHECK_FALSE(std::ranges::contains(p_home->forbidden(), backId));
    }

    TEST_CASE("a reference exercises what was asked for and inherits what was not")
    {
        // Two halves of one rule. Asking for identify on an element that also
        // declares interact grants no click: exercising interact IS the
        // authorisation, and a page taking up a mark as evidence did not ask
        // for it. Asking for nothing inherits the element's placement uses --
        // and its search region, which is the default a caller gets by not
        // naming one rather than a case they opt into.
        auto const backId   = annotation::test::elementId(k_actionId);
        auto const homeId   = annotation::test::pageId(k_pageId);
        auto const battleId = annotation::test::pageId(k_secondPageId);
        auto const onBattle = [backId, battleId](
                                  EditableReference const& reference
                              )
        {
            return reference.pageId == battleId
                && reference.elementId == backId;
        };

        auto draft = makeAuthoringDraft(twoPageDocument());
        draft.elements.emplace_back(
            EditableElement{
                .id   = backId,
                .name = "back",
                .capabilities = EditableCapabilities{
                    .identify = annotation::Identify{},
                    .interact = EditableInteract{},
                },
                .searchRoi = annotation::test::pixelRect(3, 3, 4, 4),
                .appearances  = {
                    EditableAppearance{
                        .name         = "on_dark",
                        .sourceId     = annotation::test::sourceId(k_sourceId),
                        .templateRect = annotation::test::pixelRect(4, 4, 2, 2),
                        .similarityBasisPoints = 9'000U,
                    },
                },
            }
        );
        draft.references.emplace_back(
            EditableReference{
                .pageId    = homeId,
                .elementId = backId,
                .holding   = annotation::Holding::Owned,
                .exercised = EditableExercised{
                    .interact = annotation::ExercisedInteract{},
                },
            }
        );

        auto const evidenceOnly = referenceElementOnPage(
            draft,
            ReferenceElementSpec{
                .elementId = backId,
                .pageId    = battleId,
                .exercised = EditableExercised{
                    .identify = annotation::ExercisedIdentify{},
                },
            }
        );
        REQUIRE(evidenceOnly.has_value());
        auto const signing = std::ranges::find_if(
            evidenceOnly->draft.references,
            onBattle
        );
        REQUIRE(signing != evidenceOnly->draft.references.end());
        CHECK_FALSE(signing->exercised.interact.has_value());

        auto const placed = referenceElementOnPage(
            std::move(draft),
            ReferenceElementSpec{
                .elementId = backId,
                .pageId    = battleId,
            }
        );
        REQUIRE(placed.has_value());
        auto const inheriting = std::ranges::find_if(
            placed->draft.references,
            onBattle
        );
        REQUIRE(inheriting != placed->draft.references.end());
        CHECK(inheriting->exercised.interact.has_value());
        CHECK_FALSE(inheriting->exercised.identify.has_value());
        CHECK_FALSE(inheriting->searchRoi.has_value());
    }

    TEST_CASE("a reference may not exercise identify and refine a region at once")
    {
        // The anchor pass reads the element-level region, before any page is
        // known, so a per-page refinement on a signature member would search
        // the same pixels a second time in the same cycle. The spec can hold
        // the pair because the row it writes does; the refusal names the
        // element, ahead of the catalog stating the same rule in its own terms.
        auto const markId   = annotation::test::elementId(k_anchorId);
        auto const battleId = annotation::test::pageId(k_secondPageId);
        auto const onBattle = [markId, battleId](
                                  EditableReference const& reference
                              )
        {
            return reference.pageId == battleId && reference.elementId == markId;
        };

        auto draft = makeAuthoringDraft(twoPageDocument());
        std::erase_if(draft.references, onBattle);

        auto const refused = referenceElementOnPage(
            draft,
            ReferenceElementSpec{
                .elementId = markId,
                .pageId    = battleId,
                .exercised = EditableExercised{
                    .identify = annotation::ExercisedIdentify{},
                },
                .searchRoi = annotation::test::pixelRect(0, 0, 4, 4),
            }
        );
        CHECK_FALSE(refused.has_value());

        // The control: the same request without the refinement is taken, so
        // the refusal is about the pair rather than about either half.
        auto const accepted = referenceElementOnPage(
            std::move(draft),
            ReferenceElementSpec{
                .elementId = markId,
                .pageId    = battleId,
                .exercised = EditableExercised{
                    .identify = annotation::ExercisedIdentify{},
                },
            }
        );
        CHECK(accepted.has_value());
    }

    TEST_CASE("a page may not identify by an element that declares no identify")
    {
        // Pixels that cannot be evidence cannot enter a signature. Refusing it
        // here rather than letting the catalog's subset rule catch it is what
        // lets the message name the element the author typed.
        auto const battleId = annotation::test::pageId(k_secondPageId);

        auto draft = makeAuthoringDraft(document());
        draft.pages.emplace_back(
            EditablePage{
                .id   = battleId,
                .name = "battle",
            }
        );

        auto const referenced = referenceElementOnPage(
            std::move(draft),
            ReferenceElementSpec{
                .elementId = annotation::test::elementId(k_actionId),
                .pageId    = battleId,
                .exercised = EditableExercised{
                    .identify = annotation::ExercisedIdentify{},
                },
            }
        );
        CHECK_FALSE(referenced.has_value());
    }

    TEST_CASE("pointing a page's evidence at a mark borrows it and keeps its region")
    {
        // The role verb reaches the same rules through the same function.
        // Minting yields Referenced, because a page taking up a mark whose
        // home is elsewhere is borrowing it -- claiming to own it is the
        // contradiction the catalog refuses once a second page does the same.
        auto const markId   = annotation::test::elementId(k_anchorId);
        auto const actionId = annotation::test::elementId(k_actionId);
        auto const battleId = annotation::test::pageId(k_secondPageId);
        auto const onBattle = [markId, battleId](
                                  EditableReference const& reference
                              )
        {
            return reference.pageId == battleId && reference.elementId == markId;
        };

        auto draft = makeAuthoringDraft(twoPageDocument());
        std::erase_if(draft.references, onBattle);

        auto const pointed = setReferenceIdentifyRole(
            draft,
            markId,
            battleId,
            annotation::SignatureRole::Forbidden
        );
        REQUIRE(pointed.has_value());
        auto const minted = std::ranges::find_if(pointed->references, onBattle);
        REQUIRE(minted != pointed->references.end());
        CHECK(minted->holding == annotation::Holding::Referenced);
        CHECK(buildAuthoringDocument(*pointed).has_value());

        // A page whose reference already refines a region is refused rather
        // than having the refinement quietly dropped to make room: both are
        // measurements the author made, and which one goes is theirs to say.
        auto const homeId = annotation::test::pageId(k_pageId);
        auto refining     = makeAuthoringDraft(document());
        for (auto& element : refining.elements)
        {
            if (element.id == actionId)
            {
                element.capabilities.identify = annotation::Identify{};
            }
        }
        auto const clickable = std::ranges::find_if(
            refining.references,
            [actionId](EditableReference const& reference)
            {
                return reference.elementId == actionId;
            }
        );
        REQUIRE(clickable != refining.references.end());
        clickable->searchRoi = annotation::test::pixelRect(3, 3, 4, 4);

        auto const refused = setReferenceIdentifyRole(
            refining,
            actionId,
            homeId,
            annotation::SignatureRole::Required
        );
        CHECK_FALSE(refused.has_value());

        // The control: the same page, the same element, once the refinement is
        // withdrawn -- so the refusal is about the region and not about the
        // reference already existing.
        clickable->searchRoi.reset();
        auto const accepted = setReferenceIdentifyRole(
            std::move(refining),
            actionId,
            homeId,
            annotation::SignatureRole::Required
        );
        REQUIRE(accepted.has_value());
        auto const promoted = std::ranges::find_if(
            accepted->references,
            [actionId](EditableReference const& reference)
            {
                return reference.elementId == actionId;
            }
        );
        REQUIRE(promoted != accepted->references.end());
        CHECK(promoted->exercised.identify.has_value());
        CHECK(promoted->exercised.interact.has_value());
    }

    TEST_CASE("moving an element's appearance moves it on every page that has it")
    {
        // Drawing the element once is only worth anything if correcting it
        // corrects it everywhere: one element, one appearance, two references.
        auto const actionId = annotation::test::elementId(k_actionId);
        auto const moved    = annotation::test::pixelRect(3, 3, 2, 2);

        auto draft = makeAuthoringDraft(document());
        draft.references.emplace_back(
            EditableReference{
                .pageId    = annotation::test::pageId(k_secondPageId),
                .elementId = actionId,
                .holding   = annotation::Holding::Referenced,
                .exercised = EditableExercised{
                    .interact = annotation::ExercisedInteract{},
                },
                .searchRoi = annotation::test::pixelRect(0, 0, 8, 8),
            }
        );

        auto const retemplated = setElementTemplateRect(
            std::move(draft),
            actionId,
            moved
        );
        REQUIRE(retemplated.has_value());
        CHECK(retemplated->referencingPages == 2U);
        auto const changed = elementIn(retemplated->draft, actionId);
        REQUIRE(changed.appearances.size() == 1U);
        CHECK(changed.appearances.front().templateRect == moved);
        // Both references still name the one, corrected element.
        CHECK(pagesReferencing(retemplated->draft, actionId).size() == 2U);
    }

    TEST_CASE("a template that outgrows a reference's range is refused")
    {
        // Widening a range the author drew would enlarge both the search cost and
        // the surface for a false match, so the author is told to fix it. The
        // moved template still fits the element's own range, so the refusal can
        // only come from the reference's narrower one.
        auto const actionId = annotation::test::elementId(k_actionId);
        auto const outgrown = annotation::test::pixelRect(0, 0, 4, 4);

        auto draft = makeAuthoringDraft(document());
        REQUIRE(elementIn(draft, actionId).searchRoi.width() == 4U);
        draft.references.emplace_back(
            EditableReference{
                .pageId    = annotation::test::pageId(k_secondPageId),
                .elementId = actionId,
                .holding   = annotation::Holding::Referenced,
                .exercised = EditableExercised{
                    .interact = annotation::ExercisedInteract{},
                },
                .searchRoi = annotation::test::pixelRect(4, 4, 3, 3),
            }
        );

        auto const retemplated = setElementTemplateRect(
            std::move(draft),
            actionId,
            outgrown
        );
        CHECK_FALSE(retemplated.has_value());

        // The control: without the narrower reference the same move is accepted,
        // so the refusal is about that range and not about the template.
        CHECK(
            setElementTemplateRect(
                makeAuthoringDraft(document()),
                actionId,
                outgrown
            ).has_value()
        );
    }

    TEST_CASE("recording a screen rewrites its case rather than adding a second")
    {
        // A screen resolves to exactly one page, so a second case for the same
        // screen would be a contradiction the document has no way to settle.
        auto const sourceId = annotation::test::sourceId(k_sourceId);
        auto const pageId   = annotation::test::pageId(k_pageId);

        auto draft = makeAuthoringDraft(document());
        REQUIRE(draft.regressions.size() == 1U);
        draft.regressions.at(0).expectation = annotation::UnknownRegression{};

        auto const claimed = claimScreenForPage(
            std::move(draft),
            ScreenClaimSpec{
                .regressionId = annotation::test::regressionId(
                    "00000000-0000-0000-0000-000000000303"
                ),
                .sourceId = sourceId,
                .pageId   = pageId,
            }
        );
        REQUIRE(claimed.has_value());
        REQUIRE(claimed->regressions.size() == 1U);
        CHECK(
            claimed->regressions.at(0).expectation
            == annotation::RegressionExpectation{
                annotation::ResolvedRegression{.pageId = pageId},
            }
        );
        CHECK(buildAuthoringDocument(*claimed).has_value());
    }

    TEST_CASE("recording a screen with no case yet adds one")
    {
        auto const sourceId = annotation::test::sourceId(k_sourceId);
        auto const pageId   = annotation::test::pageId(k_pageId);

        auto draft = makeAuthoringDraft(document());
        draft.regressions.clear();

        auto const claimed = claimScreenForPage(
            std::move(draft),
            ScreenClaimSpec{
                .regressionId = annotation::test::regressionId(
                    "00000000-0000-0000-0000-000000000303"
                ),
                .sourceId = sourceId,
                .pageId   = pageId,
            }
        );
        REQUIRE(claimed.has_value());
        REQUIRE(claimed->regressions.size() == 1U);
        CHECK(claimed->regressions.at(0).sourceId == sourceId);
        CHECK(buildAuthoringDocument(*claimed).has_value());
    }

    TEST_CASE("recording a pageless expectation with no case yet adds one")
    {
        struct Row final
        {
            PagelessExpectation                  input{};
            annotation::RegressionExpectation    expectation;
            annotation::RegressionClassification classification{};
        };
        auto const rows = std::array<Row, 2>{
            Row{
                .input          = PagelessExpectation::Unknown,
                .expectation    = annotation::UnknownRegression{},
                .classification = annotation::RegressionClassification::Negative,
            },
            Row{
                .input          = PagelessExpectation::Ambiguous,
                .expectation    = annotation::AmbiguousRegression{},
                .classification = annotation::RegressionClassification::Confusable,
            },
        };
        auto const sourceId = annotation::test::sourceId(k_sourceId);

        for (auto const& row : rows)
        {
            auto draft = makeAuthoringDraft(document());
            draft.regressions.clear();

            auto const recorded = recordScreenExpectation(
                std::move(draft),
                ScreenExpectationSpec{
                    .regressionId = annotation::test::regressionId(
                        "00000000-0000-0000-0000-000000000305"
                    ),
                    .sourceId    = sourceId,
                    .expectation = row.input,
                }
            );
            REQUIRE(recorded.has_value());
            REQUIRE(recorded->regressions.size() == 1U);
            CHECK(recorded->regressions.at(0).sourceId == sourceId);
            CHECK(recorded->regressions.at(0).expectation == row.expectation);
            CHECK(
                recorded->regressions.at(0).classification == row.classification
            );
            CHECK(buildAuthoringDocument(*recorded).has_value());
        }
    }

    TEST_CASE("recording a pageless expectation rewrites the existing case")
    {
        // A screen carries exactly one case, so recording it as ambiguous must
        // overwrite the resolved case the document starts with, not add a second.
        auto const sourceId = annotation::test::sourceId(k_sourceId);

        auto draft = makeAuthoringDraft(document());
        REQUIRE(draft.regressions.size() == 1U);

        auto const recorded = recordScreenExpectation(
            std::move(draft),
            ScreenExpectationSpec{
                .regressionId = annotation::test::regressionId(
                    "00000000-0000-0000-0000-000000000305"
                ),
                .sourceId    = sourceId,
                .expectation = PagelessExpectation::Ambiguous,
            }
        );
        REQUIRE(recorded.has_value());
        REQUIRE(recorded->regressions.size() == 1U);
        CHECK(
            recorded->regressions.at(0).expectation
            == annotation::RegressionExpectation{annotation::AmbiguousRegression{}}
        );
        CHECK(buildAuthoringDocument(*recorded).has_value());
    }

    TEST_CASE("recording an expectation for a screen outside the draft is refused")
    {
        auto const recorded = recordScreenExpectation(
            makeAuthoringDraft(document()),
            ScreenExpectationSpec{
                .regressionId = annotation::test::regressionId(
                    "00000000-0000-0000-0000-000000000305"
                ),
                .sourceId    = annotation::test::sourceId(
                    "00000000-0000-0000-0000-0000000009ff"
                ),
                .expectation = PagelessExpectation::Unknown,
            }
        );
        CHECK_FALSE(recorded.has_value());
    }

    TEST_CASE("adding a member to a page outside the draft is refused")
    {
        auto const added = addPageMember(
            makeAuthoringDraft(document()),
            PageMemberSpec{
                .elementId    = annotation::test::elementId(k_awayId),
                .pageId       = annotation::test::pageId(k_secondPageId),
                .sourceId     = annotation::test::sourceId(k_sourceId),
                .templateRect = annotation::test::pixelRect(0, 0, 2, 2),
                .searchRoi    = annotation::test::pixelRect(0, 0, 4, 4),
                .similarityBasisPoints = 9'000U,
                .kind                  = PageMemberKind::Anchor,
            }
        );
        CHECK_FALSE(added.has_value());
    }

    TEST_CASE("deleting an element withdraws it from the pages that name it")
    {
        auto const awayId   = annotation::test::elementId(k_awayId);
        auto const battleId = annotation::test::pageId(k_secondPageId);

        auto const deleted = deleteElement(
            makeAuthoringDraft(twoPageDocument()),
            awayId
        );
        REQUIRE(deleted.has_value());
        CHECK(deleted->withdrawnRoles == 1U);
        CHECK(deleted->draft.elements.size() == 1U);
        CHECK_FALSE(signatureRoleIn(deleted->draft, battleId, awayId).has_value());

        CHECK(buildAuthoringDocument(deleted->draft).has_value());
    }

    TEST_CASE("deleting the only mark a page identifies by is refused")
    {
        // The page would be left identifying no screen, and only the author can
        // say whether the page goes too or another mark takes over.
        auto const anchorId = annotation::test::elementId(k_anchorId);

        auto const deleted = deleteElement(
            makeAuthoringDraft(document()),
            anchorId
        );
        REQUIRE_FALSE(deleted.has_value());
    }

    TEST_CASE("deleting a page withdraws every reference it made")
    {
        // The clickable element is exercised by this page and one other, so the
        // deletion leaves it reachable somewhere and may proceed.
        auto const actionId = annotation::test::elementId(k_actionId);
        auto const homeId   = annotation::test::pageId(k_pageId);
        auto const battleId = annotation::test::pageId(k_secondPageId);

        auto widened = makeAuthoringDraft(document());
        widened.pages.emplace_back(
            EditablePage{
                .id   = battleId,
                .name = "battle",
            }
        );
        // Two pages may not carry the same signature, so the second page forbids
        // the mark the first requires.
        widened.references.emplace_back(
            EditableReference{
                .pageId    = battleId,
                .elementId = annotation::test::elementId(k_anchorId),
                .holding   = annotation::Holding::Referenced,
                .exercised = EditableExercised{
                    .identify = annotation::ExercisedIdentify{
                        .role = annotation::SignatureRole::Forbidden,
                    },
                },
            }
        );
        widened.references.emplace_back(
            EditableReference{
                .pageId    = battleId,
                .elementId = actionId,
                .holding   = annotation::Holding::Referenced,
                .exercised = EditableExercised{
                    .interact = annotation::ExercisedInteract{},
                },
            }
        );
        // The document's regression expects the page under test to resolve, which
        // is refused on its own; this case is about the references.
        REQUIRE(widened.regressions.size() == 1U);
        widened.regressions.at(0).expectation = annotation::UnknownRegression{};

        auto const deleted = deletePage(std::move(widened), homeId);
        REQUIRE(deleted.has_value());
        // Both of the home page's references went with it: the mark's and the
        // clickable element's.
        CHECK(deleted->withdrawnReferences == 2U);
        CHECK(deleted->draft.pages.size() == 1U);
        CHECK(pagesReferencing(deleted->draft, actionId).size() == 1U);
    }

    TEST_CASE("deleting the only page an element can be clicked on is refused")
    {
        auto const pageId = annotation::test::pageId(k_pageId);

        auto const deleted = deletePage(makeAuthoringDraft(document()), pageId);
        REQUIRE_FALSE(deleted.has_value());
    }

    TEST_CASE("deleting a page removes the regressions expecting it to resolve")
    {
        // The expectation names a page that no longer exists, so it cannot be
        // reclassified into anything the author meant. Every page authored from
        // a captured screen owns one, so refusing here would make such a page
        // undeletable.
        auto const pageId = annotation::test::pageId(k_pageId);

        auto const actionId = annotation::test::elementId(k_actionId);

        auto draft = makeAuthoringDraft(document());
        // The clickable element would be left unreachable, which is refused on
        // its own; this case is about the regression, so it goes first.
        std::erase_if(
            draft.elements,
            [actionId](EditableElement const& element)
            {
                return element.id == actionId;
            }
        );
        std::erase_if(
            draft.references,
            [actionId](EditableReference const& reference)
            {
                return reference.elementId == actionId;
            }
        );
        REQUIRE(draft.regressions.size() == 1U);
        draft.regressions.at(0).expectation = annotation::ResolvedRegression{
            .pageId = pageId,
        };

        auto const deleted = deletePage(std::move(draft), pageId);
        REQUIRE(deleted.has_value());
        CHECK(deleted->removedRegressions == 1U);
        CHECK(deleted->draft.regressions.empty());
        CHECK(deleted->draft.pages.empty());
    }

    TEST_CASE("deleting a source removes the regression cases recorded on it")
    {
        auto const sourceId = annotation::test::sourceId(k_sourceId);

        auto draft = makeAuthoringDraft(document());
        draft.elements.clear();
        draft.references.clear();
        draft.pages.clear();
        REQUIRE(draft.regressions.size() == 1U);

        auto const deleted = deleteSource(std::move(draft), sourceId);
        REQUIRE(deleted.has_value());
        CHECK(deleted->removedRegressions == 1U);
        CHECK(deleted->draft.sources.empty());
        CHECK(deleted->draft.regressions.empty());

        CHECK(buildAuthoringDocument(deleted->draft).has_value());
    }

    TEST_CASE("deleting a source still carrying appearances is refused")
    {
        // An appearance is only meaningful against the image it was cut from, so
        // the source cannot leave without it.
        auto const sourceId = annotation::test::sourceId(k_sourceId);

        auto const deleted = deleteSource(
            makeAuthoringDraft(document()),
            sourceId
        );
        REQUIRE_FALSE(deleted.has_value());
    }

    TEST_CASE("authoring draft preserves the complete canonical document")
    {
        auto const documents = std::vector<annotation::AuthoringDocument>{
            document(),
            appearanceDocument(),
        };
        for (auto const& original : documents)
        {
            auto const rebuilt = buildAuthoringDocument(
                makeAuthoringDraft(original)
            );
            REQUIRE(rebuilt.has_value());
            CHECK(
                annotation::serializeAuthoringDocument(*rebuilt)
                == annotation::serializeAuthoringDocument(original)
            );
        }
    }

    TEST_CASE("authoring history applies validated edits and supports undo and redo")
    {
        auto history = AuthoringEditHistory{document()};
        auto renamed = history.draft();

        renamed.elements.at(0).name = "renamed_marker";

        auto const applied = history.apply(renamed);
        REQUIRE(applied.has_value());
        CHECK(*applied);
        CHECK(elementName(history, 0) == "renamed_marker");
        CHECK(history.canUndo());
        CHECK_FALSE(history.canRedo());

        CHECK(history.undo());
        CHECK(elementName(history, 0) == "home_marker");
        CHECK_FALSE(history.canUndo());
        CHECK(history.canRedo());
        CHECK(
            annotation::serializeAuthoringDocument(history.document())
            == annotation::serializeAuthoringDocument(document())
        );

        CHECK(history.redo());
        CHECK(elementName(history, 0) == "renamed_marker");
        CHECK(history.canUndo());
        CHECK_FALSE(history.canRedo());
    }

    TEST_CASE("authoring history position is restored by undo and redo")
    {
        // Unlike revision(), which only advances, position() names the document
        // identity a save can be compared against: undo and redo restore it, so a
        // state returned to reads the same position it was saved at.
        auto history = AuthoringEditHistory{document()};
        auto const loaded = history.position();

        auto first = history.draft();
        first.elements.at(0).name = "first_name";
        REQUIRE(history.apply(first).has_value());
        auto const afterFirst = history.position();
        CHECK(afterFirst != loaded);

        auto second = history.draft();
        second.elements.at(0).name = "second_name";
        REQUIRE(history.apply(second).has_value());
        CHECK(history.position() != afterFirst);

        // Undo returns to the earlier positions exactly, and redo forward to the
        // later one -- a value the latched revision could never provide.
        REQUIRE(history.undo());
        CHECK(history.position() == afterFirst);
        REQUIRE(history.undo());
        CHECK(history.position() == loaded);
        REQUIRE(history.redo());
        CHECK(history.position() == afterFirst);

        // A fresh edit past a restored position mints a new identity rather than
        // reusing the abandoned redo one, so it never collides with a saved state.
        auto branch = history.draft();
        branch.elements.at(0).name = "branch_name";
        REQUIRE(history.apply(branch).has_value());
        CHECK(history.position() != loaded);
        CHECK(history.position() != afterFirst);
    }

    TEST_CASE("authoring history rejects invalid drafts without changing history")
    {
        auto history = AuthoringEditHistory{document()};
        auto renamed = history.draft();

        renamed.elements.at(0).name = "renamed_marker";
        REQUIRE(history.apply(renamed).has_value());
        REQUIRE(history.undo());
        REQUIRE(history.canRedo());

        auto invalid = history.draft();

        invalid.elements.at(0).name.clear();

        auto const applied = history.apply(invalid);
        REQUIRE_FALSE(applied.has_value());
        CHECK(elementName(history, 0) == "home_marker");
        CHECK_FALSE(history.canUndo());
        CHECK(history.canRedo());
        CHECK(history.redo());
        CHECK(elementName(history, 0) == "renamed_marker");
    }

    TEST_CASE("authoring history ignores identical edits and clears abandoned redo")
    {
        auto history = AuthoringEditHistory{document()};

        auto const unchanged = history.apply(history.draft());
        REQUIRE(unchanged.has_value());
        CHECK_FALSE(*unchanged);
        CHECK_FALSE(history.canUndo());

        auto first = history.draft();

        first.elements.at(0).name = "first_name";
        REQUIRE(history.apply(first).has_value());
        REQUIRE(history.undo());
        CHECK(history.canRedo());

        auto const pending = history.apply(history.draft());
        REQUIRE(pending.has_value());
        CHECK_FALSE(*pending);
        CHECK(history.canRedo());
        CHECK_FALSE(history.canUndo());

        auto branch = history.draft();

        branch.elements.at(0).name = "branch_name";
        REQUIRE(history.apply(branch).has_value());
        CHECK(elementName(history, 0) == "branch_name");
        CHECK_FALSE(history.canRedo());
    }

    TEST_CASE("authoring history replays redo entries in reverse order")
    {
        auto history = AuthoringEditHistory{document()};
        CHECK_FALSE(history.redo());

        auto first = history.draft();

        first.elements.at(0).name = "first_name";
        REQUIRE(history.apply(first).has_value());

        auto second = history.draft();

        second.elements.at(0).name = "second_name";
        REQUIRE(history.apply(second).has_value());

        REQUIRE(history.undo());
        REQUIRE(history.undo());
        CHECK(elementName(history, 0) == "home_marker");

        CHECK(history.redo());
        CHECK(elementName(history, 0) == "first_name");
        CHECK(history.redo());
        CHECK(elementName(history, 0) == "second_name");
        CHECK_FALSE(history.redo());
    }

    TEST_CASE("authoring history retains the configured undo boundary")
    {
        auto history = AuthoringEditHistory{document()};
        for (
            auto index = std::size_t{0};
            index < k_maximumAuthoringUndoEntries + 1U;
            ++index
        )
        {
            auto next = history.draft();

            next.elements.at(0).name = "marker_" + std::to_string(index);

            auto const applied = history.apply(next);
            REQUIRE(applied.has_value());
            REQUIRE(*applied);
        }

        for (
            auto index = std::size_t{0};
            index < k_maximumAuthoringUndoEntries;
            ++index
        )
        {
            REQUIRE(history.undo());
        }
        CHECK_FALSE(history.undo());
        CHECK(elementName(history, 0) == "marker_0");
    }

    namespace
    {
        constexpr auto k_menuSourceId = "00000000-0000-0000-0000-000000000901";
        constexpr auto k_menuAnchorId = "00000000-0000-0000-0000-000000000902";
        constexpr auto k_menuPageId   = "00000000-0000-0000-0000-000000000903";

        // The tolerance the measurement in colour-key-fixture.hpp was taken at.
        constexpr auto k_menuTolerance = uint32{12};

        [[nodiscard]]
        auto menuKey() -> annotation::ColourKey
        {
            auto const key = annotation::ColourKey::create(
                colour_key_fixture::k_textRed,
                colour_key_fixture::k_textGreen,
                colour_key_fixture::k_textBlue,
                k_menuTolerance
            );
            REQUIRE(key.has_value());
            return *key;
        }

        // A one-screen project whose single anchor covers the whole menu crop.
        // The crop is both the screen and the template, so the compiled asset is
        // the mask itself with nothing else in it.
        [[nodiscard]]
        auto menuDocument(
            std::span<uint8 const> screenPng,
            std::optional<annotation::ColourKey> colourKey
        ) -> annotation::AuthoringDocument
        {
            auto const fingerprint = annotation::test::fingerprint(
                colour_key_fixture::k_width,
                colour_key_fixture::k_height,
                96,
                96
            );
            auto const sourceId = annotation::test::sourceId(k_menuSourceId);
            auto const anchorId = annotation::test::elementId(k_menuAnchorId);
            auto const pageId   = annotation::test::pageId(k_menuPageId);

            auto const hash = annotation::sha256(
                colour_key_fixture::pngBytes(screenPng)
            );
            REQUIRE(hash.has_value());
            auto source = annotation::AuthoringSource::create(
                annotation::AuthoringSourceSpec{
                    .id          = sourceId,
                    .contentHash = *hash,
                    .fingerprint = fingerprint,
                    .provenance  = annotation::ImportedSourceProvenance{},
                }
            );
            REQUIRE(source.has_value());

            auto const wholeCrop = annotation::test::pixelRect(
                0,
                0,
                colour_key_fixture::k_width,
                colour_key_fixture::k_height
            );
            auto appearances = std::vector<annotation::Appearance>{};
            appearances.emplace_back(
                annotation::test::appearance(
                    "default",
                    sourceId,
                    wholeCrop,
                    annotation::test::threshold(),
                    colourKey
                )
            );
            auto element = annotation::test::element(
                fingerprint,
                anchorId,
                "menu_entry",
                annotation::test::capabilities(annotation::Identify{}),
                wholeCrop,
                std::move(appearances)
            );

            auto created = annotation::AuthoringDocument::create(
                annotation::test::projectId(),
                fingerprint,
                {*source},
                {std::move(element)},
                {annotation::test::page(pageId, "menu")},
                {
                    annotation::test::reference(
                        pageId,
                        anchorId,
                        annotation::test::identifiesAs()
                    ),
                },
                {}
            );
            REQUIRE(created.has_value());
            return *std::move(created);
        }

        struct DecodedImage final
        {
            uint32                 width{};
            uint32                 height{};
            std::vector<std::byte> rgba{};

            [[nodiscard]]
            auto channel(std::size_t pixel, std::size_t index) const -> uint32
            {
                return std::to_integer<uint32>(rgba.at(pixel * 4U + index));
            }

            [[nodiscard]]
            auto grey(std::size_t pixel) const -> double
            {
                return (
                    static_cast<double>(channel(pixel, 0))
                    + static_cast<double>(channel(pixel, 1))
                    + static_cast<double>(channel(pixel, 2))
                ) / 3.0;
            }
        };

        [[nodiscard]]
        auto decodeFixture(std::span<uint8 const> png) -> DecodedImage
        {
            auto decoded = image::decodePng(
                colour_key_fixture::pngBytes(png),
                "colour-key-fixture.png"
            );
            REQUIRE(decoded.has_value());
            return DecodedImage{
                .width  = decoded->width,
                .height = decoded->height,
                .rgba   = std::move(decoded->pixels),
            };
        }

        // Compiles the one-anchor project and decodes the single template asset
        // it emits. That asset's alpha channel is the whole contract this
        // feature owes the matcher.
        [[nodiscard]]
        auto compileMenuTemplate(
            std::span<uint8 const> screenPng,
            std::optional<annotation::ColourKey> colourKey
        ) -> DecodedImage
        {
            auto const document = menuDocument(screenPng, colourKey);
            auto const assets   = std::array{
                annotation::AuthoringSourceAsset{
                    .id       = annotation::test::sourceId(k_menuSourceId),
                    .pngBytes = colour_key_fixture::pngBytes(screenPng),
                },
            };
            auto compiled = annotation::compileAuthoringDocument(document, assets);
            REQUIRE(compiled.has_value());
            REQUIRE(compiled->templateAssets.size() == 1U);

            auto decoded = image::decodePng(
                compiled->templateAssets.at(0).pngBytes,
                "compiled-template.png"
            );
            REQUIRE(decoded.has_value());
            return DecodedImage{
                .width  = decoded->width,
                .height = decoded->height,
                .rgba   = std::move(decoded->pixels),
            };
        }
    }

    TEST_CASE("a colour key round-trips through the authoring document exactly")
    {
        auto const key = menuKey();
        auto const keyed = menuDocument(
            colour_key_fixture::k_menuOverBlueArtwork,
            key
        );
        auto const text = annotation::serializeAuthoringDocument(keyed);
        CHECK(text.find("colour_key = [255, 255, 255]\n") != std::string::npos);
        CHECK(text.find("colour_key_tolerance = 12\n") != std::string::npos);

        // parseAuthoringDocument refuses anything that is not byte-for-byte the
        // canonical output, so a successful parse of this text is already the
        // serialize(parse(x)) == x property; asserting it again names it.
        auto const parsed = annotation::parseAuthoringDocument(text);
        REQUIRE(parsed.has_value());
        CHECK(annotation::serializeAuthoringDocument(*parsed) == text);
        REQUIRE(parsed->elements().size() == 1U);
        REQUIRE(parsed->elements().front().appearances().size() == 1U);
        CHECK(parsed->elements().front().appearances().front().colourKey() == key);

        // The draft the editing layer works through has to carry the key across
        // both conversions, or every edit made here would silently drop it.
        auto const draft = makeAuthoringDraft(*parsed);
        REQUIRE(draft.elements.size() == 1U);
        REQUIRE(draft.elements.at(0).appearances.size() == 1U);
        CHECK(draft.elements.at(0).appearances.at(0).colourKey == key);
        auto const rebuilt = buildAuthoringDocument(draft);
        REQUIRE(rebuilt.has_value());
        CHECK(annotation::serializeAuthoringDocument(*rebuilt) == text);
    }

    TEST_CASE("an element with no colour key serializes as it did before the field")
    {
        auto const plain = menuDocument(
            colour_key_fixture::k_menuOverBlueArtwork,
            std::nullopt
        );
        auto const text = annotation::serializeAuthoringDocument(plain);
        CHECK(text.find("colour_key") == std::string::npos);

        auto const parsed = annotation::parseAuthoringDocument(text);
        REQUIRE(parsed.has_value());
        CHECK(annotation::serializeAuthoringDocument(*parsed) == text);
        REQUIRE(parsed->elements().size() == 1U);
        REQUIRE(parsed->elements().front().appearances().size() == 1U);
        CHECK_FALSE(
            parsed->elements().front().appearances().front().colourKey().has_value()
        );

        // The control for the case above: the same document with a key does emit
        // those bytes, so "no colour_key in the text" is a fact about the absent
        // key rather than about the serializer never writing one.
        auto const keyed = menuDocument(
            colour_key_fixture::k_menuOverBlueArtwork,
            menuKey()
        );
        CHECK(
            annotation::serializeAuthoringDocument(keyed).find("colour_key")
            != std::string::npos
        );
    }

    TEST_CASE("a compiled template's alpha is the mask its colour key implies")
    {
        auto const key    = menuKey();
        auto const masked = compileMenuTemplate(
            colour_key_fixture::k_menuOverBlueArtwork,
            key
        );
        REQUIRE(masked.width == colour_key_fixture::k_width);
        REQUIRE(masked.height == colour_key_fixture::k_height);

        auto const screen = decodeFixture(
            colour_key_fixture::k_menuOverBlueArtwork
        );
        auto const pixels = static_cast<std::size_t>(masked.width) * masked.height;

        auto colourChanged = std::size_t{0};
        auto alphaWrong    = std::size_t{0};
        auto opaque        = std::size_t{0};
        auto partial       = std::size_t{0};
        auto clear         = std::size_t{0};
        for (auto pixel = std::size_t{0}; pixel < pixels; ++pixel)
        {
            auto const red   = masked.channel(pixel, 0);
            auto const green = masked.channel(pixel, 1);
            auto const blue  = masked.channel(pixel, 2);
            auto const alpha = masked.channel(pixel, 3);

            // Baking a mask writes the alpha channel and nothing else.
            if (
                red != screen.channel(pixel, 0)
                || green != screen.channel(pixel, 1)
                || blue != screen.channel(pixel, 2)
            )
            {
                ++colourChanged;
            }
            if (
                alpha != key.alphaFor(
                    static_cast<uint8>(red),
                    static_cast<uint8>(green),
                    static_cast<uint8>(blue)
                )
            )
            {
                ++alphaWrong;
            }

            if (alpha == 255U)
            {
                ++opaque;
            }
            else if (alpha == 0U)
            {
                ++clear;
            }
            else
            {
                ++partial;
            }
        }
        CHECK(colourChanged == 0U);
        CHECK(alphaWrong == 0U);

        // Measured on this fixture at tolerance 12 around (255, 255, 255).
        CHECK(opaque == 328U);
        CHECK(partial == 32U);
        CHECK(clear == 3640U);

        // The no-key control. Without it every assertion above would also pass
        // on a compiler that ignored the key and left the alpha at 255, because
        // this fixture would then simply have 4000 opaque pixels.
        auto const plain = compileMenuTemplate(
            colour_key_fixture::k_menuOverBlueArtwork,
            std::nullopt
        );
        auto plainOpaque = std::size_t{0};
        for (auto pixel = std::size_t{0}; pixel < pixels; ++pixel)
        {
            if (plain.channel(pixel, 3) == 255U)
            {
                ++plainOpaque;
            }
        }
        CHECK(plainOpaque == pixels);
    }

    TEST_CASE("a colour key mask keeps the menu text and drops the artwork")
    {
        auto const masked = compileMenuTemplate(
            colour_key_fixture::k_menuOverBlueArtwork,
            menuKey()
        );
        // The same rectangle of the same UI over a different character. The
        // menu's own pixels are byte-identical between the two; everything else
        // is a different picture.
        auto const other = decodeFixture(
            colour_key_fixture::k_menuOverPurpleArtwork
        );
        REQUIRE(other.width == masked.width);
        REQUIRE(other.height == masked.height);

        auto const pixels = static_cast<std::size_t>(masked.width) * masked.height;
        auto keptCount    = std::size_t{0};
        auto keptSum      = 0.0;
        auto droppedCount = std::size_t{0};
        auto droppedSum   = 0.0;
        auto wholeSum     = 0.0;
        for (auto pixel = std::size_t{0}; pixel < pixels; ++pixel)
        {
            auto const difference = std::abs(masked.grey(pixel) - other.grey(pixel));
            wholeSum += difference;
            if (masked.channel(pixel, 3) == 255U)
            {
                ++keptCount;
                keptSum += difference;
            }
            else if (masked.channel(pixel, 3) == 0U)
            {
                ++droppedCount;
                droppedSum += difference;
            }
        }
        REQUIRE(keptCount > 0U);
        REQUIRE(droppedCount > 0U);

        // What the key selects agrees across the two backgrounds to within a
        // fiftieth of one grey level: those pixels are the UI's own.
        CHECK(keptSum / static_cast<double>(keptCount) < 0.05);

        // What it drops does not agree at all: that is the artwork.
        CHECK(droppedSum / static_cast<double>(droppedCount) > 40.0);

        // The control that makes the first assertion mean something. An
        // unmasked template compares this whole rectangle, and over it the two
        // captures differ by more than a fifth of the grey range -- so "the kept
        // pixels agree" is a fact about the mask, not about the rectangle.
        CHECK(wholeSum / static_cast<double>(pixels) > 40.0);

        // Roughly a twelfth of the box, which is what a line of text over
        // artwork looks like. A key that had caught the artwork instead would
        // keep most of the box and still pass the agreement test above on a
        // second capture of the same artwork.
        CHECK(keptCount * 100U / pixels == 8U);
    }

    TEST_CASE("a colour key change undoes and redoes like any other edit")
    {
        auto history        = AuthoringEditHistory{document()};
        auto const anchorId = annotation::test::elementId(k_anchorId);
        auto const key      = annotation::ColourKey::create(200, 40, 40, 9);
        REQUIRE(key.has_value());

        auto const storedKey = [&history, anchorId]
        {
            auto const* element = history.document().findElement(anchorId);
            REQUIRE(element != nullptr);
            REQUIRE(element->appearances().size() == 1U);
            return element->appearances().front().colourKey();
        };
        REQUIRE_FALSE(storedKey().has_value());

        auto keyed = setElementColourKey(history.draft(), anchorId, *key);
        REQUIRE(keyed.has_value());
        auto const applied = history.apply(*keyed);
        REQUIRE(applied.has_value());
        REQUIRE(*applied);
        CHECK(storedKey() == *key);
        CHECK(history.canUndo());

        REQUIRE(history.undo());
        CHECK_FALSE(storedKey().has_value());
        REQUIRE(history.canRedo());
        REQUIRE(history.redo());
        CHECK(storedKey() == *key);

        // Setting the same key again is not a change, exactly as re-typing an
        // unchanged name is not. Without this the case above would pass on an
        // apply() that recorded an undo entry for every frame of a slider drag.
        auto same = setElementColourKey(history.draft(), anchorId, *key);
        REQUIRE(same.has_value());
        auto const again = history.apply(*same);
        REQUIRE(again.has_value());
        CHECK_FALSE(*again);

        // Clearing it is its own edit and its own undo entry.
        auto cleared = setElementColourKey(history.draft(), anchorId, std::nullopt);
        REQUIRE(cleared.has_value());
        auto const removal = history.apply(*cleared);
        REQUIRE(removal.has_value());
        REQUIRE(*removal);
        CHECK_FALSE(storedKey().has_value());
        REQUIRE(history.undo());
        CHECK(storedKey() == *key);
    }
}
