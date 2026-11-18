include(GoogleTest)
add_executable(tepegoz_tests TepegozTest.cpp)
# The tests build services from configurations; none is started, so no socket is opened.
target_link_libraries(tepegoz_tests PRIVATE Tepegoz config_source GTest::gtest_main)
gtest_discover_tests(tepegoz_tests)
