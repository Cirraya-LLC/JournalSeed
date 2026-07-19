find_package(Catch2 3.8.1 CONFIG REQUIRED)

add_executable(journalseed_tests
  money_test.cpp
  journal_entry_test.cpp
  app_config_test.cpp
  formula_graph_test.cpp
  lua_registry_test.cpp
)
target_link_libraries(journalseed_tests PRIVATE journalseed_core journalseed_lua Catch2::Catch2WithMain)
target_compile_features(journalseed_tests PRIVATE cxx_std_23)
journalseed_set_warnings(journalseed_tests)
journalseed_enable_sanitizers(journalseed_tests)

include(Catch)
catch_discover_tests(journalseed_tests)
