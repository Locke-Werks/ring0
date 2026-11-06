cmake_minimum_required(VERSION 3.28)
project(ring0 VERSION 0.2.4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Quick ShaderTools Concurrent GuiPrivate)
qt_standard_project_setup(REQUIRES 6.8)

# core: Win32/NT only, no Qt. Kept as a static lib so it can be tested headless.
file(GLOB RING0_CORE_SOURCES CONFIGURE_DEPENDS src/core/*.cpp)
add_library(ring0_core STATIC ${RING0_CORE_SOURCES})
target_include_directories(ring0_core PUBLIC src)
target_compile_definitions(ring0_core PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(ring0_core PUBLIC
    ntdll advapi32 iphlpapi pdh psapi wintrust crypt32 dbghelp shell32 ole32 uuid ws2_32 dxgi tdh)

enable_testing()
add_executable(etw_parse_test tests/etw_parse_test.cpp)
target_include_directories(etw_parse_test PRIVATE src)
add_test(NAME etw_parse COMMAND etw_parse_test)

file(GLOB RING0_APP_SOURCES CONFIGURE_DEPENDS src/app/*.cpp src/gpu/*.cpp)
# Listing the manifest as a source makes Qt skip generating its own.
qt_add_executable(ring0 WIN32 ${RING0_APP_SOURCES} res/ring0.rc res/ring0.manifest)
# qmltyperegistrations includes headers by bare name, so the header dirs go on the path.
target_include_directories(ring0 PRIVATE src src/app src/gpu)
target_link_libraries(ring0 PRIVATE ring0_core Qt6::Core Qt6::Gui Qt6::Quick Qt6::Concurrent Qt6::GuiPrivate dwmapi)

file(GLOB RING0_QML RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS
    src/qml/*.qml src/qml/views/*.qml)
set_source_files_properties(src/qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
qt_add_qml_module(ring0
    URI Ring0
    VERSION 1.0
    QML_FILES ${RING0_QML})

file(GLOB RING0_SHADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS
    src/gpu/shaders/*.vert src/gpu/shaders/*.frag)
if(RING0_SHADERS)
    qt_add_shaders(ring0 "ring0_shaders"
        BATCHABLE
        PREFIX "/shaders"
        BASE src/gpu/shaders
        FILES ${RING0_SHADERS})
endif()

# Warnings-as-errors covers our sources only. Qt's generated qmlcache and moc
# output trips C4702 inside Qt headers, which is not ours to fix.
if(MSVC)
    target_compile_options(ring0_core PRIVATE /W4 /WX /permissive- /utf-8 /Zc:__cplusplus)
    target_compile_options(ring0 PRIVATE /permissive- /utf-8 /Zc:__cplusplus)
    set_source_files_properties(${RING0_APP_SOURCES} PROPERTIES COMPILE_OPTIONS "/W4;/WX")
endif()


# PawnIO modules (LGPL-2.1, signed by the PawnIO project) are loaded from a
# pawnio folder beside the exe to read CPU temperature.
add_custom_command(TARGET ring0 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/assets/pawnio $<TARGET_FILE_DIR:ring0>/pawnio)

# Flat layout: the exe and the Qt runtime at the install root, plugins and QML
# modules beside them. Every directory is named explicitly because the defaults
# moved between Qt versions (6.10 deploys into bin\, stranding ring0.exe away
# from its DLLs; 6.11 deploys flat).
install(TARGETS ring0 RUNTIME DESTINATION .)
install(DIRECTORY assets/pawnio DESTINATION .)
qt_generate_deploy_script(
    TARGET ring0
    OUTPUT_SCRIPT ring0_deploy
    CONTENT "
qt_deploy_runtime_dependencies(
    EXECUTABLE \"$<TARGET_FILE_NAME:ring0>\"
    BIN_DIR .
    LIB_DIR .
    PLUGINS_DIR plugins
    QML_DIR qml
    DEPLOY_TOOL_OPTIONS --qmldir \"${CMAKE_CURRENT_SOURCE_DIR}/src/qml\" --no-compiler-runtime --no-translations
)")
install(SCRIPT ${ring0_deploy})

# The MSVC runtime ships app-locally beside ring0.exe, Microsoft's supported
# alternative to running vc_redist, which the deploy tool would otherwise bundle
# and nothing would ever launch. The UI is English only, so no Qt translations.
set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
set(CMAKE_INSTALL_UCRT_LIBRARIES FALSE)
include(InstallRequiredSystemLibraries)
