cmake_minimum_required(VERSION 3.20)
project(GameCubeMenuC VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# Private input, state, captures and build products share one ignored boundary.
file(REAL_PATH "${CMAKE_BINARY_DIR}" GC_BUILD_PATH)
file(REAL_PATH "${CMAKE_SOURCE_DIR}/Files" GC_FILES_PATH)
string(FIND "${GC_BUILD_PATH}/" "${GC_FILES_PATH}/" GC_BUILD_IN_FILES)
if(NOT GC_BUILD_IN_FILES EQUAL 0)
    message(FATAL_ERROR "Configure inside Files, for example -B Files/build.")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Common/CMakeLists.txt")
    message(FATAL_ERROR "Initialize Common with git submodule update --init.")
endif()
set(CC_FRAME_WIDTH 640 CACHE STRING "Logical framebuffer width")
set(CC_FRAME_HEIGHT 480 CACHE STRING "Logical framebuffer height")
set(CC_ASPECT_WIDTH 4 CACHE STRING "Display aspect numerator")
set(CC_ASPECT_HEIGHT 3 CACHE STRING "Display aspect denominator")
add_subdirectory(Common)

function(gc_warnings target)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic
            -Wconversion -Werror)
    endif()
endfunction()

add_library(gamecube_core STATIC src/menu.c src/menu_clock.c src/menu_cards.c
    src/text.c src/font.c src/ipl.c src/ipl_model.c src/ipl_texture.c
    src/ipl_material.c src/ipl_animation.c src/audio.c src/audio_resources.c
    src/audio_dsp.c
    src/audio_sequence.c src/audio_envelope.c
    src/card_image.c src/card_checksum.c src/card_art.c src/services.c src/state.c src/disc.c
    src/startup.c src/startup_resources.c src/startup_motion.c
    src/layout.c src/texture_collection.c src/menu_animation.c src/face_geometry.c
    src/edit_geometry.c
    src/boot_control.c src/help.c src/page_transition.c src/card_operation.c
    src/launch_control.c src/navigation_sound.c src/input_control.c src/config.c
    src/card_popup.c
    src/frame_control.c src/card_usage.c src/card_lighting.c src/card_cells.c
    src/disc_control.c src/error_control.c
    src/value_morph.c src/option_values.c)
target_include_directories(gamecube_core PUBLIC include)
target_link_libraries(gamecube_core PUBLIC console_common)
gc_warnings(gamecube_core)
if(APPLE)
    target_sources(gamecube_core PRIVATE src/platform/audio_apple.c)
    target_link_libraries(gamecube_core PUBLIC "-framework AudioToolbox")
endif()

add_library(gamecube_scene STATIC src/render.c src/render_resources.c
    src/render_mesh.c src/render_layout.c src/render_material.c src/card_runtime.c
    src/frame_history.c)
target_include_directories(gamecube_scene PUBLIC include PRIVATE src)
target_link_libraries(gamecube_scene PUBLIC gamecube_core)
gc_warnings(gamecube_scene)

# The offline backend is shared by capture tools and headless scene tests.
add_library(gamecube_software STATIC src/software.c src/software_material.c)
target_include_directories(gamecube_software PRIVATE src)
target_link_libraries(gamecube_software PUBLIC console_common)
gc_warnings(gamecube_software)

add_library(gamecube_tool_io STATIC tools/output.c)
target_include_directories(gamecube_tool_io PUBLIC tools)
gc_warnings(gamecube_tool_io)

add_executable(gc-ipl-tool tools/ipl/main.c)
target_link_libraries(gc-ipl-tool PRIVATE gamecube_core gamecube_tool_io)
gc_warnings(gc-ipl-tool)

add_executable(gc-audio-tool tools/audio/main.c)
target_link_libraries(gc-audio-tool PRIVATE gamecube_core gamecube_tool_io)
target_include_directories(gc-audio-tool PRIVATE src)
gc_warnings(gc-audio-tool)

add_executable(gamecube-render tools/render.c)
target_include_directories(gamecube-render PRIVATE src)
target_link_libraries(gamecube-render PRIVATE gamecube_scene gamecube_software
    gamecube_tool_io)
gc_warnings(gamecube-render)

option(GC_BUILD_APP "Build the native menu application" ON)
if(APPLE)
    set(GC_BACKEND metal CACHE STRING "Graphics backend")
else()
    set(GC_BACKEND gles2 CACHE STRING "Graphics backend")
endif()
set_property(CACHE GC_BACKEND PROPERTY STRINGS metal gles2)
if(GC_BUILD_APP)
    if(APPLE)
        add_executable(gamecube-menu MACOSX_BUNDLE src/main.c src/app_options.c)
        set_target_properties(gamecube-menu PROPERTIES
            MACOSX_BUNDLE_GUI_IDENTIFIER "org.menu-recovery.gamecube-menu-c"
            MACOSX_BUNDLE_BUNDLE_NAME "GameCube Menu in C")
        add_custom_command(TARGET gamecube-menu POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E rm -f "${CMAKE_BINARY_DIR}/gamecube-menu"
            COMMAND "${CMAKE_COMMAND}" -E copy
                "${PROJECT_SOURCE_DIR}/tools/launch.sh"
                "${CMAKE_BINARY_DIR}/gamecube-menu" VERBATIM)
        set_property(TARGET gamecube-menu APPEND PROPERTY LINK_DEPENDS
            "${PROJECT_SOURCE_DIR}/tools/launch.sh")
    else()
        add_executable(gamecube-menu src/main.c src/app_options.c)
    endif()
    target_include_directories(gamecube-menu PRIVATE src)
    target_link_libraries(gamecube-menu PRIVATE gamecube_scene)
    cc_add_platform(gamecube-menu "${GC_BACKEND}")
    gc_warnings(gamecube-menu)
endif()

include(CTest)
if(BUILD_TESTING)
    # Every test keeps assertions enabled, including release builds.
    function(gc_test_executable target)
        add_executable(${target} ${ARGN})
        target_include_directories(${target} PRIVATE src)
        target_compile_options(${target} PRIVATE -UNDEBUG)
        gc_warnings(${target})
    endfunction()

    function(gc_test_directory name directory)
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tests/${directory}")
        set_tests_properties(${name} PROPERTIES
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/tests/${directory}")
    endfunction()

    gc_test_executable(gc-tool-output-test tests/tool_output_test.c)
    target_link_libraries(gc-tool-output-test PRIVATE gamecube_tool_io)
    add_test(NAME gc-tool-output COMMAND gc-tool-output-test)
    gc_test_directory(gc-tool-output tool-output)
    foreach(name menu ipl ipl_model ipl_material ipl_animation card_image card_art
        state services audio text font disc startup layout texture_collection menu_animation
        face_geometry edit_geometry boot_control help page_transition card_operation
        launch_control navigation_sound input_control config frame_control card_popup
        card_usage card_lighting card_cells value_morph disc_control error_control)
        gc_test_executable(gc-${name}-test tests/${name}_test.c)
        target_link_libraries(gc-${name}-test PRIVATE gamecube_core)
        add_test(NAME gc-${name} COMMAND gc-${name}-test)
    endforeach()
    foreach(region USA EUR JAP)
        set(GC_PRIVATE_IPL "${PROJECT_SOURCE_DIR}/Files/GameCube_BIOS/${region}/IPL.bin")
        if(EXISTS "${GC_PRIVATE_IPL}")
            foreach(module audio text font startup boot_control layout
                texture_collection menu_animation face_geometry edit_geometry
                help page_transition card_popup card_usage card_lighting
                card_cells value_morph scene card-runtime card-cells-scene)
                string(REPLACE "_" "-" test_name "${module}")
                add_test(NAME gc-${test_name}-${region}
                    COMMAND gc-${module}-test "${GC_PRIVATE_IPL}")
            endforeach()
            add_test(NAME gc-render-material-${region}
                COMMAND gc-render_material-test
                    "${CMAKE_BINARY_DIR}/render-material-${region}.ppm"
                    "${GC_PRIVATE_IPL}")
            gc_test_directory(gc-card-runtime-${region} card-runtime/${region})
            foreach(module app-tick app-control)
                add_test(NAME gc-${module}-${region}
                    COMMAND gc-${module}-test "${GC_PRIVATE_IPL}" "${region}")
                gc_test_directory(gc-${module}-${region} ${module}/${region})
            endforeach()
            file(MAKE_DIRECTORY
                "${CMAKE_BINARY_DIR}/tests/app-control/${region}/Files")
        endif()
    endforeach()
    gc_test_executable(gc-software-test tests/software_test.c)
    target_link_libraries(gc-software-test PRIVATE gamecube_core gamecube_software)
    add_test(NAME gc-software COMMAND gc-software-test
        "${CMAKE_BINARY_DIR}/software-test.ppm")
    gc_test_executable(gc-render_material-test tests/render_material_test.c)
    target_link_libraries(gc-render_material-test PRIVATE gamecube_scene gamecube_software)
    add_test(NAME gc-render-material COMMAND gc-render_material-test
        "${CMAKE_BINARY_DIR}/render-material-test.ppm")
    gc_test_executable(gc-scene-test tests/scene_test.c)
    target_link_libraries(gc-scene-test PRIVATE gamecube_scene gamecube_software)
    add_test(NAME gc-scene COMMAND gc-scene-test)
    gc_test_executable(gc-card-cells-scene-test tests/card_cells_scene_test.c)
    target_link_libraries(gc-card-cells-scene-test PRIVATE gamecube_scene gamecube_software)
    add_test(NAME gc-card-cells-scene COMMAND gc-card-cells-scene-test)
    gc_test_executable(gc-card-runtime-test tests/card_runtime_test.c)
    target_link_libraries(gc-card-runtime-test PRIVATE gamecube_scene gamecube_software)
    add_test(NAME gc-card-runtime COMMAND gc-card-runtime-test)
    gc_test_directory(gc-card-runtime card-runtime/base)
    gc_test_executable(gc-frame-history-test tests/frame_history_test.c)
    target_link_libraries(gc-frame-history-test PRIVATE gamecube_scene)
    add_test(NAME gc-frame-history COMMAND gc-frame-history-test)
    gc_test_executable(gc-app-tick-test tests/app_tick_test.c src/app_options.c)
    target_link_libraries(gc-app-tick-test PRIVATE gamecube_scene gamecube_software)
    gc_test_executable(gc-app-control-test tests/app_control_test.c src/app_options.c)
    target_link_libraries(gc-app-control-test PRIVATE gamecube_scene gamecube_software)
    gc_test_executable(gc-app-options-test tests/app_options_test.c src/app_options.c)
    target_link_libraries(gc-app-options-test PRIVATE gamecube_core)
    add_test(NAME gc-app-options COMMAND gc-app-options-test)
    if(GC_BUILD_APP)
        add_test(NAME gc-cli-help COMMAND gamecube-menu --help)
        add_test(NAME gc-cli-inspection-flags COMMAND gamecube-menu --delaystart --step --help)
        add_test(NAME gc-cli-card-override COMMAND gamecube-menu --noinsert ab --help)
        add_test(NAME gc-cli-invalid-card-override COMMAND gamecube-menu --noinsert c)
        add_test(NAME gc-cli-invalid-frames COMMAND gamecube-menu --frames 0)
        add_test(NAME gc-cli-invalid-delay COMMAND gamecube-menu --delaystart -1)
        add_test(NAME gc-cli-invalid-region COMMAND gamecube-menu --region INVALID)
        set_tests_properties(gc-cli-help gc-cli-inspection-flags
            gc-cli-card-override gc-cli-invalid-card-override gc-cli-invalid-frames
            gc-cli-invalid-delay gc-cli-invalid-region PROPERTIES
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
        set_tests_properties(gc-cli-invalid-card-override gc-cli-invalid-frames
            gc-cli-invalid-delay gc-cli-invalid-region PROPERTIES WILL_FAIL TRUE)
    endif()
endif()

find_program(GC_CLANG_FORMAT NAMES clang-format HINTS
    /Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin)
if(GC_CLANG_FORMAT)
    file(GLOB_RECURSE GC_FORMAT_SOURCES CONFIGURE_DEPENDS
        include/*.h src/*.c src/*.h tools/*.c tests/*.c)
    add_custom_target(gc-check-format COMMAND "${GC_CLANG_FORMAT}"
        --dry-run --Werror ${GC_FORMAT_SOURCES} VERBATIM)
endif()
