# This repository's own run. It is the one that claims the migration report's
# contract IDs as named CTests, because the IDs describe the Operator this
# repository ships; every other run is a consumer's single aggregate gate.
#
# C-09 through C-13 are here because their store behaviour is here. Only their
# schema shape stayed in tests/operator, where it is registered under the
# schema- name, so the contract- name belongs to the case that goes red when the
# ledger stops enforcing them.
uf_add_conformance_suite(
    TARGET  conformance-umbraflow
    PROJECT umbraflow
    SOURCES provider.cpp
    LIBS    uf::image
    CASES
        contract-control-c01
        contract-control-c06
        contract-control-c09
        contract-control-c10
        contract-control-c11
        contract-control-c12
        contract-control-c13
)
