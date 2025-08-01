set(KTX_FEATURE_STATIC_LIBRARY ON)
set(KTX_FEATURE_TOOLS OFF)
set(KTX_FEATURE_TESTS OFF)
set(KTX_FEATURE_VK_UPLOAD ON)
set(KTX_FEATURE_GL_UPLOAD OFF)
set(KTX_FEATURE_LOADTEST_APPS OFF)

set(FASTGLTF_COMPILE_AS_CPP20 ON)

FetchContent_MakeAvailable(simdjson spdlog glm fastgltf imgui stb implot uni-algo entt)

if (WIVRN_USE_LIBKTX)
    FetchContent_MakeAvailable(libktx)
endif()

if (NOT BOOST_FOUND)
    FetchContent_MakeAvailable(boost)
endif()

include(FreetypeHarfbuzz)
include(CompileGLSL)
include(I18n)

add_library(stb INTERFACE)
target_include_directories(stb INTERFACE ${stb_SOURCE_DIR})

if(ANDROID)
    add_library(wivrn MODULE)

    find_library(ANDROID_LIBRARY NAMES android REQUIRED)
    find_library(MEDIA_LIBRARY NAMES mediandk REQUIRED)
    find_library(AAUDIO_LIBRARY NAMES aaudio REQUIRED)
    target_link_libraries(wivrn ${ANDROID_LIBRARY} ${MEDIA_LIBRARY} ${AAUDIO_LIBRARY})

    # The OpenXR loader must be dynamically loaded or the Oculus store will refuse it with "Oculus SDK not found or older than 1.0"
    set(DYNAMIC_LOADER ON)
    FetchContent_MakeAvailable(openxr_loader)
    target_link_libraries(wivrn openxr_loader)


    target_link_libraries(wivrn native_app_glue)
    target_compile_definitions(wivrn PRIVATE -DXR_USE_PLATFORM_ANDROID -DVK_USE_PLATFORM_ANDROID_KHR)
    target_link_options(wivrn PRIVATE -Wl,--undefined=ANativeActivity_onCreate)

    file(GLOB PLATFORM_SOURCE CONFIGURE_DEPENDS
        "android/*.cpp"
        "audio/android/*.cpp"
        "decoder/android/*.cpp"
        )
    target_sources(wivrn PRIVATE ${PLATFORM_SOURCE})
else()
    add_executable(wivrn)

    if (OpenXR_FOUND)
        target_link_libraries(wivrn OpenXR::openxr_loader OpenXR::headers)
    else()
        FetchContent_MakeAvailable(openxr_loader)
        target_link_libraries(wivrn openxr_loader)
    endif()

    target_link_libraries(wivrn PkgConfig::LIBAV Fontconfig::Fontconfig)


    file(GLOB PLATFORM_SOURCE CONFIGURE_DEPENDS
        "decoder/ffmpeg/*.cpp")
    target_sources(wivrn PRIVATE ${PLATFORM_SOURCE})

    file(CREATE_LINK ${CMAKE_CURRENT_BINARY_DIR}/share ${CMAKE_BINARY_DIR}/share SYMBOLIC)
endif()



##############################################################################
# Main sources
#
target_compile_definitions(wivrn PRIVATE "XR_USE_TIMESPEC=1")

set_target_properties(wivrn PROPERTIES CXX_VISIBILITY_PRESET hidden)

file(GLOB LOCAL_SOURCE CONFIGURE_DEPENDS
    "*.cpp"
    "decoder/*.cpp"
    "scenes/*.cpp"
    "utils/*.cpp"
    "vk/*.cpp"
    "xr/*.cpp"
    "render/*.cpp"
)

file(GLOB_RECURSE VULKAN_SHADERS CONFIGURE_DEPENDS "*.glsl")
target_sources(wivrn PRIVATE ${LOCAL_SOURCE} ${VULKAN_SHADERS})
wivrn_compile_glsl(wivrn ${VULKAN_SHADERS})

target_link_libraries(wivrn Vulkan::Vulkan spdlog::spdlog glm::glm fastgltf FreetypeHarfbuzz stb Boost::url Boost::locale)

if (WIVRN_USE_LIBKTX)
    target_link_libraries(wivrn ktx_read)
endif()

target_compile_definitions(wivrn PRIVATE -DXR_USE_GRAPHICS_API_VULKAN)
target_compile_definitions(wivrn PRIVATE -DVMA_STATS_STRING_ENABLED=0)
target_compile_definitions(wivrn PRIVATE -DGLM_ENABLE_EXPERIMENTAL)

target_include_directories(wivrn PRIVATE .)

if(NOT ANDROID)
    install(TARGETS wivrn)
endif()

target_link_libraries(wivrn simdjson uni-algo wivrn-common wivrn-external)



##############################################################################
# Dear ImGui
#
add_library(wivrn-imgui STATIC)
target_sources(wivrn-imgui PRIVATE
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/misc/freetype/imgui_freetype.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
)

target_include_directories(wivrn-imgui PUBLIC ${imgui_SOURCE_DIR})
target_include_directories(wivrn-imgui PUBLIC ${implot_SOURCE_DIR})
# target_compile_definitions(wivrn-imgui PUBLIC "IMGUI_USER_CONFIG=\"render/imgui_config.h\"")
target_compile_definitions(wivrn-imgui PUBLIC IMGUI_USE_WCHAR32)
target_compile_definitions(wivrn-imgui PUBLIC IMGUI_ENABLE_FREETYPE)
# target_compile_definitions(wivrn-imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS) # incompatible with implot
# target_compile_definitions(wivrn-imgui PUBLIC IMGUI_ENABLE_FREETYPE_LUNASVG)
target_compile_features(wivrn-imgui PRIVATE cxx_std_23)
target_link_libraries(wivrn-imgui FreetypeHarfbuzz)

target_link_libraries(wivrn wivrn-imgui)


##############################################################################
# EnTT
#
target_link_libraries(wivrn EnTT::EnTT)

##############################################################################
# Vulkan Validation Layer
#
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/vk_layer_settings.txt
"khronos_validation.debug_action = VK_DBG_LAYER_ACTION_LOG_MSG,VK_DBG_LAYER_ACTION_BREAK
khronos_validation.report_flags = error,warn
khronos_validation.log_filename = stdout
khronos_validation.disables =
khronos_validation.enables = VK_VALIDATION_FEATURE_ENABLE_THREAD_SAFETY_EXT
")
endif()

if(USE_ANDROID_VALIDATION_LAYER)
    set(VVL_VERSION "1.3.231.1")

    if(NOT EXISTS ${CMAKE_BINARY_DIR}/Vulkan-ValidationLayers-${VVL_VERSION}.zip)
        if (EXISTS ${CMAKE_SOURCE_DIR}/Vulkan-ValidationLayers-${VVL_VERSION}.zip)
            file(CREATE_LINK ${CMAKE_SOURCE_DIR}/Vulkan-ValidationLayers-${VVL_VERSION}.zip ${CMAKE_BINARY_DIR}/Vulkan-ValidationLayers-${VVL_VERSION}.zip SYMBOLIC)
        else()
            file(DOWNLOAD https://github.com/KhronosGroup/Vulkan-ValidationLayers/releases/download/sdk-${VVL_VERSION}/android-binaries-${VVL_VERSION}.zip ${CMAKE_BINARY_DIR}/Vulkan-ValidationLayers-${VVL_VERSION}.zip
                EXPECTED_HASH SHA256=eec3a54222d6d33d4f787a13a45c60ac5b9e8d7bf795fc41e4242354a02e34ba)
        endif()
    endif()

    set(VVL_ZIPFILE ${CMAKE_CURRENT_SOURCE_DIR}/Vulkan-ValidationLayers-${VVL_VERSION}.zip)

    file(ARCHIVE_EXTRACT INPUT ${CMAKE_BINARY_DIR}/Vulkan-ValidationLayers-${VVL_VERSION}.zip DESTINATION ${CMAKE_BINARY_DIR}/Vulkan-ValidationLayer)

    add_library(vvl1 SHARED IMPORTED)
    set_property(TARGET vvl1 PROPERTY IMPORTED_LOCATION ${CMAKE_BINARY_DIR}/Vulkan-ValidationLayer/${CMAKE_ANDROID_ARCH_ABI}/libVkLayer_khronos_validation.so)
    add_library(vvl2 SHARED IMPORTED)
    set_property(TARGET vvl2 PROPERTY IMPORTED_LOCATION ${CMAKE_BINARY_DIR}/Vulkan-ValidationLayer/${CMAKE_ANDROID_ARCH_ABI}/libc++_shared.so)

    target_link_libraries(wivrn vvl1 vvl2) # Hack to get gradle to package the validation layer in the APK
endif()


##############################################################################
# Assets
#
if(ANDROID)
    set(ASSETS_DIR ${CMAKE_ANDROID_ASSETS_DIRECTORIES})
else()
    set(ASSETS_DIR ${CMAKE_CURRENT_BINARY_DIR}/share/wivrn/assets)
    file(MAKE_DIRECTORY ${ASSETS_DIR})
    install(DIRECTORY "${ASSETS_DIR}" DESTINATION "${CMAKE_INSTALL_DATADIR}/wivrn")
endif()


##############################################################################
# Internationalization
#
create_mo_files(wivrn wivrn "${CMAKE_CURRENT_SOURCE_DIR}/../locale")
create_glyphset(wivrn wivrn "${CMAKE_CURRENT_SOURCE_DIR}/../locale")


##############################################################################
# License files
#
set(LICENSES_DIR "${ASSETS_DIR}/licenses")

file(MAKE_DIRECTORY "${LICENSES_DIR}")
file(COPY_FILE "${CMAKE_SOURCE_DIR}/COPYING" "${LICENSES_DIR}/WiVRn")
file(COPY_FILE "${simdjson_SOURCE_DIR}/LICENSE" "${LICENSES_DIR}/simdjson")
file(COPY_FILE "${CMAKE_SOURCE_DIR}/LICENSE-OFL-1.1" "${LICENSES_DIR}/FontAwesome")
if (OPENXR_SOURCE_DIR)
    file(COPY_FILE "${OPENXR_SOURCE_DIR}/LICENSE" "${LICENSES_DIR}/openxr-loader")
endif()


##############################################################################
# Generate assets from SVGs
#
file(REAL_PATH ${WIVRN_CSS} WIVRN_CSS_ABS BASE_DIRECTORY "${CMAKE_SOURCE_DIR}")
set(RSVG_OPTS "--stylesheet=${WIVRN_CSS_ABS}")

set(IMG_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../images)
set(AST_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../assets)

add_custom_command(OUTPUT ${ASSETS_DIR}/wivrn.png
    COMMAND ${RSVG_CONVERT} ${IMG_SRC_DIR}/wivrn.svg -w 540 -h 540 -o ${ASSETS_DIR}/wivrn.png ${RSVG_OPTS}
    DEPENDS ${IMG_SRC_DIR}/wivrn.svg ${WIVRN_CSS_ABS})

add_custom_command(OUTPUT ${ASSETS_DIR}/wivrn-pride.png
    COMMAND ${RSVG_CONVERT} ${IMG_SRC_DIR}/wivrn-pride.svg -w 540 -h 540 -o ${ASSETS_DIR}/wivrn-pride.png ${RSVG_OPTS}
    DEPENDS ${IMG_SRC_DIR}/wivrn-pride.svg ${WIVRN_CSS_ABS})

add_custom_target(assets ALL DEPENDS
    ${ASSETS_DIR}/wivrn.png
    ${ASSETS_DIR}/wivrn-pride.png
)

add_dependencies(wivrn assets)

if(ANDROID)
    find_program(TOKTX toktx REQUIRED)
    add_custom_command(OUTPUT ${ASSETS_DIR}/wivrn.ktx2
        COMMAND ${TOKTX} --encode astc --astc_blk_d 4x4 --astc_quality 100 --genmipmap ${ASSETS_DIR}/wivrn.ktx2 ${ASSETS_DIR}/wivrn.png
        DEPENDS ${ASSETS_DIR}/wivrn.png)
    add_custom_command(OUTPUT ${ASSETS_DIR}/wivrn-pride.ktx2
        COMMAND ${TOKTX} --encode astc --astc_blk_d 4x4 --astc_quality 100 --genmipmap ${ASSETS_DIR}/wivrn-pride.ktx2 ${ASSETS_DIR}/wivrn-pride.png
        DEPENDS ${ASSETS_DIR}/wivrn-pride.png)

    add_custom_command(OUTPUT ${ASSETS_DIR}/WoodFloor051_1K-JPG_Color.ktx2
        COMMAND ${TOKTX} --encode astc --astc_blk_d 12x12 --astc_quality 100 --genmipmap ${ASSETS_DIR}/WoodFloor051_1K-JPG_Color.ktx2 ${AST_SRC_DIR}/WoodFloor051_1K-JPG_Color.jpg
        DEPENDS ${AST_SRC_DIR}/WoodFloor051_1K-JPG_Color.jpg)

    add_custom_command(OUTPUT ${ASSETS_DIR}/vr_splash.png
        COMMAND ${CMAKE_COMMAND} -E copy ${ASSETS_DIR}/wivrn.png ${ASSETS_DIR}/vr_splash.png
        DEPENDS ${ASSETS_DIR}/wivrn.png)

    add_custom_target(assets-android ALL DEPENDS
        ${ASSETS_DIR}/wivrn.ktx2
        ${ASSETS_DIR}/wivrn-pride.ktx2
        ${ASSETS_DIR}/WoodFloor051_1K-JPG_Color.ktx2
        ${ASSETS_DIR}/vr_splash.png
    )
    add_dependencies(assets-android assets)
    add_dependencies(wivrn assets-android)
endif()



##############################################################################
# Copy the assets to be able to find them when started from the build
# directory
if(NOT ANDROID)
    file(GLOB_RECURSE ASSETS CONFIGURE_DEPENDS LIST_DIRECTORIES FALSE RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/../assets" "${CMAKE_CURRENT_SOURCE_DIR}/../assets/*" )

    set(COPIED_ASSETS)

    foreach(ASSET IN LISTS ASSETS)
        add_custom_command(OUTPUT ${ASSETS_DIR}/${ASSET}
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/../assets/${ASSET} ${ASSETS_DIR}/${ASSET}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../assets/${ASSET})

        set(COPIED_ASSETS ${COPIED_ASSETS} ${ASSETS_DIR}/${ASSET})
    endforeach()
    add_custom_target(assets-builddir ALL DEPENDS ${COPIED_ASSETS})
    add_dependencies(wivrn assets-builddir)

    set_target_properties(wivrn PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    install(DIRECTORY "${ASSETS_DIR}" DESTINATION ${CMAKE_INSTALL_DATADIR}/wivrn)
endif()
