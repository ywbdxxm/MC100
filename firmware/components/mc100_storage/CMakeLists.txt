idf_component_register(SRCS "crc32.c" "wav.c" "journal.c" "writer.c" "recovery.c"
                       INCLUDE_DIRS "include"
                       REQUIRES mc100_core)
target_compile_features(${COMPONENT_LIB} PUBLIC c_std_11)
