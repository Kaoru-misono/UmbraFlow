#pragma once

#include "host-delivery-fixture.hpp"

#include <deployment/project-deployment.hpp>

#include <operator/effective-plan.hpp>

#include <core/error/result.hpp>

#include <domain/error.hpp>

#include <string_view>
#include <utility>

namespace uf::operator_runtime::conformance
{
    // The plan authority a deployment builds. The exact operator protocol bytes
    // are the ones the session manifest is pinned to, and the RuntimeModel
    // binding is the model the Host parsed out of the artifact that manifest
    // pins, so an authority that answers for another schema or another model
    // cannot be created at all.
    //
    // The two readers are the deployment's, not the suite's: the operator
    // protocol is the Operator's own schema and is the same for every project,
    // so what a provider supplies is the documents and what a consumer writes
    // is a ProjectVocabulary and never a JSON reader.
    //
    // What the suite adds is one refusal of its own: the step reader below also
    // refuses a UI-action step naming anything but `uiAction`. A contract run
    // drives exactly one UI action -- the one the project's ProjectVocabulary
    // names -- so a plan that named another would be telling the suite two
    // different things about what this Operation does. That refusal is the
    // suite's and is about agreement with the run; the Operator's own refusal,
    // against the installed model, is in mintStep and holds for production
    // callers that never see this file.
    [[nodiscard]]
    inline auto planAuthority(
        VerifiedProjectRegistration const& registration,
        SessionManifest const& manifest,
        task::RuntimeModelBinding const& runtimeModel,
        std::string_view exactOperatorProtocolSchemaBytes,
        task::UiActionUnderTest const& uiAction
    ) -> Result<OperatorPlanAuthority>
    {
        return OperatorPlanAuthority::create(
            registration,
            manifest,
            runtimeModel,
            exactOperatorProtocolSchemaBytes,
            deployment::readPlanProposal,
            [uiAction](ValidatedDocument const& intent) -> Result<StepIntentClaims>
            {
                UF_TRY_VALUE(claims, deployment::readStepIntent(intent));
                if (claims.kind != StepKind::UiAction)
                {
                    return claims;
                }
                if (
                    claims.surfaceId != uiAction.surface
                    || claims.uiTargetId != uiAction.uiTarget
                    || claims.actionId != uiAction.action
                )
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "a UIActionIntent names a UI action other than the one "
                        "this run agreed on"
                    );
                }
                return claims;
            }
        );
    }
}
