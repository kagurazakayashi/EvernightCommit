add_executable(gc_tests
  support/test_main.cpp
  app_state_tests.cpp
  layout_tests.cpp
  ui_metrics_tests.cpp
  utf_text_tests.cpp)

target_include_directories(gc_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gc_tests PRIVATE gc_core gc_platform gc_ui)
set_target_properties(gc_tests PROPERTIES WIN32_EXECUTABLE FALSE OUTPUT_NAME "gc_tests")

add_test(NAME gc_tests COMMAND gc_tests)
set_tests_properties(gc_tests PROPERTIES PASS_REGULAR_EXPRESSION "失败 0 个")
