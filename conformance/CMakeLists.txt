# Each exemplar is one project the suite runs against, and each is written the
# way a consuming repository writes its own: a CMakeLists.txt that calls
# uf_add_conformance_suite, and a provider translation unit. They are exemplars
# rather than fixtures because they are documentation a consumer copies, not
# scaffolding the suite needs -- nothing in source/ or include/ knows either of
# them by name.
add_subdirectory(exemplars/umbraflow)
add_subdirectory(exemplars/arcana-expedition)
