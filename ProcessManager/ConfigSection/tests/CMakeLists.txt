find_package(GTest REQUIRED)

include(GoogleTest)
add_executable(config_section_tests ConfigSectionTest.cpp)
target_link_libraries(config_section_tests PRIVATE config_section GTest::gtest_main)
gtest_discover_tests(config_section_tests)
