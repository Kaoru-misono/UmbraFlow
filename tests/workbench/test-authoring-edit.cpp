#include "../annotation/test-helpers.hpp"

#include <authoring-edit.hpp>

#include <annotation/authoring-document.hpp>
#include <annotation/content-hash.hpp>

#include <core/types/integer.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
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
            auto const anchorId    = annotation::test::recognizerId(k_anchorId);
            auto const actionId    = annotation::test::recognizerId(k_actionId);
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
                    annotation::test::anchorElement(
                        fingerprint,
                        anchorId,
                        "home_marker",
                        sourceId,
                        annotation::test::pixelRect(0, 0, 2, 2),
                        annotation::test::pixelRect(0, 0, 4, 4)
                    ),
                    annotation::test::interactiveElement(
                        fingerprint,
                        actionId,
                        "daily_button",
                        sourceId,
                        annotation::test::pixelRect(4, 4, 2, 2),
                        annotation::test::pixelRect(3, 3, 4, 4),
                        *click
                    ),
                },
                {annotation::test::page(pageId, "home", {anchorId})},
                {
                    annotation::test::placement(
                        pageId,
                        actionId,
                        annotation::test::pixelRect(3, 3, 4, 4)
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
        auto variantDocument() -> annotation::AuthoringDocument
        {
            auto const fingerprint = annotation::test::fingerprint(8, 8, 96, 96);
            auto const sourceId    = annotation::test::sourceId(k_sourceId);
            auto const anchorId    = annotation::test::recognizerId(k_anchorId);
            auto const awayId      = annotation::test::recognizerId(k_awayId);
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
                    annotation::test::anchorElement(
                        fingerprint,
                        anchorId,
                        "home_marker",
                        sourceId,
                        annotation::test::pixelRect(0, 0, 2, 2),
                        annotation::test::pixelRect(0, 0, 4, 4)
                    ),
                    annotation::test::anchorElement(
                        fingerprint,
                        awayId,
                        "away_marker",
                        sourceId,
                        annotation::test::pixelRect(4, 4, 2, 2),
                        annotation::test::pixelRect(3, 3, 4, 4)
                    ),
                },
                {
                    annotation::test::page(
                        pageId,
                        "home",
                        {anchorId},
                        {awayId}
                    ),
                },
                {},
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
        // to authorize can be observed picking the recognizer's own page rather
        // than the first one. "battle" survives losing its required anchor
        // because it still forbids one.
        [[nodiscard]]
        auto twoPageDocument() -> annotation::AuthoringDocument
        {
            auto const fingerprint = annotation::test::fingerprint(8, 8, 96, 96);
            auto const sourceId    = annotation::test::sourceId(k_sourceId);
            auto const anchorId    = annotation::test::recognizerId(k_anchorId);
            auto const awayId      = annotation::test::recognizerId(k_awayId);
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
                    annotation::test::anchorElement(
                        fingerprint,
                        anchorId,
                        "home_marker",
                        sourceId,
                        annotation::test::pixelRect(0, 0, 2, 2),
                        annotation::test::pixelRect(0, 0, 4, 4)
                    ),
                    annotation::test::anchorElement(
                        fingerprint,
                        awayId,
                        "battle_marker",
                        sourceId,
                        annotation::test::pixelRect(4, 4, 2, 2),
                        annotation::test::pixelRect(3, 3, 4, 4)
                    ),
                },
                {
                    annotation::test::page(homeId, "home", {anchorId}),
                    annotation::test::page(
                        battleId,
                        "battle",
                        {awayId},
                        {anchorId}
                    ),
                },
                {},
                {}
            );
            REQUIRE(created.has_value());
            return *std::move(created);
        }

        [[nodiscard]]
        auto recognizerName(
            AuthoringEditHistory const& history,
            std::size_t index
        ) -> std::string
        {
            return history.draft().recognizers.at(index).name;
        }

        // Reads a recognizer out of a draft by identity rather than by position,
        // so a test states which recognizer it means instead of depending on the
        // catalog's ordering.
        [[nodiscard]]
        auto recognizerIn(
            AuthoringDraft const& draft,
            annotation::RecognizerId id
        ) -> EditableRecognizer
        {
            auto const found = std::ranges::find(
                draft.recognizers,
                id,
                &EditableRecognizer::id
            );
            REQUIRE(found != draft.recognizers.end());
            return *found;
        }

        [[nodiscard]]
        auto pageIn(
            AuthoringDraft const& draft,
            annotation::PageId id
        ) -> EditablePage
        {
            auto const found = std::ranges::find(
                draft.pages,
                id,
                &EditablePage::id
            );
            REQUIRE(found != draft.pages.end());
            return *found;
        }
    }

    TEST_CASE("a fresh name avoids recognizer and page names alike")
    {
        // The catalog compares a page name against recognizer names too, so a
        // fresh name checked against one kind still collides.
        auto draft = makeAuthoringDraft(document());
        draft.pages.at(0).name       = "thing_1";
        draft.recognizers.at(0).name = "thing_2";

        CHECK(freshAuthoringName(draft, "thing") == "thing_3");
        CHECK(freshAuthoringName(draft, "other") == "other_1");
    }

    TEST_CASE("a page created from a screen anchors, requires, and expects it")
    {
        // None of the three can be authored on its own: an empty signature is
        // rejected, and an anchor joins a page only through a signature.
        auto const sourceId  = annotation::test::sourceId(k_sourceId);
        auto const pageId    = annotation::test::pageId(k_secondPageId);
        auto const anchorId  = annotation::test::recognizerId(k_awayId);
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

        auto const anchor = recognizerIn(created->draft, anchorId);
        CHECK(anchor.annotationType == annotation::AnnotationType::PageAnchor);
        CHECK(anchor.sourceId == sourceId);
        // An anchor joins a page through its signature, never a placement.
        CHECK(pagesPlacedOn(created->draft, anchorId).empty());

        auto const page = pageIn(created->draft, pageId);
        CHECK(std::ranges::contains(page.required, anchorId));

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
                .anchorId = annotation::test::recognizerId(k_awayId),
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
        auto const newId    = annotation::test::recognizerId(k_awayId);

        auto const spec = [&](PageMemberKind kind)
        {
            return PageMemberSpec{
                .recognizerId = newId,
                .pageId       = pageId,
                .sourceId     = sourceId,
                .templateRect = annotation::test::pixelRect(0, 0, 2, 2),
                .searchRoi    = annotation::test::pixelRect(0, 0, 4, 4),
                .similarityBasisPoints = 9'000U,
                .kind                  = kind,
            };
        };

        SUBCASE("an anchor enters the page signature and authorizes nothing")
        {
            auto const added = addPageMember(
                makeAuthoringDraft(document()),
                spec(PageMemberKind::Anchor)
            );
            REQUIRE(added.has_value());

            auto const recognizer = recognizerIn(added->draft, newId);
            CHECK(
                recognizer.annotationType
                == annotation::AnnotationType::PageAnchor
            );
            CHECK(pagesPlacedOn(added->draft, newId).empty());
            CHECK(
                std::ranges::contains(
                    pageIn(added->draft, pageId).required,
                    newId
                )
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

            auto const recognizer = recognizerIn(added->draft, newId);
            CHECK(
                recognizer.annotationType
                == annotation::AnnotationType::ActionTarget
            );
            CHECK(std::ranges::contains(pagesPlacedOn(added->draft, newId), pageId));
            CHECK_FALSE(
                std::ranges::contains(
                    pageIn(added->draft, pageId).required,
                    newId
                )
            );
            CHECK(buildAuthoringDocument(added->draft).has_value());
        }
    }

    TEST_CASE("sharing a region places the same element with a range of its own")
    {
        auto const actionId = annotation::test::recognizerId(k_actionId);
        auto const pageId   = annotation::test::pageId(k_secondPageId);
        auto const roi      = annotation::test::pixelRect(2, 2, 6, 6);

        auto draft = makeAuthoringDraft(twoPageDocument());
        // twoPageDocument has no action target, so give it the one being shared,
        // placed on the home page.
        draft.recognizers.emplace_back(
            EditableRecognizer{
                .id             = actionId,
                .name           = "back",
                .annotationType = annotation::AnnotationType::ActionTarget,
                .sourceId       = annotation::test::sourceId(k_sourceId),
                .templateRect   = annotation::test::pixelRect(4, 4, 2, 2),
                .searchRoi      = annotation::test::pixelRect(3, 3, 4, 4),
                .similarityBasisPoints = 9'000U,
                .defaultClick   = {},
            }
        );
        draft.placements.emplace_back(
            EditablePlacement{
                .pageId    = annotation::test::pageId(k_pageId),
                .elementId = actionId,
                .searchRoi = annotation::test::pixelRect(3, 3, 4, 4),
            }
        );

        auto const shared = shareRegionOnPage(
            std::move(draft),
            SharedRegionSpec{
                .elementId = actionId,
                .pageId    = pageId,
                .searchRoi = roi,
            }
        );
        REQUIRE(shared.has_value());

        // No copy is minted: the same element gains a second placement carrying
        // its own search region.
        CHECK(pagesPlacedOn(shared->draft, actionId).size() == 2U);
        auto const placed = std::ranges::find_if(
            shared->draft.placements,
            [&](EditablePlacement const& placement)
            {
                return placement.elementId == actionId
                    && placement.pageId == pageId;
            }
        );
        REQUIRE(placed != shared->draft.placements.end());
        CHECK(placed->searchRoi == roi);
        CHECK(recognizerIn(shared->draft, actionId).shared);
        CHECK(buildAuthoringDocument(shared->draft).has_value());
    }

    TEST_CASE("sharing a region onto a page that already has it is refused")
    {
        auto const actionId = annotation::test::recognizerId(k_actionId);
        auto const pageId   = annotation::test::pageId(k_pageId);

        auto const shared = shareRegionOnPage(
            makeAuthoringDraft(document()),
            SharedRegionSpec{
                .elementId = actionId,
                .pageId    = pageId,
                .searchRoi = annotation::test::pixelRect(0, 0, 8, 8),
            }
        );
        CHECK_FALSE(shared.has_value());
    }

    TEST_CASE("sharing a page anchor is refused")
    {
        auto const shared = shareRegionOnPage(
            makeAuthoringDraft(document()),
            SharedRegionSpec{
                .elementId = annotation::test::recognizerId(k_anchorId),
                .pageId    = annotation::test::pageId(k_pageId),
                .searchRoi = annotation::test::pixelRect(0, 0, 8, 8),
            }
        );
        CHECK_FALSE(shared.has_value());
    }

    TEST_CASE("marking a region reusable survives a document round trip")
    {
        // The mark is intent, stated before any second page exists, so it has to
        // be stored: nothing else in the document distinguishes a region the
        // author means to reuse from one they do not.
        auto const actionId = annotation::test::recognizerId(k_actionId);

        auto const marked = setRegionShared(
            makeAuthoringDraft(document()),
            actionId,
            true
        );
        REQUIRE(marked.has_value());
        CHECK(recognizerIn(*marked, actionId).shared);

        auto const rebuilt = buildAuthoringDocument(*marked);
        REQUIRE(rebuilt.has_value());
        CHECK(recognizerIn(makeAuthoringDraft(*rebuilt), actionId).shared);
    }

    TEST_CASE("only an interactive region can be marked reusable")
    {
        // A mark identifies a page through its signature, where reuse means
        // something else entirely.
        auto const marked = setRegionShared(
            makeAuthoringDraft(document()),
            annotation::test::recognizerId(k_anchorId),
            true
        );
        CHECK_FALSE(marked.has_value());
    }

    TEST_CASE("unmarking a region placed on several pages is now allowed")
    {
        // Under v2 the shared mark is pure intent and groups nothing, so it can
        // be taken off freely -- there are no copies to leave orphaned, only one
        // element placed on N pages.
        auto const actionId = annotation::test::recognizerId(k_actionId);

        auto draft = makeAuthoringDraft(document());
        draft.placements.emplace_back(
            EditablePlacement{
                .pageId    = annotation::test::pageId(k_secondPageId),
                .elementId = actionId,
                .searchRoi = annotation::test::pixelRect(3, 3, 4, 4),
            }
        );
        draft.pages.emplace_back(
            EditablePage{
                .id        = annotation::test::pageId(k_secondPageId),
                .name      = "battle",
                .required  = {},
                .forbidden = {annotation::test::recognizerId(k_anchorId)},
            }
        );
        REQUIRE(pagesPlacedOn(draft, actionId).size() == 2U);

        auto const unmarked = setRegionShared(std::move(draft), actionId, false);
        REQUIRE(unmarked.has_value());
        CHECK_FALSE(recognizerIn(*unmarked, actionId).shared);
    }

    TEST_CASE("moving an element's template moves it on every page it is placed")
    {
        // Drawing the element once is only worth anything if correcting it
        // corrects it everywhere; under v2 that is one element and two placements.
        auto const actionId = annotation::test::recognizerId(k_actionId);
        auto const moved    = annotation::test::pixelRect(3, 3, 2, 2);

        auto draft = makeAuthoringDraft(document());
        draft.placements.emplace_back(
            EditablePlacement{
                .pageId    = annotation::test::pageId(k_secondPageId),
                .elementId = actionId,
                .searchRoi = annotation::test::pixelRect(0, 0, 8, 8),
            }
        );

        auto const retemplated = setElementTemplateRect(
            std::move(draft),
            actionId,
            moved
        );
        REQUIRE(retemplated.has_value());
        CHECK(retemplated->otherPlacements == 2U);
        CHECK(recognizerIn(retemplated->draft, actionId).templateRect == moved);
        // Both placements still reference the one, corrected element.
        CHECK(pagesPlacedOn(retemplated->draft, actionId).size() == 2U);
    }

    TEST_CASE("a template that outgrows a placement's range is refused")
    {
        // Widening a range the author drew would enlarge both the search cost and
        // the surface for a false match, so the author is told to fix it.
        auto const actionId = annotation::test::recognizerId(k_actionId);

        auto draft = makeAuthoringDraft(document());
        draft.placements.emplace_back(
            EditablePlacement{
                .pageId    = annotation::test::pageId(k_secondPageId),
                .elementId = actionId,
                .searchRoi = annotation::test::pixelRect(4, 4, 3, 3),
            }
        );

        auto const retemplated = setElementTemplateRect(
            std::move(draft),
            actionId,
            annotation::test::pixelRect(0, 0, 5, 5)
        );
        CHECK_FALSE(retemplated.has_value());
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

    TEST_CASE("adding a member to a page outside the draft is refused")
    {
        auto const added = addPageMember(
            makeAuthoringDraft(document()),
            PageMemberSpec{
                .recognizerId = annotation::test::recognizerId(k_awayId),
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

    TEST_CASE("becoming an action target authorizes a page in the same edit")
    {
        // A page anchor cannot authorize a page and an action target must, so
        // neither half of this change can be committed on its own.
        auto const awayId = annotation::test::recognizerId(k_awayId);
        auto const pageId = annotation::test::pageId(k_pageId);

        auto const retyped = retypeRecognizer(
            makeAuthoringDraft(variantDocument()),
            awayId,
            annotation::AnnotationType::ActionTarget
        );
        REQUIRE(retyped.has_value());
        REQUIRE(retyped->authorizedPage.has_value());
        CHECK(*retyped->authorizedPage == pageId);

        auto const changed = recognizerIn(retyped->draft, awayId);
        CHECK(changed.annotationType == annotation::AnnotationType::ActionTarget);
        auto const placed = pagesPlacedOn(retyped->draft, awayId);
        REQUIRE(placed.size() == 1U);
        CHECK(placed.front() == pageId);

        // Only a page anchor may hold a signature role, so the recognizer is
        // withdrawn from the page it was forbidden on.
        auto const page = pageIn(retyped->draft, pageId);
        CHECK(page.forbidden.empty());
        CHECK(page.required.size() == 1U);
        CHECK(retyped->withdrawnRoles == 1U);
        CHECK(retyped->clearedAuthorizations == 0U);
        CHECK_FALSE(retyped->clearedClick);

        CHECK(buildAuthoringDocument(retyped->draft).has_value());
    }

    TEST_CASE("an action target is authorized on the page it anchored")
    {
        // The recognizer anchors the second page, so authorizing the first would
        // be arbitrary: it is known to appear on its own page and nowhere else.
        auto const awayId   = annotation::test::recognizerId(k_awayId);
        auto const homeId   = annotation::test::pageId(k_pageId);
        auto const battleId = annotation::test::pageId(k_secondPageId);

        auto const draft = makeAuthoringDraft(twoPageDocument());
        REQUIRE(draft.pages.front().id == homeId);

        auto const retyped = retypeRecognizer(
            draft,
            awayId,
            annotation::AnnotationType::ActionTarget
        );
        REQUIRE(retyped.has_value());
        REQUIRE(retyped->authorizedPage.has_value());
        CHECK(*retyped->authorizedPage == battleId);

        auto const placed = pagesPlacedOn(retyped->draft, awayId);
        REQUIRE(placed.size() == 1U);
        CHECK(placed.front() == battleId);

        CHECK(buildAuthoringDocument(retyped->draft).has_value());
    }

    TEST_CASE("becoming a page anchor drops the click and the authorizations")
    {
        auto const actionId = annotation::test::recognizerId(k_actionId);

        auto const retyped = retypeRecognizer(
            makeAuthoringDraft(document()),
            actionId,
            annotation::AnnotationType::PageAnchor
        );
        REQUIRE(retyped.has_value());
        CHECK_FALSE(retyped->authorizedPage.has_value());

        auto const changed = recognizerIn(retyped->draft, actionId);
        CHECK(changed.annotationType == annotation::AnnotationType::PageAnchor);
        CHECK(pagesPlacedOn(retyped->draft, actionId).empty());
        CHECK_FALSE(changed.defaultClick.has_value());

        // The conversion is lossy in both fields, so both are reported.
        CHECK(retyped->clearedAuthorizations == 1U);
        CHECK(retyped->clearedClick);
        CHECK(retyped->withdrawnRoles == 0U);

        CHECK(buildAuthoringDocument(retyped->draft).has_value());
    }

    TEST_CASE("becoming an info region keeps the authorizations without the click")
    {
        auto const actionId = annotation::test::recognizerId(k_actionId);
        auto const pageId   = annotation::test::pageId(k_pageId);

        auto const retyped = retypeRecognizer(
            makeAuthoringDraft(document()),
            actionId,
            annotation::AnnotationType::InfoRegion
        );
        REQUIRE(retyped.has_value());
        CHECK_FALSE(retyped->authorizedPage.has_value());

        auto const changed = recognizerIn(retyped->draft, actionId);
        CHECK(changed.annotationType == annotation::AnnotationType::InfoRegion);
        auto const placed = pagesPlacedOn(retyped->draft, actionId);
        REQUIRE(placed.size() == 1U);
        CHECK(placed.front() == pageId);
        CHECK_FALSE(changed.defaultClick.has_value());
        CHECK(retyped->clearedAuthorizations == 0U);
        CHECK(retyped->clearedClick);

        CHECK(buildAuthoringDocument(retyped->draft).has_value());
    }

    TEST_CASE("retyping the only recognizer a page names is refused")
    {
        // Withdrawing the anchor would leave the page naming nothing, which no
        // repair inside this recognizer can fix.
        auto const anchorId = annotation::test::recognizerId(k_anchorId);

        auto const retyped = retypeRecognizer(
            makeAuthoringDraft(document()),
            anchorId,
            annotation::AnnotationType::ActionTarget
        );
        REQUIRE_FALSE(retyped.has_value());
    }

    TEST_CASE("becoming an action target with no page to authorize is refused")
    {
        auto const anchorId = annotation::test::recognizerId(k_anchorId);

        // A freshly captured project: recognizers but no page yet.
        auto draft = makeAuthoringDraft(document());
        draft.pages.clear();
        draft.placements.clear();

        auto const retyped = retypeRecognizer(
            std::move(draft),
            anchorId,
            annotation::AnnotationType::ActionTarget
        );
        REQUIRE_FALSE(retyped.has_value());
    }

    TEST_CASE("retyping to the type already held changes nothing")
    {
        auto const anchorId = annotation::test::recognizerId(k_anchorId);
        auto const original = document();

        auto const retyped = retypeRecognizer(
            makeAuthoringDraft(original),
            anchorId,
            annotation::AnnotationType::PageAnchor
        );
        REQUIRE(retyped.has_value());
        CHECK_FALSE(retyped->authorizedPage.has_value());

        auto const rebuilt = buildAuthoringDocument(retyped->draft);
        REQUIRE(rebuilt.has_value());
        CHECK(
            annotation::serializeAuthoringDocument(*rebuilt)
            == annotation::serializeAuthoringDocument(original)
        );
    }

    TEST_CASE("deleting a recognizer withdraws it from the pages that name it")
    {
        auto const awayId   = annotation::test::recognizerId(k_awayId);
        auto const battleId = annotation::test::pageId(k_secondPageId);

        auto const deleted = deleteRecognizer(
            makeAuthoringDraft(twoPageDocument()),
            awayId
        );
        REQUIRE(deleted.has_value());
        CHECK(deleted->withdrawnRoles == 1U);
        CHECK(deleted->draft.recognizers.size() == 1U);
        CHECK(pageIn(deleted->draft, battleId).required.empty());

        CHECK(buildAuthoringDocument(deleted->draft).has_value());
    }

    TEST_CASE("deleting the only recognizer a page names is refused")
    {
        // The page would be left identifying no screen, and only the author can
        // say whether the page goes too or another anchor takes over.
        auto const anchorId = annotation::test::recognizerId(k_anchorId);

        auto const deleted = deleteRecognizer(
            makeAuthoringDraft(document()),
            anchorId
        );
        REQUIRE_FALSE(deleted.has_value());
    }

    TEST_CASE("deleting a page clears it from the recognizers that authorize it")
    {
        // The action target authorizes the page and one other, so the deletion
        // leaves it authorized somewhere and may proceed.
        auto const actionId = annotation::test::recognizerId(k_actionId);
        auto const homeId   = annotation::test::pageId(k_pageId);
        auto const battleId = annotation::test::pageId(k_secondPageId);

        auto widened = makeAuthoringDraft(document());
        // Place the action target on the second page too, so deleting the first
        // leaves it placed and may proceed.
        widened.placements.emplace_back(
            EditablePlacement{
                .pageId    = battleId,
                .elementId = actionId,
                .searchRoi = annotation::test::pixelRect(3, 3, 4, 4),
            }
        );
        // Two pages may not carry the same signature, so the second page forbids
        // the anchor the first requires.
        widened.pages.emplace_back(
            EditablePage{
                .id        = battleId,
                .name      = "battle",
                .required  = {},
                .forbidden = {annotation::test::recognizerId(k_anchorId)},
            }
        );
        // The document's regression expects the page under test to resolve, which
        // is refused on its own; this case is about the placements.
        REQUIRE(widened.regressions.size() == 1U);
        widened.regressions.at(0).expectation = annotation::UnknownRegression{};

        auto const deleted = deletePage(std::move(widened), homeId);
        REQUIRE(deleted.has_value());
        CHECK(deleted->clearedAuthorizations == 1U);
        CHECK(deleted->draft.pages.size() == 1U);
        CHECK(pagesPlacedOn(deleted->draft, actionId).size() == 1U);
    }

    TEST_CASE("deleting an action target's only authorized page is refused")
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

        auto draft = makeAuthoringDraft(document());
        draft.placements.clear();
        for (auto& recognizer : draft.recognizers)
        {
            recognizer.annotationType = annotation::AnnotationType::PageAnchor;
        }
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
        draft.recognizers.clear();
        draft.placements.clear();
        draft.pages.clear();
        REQUIRE(draft.regressions.size() == 1U);

        auto const deleted = deleteSource(std::move(draft), sourceId);
        REQUIRE(deleted.has_value());
        CHECK(deleted->removedRegressions == 1U);
        CHECK(deleted->draft.sources.empty());
        CHECK(deleted->draft.regressions.empty());

        CHECK(buildAuthoringDocument(deleted->draft).has_value());
    }

    TEST_CASE("deleting a source still carrying recognizers is refused")
    {
        // A recognizer's rectangles are only meaningful against the image they
        // were drawn on, so the source cannot leave without them.
        auto const sourceId = annotation::test::sourceId(k_sourceId);

        auto const deleted = deleteSource(
            makeAuthoringDraft(document()),
            sourceId
        );
        REQUIRE_FALSE(deleted.has_value());
    }

    TEST_CASE("retyping a recognizer the draft does not hold is refused")
    {
        auto const strangerId = annotation::test::recognizerId(k_awayId);

        auto const retyped = retypeRecognizer(
            makeAuthoringDraft(document()),
            strangerId,
            annotation::AnnotationType::InfoRegion
        );
        REQUIRE_FALSE(retyped.has_value());
    }

    TEST_CASE("authoring draft preserves the complete canonical document")
    {
        auto const documents = std::vector<annotation::AuthoringDocument>{
            document(),
            variantDocument(),
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

        renamed.recognizers.at(0).name = "renamed_marker";

        auto const applied = history.apply(renamed);
        REQUIRE(applied.has_value());
        CHECK(*applied);
        CHECK(recognizerName(history, 0) == "renamed_marker");
        CHECK(history.canUndo());
        CHECK_FALSE(history.canRedo());

        CHECK(history.undo());
        CHECK(recognizerName(history, 0) == "home_marker");
        CHECK_FALSE(history.canUndo());
        CHECK(history.canRedo());
        CHECK(
            annotation::serializeAuthoringDocument(history.document())
            == annotation::serializeAuthoringDocument(document())
        );

        CHECK(history.redo());
        CHECK(recognizerName(history, 0) == "renamed_marker");
        CHECK(history.canUndo());
        CHECK_FALSE(history.canRedo());
    }

    TEST_CASE("authoring history rejects invalid drafts without changing history")
    {
        auto history = AuthoringEditHistory{document()};
        auto renamed = history.draft();

        renamed.recognizers.at(0).name = "renamed_marker";
        REQUIRE(history.apply(renamed).has_value());
        REQUIRE(history.undo());
        REQUIRE(history.canRedo());

        auto invalid = history.draft();

        invalid.recognizers.at(0).name.clear();

        auto const applied = history.apply(invalid);
        REQUIRE_FALSE(applied.has_value());
        CHECK(recognizerName(history, 0) == "home_marker");
        CHECK_FALSE(history.canUndo());
        CHECK(history.canRedo());
        CHECK(history.redo());
        CHECK(recognizerName(history, 0) == "renamed_marker");
    }

    TEST_CASE("authoring history ignores identical edits and clears abandoned redo")
    {
        auto history = AuthoringEditHistory{document()};

        auto const unchanged = history.apply(history.draft());
        REQUIRE(unchanged.has_value());
        CHECK_FALSE(*unchanged);
        CHECK_FALSE(history.canUndo());

        auto first = history.draft();

        first.recognizers.at(0).name = "first_name";
        REQUIRE(history.apply(first).has_value());
        REQUIRE(history.undo());
        CHECK(history.canRedo());

        auto const pending = history.apply(history.draft());
        REQUIRE(pending.has_value());
        CHECK_FALSE(*pending);
        CHECK(history.canRedo());
        CHECK_FALSE(history.canUndo());

        auto branch = history.draft();

        branch.recognizers.at(0).name = "branch_name";
        REQUIRE(history.apply(branch).has_value());
        CHECK(recognizerName(history, 0) == "branch_name");
        CHECK_FALSE(history.canRedo());
    }

    TEST_CASE("authoring history replays redo entries in reverse order")
    {
        auto history = AuthoringEditHistory{document()};
        CHECK_FALSE(history.redo());

        auto first = history.draft();

        first.recognizers.at(0).name = "first_name";
        REQUIRE(history.apply(first).has_value());

        auto second = history.draft();

        second.recognizers.at(0).name = "second_name";
        REQUIRE(history.apply(second).has_value());

        REQUIRE(history.undo());
        REQUIRE(history.undo());
        CHECK(recognizerName(history, 0) == "home_marker");

        CHECK(history.redo());
        CHECK(recognizerName(history, 0) == "first_name");
        CHECK(history.redo());
        CHECK(recognizerName(history, 0) == "second_name");
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

            next.recognizers.at(0).name = "marker_" + std::to_string(index);

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
        CHECK(recognizerName(history, 0) == "marker_0");
    }
}
