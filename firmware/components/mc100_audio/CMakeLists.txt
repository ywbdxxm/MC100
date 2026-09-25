set(MC100_AUDIO_SRCS "frame_assembler.c" "pcm_filter.c")
if(NOT CONFIG_MC100_APP_PRODUCT OR CONFIG_MC100_LEGACY_RUNTIME)
    list(APPEND MC100_AUDIO_SRCS
         "audio.c" "preroll.c" "stream.c" "vad_fixed.c")
endif()

idf_component_register(
    SRCS ${MC100_AUDIO_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES mc100_core
)
