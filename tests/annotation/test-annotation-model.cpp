#include "test-helpers.hpp"

#include <annotation/authoring-compiler.hpp>
#include <annotation/authoring-document.hpp>
#include <annotation/capabilities.hpp>
#include <annotation/catalog.hpp>
#include <annotation/content-hash.hpp>
#include <annotation/recognition-runtime.hpp>
#include <annotation/resource.hpp>
#include <annotation/runtime-manifest.hpp>

#include <core/safety/checked-access.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/space.hpp>
#include <domain/time.hpp>

#include <image/png.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uf::annotation
{
    namespace
    {
        constexpr auto k_elementA = "00000000-0000-0000-0000-000000000001";
        constexpr auto k_elementB = "00000000-0000-0000-0000-000000000002";
        constexpr auto k_elementC = "00000000-0000-0000-0000-000000000003";
        constexpr auto k_pageP    = "00000000-0000-0000-0000-000000000101";
        constexpr auto k_pageQ    = "00000000-0000-0000-0000-000000000102";
        constexpr auto k_sourceS  = "00000000-0000-0000-0000-000000000201";

        [[nodiscard]]
        auto identifyingElement(
            std::string name,
            char const* id
        ) -> CompiledElement
        {
            return test::element(
                test::fingerprint(16, 16),
                test::elementId(id),
                std::move(name),
                test::capabilities(Identify{}),
                test::pixelRect(0, 0, 8, 8),
                std::vector<CompiledAppearance>{
                    test::compiledAppearance("only", test::pixelRect(0, 0, 4, 4)),
                }
            );
        }

        [[nodiscard]]
        auto interactingElement(
            ElementCapabilities elementCapabilities
        ) -> CompiledElement
        {
            return test::element(
                test::fingerprint(16, 16),
                test::elementId(k_elementB),
                "target",
                std::move(elementCapabilities),
                test::pixelRect(0, 0, 8, 8),
                std::vector<CompiledAppearance>{
                    test::compiledAppearance("only", test::pixelRect(0, 0, 4, 4)),
                }
            );
        }

        [[nodiscard]]
        auto buildCatalog(
            std::vector<CompiledElement> elements,
            std::vector<PageSpec> pages,
            std::vector<PageReference> references
        ) -> Result<RecognitionCatalog>
        {
            return RecognitionCatalog::create(
                test::projectId(),
                test::fingerprint(16, 16),
                std::move(elements),
                std::move(pages),
                std::move(references)
            );
        }

        [[nodiscard]]
        auto anchorAndTargetElements() -> std::vector<CompiledElement>
        {
            auto elements = std::vector<CompiledElement>{};
            elements.emplace_back(identifyingElement("mark", k_elementA));
            elements.emplace_back(
                interactingElement(test::capabilities(std::nullopt, Interact{}))
            );
            return elements;
        }

        [[nodiscard]]
        auto onePage() -> std::vector<PageSpec>
        {
            auto pages = std::vector<PageSpec>{};
            pages.emplace_back(test::page(test::pageId(k_pageP), "home"));
            return pages;
        }
    }

    TEST_CASE("an element with no appearances cannot be identity evidence")
    {
        auto const projectFingerprint = test::fingerprint(16, 16);
        auto const searchRoi          = test::pixelRect(0, 0, 8, 8);

        auto const rejected = Element::create(
            projectFingerprint,
            Element::Spec{
                .id           = test::elementId(k_elementA),
                .name         = test::resourceName("mark"),
                .capabilities = test::capabilities(Identify{}),
                .searchRoi    = searchRoi,
            }
        );
        REQUIRE(!rejected.has_value());
        test::requireErrorKind(rejected.error(), AutomationErrorKind::InvalidResource);

        // The same capability with one appearance is accepted, so what was
        // refused is the missing pixels rather than identify itself.
        auto const withPixels = Element::create(
            projectFingerprint,
            Element::Spec{
                .id           = test::elementId(k_elementA),
                .name         = test::resourceName("mark"),
                .capabilities = test::capabilities(Identify{}),
                .searchRoi    = searchRoi,
                .appearances     = std::vector<Appearance>{
                    test::appearance(
                        "only",
                        test::sourceId(k_sourceS),
                        test::pixelRect(0, 0, 4, 4)
                    ),
                },
            }
        );
        CHECK(withPixels.has_value());

        // An empty appearance list stays legal for the capabilities the page
        // locates: a readable cell and a click target inside a resolved page.
        auto const locatedByPage = Element::create(
            projectFingerprint,
            Element::Spec{
                .id   = test::elementId(k_elementA),
                .name = test::resourceName("slot"),
                .capabilities = test::capabilities(
                    std::nullopt,
                    Interact{},
                    Read{}
                ),
                .searchRoi = searchRoi,
            }
        );
        CHECK(locatedByPage.has_value());
    }

    TEST_CASE("a page may only exercise what its element declares")
    {
        auto references = std::vector<PageReference>{};
        references.emplace_back(
            test::reference(
                test::pageId(k_pageP),
                test::elementId(k_elementA),
                test::identifiesAs()
            )
        );
        // The target declares interact alone, so identifying with it is a
        // capability it never offered.
        references.emplace_back(
            test::reference(
                test::pageId(k_pageP),
                test::elementId(k_elementB),
                test::exercised(ExercisedIdentify{}, ExercisedInteract{})
            )
        );
        auto const rejected = buildCatalog(
            anchorAndTargetElements(),
            onePage(),
            std::move(references)
        );
        REQUIRE(!rejected.has_value());
        test::requireErrorKind(rejected.error(), AutomationErrorKind::InvalidResource);

        auto accepted = std::vector<PageReference>{};
        accepted.emplace_back(
            test::reference(
                test::pageId(k_pageP),
                test::elementId(k_elementA),
                test::identifiesAs()
            )
        );
        accepted.emplace_back(
            test::reference(
                test::pageId(k_pageP),
                test::elementId(k_elementB),
                test::interacts()
            )
        );
        CHECK(
            buildCatalog(
                anchorAndTargetElements(),
                onePage(),
                std::move(accepted)
            ).has_value()
        );
    }

    TEST_CASE("a reference that identifies may not refine the search region")
    {
        auto references = std::vector<PageReference>{};
        references.emplace_back(
            test::reference(
                test::pageId(k_pageP),
                test::elementId(k_elementA),
                test::identifiesAs(),
                Holding::Owned,
                test::pixelRect(0, 0, 6, 6)
            )
        );
        references.emplace_back(
            test::reference(
                test::pageId(k_pageP),
                test::elementId(k_elementB),
                test::interacts()
            )
        );
        auto const rejected = buildCatalog(
            anchorAndTargetElements(),
            onePage(),
            std::move(references)
        );
        REQUIRE(!rejected.has_value());
        test::requireErrorKind(rejected.error(), AutomationErrorKind::InvalidResource);

        // The same refinement on a reference that only interacts is fine, so it
        // is identify that refuses it and not the rectangle.
        auto accepted = std::vector<PageReference>{};
        accepted.emplace_back(
            test::reference(
                test::pageId(k_pageP),
                test::elementId(k_elementA),
                test::identifiesAs()
            )
        );
        accepted.emplace_back(
            test::reference(
                test::pageId(k_pageP),
                test::elementId(k_elementB),
                test::interacts(),
                Holding::Owned,
                test::pixelRect(0, 0, 6, 6)
            )
        );
        CHECK(
            buildCatalog(
                anchorAndTargetElements(),
                onePage(),
                std::move(accepted)
            ).has_value()
        );
    }

    TEST_CASE("at most one page may own an element")
    {
        auto pages = onePage();
        pages.emplace_back(test::page(test::pageId(k_pageQ), "sortie"));

        auto owningTwice = std::vector<PageReference>{};
        owningTwice.emplace_back(
            test::reference(
                test::pageId(k_pageP),
                test::elementId(k_elementA),
                test::identifiesAs(SignatureRole::Required)
            )
        );
        owningTwice.emplace_back(
            test::reference(
                test::pageId(k_pageQ),
                test::elementId(k_elementA),
                test::identifiesAs(SignatureRole::Forbidden),
                Holding::Referenced
            )
        );
        owningTwice.emplace_back(
            test::reference(
                test::pageId(k_pageP),
                test::elementId(k_elementB),
                test::interacts(),
                Holding::Owned
            )
        );
        owningTwice.emplace_back(
            test::reference(
                test::pageId(k_pageQ),
                test::elementId(k_elementB),
                test::interacts(),
                Holding::Owned
            )
        );
        auto const rejected = buildCatalog(
            anchorAndTargetElements(),
            pages,
            std::move(owningTwice)
        );
        REQUIRE(!rejected.has_value());
        test::requireErrorKind(rejected.error(), AutomationErrorKind::InvalidResource);

        auto borrowed = std::vector<PageReference>{};
        borrowed.emplace_back(
            test::reference(
                test::pageId(k_pageP),
                test::elementId(k_elementA),
                test::identifiesAs(SignatureRole::Required)
            )
        );
        borrowed.emplace_back(
            test::reference(
                test::pageId(k_pageQ),
                test::elementId(k_elementA),
                test::identifiesAs(SignatureRole::Forbidden),
                Holding::Referenced
            )
        );
        borrowed.emplace_back(
            test::reference(
                test::pageId(k_pageP),
                test::elementId(k_elementB),
                test::interacts(),
                Holding::Owned
            )
        );
        borrowed.emplace_back(
            test::reference(
                test::pageId(k_pageQ),
                test::elementId(k_elementB),
                test::interacts(),
                Holding::Referenced
            )
        );
        CHECK(
            buildCatalog(
                anchorAndTargetElements(),
                std::move(pages),
                std::move(borrowed)
            ).has_value()
        );
    }

    TEST_CASE("a pinned appearance must be one the element declares")
    {
        auto references = std::vector<PageReference>{};
        references.emplace_back(
            test::reference(
                test::pageId(k_pageP),
                test::elementId(k_elementA),
                test::identifiesAs()
            )
        );
        references.emplace_back(
            test::reference(
                test::pageId(k_pageP),
                test::elementId(k_elementB),
                test::interacts(),
                Holding::Owned,
                std::nullopt,
                test::resourceName("on_light")
            )
        );
        auto const rejected = buildCatalog(
            anchorAndTargetElements(),
            onePage(),
            std::move(references)
        );
        REQUIRE(!rejected.has_value());
        test::requireErrorKind(rejected.error(), AutomationErrorKind::InvalidResource);

        auto accepted = std::vector<PageReference>{};
        accepted.emplace_back(
            test::reference(
                test::pageId(k_pageP),
                test::elementId(k_elementA),
                test::identifiesAs()
            )
        );
        accepted.emplace_back(
            test::reference(
                test::pageId(k_pageP),
                test::elementId(k_elementB),
                test::interacts(),
                Holding::Owned,
                std::nullopt,
                test::resourceName("only")
            )
        );
        CHECK(
            buildCatalog(
                anchorAndTargetElements(),
                onePage(),
                std::move(accepted)
            ).has_value()
        );
    }

    TEST_CASE("an element that declares interact must be exercised by some page")
    {
        // Declares both, and the page uses only the reading half, so nothing
        // could ever deliver the click the element advertises.
        auto readOnlyUse = std::vector<PageReference>{};
        readOnlyUse.emplace_back(
            test::reference(
                test::pageId(k_pageP),
                test::elementId(k_elementA),
                test::identifiesAs()
            )
        );
        readOnlyUse.emplace_back(
            test::reference(
                test::pageId(k_pageP),
                test::elementId(k_elementB),
                test::exercised(std::nullopt, std::nullopt, ExercisedRead{})
            )
        );
        auto bothDeclared = std::vector<CompiledElement>{};
        bothDeclared.emplace_back(identifyingElement("mark", k_elementA));
        bothDeclared.emplace_back(
            interactingElement(
                test::capabilities(std::nullopt, Interact{}, Read{})
            )
        );
        auto const rejected = buildCatalog(
            std::move(bothDeclared),
            onePage(),
            std::move(readOnlyUse)
        );
        REQUIRE(!rejected.has_value());
        test::requireErrorKind(rejected.error(), AutomationErrorKind::InvalidResource);

        auto bothUsed = std::vector<PageReference>{};
        bothUsed.emplace_back(
            test::reference(
                test::pageId(k_pageP),
                test::elementId(k_elementA),
                test::identifiesAs()
            )
        );
        bothUsed.emplace_back(
            test::reference(
                test::pageId(k_pageP),
                test::elementId(k_elementB),
                test::exercised(std::nullopt, ExercisedInteract{}, ExercisedRead{})
            )
        );
        auto declaredAgain = std::vector<CompiledElement>{};
        declaredAgain.emplace_back(identifyingElement("mark", k_elementA));
        declaredAgain.emplace_back(
            interactingElement(
                test::capabilities(std::nullopt, Interact{}, Read{})
            )
        );
        CHECK(
            buildCatalog(
                std::move(declaredAgain),
                onePage(),
                std::move(bothUsed)
            ).has_value()
        );
    }

    TEST_CASE("a page signature is derived from the references that identify")
    {
        auto elements = std::vector<CompiledElement>{};
        elements.emplace_back(identifyingElement("mark", k_elementA));
        elements.emplace_back(identifyingElement("decoy", k_elementC));
        elements.emplace_back(
            interactingElement(test::capabilities(std::nullopt, Interact{}))
        );

        auto references = std::vector<PageReference>{};
        references.emplace_back(
            test::reference(
                test::pageId(k_pageP),
                test::elementId(k_elementA),
                test::identifiesAs(SignatureRole::Required)
            )
        );
        references.emplace_back(
            test::reference(
                test::pageId(k_pageP),
                test::elementId(k_elementC),
                test::identifiesAs(SignatureRole::Forbidden)
            )
        );
        references.emplace_back(
            test::reference(
                test::pageId(k_pageP),
                test::elementId(k_elementB),
                test::interacts()
            )
        );

        auto const catalog = buildCatalog(
            std::move(elements),
            onePage(),
            std::move(references)
        );
        REQUIRE(catalog.has_value());
        auto const* p_page = catalog->findPage(test::pageId(k_pageP));
        REQUIRE(p_page != nullptr);
        REQUIRE(p_page->required().size() == 1U);
        CHECK(p_page->required().front() == test::elementId(k_elementA));
        REQUIRE(p_page->forbidden().size() == 1U);
        CHECK(p_page->forbidden().front() == test::elementId(k_elementC));

        // The element the page only clicks contributes to neither set.
        CHECK(
            !std::ranges::contains(p_page->required(), test::elementId(k_elementB))
        );
        CHECK(
            !std::ranges::contains(p_page->forbidden(), test::elementId(k_elementB))
        );
    }

    TEST_CASE("a page nothing identifies cannot be recognised and is refused")
    {
        auto references = std::vector<PageReference>{};
        references.emplace_back(
            test::reference(
                test::pageId(k_pageP),
                test::elementId(k_elementB),
                test::interacts()
            )
        );
        auto const rejected = buildCatalog(
            anchorAndTargetElements(),
            onePage(),
            std::move(references)
        );
        REQUIRE(!rejected.has_value());
        test::requireErrorKind(rejected.error(), AutomationErrorKind::InvalidResource);
    }

    namespace
    {
        [[nodiscard]]
        constexpr auto asByte(uint8 value) noexcept -> std::byte
        {
            return static_cast<std::byte>(value);
        }

        [[nodiscard]]
        auto encodedSource() -> std::vector<std::byte>
        {
            auto pixels = std::vector<std::byte>{};
            pixels.reserve(std::size_t{4} * 4U * 4U);
            for (auto index = std::size_t{0}; index < std::size_t{16}; ++index)
            {
                pixels.emplace_back(asByte(static_cast<uint8>(index * 3U)));
                pixels.emplace_back(asByte(static_cast<uint8>(index * 5U)));
                pixels.emplace_back(asByte(static_cast<uint8>(index * 7U)));
                pixels.emplace_back(asByte(255));
            }
            auto encoded = image::encodeRgbaPng("model-source.png", 4, 4, pixels);
            REQUIRE(encoded.has_value());
            return *std::move(encoded);
        }

        struct DocumentFixture final
        {
            AuthoringDocument    document;
            AuthoringSourceAsset sourceAsset;
        };

        auto replaceOnce(
            std::string& text,
            std::string_view from,
            std::string_view to
        ) -> void
        {
            auto const position = text.find(from);
            REQUIRE(position != std::string::npos);
            text.replace(position, from.size(), to);
        }

        // The same bytes with two complete page tables exchanged: valid data in
        // an order the writer would never emit.
        [[nodiscard]]
        auto swappedPageBlocks(std::string const& encoded) -> std::string
        {
            auto const first = encoded.find("\n[[page]]\n");
            REQUIRE(first != std::string::npos);
            auto const second = encoded.find("\n[[page]]\n", first + 1U);
            REQUIRE(second != std::string::npos);
            auto const end = encoded.find("\n[[", second + 1U);
            REQUIRE(end != std::string::npos);
            return (
                encoded.substr(0, first)
                + encoded.substr(second, end - second)
                + encoded.substr(first, second - first)
                + encoded.substr(end)
            );
        }

        // One element shared by two pages -- the shape the compiler could not
        // represent before, because it minted a derived id per page and left
        // the element's own id with no compiled element at all.
        [[nodiscard]]
        auto sharedElementFixture() -> DocumentFixture
        {
            auto const projectFingerprint = test::fingerprint(4, 4);
            auto pngBytes                 = encodedSource();
            auto const sourceHash         = sha256(pngBytes);
            REQUIRE(sourceHash.has_value());
            auto source = AuthoringSource::create(
                AuthoringSourceSpec{
                    .id          = test::sourceId(k_sourceS),
                    .contentHash = *sourceHash,
                    .fingerprint = projectFingerprint,
                    .provenance  = ImportedSourceProvenance{},
                }
            );
            REQUIRE(source.has_value());

            auto elements = std::vector<Element>{};
            elements.emplace_back(
                test::element(
                    projectFingerprint,
                    test::elementId(k_elementA),
                    "home_mark",
                    test::capabilities(Identify{}),
                    test::pixelRect(0, 0, 4, 4),
                    std::vector<Appearance>{
                        test::appearance(
                            "only",
                            test::sourceId(k_sourceS),
                            test::pixelRect(0, 0, 2, 2)
                        ),
                    }
                )
            );
            elements.emplace_back(
                test::element(
                    projectFingerprint,
                    test::elementId(k_elementC),
                    "sortie_mark",
                    test::capabilities(Identify{}),
                    test::pixelRect(0, 0, 4, 4),
                    std::vector<Appearance>{
                        test::appearance(
                            "only",
                            test::sourceId(k_sourceS),
                            test::pixelRect(2, 2, 2, 2)
                        ),
                    }
                )
            );
            // Two appearances of one semantic element, which is the case the
            // whole appearance list exists for.
            elements.emplace_back(
                test::element(
                    projectFingerprint,
                    test::elementId(k_elementB),
                    "back",
                    test::capabilities(
                        std::nullopt,
                        Interact{.clickOffset = test::templateOffset(1, 1, 2, 2)}
                    ),
                    test::pixelRect(0, 0, 4, 4),
                    std::vector<Appearance>{
                        test::appearance(
                            "on_dark",
                            test::sourceId(k_sourceS),
                            test::pixelRect(0, 0, 2, 2)
                        ),
                        test::appearance(
                            "on_light",
                            test::sourceId(k_sourceS),
                            test::pixelRect(2, 0, 2, 2)
                        ),
                    }
                )
            );

            auto pages = std::vector<PageSpec>{};
            pages.emplace_back(test::page(test::pageId(k_pageP), "home"));
            pages.emplace_back(test::page(test::pageId(k_pageQ), "sortie"));

            auto references = std::vector<PageReference>{};
            references.emplace_back(
                test::reference(
                    test::pageId(k_pageP),
                    test::elementId(k_elementA),
                    test::identifiesAs()
                )
            );
            references.emplace_back(
                test::reference(
                    test::pageId(k_pageQ),
                    test::elementId(k_elementC),
                    test::identifiesAs()
                )
            );
            references.emplace_back(
                test::reference(
                    test::pageId(k_pageP),
                    test::elementId(k_elementB),
                    test::interacts(),
                    Holding::Owned,
                    std::nullopt,
                    test::resourceName("on_dark")
                )
            );
            references.emplace_back(
                test::reference(
                    test::pageId(k_pageQ),
                    test::elementId(k_elementB),
                    test::interacts(),
                    Holding::Referenced,
                    std::nullopt,
                    test::resourceName("on_light")
                )
            );

            auto sources = std::vector<AuthoringSource>{};
            sources.emplace_back(*std::move(source));
            auto document = AuthoringDocument::create(
                test::projectId(),
                projectFingerprint,
                std::move(sources),
                std::move(elements),
                std::move(pages),
                std::move(references),
                std::vector<RegressionCase>{}
            );
            REQUIRE(document.has_value());
            return DocumentFixture{
                .document    = *std::move(document),
                .sourceAsset = AuthoringSourceAsset{
                    .id       = test::sourceId(k_sourceS),
                    .pngBytes = std::move(pngBytes),
                },
            };
        }
    }

    TEST_CASE("the authoring document round-trips through its canonical form")
    {
        auto const fixture = sharedElementFixture();
        auto const encoded = serializeAuthoringDocument(fixture.document);
        auto const parsed  = parseAuthoringDocument(encoded);
        REQUIRE(parsed.has_value());
        CHECK(serializeAuthoringDocument(*parsed) == encoded);
        CHECK(parsed->elements().size() == 3U);
        CHECK(parsed->references().size() == 4U);

        auto const* p_back = parsed->findElement(test::elementId(k_elementB));
        REQUIRE(p_back != nullptr);
        REQUIRE(p_back->appearances().size() == 2U);
        CHECK(p_back->appearances().front().name().value() == "on_dark");
        CHECK(p_back->capabilities().hasInteract());
    }

    TEST_CASE("the retired authoring schema is refused rather than upgraded")
    {
        auto const fixture = sharedElementFixture();
        auto encoded       = serializeAuthoringDocument(fixture.document);
        replaceOnce(encoded, "umbraflow-authoring/v4", "umbraflow-authoring/v3");

        auto const parsed = parseAuthoringDocument(encoded);
        REQUIRE(!parsed.has_value());
        test::requireErrorKind(parsed.error(), AutomationErrorKind::InvalidResource);
        // Named so that this stays a test of the schema gate. Without the
        // message, the canonical round-trip check would refuse the same bytes
        // for its own reason and the case could never go red.
        CHECK(parsed.error().message().contains("schema"));
    }

    TEST_CASE("a document out of canonical order is data the parser still refuses")
    {
        auto const fixture = sharedElementFixture();
        auto const encoded = serializeAuthoringDocument(fixture.document);
        auto const swapped = swappedPageBlocks(encoded);
        REQUIRE(swapped != encoded);
        REQUIRE(swapped.size() == encoded.size());

        auto const parsed = parseAuthoringDocument(swapped);
        REQUIRE(!parsed.has_value());
        test::requireErrorKind(parsed.error(), AutomationErrorKind::InvalidResource);
        CHECK(parsed.error().message().contains("canonical"));
    }

    TEST_CASE("every element compiles to one element under its own id")
    {
        auto const fixture = sharedElementFixture();
        auto const assets  = std::vector<AuthoringSourceAsset>{fixture.sourceAsset};
        auto const compiled = compileAuthoringDocument(fixture.document, assets);
        REQUIRE(compiled.has_value());

        auto const& catalog = compiled->runtimeManifest.catalog();
        CHECK(catalog.elements().size() == 3U);

        // The element two pages reference is present under the id the author
        // minted, which is the id its page signatures and every script name.
        auto const* p_back = catalog.findElement(test::elementId(k_elementB));
        REQUIRE(p_back != nullptr);
        CHECK(p_back->name().value() == "back");
        REQUIRE(p_back->appearances().size() == 2U);

        // One template asset per appearance, not per page.
        CHECK(compiled->runtimeManifest.assets().size() == 4U);
        CHECK(
            compiled->runtimeManifest.findAsset(
                test::elementId(k_elementB),
                test::resourceName("on_light")
            ) != nullptr
        );

        // The page references reach the runtime unchanged; they are what
        // authorises a click there.
        CHECK(catalog.references().size() == 4U);
        auto const* p_reference = catalog.findReference(
            test::pageId(k_pageQ),
            test::elementId(k_elementB)
        );
        REQUIRE(p_reference != nullptr);
        CHECK(p_reference->holding == Holding::Referenced);
        CHECK(p_reference->exercised.hasInteract());
    }

    TEST_CASE("the runtime manifest round-trips and refuses its retired schema")
    {
        auto const fixture = sharedElementFixture();
        auto const assets  = std::vector<AuthoringSourceAsset>{fixture.sourceAsset};
        auto const compiled = compileAuthoringDocument(fixture.document, assets);
        REQUIRE(compiled.has_value());

        auto const parsed = parseRuntimeManifest(compiled->runtimeManifestToml);
        REQUIRE(parsed.has_value());
        CHECK(serializeRuntimeManifest(*parsed) == compiled->runtimeManifestToml);

        auto encoded = compiled->runtimeManifestToml;
        replaceOnce(encoded, "umbraflow-annotations/v3", "umbraflow-annotations/v2");
        auto const rejected = parseRuntimeManifest(encoded);
        REQUIRE(!rejected.has_value());
        test::requireErrorKind(rejected.error(), AutomationErrorKind::InvalidResource);
        CHECK(rejected.error().message().contains("schema"));
    }

    namespace
    {
        constexpr auto k_backgroundLevel = uint8{200};
        constexpr auto k_loosePixel      = uint8{100};
        constexpr auto k_shiftedPixel    = uint8{108};

        // A 4x4 screen where the top-left pixel is one grey and a distinctive
        // 2x2 block sits at (2, 2).
        [[nodiscard]]
        auto greyScreen(uint8 topLeft) -> std::vector<std::byte>
        {
            auto pixels = std::vector<std::byte>{};
            pixels.reserve(std::size_t{4} * 4U * 4U);
            constexpr auto k_block = std::array<uint8, 4>{10, 20, 30, 40};
            for (auto y = std::size_t{0}; y < 4U; ++y)
            {
                for (auto x = std::size_t{0}; x < 4U; ++x)
                {
                    auto level = k_backgroundLevel;
                    if (x == 0U && y == 0U)
                    {
                        level = topLeft;
                    }
                    else if (x >= 2U && y >= 2U)
                    {
                        level = checkedAt(k_block, (y - 2U) * 2U + (x - 2U));
                    }
                    pixels.emplace_back(asByte(level));
                    pixels.emplace_back(asByte(level));
                    pixels.emplace_back(asByte(level));
                    pixels.emplace_back(asByte(255));
                }
            }
            return pixels;
        }

        [[nodiscard]]
        auto frameWithPixels(
            ProjectFingerprint projectFingerprint,
            std::vector<std::byte> bgra
        ) -> Frame
        {
            auto const transform = CoordinateTransform::create(
                Point<DesktopSpace>{0.0F, 0.0F},
                static_cast<float>(projectFingerprint.width()),
                static_cast<float>(projectFingerprint.height()),
                projectFingerprint.width(),
                projectFingerprint.height()
            );
            REQUIRE(transform.has_value());
            auto const pixels = std::shared_ptr<FrameBuffer const>{
                std::make_shared<FrameBuffer>(std::move(bgra))
            };
            auto frame = Frame::create(
                FrameId{1},
                CaptureSessionId{1},
                TargetGeneration::initial(),
                test::instantAt(MonotonicInstant::Duration{0}),
                projectFingerprint.width(),
                projectFingerprint.height(),
                std::size_t{4} * 4U,
                PixelFormat::Bgra8,
                pixels,
                *transform
            );
            REQUIRE(frame.has_value());
            return *std::move(frame);
        }
    }

    TEST_CASE("declaration order settles ties and never outranks a better margin")
    {
        auto const projectFingerprint = test::fingerprint(4, 4);
        auto sourcePixels = greyScreen(k_loosePixel);
        auto pngBytes     = image::encodeRgbaPng("fold-source.png", 4, 4, sourcePixels);
        REQUIRE(pngBytes.has_value());
        auto const sourceHash = sha256(*pngBytes);
        REQUIRE(sourceHash.has_value());
        auto source = AuthoringSource::create(
            AuthoringSourceSpec{
                .id          = test::sourceId(k_sourceS),
                .contentHash = *sourceHash,
                .fingerprint = projectFingerprint,
                .provenance  = ImportedSourceProvenance{},
            }
        );
        REQUIRE(source.has_value());

        auto elements = std::vector<Element>{};
        elements.emplace_back(
            test::element(
                projectFingerprint,
                test::elementId(k_elementA),
                "home_mark",
                test::capabilities(Identify{}),
                test::pixelRect(0, 0, 4, 4),
                std::vector<Appearance>{
                    test::appearance(
                        "only",
                        test::sourceId(k_sourceS),
                        test::pixelRect(2, 2, 2, 2)
                    ),
                }
            )
        );
        // The wide one is declared FIRST. A single pixel matches almost
        // anywhere, so "first past the threshold wins" would answer with it and
        // hand back its own rectangle.
        elements.emplace_back(
            test::element(
                projectFingerprint,
                test::elementId(k_elementB),
                "back",
                test::capabilities(std::nullopt, Interact{}),
                test::pixelRect(0, 0, 4, 4),
                std::vector<Appearance>{
                    test::appearance(
                        "wide",
                        test::sourceId(k_sourceS),
                        test::pixelRect(0, 0, 1, 1)
                    ),
                    test::appearance(
                        "narrow",
                        test::sourceId(k_sourceS),
                        test::pixelRect(2, 2, 2, 2)
                    ),
                }
            )
        );

        auto pages = std::vector<PageSpec>{};
        pages.emplace_back(test::page(test::pageId(k_pageP), "home"));
        auto references = std::vector<PageReference>{};
        references.emplace_back(
            test::reference(
                test::pageId(k_pageP),
                test::elementId(k_elementA),
                test::identifiesAs()
            )
        );
        references.emplace_back(
            test::reference(
                test::pageId(k_pageP),
                test::elementId(k_elementB),
                test::interacts()
            )
        );
        auto sources = std::vector<AuthoringSource>{};
        sources.emplace_back(*std::move(source));
        auto document = AuthoringDocument::create(
            test::projectId(),
            projectFingerprint,
            std::move(sources),
            std::move(elements),
            std::move(pages),
            std::move(references),
            std::vector<RegressionCase>{}
        );
        REQUIRE(document.has_value());

        auto assets = std::vector<AuthoringSourceAsset>{};
        assets.emplace_back(
            AuthoringSourceAsset{
                .id       = test::sourceId(k_sourceS),
                .pngBytes = *pngBytes,
            }
        );
        auto compiled = compileAuthoringDocument(*document, assets);
        REQUIRE(compiled.has_value());

        auto encodedTemplates = std::vector<EncodedRuntimeTemplate>{};
        for (auto& asset : compiled->templateAssets)
        {
            encodedTemplates.emplace_back(
                EncodedRuntimeTemplate{
                    .hash     = asset.hash,
                    .pngBytes = std::move(asset.pngBytes),
                }
            );
        }
        auto runtime = RecognitionRuntime::create(
            std::move(compiled->runtimeManifest),
            std::move(encodedTemplates)
        );
        REQUIRE(runtime.has_value());

        // The live screen shifts the single pixel the wide appearance was cut
        // from, so that one still passes its threshold but no longer exactly:
        // the narrow appearance is the strictly better margin.
        auto const frame = frameWithPixels(
            projectFingerprint,
            greyScreen(k_shiftedPixel)
        );
        auto const attempt = runtime->evaluateActionTarget(
            frame,
            projectFingerprint,
            test::pageId(k_pageP),
            test::elementId(k_elementB),
            RecognitionPolicy{.maximumPixelComparisons = 1'000'000}
        );
        REQUIRE(attempt.has_value());
        auto const* p_evidence = std::get_if<AnchorEvidence>(&attempt->result);
        REQUIRE(p_evidence != nullptr);
        CHECK(p_evidence->hit());
        REQUIRE(p_evidence->appearanceName().has_value());
        CHECK(p_evidence->appearanceName()->value() == "narrow");
        REQUIRE(p_evidence->matchedRect().has_value());
        CHECK(*p_evidence->matchedRect() == test::pixelRect(2, 2, 2, 2));
    }

    TEST_CASE("an element with no appearances is located where the page put it")
    {
        auto const fixture = sharedElementFixture();
        auto const assets  = std::vector<AuthoringSourceAsset>{fixture.sourceAsset};
        auto compiled      = compileAuthoringDocument(fixture.document, assets);
        REQUIRE(compiled.has_value());

        auto encodedTemplates = std::vector<EncodedRuntimeTemplate>{};
        for (auto& asset : compiled->templateAssets)
        {
            encodedTemplates.emplace_back(
                EncodedRuntimeTemplate{
                    .hash     = asset.hash,
                    .pngBytes = std::move(asset.pngBytes),
                }
            );
        }
        auto runtime = RecognitionRuntime::create(
            std::move(compiled->runtimeManifest),
            std::move(encodedTemplates)
        );
        REQUIRE(runtime.has_value());

        auto const projectFingerprint = test::fingerprint(4, 4);
        auto const frame              = test::frame(
            projectFingerprint,
            CaptureSessionId{1},
            TargetGeneration::initial(),
            FrameId{1},
            test::instantAt(MonotonicInstant::Duration{0})
        );
        // The page does not exercise interact on its anchor, so there is no
        // action there to locate: authorisation IS the reference.
        auto const refused = runtime->evaluateActionTarget(
            frame,
            projectFingerprint,
            test::pageId(k_pageP),
            test::elementId(k_elementA),
            RecognitionPolicy{.maximumPixelComparisons = 1'000'000}
        );
        REQUIRE(!refused.has_value());
        test::requireErrorKind(refused.error(), AutomationErrorKind::InvalidResource);
    }
}
