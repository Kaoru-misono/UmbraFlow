#include "../annotation/test-helpers.hpp"
#include "authoring-fixture.hpp"

#include <authoring-edit.hpp>
#include <edit-page.hpp>
#include <page-view.hpp>

#include <annotation/authoring-document.hpp>
#include <annotation/resource.hpp>
#include <annotation/content-hash.hpp>

#include <core/error/result.hpp>

#include <doctest/doctest.h>

#include <algorithm>
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
        constexpr auto k_sourceId  = "00000000-0000-0000-0000-000000000801";
        constexpr auto k_anchorId  = "00000000-0000-0000-0000-000000000811";
        constexpr auto k_awayId    = "00000000-0000-0000-0000-000000000812";
        constexpr auto k_regionId  = "00000000-0000-0000-0000-000000000821";
        constexpr auto k_homePage  = "00000000-0000-0000-0000-000000000831";
        constexpr auto k_awayPage  = "00000000-0000-0000-0000-000000000832";
        constexpr auto k_regId     = "00000000-0000-0000-0000-000000000841";

        // A two-page project: "home" identified by home_marker with an
        // interactive region on it, and "away" identified by away_marker. The
        // source is recorded as an example of the home page. Enough shape to
        // reach every EditPage operation the phase-1 plan names.
        [[nodiscard]]
        auto document() -> annotation::AuthoringDocument
        {
            auto const fingerprint = annotation::test::fingerprint(8, 8, 96, 96);
            auto const sourceId    = annotation::test::sourceId(k_sourceId);
            auto const anchorId    = annotation::test::elementId(k_anchorId);
            auto const awayId      = annotation::test::elementId(k_awayId);
            auto const regionId    = annotation::test::elementId(k_regionId);
            auto const homePage    = annotation::test::pageId(k_homePage);
            auto const awayPage    = annotation::test::pageId(k_awayPage);
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
                        "away_marker",
                        sourceId,
                        annotation::test::pixelRect(0, 0, 2, 2),
                        annotation::test::pixelRect(0, 0, 4, 4)
                    ),
                    test::clickableElement(
                        fingerprint,
                        regionId,
                        "daily_button",
                        sourceId,
                        annotation::test::pixelRect(4, 4, 2, 2),
                        annotation::test::pixelRect(3, 3, 4, 4)
                    ),
                },
                {
                    annotation::test::page(homePage, "home"),
                    annotation::test::page(awayPage, "away"),
                },
                {
                    annotation::test::reference(
                        homePage,
                        anchorId,
                        annotation::test::identifiesAs()
                    ),
                    annotation::test::reference(
                        awayPage,
                        awayId,
                        annotation::test::identifiesAs()
                    ),
                    annotation::test::reference(
                        homePage,
                        regionId,
                        annotation::test::interacts(),
                        annotation::Holding::Owned,
                        annotation::test::pixelRect(3, 3, 4, 4)
                    ),
                },
                {
                    annotation::RegressionCase{
                        annotation::RegressionSpec{
                            .id       = annotation::test::regressionId(k_regId),
                            .sourceId = sourceId,
                            .classification =
                                annotation::RegressionClassification::Positive,
                            .expectation    = annotation::ResolvedRegression{
                                .pageId = homePage,
                            },
                        }
                    },
                }
            );
            REQUIRE(created.has_value());
            return *std::move(created);
        }

        [[nodiscard]]
        auto history() -> AuthoringEditHistory
        {
            return AuthoringEditHistory{document()};
        }

        // Opens the page against the history's current version, which is what
        // every caller that owns a history does.
        [[nodiscard]]
        auto openPage(
            AuthoringEditHistory const& source,
            annotation::PageId id
        ) -> Result<EditPage>
        {
            return EditPage::open(source.draft(), source.revision(), id);
        }

        [[nodiscard]]
        auto elementName(
            AuthoringEditHistory const& source,
            annotation::ElementId id
        ) -> std::string
        {
            for (auto const& element : source.document().catalog().elements())
            {
                if (element.id() == id)
                {
                    return element.name().value();
                }
            }
            return {};
        }
    }

    TEST_CASE("minted resource ids are well-formed version-4 UUIDs")
    {
        auto const id   = mintResourceId();
        auto const text = id.toString();

        REQUIRE(text.size() == 36U);
        CHECK(text.at(8) == '-');
        CHECK(text.at(13) == '-');
        CHECK(text.at(18) == '-');
        CHECK(text.at(23) == '-');

        // RFC 4122: the version nibble is 4 and the appearance nibble is 8..b.
        CHECK(text.at(14) == '4');
        auto const appearance = text.at(19);
        CHECK(
            (
                appearance == '8'
                || appearance == '9'
                || appearance == 'a'
                || appearance == 'b'
            )
        );

        auto const reparsed = annotation::ResourceId::parse(text);
        REQUIRE(reparsed.has_value());
        CHECK(*reparsed == id);
    }

    TEST_CASE("distinct mints do not collide")
    {
        CHECK(mintResourceId() != mintResourceId());
    }

    TEST_CASE("a drawn template seeds a search region that contains it")
    {
        auto const roi = searchRoiForDrawnTemplate(
            annotation::test::pixelRect(2, 2, 3, 3),
            8,
            8
        );
        REQUIRE(roi.has_value());
        // Grown by the template extent on every side, clamped to the frame.
        CHECK(*roi == annotation::test::pixelRect(0, 0, 8, 8));

        auto const inner = searchRoiForDrawnTemplate(
            annotation::test::pixelRect(4, 4, 2, 2),
            100,
            100
        );
        REQUIRE(inner.has_value());
        CHECK(*inner == annotation::test::pixelRect(2, 2, 6, 6));
        // The seed always encloses the template it was derived from.
        CHECK(inner->x() <= 4U);
        CHECK(inner->x() + inner->width() >= 6U);
    }

    TEST_CASE("an opened page edits its own copy until the commit is applied")
    {
        auto edits = history();
        auto const anchorId = annotation::test::elementId(k_anchorId);

        auto page = openPage(edits, annotation::test::pageId(k_homePage));
        REQUIRE(page.has_value());

        auto anchor = page->anchor(anchorId);
        REQUIRE(anchor.has_value());
        CHECK(anchor->name() == "home_marker");
        CHECK(anchor->rename("renamed_marker").has_value());

        // The editor owns a copy of the whole draft, so the live document is
        // untouched no matter how much is edited through it.
        auto const committed = std::move(*page).commit();
        CHECK(elementName(edits, anchorId) == "home_marker");

        auto const applied = applyCommittedPage(edits, committed);
        REQUIRE(applied.has_value());
        CHECK(*applied);
        CHECK(elementName(edits, anchorId) == "renamed_marker");
        CHECK(edits.canUndo());
    }

    TEST_CASE("a commit over a stale base is refused after an undo")
    {
        auto edits = history();
        auto const anchorId = annotation::test::elementId(k_anchorId);

        // A first edit lands so there is something to undo, and to advance the
        // revision the second EditPage will be opened against.
        {
            auto first = openPage(edits, annotation::test::pageId(k_homePage));
            REQUIRE(first.has_value());
            REQUIRE(first->anchor(anchorId).has_value());
            REQUIRE(first->anchor(anchorId)->rename("first_name").has_value());
            REQUIRE(
                applyCommittedPage(edits, std::move(*first).commit()).has_value()
            );
        }
        REQUIRE(elementName(edits, anchorId) == "first_name");

        auto page = openPage(edits, annotation::test::pageId(k_homePage));
        REQUIRE(page.has_value());
        REQUIRE(page->anchor(anchorId)->rename("second_name").has_value());

        // The author undoes between opening the page and committing it, so the
        // version this draft was built against is no longer current.
        REQUIRE(edits.undo());
        REQUIRE(elementName(edits, anchorId) == "home_marker");

        // The stale commit must not resurrect the undone version.
        auto const applied = applyCommittedPage(edits, std::move(*page).commit());
        REQUIRE_FALSE(applied.has_value());
        CHECK(elementName(edits, anchorId) == "home_marker");
    }

    TEST_CASE("a rejected commit moves nothing")
    {
        auto edits = history();
        auto const anchorId = annotation::test::elementId(k_anchorId);

        auto page = openPage(edits, annotation::test::pageId(k_homePage));
        REQUIRE(page.has_value());
        // An empty name cannot survive the rebuild, so the commit is refused.
        REQUIRE(page->anchor(anchorId)->rename("").has_value());

        auto const applied = applyCommittedPage(edits, std::move(*page).commit());
        REQUIRE_FALSE(applied.has_value());
        CHECK(elementName(edits, anchorId) == "home_marker");
        CHECK_FALSE(edits.canUndo());
    }

    TEST_CASE("placeAnchor names the new mark and one commit installs it")
    {
        auto edits = history();

        auto page = openPage(edits, annotation::test::pageId(k_homePage));
        REQUIRE(page.has_value());
        auto added = page->placeAnchor(
            EditPage::NewAnchorSpec{.sourceId = annotation::test::sourceId(k_sourceId)}
        );
        REQUIRE(added.has_value());
        auto const addedId = added->id;

        auto const applied = applyCommittedPage(edits, std::move(*page).commit());
        REQUIRE(applied.has_value());
        CHECK(*applied);
        CHECK(elementName(edits, addedId) == "anchor_1");
        // One transaction, one undo entry.
        CHECK(edits.canUndo());
    }

    TEST_CASE("placeDrawn creates a member from the rectangle it was given")
    {
        auto edits = history();
        auto const sourceId = annotation::test::sourceId(k_sourceId);
        auto const homePage = annotation::test::pageId(k_homePage);
        auto const drawn    = annotation::test::pixelRect(2, 2, 3, 3);

        auto page = openPage(edits, homePage);
        REQUIRE(page.has_value());
        auto added = page->placeDrawn(
            EditPage::NewDrawnMemberSpec{
                .sourceId     = sourceId,
                .kind         = PageMemberKind::ActionTarget,
                .templateRect = drawn,
            }
        );
        REQUIRE(added.has_value());
        CHECK(added->kind == PageMemberKind::ActionTarget);
        auto const newId = added->id;

        REQUIRE(
            applyCommittedPage(edits, std::move(*page).commit()).has_value()
        );
        // One transaction, one undo entry.
        CHECK(edits.canUndo());

        auto const* element =
            edits.document().catalog().findElement(newId);
        REQUIRE(element != nullptr);
        REQUIRE(element->appearances().size() == 1U);
        CHECK(element->appearances().front().templateRect == drawn);
        CHECK(element->capabilities().hasInteract());

        // The element's search region is the one seeded from the given template
        // -- grown by its extent and clamped to the 8x8 frame -- not the whole
        // frame by luck. It always encloses the template, and the page's
        // reference inherits it rather than pinning a copy.
        CHECK(element->searchRoi() == annotation::test::pixelRect(0, 0, 8, 8));
        auto const* reference = edits.document().catalog().findReference(
            homePage,
            newId
        );
        REQUIRE(reference != nullptr);
        CHECK_FALSE(reference->searchRoi.has_value());
    }

    TEST_CASE("placeDrawn refuses a template that is too small")
    {
        auto const edits    = history();
        auto const sourceId = annotation::test::sourceId(k_sourceId);

        auto page = openPage(edits, annotation::test::pageId(k_homePage));
        REQUIRE(page.has_value());
        auto const added = page->placeDrawn(
            EditPage::NewDrawnMemberSpec{
                .sourceId     = sourceId,
                .kind         = PageMemberKind::Anchor,
                .templateRect = annotation::test::pixelRect(0, 0, 1, 1),
            }
        );
        REQUIRE_FALSE(added.has_value());
    }

    TEST_CASE("placeExisting authorizes an existing region on this page")
    {
        auto edits = history();
        auto const regionId = annotation::test::elementId(k_regionId);
        auto const awayPage = annotation::test::pageId(k_awayPage);

        auto page = openPage(edits, awayPage);
        REQUIRE(page.has_value());
        CHECK(page->placeExisting(regionId).has_value());
        REQUIRE(
            applyCommittedPage(edits, std::move(*page).commit()).has_value()
        );

        // The reference IS the authorisation, and it is Referenced rather than
        // Owned: these pixels are borrowed from the page that owns them.
        auto const* reference = edits.document().catalog().findReference(
            awayPage,
            regionId
        );
        REQUIRE(reference != nullptr);
        CHECK(reference->exercised.hasInteract());
        CHECK(reference->holding == annotation::Holding::Referenced);
    }

    TEST_CASE("classifyScreen sets an existing case's classification")
    {
        auto edits = history();
        auto const sourceId = annotation::test::sourceId(k_sourceId);

        auto page = openPage(edits, annotation::test::pageId(k_homePage));
        REQUIRE(page.has_value());
        CHECK(
            page->classifyScreen(
                sourceId,
                annotation::RegressionClassification::Negative
            ).has_value()
        );
        REQUIRE(
            applyCommittedPage(edits, std::move(*page).commit()).has_value()
        );

        auto classification =
            std::optional<annotation::RegressionClassification>{};
        for (auto const& regression : edits.document().regressions())
        {
            if (regression.sourceId() == sourceId)
            {
                classification = regression.classification();
            }
        }
        REQUIRE(classification.has_value());
        CHECK(*classification == annotation::RegressionClassification::Negative);
    }

    TEST_CASE("setTemplateRect moves the one element on every page it is placed")
    {
        auto edits = history();
        auto const regionId = annotation::test::elementId(k_regionId);
        auto const homePage = annotation::test::pageId(k_homePage);
        auto const awayPage = annotation::test::pageId(k_awayPage);
        auto const newRect  = annotation::test::pixelRect(3, 3, 3, 3);

        auto page = openPage(edits, homePage);
        REQUIRE(page.has_value());

        // Put the region on a second page, so a template correction has more
        // than one reference to be seen by.
        REQUIRE(page->region(regionId)->referenceOnPage(awayPage).has_value());
        REQUIRE(page->region(regionId)->pagesReferencing().size() == 2U);

        REQUIRE(page->region(regionId)->setTemplateRect(newRect).has_value());
        REQUIRE(
            applyCommittedPage(edits, std::move(*page).commit()).has_value()
        );

        // One element, one appearance, corrected once -- yet referenced by both
        // pages, so both see the correction and neither holds a copy.
        auto const* element =
            edits.document().catalog().findElement(regionId);
        REQUIRE(element != nullptr);
        REQUIRE(element->appearances().size() == 1U);
        CHECK(element->appearances().front().templateRect == newRect);
        CHECK(
            edits.document().catalog().findReference(homePage, regionId)
            != nullptr
        );
        CHECK(
            edits.document().catalog().findReference(awayPage, regionId)
            != nullptr
        );
    }

    TEST_CASE("a page view assembles the authored data and ids")
    {
        auto const edits    = history();
        auto const anchorId = annotation::test::elementId(k_anchorId);
        auto const regionId = annotation::test::elementId(k_regionId);

        auto page = openPage(edits, annotation::test::pageId(k_homePage));
        REQUIRE(page.has_value());
        auto const view = page->view();

        CHECK(view.id == annotation::test::pageId(k_homePage));
        CHECK(view.name == "home");
        REQUIRE(view.claimedScreen.has_value());
        CHECK(*view.claimedScreen == annotation::test::sourceId(k_sourceId));

        REQUIRE(view.identifiedBy.size() == 1U);
        CHECK(view.identifiedBy.front().id == anchorId);
        CHECK(view.identifiedBy.front().name == "home_marker");

        REQUIRE(view.regions.size() == 1U);
        CHECK(view.regions.front().id == regionId);
        CHECK(view.regions.front().name == "daily_button");

        // Two pages, so PageView::all yields both, in draft order.
        auto const views = PageView::all(edits.draft());
        REQUIRE(views.size() == 2U);
        CHECK(views.front().name == "home");
        CHECK(views.back().name == "away");
    }

    TEST_CASE("every capability a page exercises puts the element in a group")
    {
        // The bug this pins: a page member that no group rendered was reachable
        // only through undo. Reachability is a property of the VIEW, so the
        // assertion is on the view. Under a capability set one element sits in
        // several groups at once, which is the case the model change exists for.
        auto const edits    = history();
        auto const regionId = annotation::test::elementId(k_regionId);
        auto const homePage = annotation::test::pageId(k_homePage);

        auto draft           = edits.draft();
        auto const reference = std::ranges::find_if(
            draft.references,
            [&](EditableReference const& candidate)
            {
                return candidate.pageId == homePage
                    && candidate.elementId == regionId;
            }
        );
        REQUIRE(reference != draft.references.end());

        auto const target = std::ranges::find(
            draft.elements,
            regionId,
            &EditableElement::id
        );
        REQUIRE(target != draft.elements.end());
        target->capabilities.read = annotation::Read{};
        reference->exercised.read = annotation::ExercisedRead{};

        auto const view = PageView::of(draft, homePage);
        REQUIRE(view.has_value());

        auto const has = [&](std::vector<PageView::MemberRow> const& rows)
        {
            return std::ranges::any_of(
                rows,
                [&](PageView::MemberRow const& row) { return row.id == regionId; }
            );
        };
        CHECK(has(view->regions));
        CHECK(has(view->infos));
        CHECK_FALSE(has(view->identifiedBy));

        // And it is a document the model accepts, so the two groups describe a
        // state an author can actually reach.
        CHECK(buildAuthoringDocument(draft).has_value());
    }

    TEST_CASE("createFrom builds a page around a screen and its first anchor")
    {
        auto edits = history();
        auto const sourceId = annotation::test::sourceId(k_sourceId);

        auto page = EditPage::createFrom(
            edits.draft(),
            edits.revision(),
            sourceId
        );
        REQUIRE(page.has_value());

        // The page comes with exactly one identifying mark.
        auto const view = page->view();
        REQUIRE(view.identifiedBy.size() == 1U);
        auto const anchorId = view.identifiedBy.front().id;

        REQUIRE(
            applyCommittedPage(edits, std::move(*page).commit()).has_value()
        );
        CHECK(edits.document().catalog().pages().size() == 3U);
        CHECK(elementName(edits, anchorId).empty() == false);
    }

    TEST_CASE("placeRegion authorizes a fresh interactive region on this page")
    {
        auto edits = history();
        auto const awayPage = annotation::test::pageId(k_awayPage);

        auto page = openPage(edits, awayPage);
        REQUIRE(page.has_value());
        auto added = page->placeRegion(
            EditPage::NewRegionSpec{.sourceId = annotation::test::sourceId(k_sourceId)}
        );
        REQUIRE(added.has_value());
        auto const newId = added->id;
        REQUIRE(
            applyCommittedPage(edits, std::move(*page).commit()).has_value()
        );

        auto const* element =
            edits.document().catalog().findElement(newId);
        REQUIRE(element != nullptr);
        CHECK(element->capabilities().hasInteract());
        auto const* reference = edits.document().catalog().findReference(
            awayPage,
            newId
        );
        REQUIRE(reference != nullptr);
        CHECK(reference->exercised.hasInteract());
    }

    TEST_CASE("claimScreen records the screen as this page's example")
    {
        auto edits = history();
        auto const awayPage = annotation::test::pageId(k_awayPage);
        auto const sourceId = annotation::test::sourceId(k_sourceId);

        auto page = openPage(edits, awayPage);
        REQUIRE(page.has_value());
        CHECK(page->claimScreen(sourceId).has_value());
        REQUIRE(
            applyCommittedPage(edits, std::move(*page).commit()).has_value()
        );

        // A screen resolves to one page, so the claim re-points the existing
        // case; the away page's view now names the source as its example.
        auto reopened = openPage(edits, awayPage);
        REQUIRE(reopened.has_value());
        auto const view = reopened->view();
        REQUIRE(view.claimedScreen.has_value());
        CHECK(*view.claimedScreen == sourceId);
    }

    namespace
    {
        // The base project with the interactive region placed on the away page as
        // well as home, at a different search rectangle on each, so an edit to one
        // page's range can be checked against the other's.
        [[nodiscard]]
        auto twoPlacementDocument() -> annotation::AuthoringDocument
        {
            auto const fingerprint = annotation::test::fingerprint(8, 8, 96, 96);
            auto const sourceId    = annotation::test::sourceId(k_sourceId);
            auto const anchorId    = annotation::test::elementId(k_anchorId);
            auto const awayId      = annotation::test::elementId(k_awayId);
            auto const regionId    = annotation::test::elementId(k_regionId);
            auto const homePage    = annotation::test::pageId(k_homePage);
            auto const awayPage    = annotation::test::pageId(k_awayPage);
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
                        "away_marker",
                        sourceId,
                        annotation::test::pixelRect(0, 0, 2, 2),
                        annotation::test::pixelRect(0, 0, 4, 4)
                    ),
                    test::clickableElement(
                        fingerprint,
                        regionId,
                        "daily_button",
                        sourceId,
                        annotation::test::pixelRect(4, 4, 2, 2),
                        annotation::test::pixelRect(3, 3, 4, 4)
                    ),
                },
                {
                    annotation::test::page(homePage, "home"),
                    annotation::test::page(awayPage, "away"),
                },
                {
                    annotation::test::reference(
                        homePage,
                        anchorId,
                        annotation::test::identifiesAs()
                    ),
                    annotation::test::reference(
                        awayPage,
                        awayId,
                        annotation::test::identifiesAs()
                    ),
                    annotation::test::reference(
                        homePage,
                        regionId,
                        annotation::test::interacts(),
                        annotation::Holding::Owned,
                        annotation::test::pixelRect(3, 3, 4, 4)
                    ),
                    annotation::test::reference(
                        awayPage,
                        regionId,
                        annotation::test::interacts(),
                        annotation::Holding::Referenced,
                        annotation::test::pixelRect(4, 4, 4, 4)
                    ),
                },
                {
                    annotation::RegressionCase{
                        annotation::RegressionSpec{
                            .id       = annotation::test::regressionId(k_regId),
                            .sourceId = sourceId,
                            .classification =
                                annotation::RegressionClassification::Positive,
                            .expectation    = annotation::ResolvedRegression{
                                .pageId = homePage,
                            },
                        }
                    },
                }
            );
            REQUIRE(created.has_value());
            return *std::move(created);
        }

        // The region searched for one element on one page: the reference's
        // refinement when it made one, the element's own otherwise.
        [[nodiscard]]
        auto referenceRoi(
            annotation::AuthoringDocument const& source,
            annotation::PageId page,
            annotation::ElementId element
        ) -> std::optional<PixelRect>
        {
            auto const* p_reference = source.catalog().findReference(
                page,
                element
            );
            auto const* p_element = source.findElement(element);
            if (p_reference == nullptr || p_element == nullptr)
            {
                return std::nullopt;
            }
            return p_reference->searchRoi.value_or(p_element->searchRoi());
        }
    }

    TEST_CASE("a range edit through a page moves only that page's reference")
    {
        auto edits = AuthoringEditHistory{twoPlacementDocument()};
        auto const regionId = annotation::test::elementId(k_regionId);
        auto const homePage = annotation::test::pageId(k_homePage);
        auto const awayPage = annotation::test::pageId(k_awayPage);
        auto const edited   = annotation::test::pixelRect(1, 1, 6, 6);

        auto page = openPage(edits, homePage);
        REQUIRE(page.has_value());
        REQUIRE(page->region(regionId)->setSearchRoi(edited).has_value());
        REQUIRE(
            applyCommittedPage(edits, std::move(*page).commit()).has_value()
        );

        // The home reference moved; the away reference and the element's own
        // default range are both untouched -- the defect this covers moved all of
        // them at once.
        CHECK(referenceRoi(edits.document(), homePage, regionId) == edited);
        CHECK(
            referenceRoi(edits.document(), awayPage, regionId)
            == annotation::test::pixelRect(4, 4, 4, 4)
        );
        auto const* element = edits.document().findElement(regionId);
        REQUIRE(element != nullptr);
        CHECK(element->searchRoi() == annotation::test::pixelRect(3, 3, 4, 4));
    }

    TEST_CASE("a range edit from a page that does not reference it moves the default")
    {
        // The region is placed on home only, so editing its range from the away
        // page has no reference to refine and falls back to the element's own
        // default rather than silently dropping the edit.
        auto edits = history();
        auto const regionId = annotation::test::elementId(k_regionId);
        auto const homePage = annotation::test::pageId(k_homePage);
        auto const awayPage = annotation::test::pageId(k_awayPage);
        auto const edited   = annotation::test::pixelRect(1, 1, 6, 6);

        auto page = openPage(edits, awayPage);
        REQUIRE(page.has_value());
        REQUIRE(page->region(regionId)->setSearchRoi(edited).has_value());
        REQUIRE(
            applyCommittedPage(edits, std::move(*page).commit()).has_value()
        );

        auto const* element = edits.document().findElement(regionId);
        REQUIRE(element != nullptr);
        CHECK(element->searchRoi() == edited);
        CHECK(
            referenceRoi(edits.document(), homePage, regionId)
            == annotation::test::pixelRect(3, 3, 4, 4)
        );
    }
}
