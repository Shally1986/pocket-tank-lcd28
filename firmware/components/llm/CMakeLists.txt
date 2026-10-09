# On-device inference engine — shared verbatim with the PC sim via ../../../common/llm
set(LLM ${CMAKE_CURRENT_LIST_DIR}/../../../common/llm)
idf_component_register(
    SRCS "${LLM}/q4_model.c" "${LLM}/word_tok.c" "${LLM}/advisor_core.c"
    INCLUDE_DIRS "${LLM}"
    REQUIRES esp_timer tankcore
)
# Xtensa GCC 13.2 (IDF 5.3.5) hits a backend ICE in q4_model.c with
# -ffast-math: the postreload pass emits a movsf from a constant pool that
# "does not satisfy its constraints" (recog.cc:2692) at -O3 and -O2 alike.
# It is -ffinite-math-only that arms the bad transform; -fno-finite-math-only
# after -ffast-math builds cleanly and deterministically, keeping the rest of
# the fast-math set (unsafe math, contraction, no signed zeros). Revisit on a
# newer toolchain (IDF >=5.4 / GCC 14).
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -ffast-math -fno-finite-math-only)
if(CONFIG_POCKET_TANK_NO_PIE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE POCKET_TANK_NO_PIE)
endif()
