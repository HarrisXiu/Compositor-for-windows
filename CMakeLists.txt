cmake_minimum_required(VERSION 3.24)
project(CompositorWindows VERSION 0.4.0 LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
if(MSVC)
    # Avoid MSVC incremental-link state in distributable Release binaries.
    add_link_options("$<$<CONFIG:Release>:/INCREMENTAL:NO>")
endif()
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Concurrent Svg Network)
include(CTest)
set(COMPOSITOR_LIBRAW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.cache/LibRaw/LibRaw-0.22.2" CACHE PATH "LibRaw Windows SDK")
if(NOT EXISTS "${COMPOSITOR_LIBRAW_ROOT}/lib/libraw.lib")
    message(FATAL_ERROR "LibRaw 0.22.2 SDK required. Run windows/build.ps1 to fetch the pinned dependency.")
endif()
add_library(LibRaw SHARED IMPORTED)
set_target_properties(LibRaw PROPERTIES
    IMPORTED_IMPLIB "${COMPOSITOR_LIBRAW_ROOT}/lib/libraw.lib"
    IMPORTED_LOCATION "${COMPOSITOR_LIBRAW_ROOT}/bin/libraw.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${COMPOSITOR_LIBRAW_ROOT}")

set(COMPOSITOR_ONNX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.cache/microsoft.ml.onnxruntime.directml.1.24.4" CACHE PATH "ONNX Runtime DirectML SDK")
set(COMPOSITOR_DML_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.cache/microsoft.ai.directml.1.15.4" CACHE PATH "Microsoft DirectML SDK")
set(COMPOSITOR_ONNX_NATIVE "${COMPOSITOR_ONNX_ROOT}/runtimes/win-x64/native")
if(NOT EXISTS "${COMPOSITOR_ONNX_NATIVE}/onnxruntime.lib" OR NOT EXISTS "${COMPOSITOR_DML_ROOT}/bin/x64-win/DirectML.dll")
    message(FATAL_ERROR "ONNX Runtime DirectML 1.24.4 and DirectML 1.15.4 SDKs required. Run windows/fetch-onnx-runtime.ps1.")
endif()
add_library(OnnxRuntime SHARED IMPORTED)
set_target_properties(OnnxRuntime PROPERTIES
    IMPORTED_IMPLIB "${COMPOSITOR_ONNX_NATIVE}/onnxruntime.lib"
    IMPORTED_LOCATION "${COMPOSITOR_ONNX_NATIVE}/onnxruntime.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${COMPOSITOR_ONNX_ROOT}/build/native/include;${COMPOSITOR_DML_ROOT}/include")
add_library(DirectML SHARED IMPORTED)
set_target_properties(DirectML PROPERTIES
    IMPORTED_IMPLIB "${COMPOSITOR_DML_ROOT}/bin/x64-win/DirectML.lib"
    IMPORTED_LOCATION "${COMPOSITOR_DML_ROOT}/bin/x64-win/DirectML.dll")

set(PIXELS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Compositor/Rendering")
add_library(compositor_pixels STATIC
    ${PIXELS_DIR}/AdjustPixels.c ${PIXELS_DIR}/BrushPixels.c
    ${PIXELS_DIR}/ContentFill.c ${PIXELS_DIR}/HealPixels.c
    ${PIXELS_DIR}/LensPixels.c ${PIXELS_DIR}/LevelsPixels.c
    ${PIXELS_DIR}/NoisePixels.c ${PIXELS_DIR}/WandPixels.c)
set_target_properties(compositor_pixels PROPERTIES AUTOMOC OFF)
target_include_directories(compositor_pixels PUBLIC ${PIXELS_DIR})
if(MSVC)
    target_compile_definitions(compositor_pixels PRIVATE _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
    target_compile_options(compositor_pixels PRIVATE /utf-8)
endif()

add_library(compositor_core STATIC
    windows/src/selection_operations.cpp windows/src/layer_operations.cpp windows/src/image_operations.cpp
    windows/src/document.cpp windows/src/render.cpp windows/src/blend.cpp windows/src/filters.cpp windows/src/camera_raw.cpp windows/src/effects.cpp windows/src/editable_layers.cpp windows/src/photoshop.cpp windows/src/photoshop_editable.cpp windows/src/raw_import.cpp windows/src/dither.cpp windows/src/dither_pixels.cpp windows/src/warp_brush.cpp windows/src/demo.cpp)
target_include_directories(compositor_core PUBLIC windows/src)
target_link_libraries(compositor_core PUBLIC Qt6::Core Qt6::Gui Qt6::Svg Qt6::Concurrent compositor_pixels LibRaw)

add_library(compositor_ai STATIC windows/src/ai_session.cpp windows/src/ai_preprocess.cpp windows/src/ai_sam.cpp
    windows/src/ai_models.cpp windows/src/ai_download.cpp)
qt_add_resources(compositor_ai ai_licenses PREFIX "/ai-licenses" BASE "windows/licenses"
    FILES windows/licenses/BiRefNet-MIT.txt windows/licenses/Apache-2.0.txt windows/licenses/Microsoft-MIT.txt)
target_include_directories(compositor_ai PUBLIC windows/src)
target_link_libraries(compositor_ai PUBLIC compositor_core OnnxRuntime DirectML Qt6::Concurrent Qt6::Network dxgi d3d12)
target_compile_definitions(compositor_ai PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

add_library(compositor_ui STATIC
    windows/src/shortcuts.cpp windows/src/shortcuts.h windows/src/canvas_layout.cpp windows/src/canvas_interaction.cpp windows/src/editor_view.cpp windows/src/tool_options_extra.cpp
    windows/src/editor.cpp windows/src/editor.h windows/src/editor_page.cpp
    windows/src/editor_menus.cpp windows/src/ai_models_dialog.cpp windows/src/tool_options.cpp windows/src/layer_panel.cpp
    windows/src/editor_layers.cpp windows/src/layer_transfer.cpp windows/src/image_dialogs.cpp windows/src/crop_tool.cpp
    windows/src/selection_dialogs.cpp windows/src/document_dialogs.cpp windows/src/filter_dialogs.cpp windows/src/effect_dialogs.cpp
    windows/src/editable_dialogs.cpp windows/src/canvas_tools.cpp windows/src/canvas_paint.cpp
    windows/src/canvas.cpp windows/src/canvas.h windows/src/curve_editor.cpp windows/src/raw_dialog.cpp windows/src/image_scope.cpp windows/src/language.cpp windows/src/language.h)
qt_add_resources(compositor_ui translations PREFIX "/translations" BASE "windows/translations"
    FILES windows/translations/zh_CN.json windows/translations/ja_JP.json)
target_link_libraries(compositor_ui PUBLIC compositor_core compositor_ai Qt6::Widgets Qt6::Concurrent)
qt_add_executable(Compositor WIN32 windows/src/main.cpp)
target_link_libraries(Compositor PRIVATE compositor_ui)
qt_add_executable(compositor_ai_probe windows/src/ai_probe.cpp)
target_link_libraries(compositor_ai_probe PRIVATE compositor_ai)
if(MSVC)
    target_compile_options(compositor_ai_probe PRIVATE /W4 /utf-8 /permissive- /EHsc)
endif()
if(MSVC)
    foreach(target compositor_core compositor_ai compositor_ui Compositor)
        target_compile_options(${target} PRIVATE /W4 /utf-8 /permissive- /EHsc)
    endforeach()
endif()
install(TARGETS Compositor RUNTIME DESTINATION .)
add_custom_command(TARGET Compositor POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:LibRaw>" "$<TARGET_FILE_DIR:Compositor>")

if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(compositor_ai_tests windows/tests/ai_tests.cpp)
    target_link_libraries(compositor_ai_tests PRIVATE compositor_ai Qt6::Test Qt6::Widgets)
    if(MSVC)
        target_compile_options(compositor_ai_tests PRIVATE /utf-8)
    endif()
    qt_add_executable(compositor_model_tests windows/tests/model_tests.cpp)
    target_link_libraries(compositor_model_tests PRIVATE compositor_ui Qt6::Test)
    if(MSVC)
        target_compile_options(compositor_model_tests PRIVATE /utf-8)
    endif()
    add_test(NAME compositor_models COMMAND compositor_model_tests)
    set_tests_properties(compositor_models PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME compositor_ai COMMAND compositor_ai_tests)
    set_tests_properties(compositor_ai PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(compositor_tests windows/tests/core_tests.cpp)
    target_link_libraries(compositor_tests PRIVATE compositor_core Qt6::Test)
    if(MSVC)
        target_compile_options(compositor_tests PRIVATE /utf-8)
    endif()
    add_test(NAME compositor_core COMMAND compositor_tests)
    qt_add_executable(compositor_ui_tests windows/tests/ui_tests.cpp)
    target_link_libraries(compositor_ui_tests PRIVATE compositor_ui Qt6::Test)
    if(MSVC)
        target_compile_options(compositor_ui_tests PRIVATE /utf-8)
    endif()
    add_test(NAME compositor_ui COMMAND compositor_ui_tests)
    set_tests_properties(compositor_ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(compositor_photoshop_tests windows/tests/photoshop_tests.cpp)
    target_link_libraries(compositor_photoshop_tests PRIVATE compositor_core Qt6::Test)
    if(MSVC)
        target_compile_options(compositor_photoshop_tests PRIVATE /utf-8)
    endif()
    add_test(NAME compositor_photoshop COMMAND compositor_photoshop_tests)
    set_tests_properties(compositor_photoshop PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(compositor_raw_tests windows/tests/raw_tests.cpp)
    target_link_libraries(compositor_raw_tests PRIVATE compositor_core Qt6::Test)
    if(MSVC)
        target_compile_options(compositor_raw_tests PRIVATE /utf-8)
    endif()
    add_test(NAME compositor_raw COMMAND compositor_raw_tests)
    qt_add_executable(compositor_dither_tests windows/tests/dither_tests.cpp)
    target_link_libraries(compositor_dither_tests PRIVATE compositor_core Qt6::Test)
    if(MSVC)
        target_compile_options(compositor_dither_tests PRIVATE /utf-8)
    endif()
    add_test(NAME compositor_dither COMMAND compositor_dither_tests)
    set_tests_properties(compositor_dither PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(compositor_layer_tests windows/tests/layer_tests.cpp)
    target_link_libraries(compositor_layer_tests PRIVATE compositor_ui Qt6::Test)
    if(MSVC)
        target_compile_options(compositor_layer_tests PRIVATE /utf-8)
    endif()
    add_test(NAME compositor_layers COMMAND compositor_layer_tests)
    set_tests_properties(compositor_layers PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(compositor_canvas_tests windows/tests/canvas_tests.cpp)
    target_link_libraries(compositor_canvas_tests PRIVATE compositor_ui Qt6::Test)
    if(MSVC)
        target_compile_options(compositor_canvas_tests PRIVATE /utf-8)
    endif()
    add_test(NAME compositor_canvas COMMAND compositor_canvas_tests)
    set_tests_properties(compositor_canvas PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(WIN32)
        foreach(target compositor_model_tests compositor_tests compositor_ai_tests compositor_ui_tests compositor_photoshop_tests compositor_raw_tests compositor_dither_tests compositor_layer_tests compositor_canvas_tests)
            foreach(library Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent Qt6::Svg Qt6::Network Qt6::Test LibRaw)
                add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${library}>" "$<TARGET_FILE_DIR:${target}>")
            endforeach()
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/platforms"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt6::QOffscreenIntegrationPlugin>" "$<TARGET_FILE_DIR:${target}>/platforms")
        endforeach()
    endif()
endif()
set(COMPOSITOR_AI_RUNTIME_TARGETS Compositor compositor_ai_probe)
if(BUILD_TESTING)
    list(APPEND COMPOSITOR_AI_RUNTIME_TARGETS compositor_ai_tests compositor_model_tests compositor_ui_tests compositor_layer_tests compositor_canvas_tests)
endif()
foreach(target ${COMPOSITOR_AI_RUNTIME_TARGETS})
    foreach(library OnnxRuntime DirectML Qt6::Core Qt6::Gui Qt6::Concurrent Qt6::Svg Qt6::Network LibRaw)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${library}>" "$<TARGET_FILE_DIR:${target}>")
    endforeach()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${COMPOSITOR_ONNX_NATIVE}/onnxruntime_providers_shared.dll" "$<TARGET_FILE_DIR:${target}>")
endforeach()
