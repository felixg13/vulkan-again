add_executable(renderer_tests renderer_test.cpp)
target_compile_features(renderer_tests PRIVATE cxx_std_23)
target_link_libraries(renderer_tests PRIVATE vulkan_engine_lib GTest::gtest_main)
set_target_properties(renderer_tests PROPERTIES BUILD_RPATH "$<TARGET_FILE_DIR:slang::slang>")

include(CompilerWarnings)
set_project_warnings(renderer_tests)

include(Sanitizers)
enable_sanitizers(renderer_tests)

add_test(NAME renderer_tests COMMAND renderer_tests
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
