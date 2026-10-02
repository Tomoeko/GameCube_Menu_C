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

add_library(gamecube_core STATIC
    src/app/error_control.c
    src/app/frame_control.c
    src/app/input_control.c
    src/audio/audio.c
    src/audio/audio_dsp.c
    src/audio/audio_envelope.c
    src/audio/audio_math.c
    src/audio/audio_resources.c
    src/audio/audio_sequence.c
    src/audio/navigation_sound.c
    src/cards/card_art.c
    src/cards/card_cells.c
    src/cards/card_checksum.c
    src/cards/card_image.c
    src/cards/card_lighting.c
    src/cards/card_operation.c
    src/cards/card_popup.c
    src/cards/card_usage.c
    src/disc/disc.c
    src/disc/disc_control.c
    src/disc/launch_control.c
    src/menu/edit_geometry.c
    src/menu/face_geometry.c
    src/menu/help.c
    src/menu/menu.c
    src/menu/menu_animation.c
    src/menu/menu_cards.c
    src/menu/menu_clock.c
    src/menu/page_transition.c
    src/menu/value_morph.c
    src/resources/font.c
    src/resources/ipl.c
    src/resources/ipl_animation.c
    src/resources/ipl_material.c
    src/resources/ipl_model.c
    src/resources/ipl_texture.c
    src/resources/layout.c
    src/resources/text.c
    src/resources/texture_collection.c
    src/startup/boot_control.c
    src/startup/startup.c
    src/startup/startup_motion.c
    src/startup/startup_resources.c
    src/state/config.c
    src/state/services.c
    src/state/state.c
    src/support/option_values.c)
target_include_directories(gamecube_core PUBLIC include PRIVATE src)
target_link_libraries(gamecube_core PUBLIC console_common)
gc_warnings(gamecube_core)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    # Ordinary recovered audio operations round separately; explicit fmaf
    # preserves the firmware's single-rounding multiply-add instructions.
    set_source_files_properties(
        src/audio/audio_dsp.c
        src/audio/audio_envelope.c
        src/audio/audio_math.c
        src/audio/audio_resources.c
        src/audio/audio_sequence.c
        PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()
if(APPLE)
    target_sources(gamecube_core PRIVATE src/platform/apple/audio_apple.c)
    target_link_libraries(gamecube_core PUBLIC "-framework AudioToolbox")
endif()

add_library(gamecube_scene STATIC
    src/app/frame_history.c
    src/cards/card_runtime.c
    src/render/render.c
    src/render/render_layout.c
    src/render/render_material.c
    src/render/render_mesh.c
    src/render/render_resources.c
    src/render/render_state.c)
target_include_directories(gamecube_scene PUBLIC include PRIVATE src)
target_link_libraries(gamecube_scene PUBLIC gamecube_core)
gc_warnings(gamecube_scene)

# The offline backend is shared by capture tools and headless scene tests.
add_library(gamecube_software STATIC
    src/render/software/software.c
    src/render/software/software_material.c)
target_include_directories(gamecube_software PRIVATE src)
target_link_libraries(gamecube_software PUBLIC console_common)
gc_warnings(gamecube_software)

add_library(gamecube_tool_io STATIC tools/support/output.c)
target_include_directories(gamecube_tool_io PUBLIC tools)
gc_warnings(gamecube_tool_io)

add_executable(gc-ipl-tool tools/ipl/main.c)
target_link_libraries(gc-ipl-tool PRIVATE gamecube_core gamecube_tool_io)
gc_warnings(gc-ipl-tool)

add_executable(gc-audio-tool tools/audio/main.c)
target_link_libraries(gc-audio-tool PRIVATE gamecube_core gamecube_tool_io)
target_include_directories(gc-audio-tool PRIVATE src)
gc_warnings(gc-audio-tool)

add_executable(gamecube-render tools/render/main.c)
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
        add_executable(gamecube-menu MACOSX_BUNDLE src/app/main.c src/app/app_options.c)
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
        add_executable(gamecube-menu src/app/main.c src/app/app_options.c)
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

    gc_test_executable(gc-tool-output-test tests/tools/tool_output_test.c)
    target_link_libraries(gc-tool-output-test PRIVATE gamecube_tool_io)
    add_test(NAME gc-tool-output COMMAND gc-tool-output-test)
    gc_test_directory(gc-tool-output tool-output)
    if(APPLE)
        gc_test_executable(gc-audio_device-test tests/audio/audio_device_test.c)
        target_include_directories(gc-audio_device-test PRIVATE include)
        add_test(NAME gc-audio-device COMMAND gc-audio_device-test)
    endif()
    foreach(source
        app/error_control app/frame_control app/input_control
        audio/audio audio/audio_control audio/audio_cycle audio/audio_effect
        audio/audio_output audio/audio_release audio/audio_resource audio/audio_track
        audio/audio_voice
        audio/navigation_sound
        cards/card_art cards/card_cells cards/card_image cards/card_lighting
        cards/card_operation cards/card_popup cards/card_usage
        disc/disc disc/disc_control disc/launch_control
        menu/edit_geometry menu/face_geometry menu/help menu/menu menu/menu_animation
        menu/page_transition menu/value_morph
        resources/font resources/ipl resources/ipl_animation resources/ipl_material
        resources/ipl_model resources/layout resources/text resources/texture_collection
        startup/boot_control startup/startup
        state/config state/services state/state)
        get_filename_component(name "${source}" NAME)
        gc_test_executable(gc-${name}-test tests/${source}_test.c)
        target_link_libraries(gc-${name}-test PRIVATE gamecube_core)
        add_test(NAME gc-${name} COMMAND gc-${name}-test)
    endforeach()
    foreach(region USA EUR JAP)
        set(GC_PRIVATE_IPL "${PROJECT_SOURCE_DIR}/Files/GameCube_BIOS/${region}/IPL.bin")
        if(EXISTS "${GC_PRIVATE_IPL}")
            foreach(module audio audio_resource text font startup boot_control layout
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
    gc_test_executable(gc-software-test tests/render/software_test.c)
    target_link_libraries(gc-software-test PRIVATE gamecube_core gamecube_software)
    add_test(NAME gc-software COMMAND gc-software-test
        "${CMAKE_BINARY_DIR}/software-test.ppm")
    gc_test_executable(gc-render_material-test tests/render/render_material_test.c)
    target_link_libraries(gc-render_material-test PRIVATE
        gamecube_scene gamecube_software)
    add_test(NAME gc-render-material COMMAND gc-render_material-test
        "${CMAKE_BINARY_DIR}/render-material-test.ppm")
    gc_test_executable(gc-scene-test tests/render/scene_test.c)
    target_link_libraries(gc-scene-test PRIVATE gamecube_scene gamecube_software)
    add_test(NAME gc-scene COMMAND gc-scene-test)
    gc_test_executable(gc-card-cells-scene-test tests/cards/card_cells_scene_test.c)
    target_link_libraries(gc-card-cells-scene-test PRIVATE
        gamecube_scene gamecube_software)
    add_test(NAME gc-card-cells-scene COMMAND gc-card-cells-scene-test)
    gc_test_executable(gc-card-runtime-test tests/cards/card_runtime_test.c)
    target_link_libraries(gc-card-runtime-test PRIVATE gamecube_scene gamecube_software)
    add_test(NAME gc-card-runtime COMMAND gc-card-runtime-test)
    gc_test_directory(gc-card-runtime card-runtime/base)
    gc_test_executable(gc-frame-history-test tests/app/frame_history_test.c)
    target_link_libraries(gc-frame-history-test PRIVATE gamecube_scene)
    add_test(NAME gc-frame-history COMMAND gc-frame-history-test)
    gc_test_executable(gc-app-tick-test
        tests/app/app_tick_test.c src/app/app_options.c)
    target_link_libraries(gc-app-tick-test PRIVATE gamecube_scene gamecube_software)
    gc_test_executable(gc-app-control-test
        tests/app/app_control_test.c src/app/app_options.c)
    target_link_libraries(gc-app-control-test PRIVATE gamecube_scene gamecube_software)
    add_test(NAME gc-app-window-controls COMMAND gc-app-control-test)
    gc_test_executable(gc-app-options-test
        tests/app/app_options_test.c src/app/app_options.c)
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
        include/*.h src/*.c src/*.h tools/*.c tools/*.h tests/*.c)
    add_custom_target(gc-check-format COMMAND "${GC_CLANG_FORMAT}"
        --dry-run --Werror ${GC_FORMAT_SOURCES} VERBATIM)
endif()
