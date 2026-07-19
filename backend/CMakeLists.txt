add_library(journalseed_core STATIC
  src/config/app_config.cpp
  src/domain/money.cpp
  src/domain/journal_entry.cpp
  src/application/formula_graph.cpp
)

target_include_directories(journalseed_core PUBLIC include)
target_link_libraries(journalseed_core PUBLIC glaze::glaze)
target_compile_features(journalseed_core PUBLIC cxx_std_23)
journalseed_set_warnings(journalseed_core)
journalseed_enable_sanitizers(journalseed_core)

add_library(journalseed_lua STATIC
  src/lua/decimal.cpp
  src/lua/function_registry.cpp
)
target_include_directories(journalseed_lua PUBLIC include)
target_link_libraries(journalseed_lua PUBLIC sol2::sol2 lua::lua mpdecimal::mpdecimal)
target_compile_features(journalseed_lua PUBLIC cxx_std_23)
journalseed_set_warnings(journalseed_lua)
journalseed_enable_sanitizers(journalseed_lua)

if(JOURNALSEED_BUILD_SERVER)
  find_package(Drogon CONFIG REQUIRED)
  find_package(libsodium CONFIG REQUIRED)
  find_package(PostgreSQL CONFIG REQUIRED)
  find_path(JOURNALSEED_LIBPQ_INCLUDE_DIR libpq-fe.h
    HINTS
      "${libpq_PACKAGE_FOLDER_DEBUG}"
      "${libpq_PACKAGE_FOLDER_RELEASE}"
      "${libpq_PACKAGE_FOLDER_RELWITHDEBINFO}"
      "${libpq_PACKAGE_FOLDER_MINSIZEREL}"
    PATH_SUFFIXES include
    REQUIRED
  )

  add_executable(journalseed
    src/main.cpp
    src/api/api_controller.cpp
    src/application/journal_service.cpp
    src/infrastructure/migration_runner.cpp
    src/infrastructure/postgres_repository.cpp
  )
  target_link_libraries(journalseed PRIVATE
    journalseed_core
    journalseed_lua
    Drogon::Drogon
    libsodium::libsodium
    PostgreSQL::PostgreSQL
  )
  target_compile_definitions(journalseed PRIVATE
    JOURNALSEED_VERSION="${PROJECT_VERSION}"
    JOURNALSEED_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
  )
  target_include_directories(journalseed SYSTEM PRIVATE "${JOURNALSEED_LIBPQ_INCLUDE_DIR}")
  set_target_properties(journalseed PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
  )
  journalseed_set_warnings(journalseed)
  journalseed_enable_sanitizers(journalseed)

  if(JOURNALSEED_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
    if(ipo_supported)
      set_property(TARGET journalseed PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    else()
      message(WARNING "LTO is unavailable: ${ipo_error}")
    endif()
  endif()
endif()
