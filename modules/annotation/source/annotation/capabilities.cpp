#include "capabilities.hpp"

#include <domain/error.hpp>

#include <optional>

namespace uf::annotation
{
    ElementCapabilities::ElementCapabilities(
        std::optional<Identify> identify,
        std::optional<Interact> interact,
        std::optional<Read> read
    ) noexcept
        : m_identify{identify}
        , m_interact{interact}
        , m_read{read}
    {
    }

    auto ElementCapabilities::create(
        std::optional<Identify> identify,
        std::optional<Interact> interact,
        std::optional<Read> read
    ) -> Result<ElementCapabilities>
    {
        if (
            !identify.has_value()
            && !interact.has_value()
            && !read.has_value()
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "element capabilities must contain at least one of identify, interact, or read"
            );
        }

        return ElementCapabilities{identify, interact, read};
    }

    auto ElementCapabilities::identify() const noexcept -> std::optional<Identify> const&
    {
        return m_identify;
    }

    auto ElementCapabilities::interact() const noexcept -> std::optional<Interact> const&
    {
        return m_interact;
    }

    auto ElementCapabilities::read() const noexcept -> std::optional<Read> const&
    {
        return m_read;
    }

    auto ElementCapabilities::hasIdentify() const noexcept -> bool
    {
        return m_identify.has_value();
    }

    auto ElementCapabilities::hasInteract() const noexcept -> bool
    {
        return m_interact.has_value();
    }

    auto ElementCapabilities::hasRead() const noexcept -> bool
    {
        return m_read.has_value();
    }

    ExercisedCapabilities::ExercisedCapabilities(
        std::optional<ExercisedIdentify> identify,
        std::optional<ExercisedInteract> interact,
        std::optional<ExercisedRead> read
    ) noexcept
        : m_identify{identify}
        , m_interact{interact}
        , m_read{read}
    {
    }

    auto ExercisedCapabilities::create(
        std::optional<ExercisedIdentify> identify,
        std::optional<ExercisedInteract> interact,
        std::optional<ExercisedRead> read
    ) -> Result<ExercisedCapabilities>
    {
        if (
            !identify.has_value()
            && !interact.has_value()
            && !read.has_value()
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "page reference must exercise at least one of identify, interact, or read"
            );
        }

        return ExercisedCapabilities{identify, interact, read};
    }

    auto ExercisedCapabilities::isSubsetOf(
        ElementCapabilities const& declared
    ) const noexcept -> bool
    {
        auto const identifyDeclared = !hasIdentify() || declared.hasIdentify();
        auto const interactDeclared = !hasInteract() || declared.hasInteract();
        auto const readDeclared     = !hasRead() || declared.hasRead();

        return identifyDeclared && interactDeclared && readDeclared;
    }

    auto ExercisedCapabilities::identify() const noexcept
        -> std::optional<ExercisedIdentify> const&
    {
        return m_identify;
    }

    auto ExercisedCapabilities::interact() const noexcept
        -> std::optional<ExercisedInteract> const&
    {
        return m_interact;
    }

    auto ExercisedCapabilities::read() const noexcept -> std::optional<ExercisedRead> const&
    {
        return m_read;
    }

    auto ExercisedCapabilities::hasIdentify() const noexcept -> bool
    {
        return m_identify.has_value();
    }

    auto ExercisedCapabilities::hasInteract() const noexcept -> bool
    {
        return m_interact.has_value();
    }

    auto ExercisedCapabilities::hasRead() const noexcept -> bool
    {
        return m_read.has_value();
    }
}
