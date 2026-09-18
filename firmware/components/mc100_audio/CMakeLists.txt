idf_component_register(
    SRCS "audio.c" "preroll.c" "stream.c" "frame_assembler.c"
    INCLUDE_DIRS "include"
    REQUIRES mc100_core
)
