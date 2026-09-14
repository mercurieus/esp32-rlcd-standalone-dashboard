cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Optional, Kconfig-gated capabilities (audio, later others) live outside
# components/ - see modules/README.md for the contract each one follows.
set(EXTRA_COMPONENT_DIRS modules)

project(layout_carousel)

# LVGL's heap moves off internal RAM and into PSRAM.
#
# LV_MEM_POOL_ALLOC is LVGL's supported hook for supplying the built-in TLSF
# allocator's pool; lv_conf_internal.h guards both macros with #ifndef, so a
# definition from here survives its Kconfig defaulting. See
# components/board_rlcd/include/lvgl_pool.h for why the pool must not sit in
# internal RAM on this board.
#
# Set here rather than in the component, because the target being configured
# is LVGL's, not ours - and the include path has to follow the define, or the
# declaration of lvgl_pool_alloc() never reaches lv_mem_core_builtin.c.
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_include_directories(${lvgl_lib}
    PRIVATE ${CMAKE_CURRENT_LIST_DIR}/components/board_rlcd/include)
target_compile_definitions(${lvgl_lib} PRIVATE
    LV_MEM_POOL_INCLUDE="lvgl_pool.h"
    LV_MEM_POOL_ALLOC=lvgl_pool_alloc)
