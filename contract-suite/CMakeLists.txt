# Each subdirectory is one project the suite runs against, and each is written
# the way a consuming repository writes its own: a CMakeLists.txt that calls
# uf_add_operator_contract_suite, and a provider translation unit. Nothing in
# source/ or include/ knows either of them by name.
add_subdirectory(fixtures/umbraflow)
add_subdirectory(fixtures/arcana-expedition)
