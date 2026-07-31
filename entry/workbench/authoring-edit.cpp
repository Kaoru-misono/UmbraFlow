#include "authoring-edit.hpp"

#include <annotation/capabilities.hpp>
#include <annotation/resource.hpp>

#include <core/error/contracts.hpp>
#include <core/safety/checked-access.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uf::workbench
{
    namespace
    {
        [[nodiscard]]
        auto missingSource(annotation::SourceId id) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "source {} is not part of this draft",
                    id.value().toString()
                )
            );
        }

        [[nodiscard]]
        auto missingElement(annotation::ElementId id) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "recognizer {} is not part of this draft",
                    id.value().toString()
                )
            );
        }

        [[nodiscard]]
        auto missingPage(annotation::PageId id) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "page {} is not part of this draft",
                    id.value().toString()
                )
            );
        }

        // The mutable half of primaryVariant, kept here because only the edit
        // transactions in this file write an appearance.
        [[nodiscard]]
        auto primaryVariantOf(
            EditableRecognizer& recognizer
        ) noexcept -> EditableVariant*
        {
            return recognizer.variants.empty()
                ? nullptr
                : &recognizer.variants.front();
        }

        [[nodiscard]]
        auto findRecognizerIn(
            AuthoringDraft& draft,
            annotation::ElementId id
        ) noexcept -> EditableRecognizer*
        {
            auto const found = std::ranges::find(
                draft.recognizers,
                id,
                &EditableRecognizer::id
            );
            return found == draft.recognizers.end() ? nullptr : &*found;
        }

        [[nodiscard]]
        auto findReferenceIn(
            AuthoringDraft& draft,
            annotation::PageId pageId,
            annotation::ElementId elementId
        ) noexcept -> EditableReference*
        {
            auto const found = std::ranges::find_if(
                draft.references,
                [pageId, elementId](EditableReference const& reference)
                {
                    return reference.pageId == pageId
                        && reference.elementId == elementId;
                }
            );
            return found == draft.references.end() ? nullptr : &*found;
        }

        // Why a page cannot exercise a capability its element does not
        // declare, one sentence each so the refusal says what the element
        // would have to become rather than only that it is not that.
        constexpr auto k_undeclaredIdentify = std::string_view{
            "is not an identifying mark, so no page's signature can be built "
            "out of it"
        };
        constexpr auto k_undeclaredInteract = std::string_view{
            "receives no action, so no page can authorise a click on it"
        };
        constexpr auto k_undeclaredRead = std::string_view{
            "holds no readable text, so no page can read it"
        };

        // Every rule one reference row has to satisfy, stated once so the verb
        // that mints a row and the verb that edits one cannot drift apart. The
        // catalog checks the same rules over the built document; refusing here
        // is what lets the message name the element the author typed.
        [[nodiscard]]
        auto validateReference(
            EditableRecognizer const& element,
            EditableExercised const& exercised,
            std::optional<PixelRect> const& searchRoi
        ) -> Status
        {
            if (
                !exercised.identify.has_value()
                && !exercised.interact.has_value()
                && !exercised.read.has_value()
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "that page would reference \"{}\" and use it for "
                        "nothing",
                        element.name
                    )
                );
            }

            // Two levels, one direction: the element declares what it can do,
            // the reference declares what this page does with it. Checked one
            // capability at a time rather than as a set difference, so the
            // refusal can say which use is missing and why.
            struct RequestedUse final
            {
                bool             requested{};
                bool             declared{};
                std::string_view undeclared{};
            };

            auto const uses = std::array{
                RequestedUse{
                    .requested  = exercised.identify.has_value(),
                    .declared   = element.capabilities.identify.has_value(),
                    .undeclared = k_undeclaredIdentify,
                },
                RequestedUse{
                    .requested  = exercised.interact.has_value(),
                    .declared   = element.capabilities.interact.has_value(),
                    .undeclared = k_undeclaredInteract,
                },
                RequestedUse{
                    .requested  = exercised.read.has_value(),
                    .declared   = element.capabilities.read.has_value(),
                    .undeclared = k_undeclaredRead,
                },
            };
            for (auto const& use : uses)
            {
                if (use.requested && !use.declared)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format("\"{}\" {}", element.name, use.undeclared)
                    );
                }
            }

            // Refused by name rather than made unrepresentable, because the row
            // this writes is flat: annotation::PageReference carries both
            // fields and the catalog refuses the pair, so a spec shaped to
            // forbid it would be a third shape of one fact and would still have
            // to be flattened here. Silently dropping either half is the one
            // outcome ruled out -- both are measurements the author made.
            if (exercised.identify.has_value() && searchRoi.has_value())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "that page identifies by \"{}\" and so may not refine "
                        "its search region: the anchor pass reads the "
                        "element's own, before any page is known",
                        element.name
                    )
                );
            }
            return ok();
        }

        // The set one new reference carries: what the caller asked for, or --
        // when they asked for nothing -- every use a placement carries on its
        // own. Identify is not one of those. Its page-side payload is the role,
        // and the element has no answer to which way its evidence points, so
        // there is nothing to inherit and a page enters a signature only by
        // asking. That is also what stops a page borrowing a mark for its
        // signature from being handed permission to click it.
        [[nodiscard]]
        auto resolveExercise(
            EditableRecognizer const& element,
            std::optional<EditableExercised> const& requested
        ) -> Result<EditableExercised>
        {
            if (requested.has_value())
            {
                return *requested;
            }

            auto inherited = EditableExercised{};
            if (element.capabilities.interact.has_value())
            {
                inherited.interact = annotation::ExercisedInteract{};
            }
            if (element.capabilities.read.has_value())
            {
                inherited.read = annotation::ExercisedRead{};
            }
            if (
                !inherited.interact.has_value()
                && !inherited.read.has_value()
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "\"{}\" only identifies a page, which it joins through "
                        "that page's signature rather than by being placed on "
                        "it",
                        element.name
                    )
                );
            }
            return inherited;
        }

        // How many of a page's references put it in a signature. A page with
        // none can recognise no screen, so this is what every withdrawal has to
        // leave positive.
        [[nodiscard]]
        auto identifyReferenceCount(
            AuthoringDraft const& draft,
            annotation::PageId pageId
        ) noexcept -> std::size_t
        {
            return static_cast<std::size_t>(
                std::ranges::count_if(
                    draft.references,
                    [pageId](EditableReference const& reference)
                    {
                        return reference.pageId == pageId
                            && reference.exercised.identify.has_value();
                    }
                )
            );
        }

        // How many pages exercise interact on one element. The document closure
        // rule requires this to stay positive for every element that declares
        // interact: something the runtime could be asked to click has to be
        // reachable somewhere it can be clicked.
        [[nodiscard]]
        auto interactReferenceCount(
            AuthoringDraft const& draft,
            annotation::ElementId elementId
        ) noexcept -> std::size_t
        {
            return static_cast<std::size_t>(
                std::ranges::count_if(
                    draft.references,
                    [elementId](EditableReference const& reference)
                    {
                        return reference.elementId == elementId
                            && reference.exercised.interact.has_value();
                    }
                )
            );
        }

        [[nodiscard]]
        auto toEditableVariant(annotation::Variant const& variant) -> EditableVariant
        {
            return EditableVariant{
                .name                  = variant.name().value(),
                .sourceId              = variant.sourceId(),
                .templateRect          = variant.templateRect(),
                .similarityBasisPoints = variant.threshold().basisPoints(),
                .colourKey             = variant.colourKey(),
            };
        }

        [[nodiscard]]
        auto toEditableCapabilities(
            annotation::ElementCapabilities const& capabilities
        ) -> EditableCapabilities
        {
            auto interact = std::optional<EditableInteract>{};
            if (auto const& declared = capabilities.interact())
            {
                auto click = std::optional<EditableTemplateOffset>{};
                if (auto const offset = declared->clickOffset)
                {
                    click = EditableTemplateOffset{
                        .x = offset->x(),
                        .y = offset->y(),
                    };
                }
                interact = EditableInteract{.clickOffset = click};
            }
            return EditableCapabilities{
                .identify = capabilities.identify(),
                .interact = interact,
                .read     = capabilities.read(),
            };
        }

        [[nodiscard]]
        auto toEditableExercised(
            annotation::ExercisedCapabilities const& exercised
        ) -> EditableExercised
        {
            return EditableExercised{
                .identify = exercised.identify(),
                .interact = exercised.interact(),
                .read     = exercised.read(),
            };
        }

        [[nodiscard]]
        auto buildCapabilities(
            EditableRecognizer const& recognizer
        ) -> Result<annotation::ElementCapabilities>
        {
            auto interact = std::optional<annotation::Interact>{};
            if (auto const& declared = recognizer.capabilities.interact)
            {
                auto clickOffset = std::optional<annotation::TemplateOffset>{};
                if (auto const click = declared->clickOffset)
                {
                    auto const* p_variant = primaryVariant(recognizer);
                    if (p_variant == nullptr)
                    {
                        return fail(
                            AutomationErrorKind::InvalidResource,
                            std::format(
                                "\"{}\" carries a default click but declares no "
                                "appearance to measure it from",
                                recognizer.name
                            )
                        );
                    }
                    UF_TRY_VALUE(
                        offset,
                        annotation::TemplateOffset::create(
                            click->x,
                            click->y,
                            p_variant->templateRect.width(),
                            p_variant->templateRect.height()
                        )
                    );
                    clickOffset = offset;
                }
                interact = annotation::Interact{.clickOffset = clickOffset};
            }
            return annotation::ElementCapabilities::create(
                recognizer.capabilities.identify,
                interact,
                recognizer.capabilities.read
            );
        }
    }

    auto primaryVariant(
        EditableRecognizer const& recognizer
    ) noexcept -> EditableVariant const*
    {
        return recognizer.variants.empty()
            ? nullptr
            : &recognizer.variants.front();
    }

    auto makeAuthoringDraft(
        annotation::AuthoringDocument const& document
    ) -> AuthoringDraft
    {
        auto sources = std::vector<EditableSource>{};
        sources.reserve(document.sources().size());
        for (auto const& source : document.sources())
        {
            sources.emplace_back(
                EditableSource{
                    .id          = source.id(),
                    .contentHash = source.contentHash(),
                    .fingerprint = source.fingerprint(),
                    .provenance  = source.provenance(),
                }
            );
        }

        // Elements and references map straight across, one to one, with no
        // inversion: the capability set, the appearances, and every page-side
        // fact are the same values on both sides.
        auto recognizers = std::vector<EditableRecognizer>{};
        recognizers.reserve(document.elements().size());
        for (auto const& element : document.elements())
        {
            auto variants = std::vector<EditableVariant>{};
            variants.reserve(element.variants().size());
            for (auto const& variant : element.variants())
            {
                variants.emplace_back(toEditableVariant(variant));
            }

            recognizers.emplace_back(
                EditableRecognizer{
                    .id           = element.id(),
                    .name         = element.name().value(),
                    .capabilities = toEditableCapabilities(element.capabilities()),
                    .searchRoi    = element.searchRoi(),
                    .variants     = std::move(variants),
                }
            );
        }

        auto references = std::vector<EditableReference>{};
        references.reserve(document.references().size());
        for (auto const& reference : document.references())
        {
            auto variant = std::optional<std::string>{};
            if (auto const& pinned = reference.variant)
            {
                variant = pinned->value();
            }
            references.emplace_back(
                EditableReference{
                    .pageId    = reference.pageId,
                    .elementId = reference.elementId,
                    .holding   = reference.holding,
                    .exercised = toEditableExercised(reference.exercised),
                    .searchRoi = reference.searchRoi,
                    .variant   = std::move(variant),
                }
            );
        }

        auto pages = std::vector<EditablePage>{};
        pages.reserve(document.catalog().pages().size());
        for (auto const& page : document.catalog().pages())
        {
            pages.emplace_back(
                EditablePage{
                    .id   = page.id(),
                    .name = page.name().value(),
                }
            );
        }

        auto regressions = std::vector<EditableRegression>{};
        regressions.reserve(document.regressions().size());
        for (auto const& regression : document.regressions())
        {
            regressions.emplace_back(
                EditableRegression{
                    .id             = regression.id(),
                    .sourceId       = regression.sourceId(),
                    .classification = regression.classification(),
                    .expectation    = regression.expectation(),
                }
            );
        }

        return AuthoringDraft{
            .projectId   = document.catalog().projectId(),
            .fingerprint = document.catalog().fingerprint(),
            .sources     = std::move(sources),
            .recognizers = std::move(recognizers),
            .references  = std::move(references),
            .pages       = std::move(pages),
            .regressions = std::move(regressions),
        };
    }

    auto buildAuthoringDocument(
        AuthoringDraft const& draft
    ) -> Result<annotation::AuthoringDocument>
    {
        auto sources = std::vector<annotation::AuthoringSource>{};
        sources.reserve(draft.sources.size());
        for (auto const& source : draft.sources)
        {
            UF_TRY_VALUE(
                validated,
                annotation::AuthoringSource::create(
                    annotation::AuthoringSourceSpec{
                        .id          = source.id,
                        .contentHash = source.contentHash,
                        .fingerprint = source.fingerprint,
                        .provenance  = source.provenance,
                    }
                )
            );
            sources.emplace_back(std::move(validated));
        }

        auto elements = std::vector<annotation::Element>{};
        elements.reserve(draft.recognizers.size());
        for (auto const& recognizer : draft.recognizers)
        {
            UF_TRY_VALUE(
                name,
                annotation::ResourceName::create(recognizer.name)
            );
            UF_TRY_VALUE(capabilities, buildCapabilities(recognizer));

            auto variants = std::vector<annotation::Variant>{};
            variants.reserve(recognizer.variants.size());
            for (auto const& variant : recognizer.variants)
            {
                UF_TRY_VALUE(
                    variantName,
                    annotation::ResourceName::create(variant.name)
                );
                UF_TRY_VALUE(
                    threshold,
                    annotation::SimilarityThreshold::create(
                        variant.similarityBasisPoints
                    )
                );
                UF_TRY_VALUE(
                    validated,
                    annotation::Variant::create(
                        annotation::Variant::Spec{
                            .name         = std::move(variantName),
                            .sourceId     = variant.sourceId,
                            .templateRect = variant.templateRect,
                            .threshold    = threshold,
                            .colourKey    = variant.colourKey,
                        }
                    )
                );
                variants.emplace_back(std::move(validated));
            }

            UF_TRY_VALUE(
                element,
                annotation::Element::create(
                    draft.fingerprint,
                    annotation::Element::Spec{
                        .id           = recognizer.id,
                        .name         = std::move(name),
                        .capabilities = std::move(capabilities),
                        .searchRoi    = recognizer.searchRoi,
                        .variants     = std::move(variants),
                    }
                )
            );
            elements.emplace_back(std::move(element));
        }

        auto references = std::vector<annotation::PageReference>{};
        references.reserve(draft.references.size());
        for (auto const& reference : draft.references)
        {
            UF_TRY_VALUE(
                exercised,
                annotation::ExercisedCapabilities::create(
                    reference.exercised.identify,
                    reference.exercised.interact,
                    reference.exercised.read
                )
            );

            auto pinned = std::optional<annotation::ResourceName>{};
            if (auto const& variant = reference.variant)
            {
                UF_TRY_VALUE(
                    validated,
                    annotation::ResourceName::create(*variant)
                );
                pinned = std::move(validated);
            }

            references.emplace_back(
                annotation::PageReference{
                    .pageId    = reference.pageId,
                    .elementId = reference.elementId,
                    .holding   = reference.holding,
                    .exercised = std::move(exercised),
                    .searchRoi = reference.searchRoi,
                    .variant   = std::move(pinned),
                }
            );
        }

        auto pages = std::vector<annotation::PageSpec>{};
        pages.reserve(draft.pages.size());
        for (auto const& page : draft.pages)
        {
            UF_TRY_VALUE(name, annotation::ResourceName::create(page.name));
            pages.emplace_back(
                annotation::PageSpec{
                    .id   = page.id,
                    .name = std::move(name),
                }
            );
        }

        auto regressions = std::vector<annotation::RegressionCase>{};
        regressions.reserve(draft.regressions.size());
        for (auto const& regression : draft.regressions)
        {
            regressions.emplace_back(
                annotation::RegressionSpec{
                    .id             = regression.id,
                    .sourceId       = regression.sourceId,
                    .classification = regression.classification,
                    .expectation    = regression.expectation,
                }
            );
        }

        return annotation::AuthoringDocument::create(
            draft.projectId,
            draft.fingerprint,
            std::move(sources),
            std::move(elements),
            std::move(pages),
            std::move(references),
            std::move(regressions)
        );
    }

    auto freshAuthoringName(
        AuthoringDraft const& draft,
        std::string_view stem
    ) -> std::string
    {
        auto const taken = [&draft](std::string_view candidate)
        {
            return std::ranges::contains(
                       draft.recognizers,
                       candidate,
                       &EditableRecognizer::name
                   )
                || std::ranges::contains(
                       draft.pages,
                       candidate,
                       &EditablePage::name
                   );
        };

        auto const limit = draft.recognizers.size() + draft.pages.size() + 1U;
        auto candidate   = std::string{};
        for (auto index = std::size_t{1}; index <= limit; ++index)
        {
            candidate = std::format("{}_{}", stem, index);
            if (!taken(candidate))
            {
                break;
            }
        }
        return candidate;
    }

    auto createPageFromSource(
        AuthoringDraft draft,
        NewPageSpec const& spec
    ) -> Result<CreatedPage>
    {
        if (
            !std::ranges::contains(
                draft.sources,
                spec.sourceId,
                &EditableSource::id
            )
        )
        {
            return missingSource(spec.sourceId);
        }

        auto anchorName = freshAuthoringName(draft, "anchor");
        draft.recognizers.emplace_back(
            EditableRecognizer{
                .id   = spec.anchorId,
                .name = anchorName,
                .capabilities = EditableCapabilities{
                    .identify = annotation::Identify{},
                },
                .searchRoi = spec.searchRoi,
                .variants  = {
                    EditableVariant{
                        .name                  = std::string{k_defaultVariantName},
                        .sourceId              = spec.sourceId,
                        .templateRect          = spec.templateRect,
                        .similarityBasisPoints = spec.similarityBasisPoints,
                    },
                },
            }
        );

        auto pageName = freshAuthoringName(draft, "page");
        draft.pages.emplace_back(
            EditablePage{
                .id   = spec.pageId,
                .name = pageName,
            }
        );

        // The reference is what derives the page's signature. Without it the
        // page names nothing and the rebuild refuses it, which is why the two
        // cannot be authored one at a time.
        draft.references.emplace_back(
            EditableReference{
                .pageId    = spec.pageId,
                .elementId = spec.anchorId,
                .holding   = annotation::Holding::Owned,
                .exercised = EditableExercised{
                    .identify = annotation::ExercisedIdentify{
                        .role = annotation::SignatureRole::Required,
                    },
                },
            }
        );

        // The case is what states "this screen is that page". Nothing else in the
        // document records it: an appearance names the screen it was cut from,
        // but that is not the same claim.
        draft.regressions.emplace_back(
            EditableRegression{
                .id             = spec.regressionId,
                .sourceId       = spec.sourceId,
                .classification = annotation::RegressionClassification::Positive,
                .expectation    = annotation::ResolvedRegression{spec.pageId},
            }
        );

        return CreatedPage{
            .draft      = std::move(draft),
            .pageName   = std::move(pageName),
            .anchorName = std::move(anchorName),
        };
    }

    namespace
    {
        struct MemberShape final
        {
            EditableCapabilities declared;
            EditableExercised    exercised;
            std::string_view     stem;
        };

        [[nodiscard]]
        auto memberShapeOf(PageMemberKind kind) -> MemberShape
        {
            switch (kind)
            {
            case PageMemberKind::Anchor:
                return MemberShape{
                    .declared = EditableCapabilities{
                        .identify = annotation::Identify{},
                    },
                    .exercised = EditableExercised{
                        .identify = annotation::ExercisedIdentify{
                            .role = annotation::SignatureRole::Required,
                        },
                    },
                    .stem = "anchor",
                };
            case PageMemberKind::ActionTarget:
                return MemberShape{
                    .declared = EditableCapabilities{
                        .interact = EditableInteract{},
                    },
                    .exercised = EditableExercised{
                        .interact = annotation::ExercisedInteract{},
                    },
                    .stem = "region",
                };
            case PageMemberKind::InfoRegion:
                return MemberShape{
                    .declared  = EditableCapabilities{.read = annotation::Read{}},
                    .exercised = EditableExercised{
                        .read = annotation::ExercisedRead{},
                    },
                    .stem = "info",
                };
            }
            UF_UNREACHABLE_MSG("unknown PageMemberKind value");
        }
    }

    auto addPageMember(
        AuthoringDraft draft,
        PageMemberSpec const& spec
    ) -> Result<AddedPageMember>
    {
        if (
            !std::ranges::contains(
                draft.sources,
                spec.sourceId,
                &EditableSource::id
            )
        )
        {
            return missingSource(spec.sourceId);
        }
        if (!std::ranges::contains(draft.pages, spec.pageId, &EditablePage::id))
        {
            return missingPage(spec.pageId);
        }

        auto shape = memberShapeOf(spec.kind);
        auto name  = freshAuthoringName(draft, shape.stem);
        draft.recognizers.emplace_back(
            EditableRecognizer{
                .id           = spec.recognizerId,
                .name         = name,
                .capabilities = std::move(shape.declared),
                .searchRoi    = spec.searchRoi,
                .variants     = {
                    EditableVariant{
                        .name                  = std::string{k_defaultVariantName},
                        .sourceId              = spec.sourceId,
                        .templateRect          = spec.templateRect,
                        .similarityBasisPoints = spec.similarityBasisPoints,
                    },
                },
            }
        );
        // One edge, whatever the capability: the page's use of the element IS
        // both its membership and, for identify, its half of the signature. The
        // reference inherits the element's search region rather than pinning a
        // copy of it, which is what "absent means the element's own" is for.
        draft.references.emplace_back(
            EditableReference{
                .pageId    = spec.pageId,
                .elementId = spec.recognizerId,
                .holding   = annotation::Holding::Owned,
                .exercised = std::move(shape.exercised),
            }
        );

        return AddedPageMember{
            .draft = std::move(draft),
            .name  = std::move(name),
        };
    }

    auto pagesReferencing(
        AuthoringDraft const& draft,
        annotation::ElementId id
    ) -> std::vector<annotation::PageId>
    {
        auto pages = std::vector<annotation::PageId>{};
        for (auto const& reference : draft.references)
        {
            if (
                reference.elementId == id
                && !std::ranges::contains(pages, reference.pageId)
            )
            {
                pages.emplace_back(reference.pageId);
            }
        }
        return pages;
    }

    auto removeReferenceFromPage(
        AuthoringDraft draft,
        annotation::ElementId id,
        annotation::PageId pageId
    ) -> Result<AuthoringDraft>
    {
        auto const* p_target = findRecognizerIn(draft, id);
        if (p_target == nullptr)
        {
            return missingElement(id);
        }
        auto const* p_reference = findReferenceIn(draft, pageId, id);
        if (p_reference == nullptr)
        {
            return draft;
        }

        if (
            p_reference->exercised.identify.has_value()
            && identifyReferenceCount(draft, pageId) <= 1U
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "\"{}\" is the only thing identifying this page; give the "
                    "page another identifying mark before withdrawing it",
                    p_target->name
                )
            );
        }
        if (
            p_target->capabilities.interact.has_value()
            && p_reference->exercised.interact.has_value()
            && interactReferenceCount(draft, id) <= 1U
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "\"{}\" is clickable only on this page; something clickable "
                    "must stay reachable somewhere, so delete it instead of "
                    "removing it here",
                    p_target->name
                )
            );
        }

        std::erase_if(
            draft.references,
            [id, pageId](EditableReference const& reference)
            {
                return reference.elementId == id && reference.pageId == pageId;
            }
        );
        return draft;
    }

    auto duplicateElement(
        AuthoringDraft draft,
        DuplicateElementSpec const& spec
    ) -> Result<DuplicatedElement>
    {
        auto const origin = std::ranges::find(
            draft.recognizers,
            spec.sourceElementId,
            &EditableRecognizer::id
        );
        if (origin == draft.recognizers.end())
        {
            return missingElement(spec.sourceElementId);
        }

        // Derived before the copy is inserted so the new name is measured
        // against the names already taken, the original's among them.
        auto name = freshAuthoringName(draft, origin->name);
        auto copy = *origin;
        copy.id   = spec.newElementId;
        copy.name = name;

        // Mirror the original's references onto the copy, each retargeted to the
        // new id and keeping its own refinements. Collected before the recognizer
        // is appended so a reallocation cannot invalidate the read.
        auto references = std::vector<EditableReference>{};
        for (auto const& reference : draft.references)
        {
            if (reference.elementId == spec.sourceElementId)
            {
                auto mirrored      = reference;
                mirrored.elementId = spec.newElementId;
                references.emplace_back(std::move(mirrored));
            }
        }

        draft.recognizers.emplace_back(std::move(copy));
        draft.references.insert(
            draft.references.end(),
            references.begin(),
            references.end()
        );

        return DuplicatedElement{
            .draft = std::move(draft),
            .name  = std::move(name),
        };
    }

    auto setElementColourKey(
        AuthoringDraft draft,
        annotation::ElementId id,
        std::optional<annotation::ColourKey> colourKey
    ) -> Result<AuthoringDraft>
    {
        auto* p_target = findRecognizerIn(draft, id);
        if (p_target == nullptr)
        {
            return missingElement(id);
        }

        // An element is drawn once and referenced by N pages, so its mask, like
        // the rectangle it masks, is one fact every page sees.
        for (auto& variant : p_target->variants)
        {
            variant.colourKey = colourKey;
        }
        return draft;
    }

    auto referenceElementOnPage(
        AuthoringDraft draft,
        ReferenceElementSpec const& spec
    ) -> Result<ReferencedElement>
    {
        auto const* p_origin = findRecognizerIn(draft, spec.elementId);
        if (p_origin == nullptr)
        {
            return missingElement(spec.elementId);
        }
        if (
            !std::ranges::contains(draft.pages, spec.pageId, &EditablePage::id)
        )
        {
            return missingPage(spec.pageId);
        }
        if (findReferenceIn(draft, spec.pageId, spec.elementId) != nullptr)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format("\"{}\" is already on that page", p_origin->name)
            );
        }

        UF_TRY_VALUE(exercised, resolveExercise(*p_origin, spec.exercised));
        UF_TRY(validateReference(*p_origin, exercised, spec.searchRoi));

        // One element, referenced again. No copy is minted -- a later appearance
        // edit touches this element once and every page sees it. The holding is
        // Referenced because this page is borrowing pixels whose home is
        // elsewhere, which is the fact the old reuse flag could only guess at.
        auto name = p_origin->name;
        draft.references.emplace_back(
            EditableReference{
                .pageId    = spec.pageId,
                .elementId = spec.elementId,
                .holding   = annotation::Holding::Referenced,
                .exercised = std::move(exercised),
                .searchRoi = spec.searchRoi,
            }
        );

        return ReferencedElement{
            .draft = std::move(draft),
            .name  = std::move(name),
        };
    }

    auto setReferenceIdentifyRole(
        AuthoringDraft draft,
        annotation::ElementId id,
        annotation::PageId pageId,
        annotation::SignatureRole role
    ) -> Result<AuthoringDraft>
    {
        auto const evidence = annotation::ExercisedIdentify{.role = role};

        auto* p_reference = findReferenceIn(draft, pageId, id);
        if (p_reference == nullptr)
        {
            UF_TRY_VALUE(
                referenced,
                referenceElementOnPage(
                    std::move(draft),
                    ReferenceElementSpec{
                        .elementId = id,
                        .pageId    = pageId,
                        .exercised = EditableExercised{.identify = evidence},
                    }
                )
            );
            return std::move(referenced.draft);
        }

        auto const* p_element = findRecognizerIn(draft, id);
        if (p_element == nullptr)
        {
            return missingElement(id);
        }

        // The row this page already has, pointed the new way, judged before it
        // is installed. Building the candidate and validating it is what keeps
        // this from being a second copy of the rules: the refusal a new row
        // would get is the refusal this one gets, refined region included.
        auto updated     = p_reference->exercised;
        updated.identify = evidence;
        UF_TRY(validateReference(*p_element, updated, p_reference->searchRoi));

        p_reference->exercised = std::move(updated);
        return draft;
    }

    auto setElementTemplateRect(
        AuthoringDraft draft,
        annotation::ElementId id,
        PixelRect templateRect
    ) -> Result<RetemplatedElement>
    {
        auto* p_target = findRecognizerIn(draft, id);
        if (p_target == nullptr)
        {
            return missingElement(id);
        }
        if (p_target->variants.empty())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "\"{}\" is located by its page and has no appearance to move",
                    p_target->name
                )
            );
        }
        if (p_target->variants.size() > 1U)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "\"{}\" has several appearances, and this editor draws one "
                    "rectangle, so it cannot say which to move",
                    p_target->name
                )
            );
        }
        if (
            templateRect.width() > p_target->searchRoi.width()
            || templateRect.height() > p_target->searchRoi.height()
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "the new template does not fit the range \"{}\" searches; "
                    "widen that range first",
                    p_target->name
                )
            );
        }

        // A reference may refine the region searched on its page, so the moved
        // template must still fit each refinement.
        auto referencingPages = std::size_t{0};
        for (auto const& reference : draft.references)
        {
            if (reference.elementId != id)
            {
                continue;
            }
            ++referencingPages;
            auto const refined = reference.searchRoi;
            if (!refined.has_value())
            {
                continue;
            }
            if (
                templateRect.width() > refined->width()
                || templateRect.height() > refined->height()
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "the new template does not fit the range \"{}\" searches "
                        "on one of its pages; widen that range first",
                        p_target->name
                    )
                );
            }
        }

        auto* p_variant = primaryVariantOf(*p_target);
        UF_CHECK(p_variant != nullptr);
        p_variant->templateRect = templateRect;
        return RetemplatedElement{
            .draft            = std::move(draft),
            .referencingPages = referencingPages,
        };
    }

    auto claimScreenForPage(
        AuthoringDraft draft,
        ScreenClaimSpec const& spec
    ) -> Result<AuthoringDraft>
    {
        if (
            !std::ranges::contains(
                draft.sources,
                spec.sourceId,
                &EditableSource::id
            )
        )
        {
            return missingSource(spec.sourceId);
        }
        if (
            !std::ranges::contains(draft.pages, spec.pageId, &EditablePage::id)
        )
        {
            return missingPage(spec.pageId);
        }

        auto const expectation = annotation::RegressionExpectation{
            annotation::ResolvedRegression{spec.pageId},
        };
        auto const existing = std::ranges::find(
            draft.regressions,
            spec.sourceId,
            &EditableRegression::sourceId
        );
        if (existing != draft.regressions.end())
        {
            existing->expectation = expectation;
            return draft;
        }

        draft.regressions.emplace_back(
            EditableRegression{
                .id             = spec.regressionId,
                .sourceId       = spec.sourceId,
                .classification = annotation::RegressionClassification::Positive,
                .expectation    = expectation,
            }
        );
        return draft;
    }

    auto recordScreenExpectation(
        AuthoringDraft draft,
        ScreenExpectationSpec const& spec
    ) -> Result<AuthoringDraft>
    {
        if (
            !std::ranges::contains(
                draft.sources,
                spec.sourceId,
                &EditableSource::id
            )
        )
        {
            return missingSource(spec.sourceId);
        }

        struct Recorded final
        {
            annotation::RegressionExpectation    expectation;
            annotation::RegressionClassification classification{};
        };
        auto const recorded = [&]() -> Recorded
        {
            switch (spec.expectation)
            {
            case PagelessExpectation::Unknown:
                return Recorded{
                    .expectation    = annotation::UnknownRegression{},
                    .classification = annotation::RegressionClassification::Negative,
                };
            case PagelessExpectation::Ambiguous:
                return Recorded{
                    .expectation    = annotation::AmbiguousRegression{},
                    .classification = annotation::RegressionClassification::Confusable,
                };
            }
            UF_UNREACHABLE_MSG("Unknown PagelessExpectation value");
        }();

        auto const existing = std::ranges::find(
            draft.regressions,
            spec.sourceId,
            &EditableRegression::sourceId
        );
        if (existing != draft.regressions.end())
        {
            existing->classification = recorded.classification;
            existing->expectation    = recorded.expectation;
            return draft;
        }

        draft.regressions.emplace_back(
            EditableRegression{
                .id             = spec.regressionId,
                .sourceId       = spec.sourceId,
                .classification = recorded.classification,
                .expectation    = recorded.expectation,
            }
        );
        return draft;
    }

    auto deleteRecognizer(
        AuthoringDraft draft,
        annotation::ElementId id
    ) -> Result<DeletedEntity>
    {
        auto const* p_target = findRecognizerIn(draft, id);
        if (p_target == nullptr)
        {
            return missingElement(id);
        }

        for (auto const& page : draft.pages)
        {
            auto const* p_reference = findReferenceIn(draft, page.id, id);
            auto const identifies = (
                p_reference != nullptr
                && p_reference->exercised.identify.has_value()
            );
            if (identifies && identifyReferenceCount(draft, page.id) <= 1U)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "\"{}\" is the only thing identifying page \"{}\"; "
                        "delete that page first, or give it another mark",
                        p_target->name,
                        page.name
                    )
                );
            }
        }

        auto withdrawnRoles = std::size_t{0};
        for (auto const& reference : draft.references)
        {
            if (
                reference.elementId == id
                && reference.exercised.identify.has_value()
            )
            {
                ++withdrawnRoles;
            }
        }
        // Every page-side use of the element goes with it: a reference to an
        // element that no longer exists is not something the model can hold.
        std::erase_if(
            draft.references,
            [id](EditableReference const& reference)
            {
                return reference.elementId == id;
            }
        );
        std::erase_if(
            draft.recognizers,
            [id](EditableRecognizer const& recognizer)
            {
                return recognizer.id == id;
            }
        );

        return DeletedEntity{
            .draft          = std::move(draft),
            .withdrawnRoles = withdrawnRoles,
        };
    }

    auto deletePage(
        AuthoringDraft draft,
        annotation::PageId id
    ) -> Result<DeletedEntity>
    {
        auto const target = std::ranges::find(
            draft.pages,
            id,
            &EditablePage::id
        );
        if (target == draft.pages.end())
        {
            return missingPage(id);
        }

        // An element declaring interact that this page is the only one to
        // exercise would be left unreachable, which the closure rule forbids;
        // that is a choice between deleting it and re-pointing it that only the
        // author can make. An element that is only read may be left unreferenced.
        for (auto const& recognizer : draft.recognizers)
        {
            if (!recognizer.capabilities.interact.has_value())
            {
                continue;
            }
            auto const* p_here = findReferenceIn(draft, id, recognizer.id);
            auto const onThisPage = (
                p_here != nullptr && p_here->exercised.interact.has_value()
            );
            if (
                onThisPage
                && interactReferenceCount(draft, recognizer.id) <= 1U
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "page \"{}\" is the only page \"{}\" can be clicked on; "
                        "place it elsewhere or delete it first",
                        target->name,
                        recognizer.name
                    )
                );
            }
        }

        auto const withdrawnReferences = std::erase_if(
            draft.references,
            [id](EditableReference const& reference)
            {
                return reference.pageId == id;
            }
        );

        // A case expecting this page to resolve cannot be reclassified into
        // anything the author meant, so it goes with the page rather than
        // blocking the deletion. Every page created from a captured screen owns
        // one, so refusing here would make such a page undeletable.
        auto const removedRegressions = std::erase_if(
            draft.regressions,
            [id](EditableRegression const& regression)
            {
                auto const* p_resolved = std::get_if<annotation::ResolvedRegression>(
                    &regression.expectation
                );
                return p_resolved != nullptr && p_resolved->pageId == id;
            }
        );
        draft.pages.erase(target);

        return DeletedEntity{
            .draft               = std::move(draft),
            .withdrawnReferences = withdrawnReferences,
            .removedRegressions  = removedRegressions,
        };
    }

    auto deleteSource(
        AuthoringDraft draft,
        annotation::SourceId id
    ) -> Result<DeletedEntity>
    {
        auto const target = std::ranges::find(
            draft.sources,
            id,
            &EditableSource::id
        );
        if (target == draft.sources.end())
        {
            return missingSource(id);
        }

        auto authored = std::size_t{0};
        for (auto const& recognizer : draft.recognizers)
        {
            authored += static_cast<std::size_t>(
                std::ranges::count(
                    recognizer.variants,
                    id,
                    &EditableVariant::sourceId
                )
            );
        }
        if (authored > 0U)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "{} appearance{} cut from this source; delete {} first",
                    authored,
                    authored == 1U ? " is" : "s are",
                    authored == 1U ? "it" : "them"
                )
            );
        }

        auto const removedRegressions = std::erase_if(
            draft.regressions,
            [id](EditableRegression const& regression) -> bool
            {
                return regression.sourceId == id;
            }
        );
        draft.sources.erase(target);

        return DeletedEntity{
            .draft              = std::move(draft),
            .removedRegressions = removedRegressions,
        };
    }

    AuthoringEditHistory::AuthoringEditHistory(
        annotation::AuthoringDocument document
    )
        : m_current{std::move(document)}
    {
    }

    auto AuthoringEditHistory::document() const noexcept
        -> annotation::AuthoringDocument const&
    {
        return m_current;
    }

    auto AuthoringEditHistory::draft() const -> AuthoringDraft
    {
        return makeAuthoringDraft(m_current);
    }

    auto AuthoringEditHistory::canUndo() const noexcept -> bool
    {
        return !m_undo.empty();
    }

    auto AuthoringEditHistory::canRedo() const noexcept -> bool
    {
        return !m_redo.empty();
    }

    auto AuthoringEditHistory::revision() const noexcept -> uint64
    {
        return m_revision;
    }

    auto AuthoringEditHistory::position() const noexcept -> uint64
    {
        return m_position;
    }

    auto AuthoringEditHistory::apply(
        AuthoringDraft const& draft
    ) -> Result<bool>
    {
        UF_TRY_VALUE(next, buildAuthoringDocument(draft));
        if (
            annotation::serializeAuthoringDocument(next)
            == annotation::serializeAuthoringDocument(m_current)
        )
        {
            return false;
        }

        if (m_undo.size() == k_maximumAuthoringUndoEntries)
        {
            m_undo.erase(m_undo.begin());
        }
        m_undo.emplace_back(
            Snapshot{.document = std::move(m_current), .position = m_position}
        );
        m_current  = std::move(next);
        m_position = m_nextPosition++;
        m_redo.clear();
        ++m_revision;
        return true;
    }

    auto AuthoringEditHistory::undo() -> bool
    {
        if (m_undo.empty()) return false;

        m_redo.emplace_back(
            Snapshot{.document = std::move(m_current), .position = m_position}
        );
        m_current  = std::move(m_undo.back().document);
        m_position = m_undo.back().position;
        m_undo.pop_back();
        ++m_revision;
        return true;
    }

    auto AuthoringEditHistory::redo() -> bool
    {
        if (m_redo.empty()) return false;

        m_undo.emplace_back(
            Snapshot{.document = std::move(m_current), .position = m_position}
        );
        m_current  = std::move(m_redo.back().document);
        m_position = m_redo.back().position;
        m_redo.pop_back();
        ++m_revision;
        return true;
    }
}
