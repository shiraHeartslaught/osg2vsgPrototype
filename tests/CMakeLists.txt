find_package(GTest CONFIG REQUIRED)

add_executable(ivevsg_tests
    test_async.cpp
    test_base_map.cpp
    test_map_navigation.cpp
    test_map_scene.cpp
    test_mercator_projection.cpp
    test_camera_framing.cpp
    test_scene_bounds.cpp
    test_scene_loader.cpp
    test_tile_scheme.cpp
    test_tile_set_builder.cpp
)
find_package(GDAL CONFIG REQUIRED) # the tiler test writes a synthetic GeoTIFF
target_link_libraries(ivevsg_tests PRIVATE ivevsg::ivevsg GTest::gtest GTest::gtest_main GDAL::GDAL)
# Default .ive for the loader integration test; the IVEVSG_TEST_IVE environment variable overrides it.
target_compile_definitions(ivevsg_tests PRIVATE "IVEVSG_TEST_IVE_DEFAULT=\"${IVEVSG_TEST_IVE}\"")

include(GoogleTest)
gtest_discover_tests(ivevsg_tests)
