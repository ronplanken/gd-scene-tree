cmake_minimum_required(VERSION 3.28)

project(gd-scene-tree VERSION 0.1.0 LANGUAGES C CXX)

set(OBS_APP "/Applications/OBS.app" CACHE PATH "OBS Studio application bundle to link against")
set(OBS_LIBOBS_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-studio-32.2.2/libobs" CACHE PATH "libobs header directory from the OBS source tree")
set(OBS_SIMDE_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/.deps/simde" CACHE PATH "simde header-only library root")
set(QT_PREFIX "/opt/homebrew/opt/qt" CACHE PATH "Qt 6 install prefix used for headers and cmake config")

set(SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src")
set(DATA "${CMAKE_CURRENT_SOURCE_DIR}/../data")

set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0")
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

list(APPEND CMAKE_PREFIX_PATH "${QT_PREFIX}")
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Svg)
get_filename_component(OBS_SRC_ROOT "${OBS_LIBOBS_INCLUDE}" DIRECTORY)

configure_file("${SRC}/plugin-support.c.in" plugin-support.c @ONLY)
file(GLOB ICON_FILES "${DATA}/icons/*.svg" "${DATA}/icons/LICENSE")

add_library(${PROJECT_NAME} MODULE
  "${SRC}/plugin-main.cpp"
  "${SRC}/scene-tree-dock.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/plugin-support.c"
  "${DATA}/locale/en-US.ini"
  ${ICON_FILES}
)

set_source_files_properties("${DATA}/locale/en-US.ini" PROPERTIES MACOSX_PACKAGE_LOCATION Resources/locale)
set_source_files_properties(${ICON_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/icons)

target_include_directories(${PROJECT_NAME} PRIVATE
  "${OBS_LIBOBS_INCLUDE}"
  "${OBS_SRC_ROOT}/frontend/api"
  "${OBS_SIMDE_INCLUDE}"
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${SRC}"
)
target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(${PROJECT_NAME} PRIVATE
  Qt6::Core
  Qt6::Widgets
  Qt6::Svg
  "${OBS_APP}/Contents/Frameworks/obs-frontend-api.dylib"
)
target_link_options(${PROJECT_NAME} PRIVATE
  "-F${OBS_APP}/Contents/Frameworks"
  "-framework" "libobs"
  "-Wl,-rpath,@executable_path/../Frameworks"
)

set(MACOSX_BUNDLE_BUNDLE_NAME "${PROJECT_NAME}")
set(MACOSX_BUNDLE_EXECUTABLE_NAME "${PROJECT_NAME}")
set(MACOSX_BUNDLE_GUI_IDENTIFIER "app.gdstreamer.obs.${PROJECT_NAME}")
set(MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
set(MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
set(MACOSX_BUNDLE_COPYRIGHT "Ron Planken")

set_target_properties(${PROJECT_NAME} PROPERTIES
  BUNDLE TRUE
  BUNDLE_EXTENSION plugin
  PREFIX ""
  OUTPUT_NAME "${PROJECT_NAME}"
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
)
