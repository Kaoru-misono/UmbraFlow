# Everything a consuming repository writes to run the suite, in full.
uf_add_operator_contract_suite(
    TARGET  contract-suite-arcana
    PROJECT arcana
    SOURCES provider.cpp
    LIBS    ${PROJECT_NAME}_image
)
