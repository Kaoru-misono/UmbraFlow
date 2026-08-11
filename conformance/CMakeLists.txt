# umbra-flow-conformance, and this repository's two runs of it.
#
# The suite is one shipped binary rather than a set of translation units a
# consumer compiles: a project is a directory of data, so the only thing a run
# varies is the directory it is pointed at. examples/umbraflow and
# examples/arcana-expedition are two such directories, written the way a
# consuming repository writes its own, and they are documentation a consumer
# copies rather than scaffolding the suite needs -- nothing in source/ or
# include/ knows either of them by name.

# Every case the suite runs. Concrete paths, never a glob: a case that stops
# being compiled must be a visible deletion here rather than a file that quietly
# left the build.
set(UF_CONFORMANCE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/source/suite-main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/suite-support.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/suite-control-ledger.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/suite-project-authority.cpp"
)

add_executable(umbra-flow-conformance ${UF_CONFORMANCE_SOURCES})
cpp_apply_safety_profile(umbra-flow-conformance)
cpp_apply_utf8_encoding(umbra-flow-conformance)
target_compile_definitions(umbra-flow-conformance PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
target_include_directories(umbra-flow-conformance PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/source"
)
# uf::image and uf::deployment are the suite's own dependencies, not a
# project's: observation-fixture.hpp decodes the project's probe frame with
# <image/png.hpp>, and the loader that turns a directory into five authorities
# is the deployment's. The Operator links neither, and must not link the second.
target_link_libraries(umbra-flow-conformance PRIVATE
    uf::operator
    uf::image
    uf::deployment
    uf::doctest
)
target_compile_features(umbra-flow-conformance PRIVATE cxx_std_23)

# The Operator links task publicly, which links engine and ocr publicly, and
# onnxruntime resolves beside the executable rather than from PATH.
cpp_stage_runtime_libraries(umbra-flow-conformance ${UF_OCR_ONNXRUNTIME_SHARED_LIBRARIES})

uf_declared_conformance_cases(UF_CONFORMANCE_DECLARED_CASES ${UF_CONFORMANCE_SOURCES})

# This repository's own run. It is the one that claims the migration report's
# contract IDs as named CTests, because the IDs describe the Operator this
# repository ships; every other run is a consumer's single aggregate gate.
#
# C-09 through C-13 are here because their store behaviour is here. Only their
# schema shape stayed in tests/operator, where it is registered under the
# schema- name, so the contract- name belongs to the case that goes red when the
# ledger stops enforcing them.
uf_add_conformance_run(
    PROJECT   umbraflow
    DIRECTORY "${PROJECT_SOURCE_DIR}/examples/umbraflow"
    DECLARED  ${UF_CONFORMANCE_DECLARED_CASES}
    CASES
        contract-control-c01
        contract-control-c06
        contract-control-c09
        contract-control-c10
        contract-control-c11
        contract-control-c12
        contract-control-c13
)

# Everything a consuming repository writes to run the suite, in full: one call
# naming a directory.
uf_add_conformance_run(
    PROJECT   arcana
    DIRECTORY "${PROJECT_SOURCE_DIR}/examples/arcana-expedition"
)
