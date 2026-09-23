idf_component_register(
    SRCS "supervisor.c"
    INCLUDE_DIRS "include"
    REQUIRES mc100_core mc100_audio mc100_storage
)
