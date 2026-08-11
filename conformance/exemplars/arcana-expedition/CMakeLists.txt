# Everything a consuming repository writes to run the suite, in full.
uf_add_conformance_suite(
    TARGET  conformance-arcana
    PROJECT arcana
    SOURCES provider.cpp
    LIBS    uf::image uf::deployment
)
