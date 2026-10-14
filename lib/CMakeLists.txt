# ivevsg: reusable library behind the ive_poc viewer.

# OSG finds its format plugins (osgdb_ive.so, osgdb_png.so, ...) at runtime through OSG_LIBRARY_PATH.
# vcpkg installs them under <triplet>/plugins (Release) and <triplet>/debug/plugins (Debug), which OSG
# does not search by default, so bake the matching directory into the library as the loader's default.
file(GLOB _osg_plugins_release "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/plugins/osgPlugins-*")
file(GLOB _osg_plugins_debug   "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/plugins/osgPlugins-*")
set(IVEVSG_OSG_PLUGINS_RELEASE "${_osg_plugins_release}" PARENT_SCOPE)
set(IVEVSG_OSG_PLUGINS_DEBUG   "${_osg_plugins_debug}"   PARENT_SCOPE)

add_library(ivevsg
    src/AsyncLayerLoader.cpp
    src/BaseMap.cpp
    src/CameraFraming.cpp
    src/FileDialog.cpp
    src/Hud.cpp
    src/MapNavigation.cpp
    src/MapScene.cpp
    src/MercatorProjection.cpp
    src/Projection.cpp
    src/SceneBounds.cpp
    src/SceneLoader.cpp
    src/TileScheme.cpp
    src/TileSetBuilder.cpp
    src/ViewerApp.cpp
)
add_library(ivevsg::ivevsg ALIAS ivevsg)

target_include_directories(ivevsg PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(ivevsg PUBLIC vsg::vsg vsgXchange::vsgXchange)
target_compile_features(ivevsg PUBLIC cxx_std_17)
target_compile_definitions(ivevsg PRIVATE
    "IVEVSG_OSG_PLUGIN_DIR=\"$<IF:$<CONFIG:Debug>,${_osg_plugins_debug},${_osg_plugins_release}>\"")
# Default base map image (see data/README.md); the app can override it with --base.
target_compile_definitions(ivevsg PRIVATE "IVEVSG_DATA_DIR=\"${PROJECT_SOURCE_DIR}/data\"")

# OpenSceneGraph (osgDB registry for plugin discovery, and writing .ive tile sets) and GDAL (reading
# georeferenced rasters for the tiler) are implementation details of the library.
find_package(unofficial-osg CONFIG REQUIRED)
find_package(GDAL CONFIG REQUIRED)
find_package(vsgImGui CONFIG REQUIRED) # Dear ImGui rendered by VSG (RenderImGui node + event forwarding)
find_package(nfd CONFIG REQUIRED)      # native file dialog (xdg-desktop-portal on Linux) for "Browse..."
target_link_libraries(ivevsg PRIVATE unofficial::osg::osg unofficial::osg::osgDB GDAL::GDAL vsgImGui::vsgImGui nfd::nfd)
target_compile_definitions(ivevsg PRIVATE IVEVSG_HAS_OSG=$<IF:$<BOOL:${IVEVSG_HAS_OSG}>,1,0>)
