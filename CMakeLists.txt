cmake_minimum_required(VERSION 3.25)

project(
  az-dashboard
  VERSION 0.1.0
  DESCRIPTION "Azure cost, trend, and waste diagnostics CLI"
  LANGUAGES CXX
)

option(AZ_DASHBOARD_BUILD_TESTS "Build az-dashboard unit tests" ON)
option(AZ_DASHBOARD_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(AZ_DASHBOARD_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(AZ_DASHBOARD_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

if(AZ_DASHBOARD_ENABLE_ASAN AND AZ_DASHBOARD_ENABLE_TSAN)
  message(FATAL_ERROR "ASan and TSan cannot be enabled together")
endif()

set(AZ_DASHBOARD_SANITIZER_FLAGS "")
if(NOT MSVC)
  set(AZ_DASHBOARD_SANITIZERS "")
  if(AZ_DASHBOARD_ENABLE_ASAN)
    list(APPEND AZ_DASHBOARD_SANITIZERS address)
  endif()
  if(AZ_DASHBOARD_ENABLE_UBSAN)
    list(APPEND AZ_DASHBOARD_SANITIZERS undefined)
  endif()
  if(AZ_DASHBOARD_ENABLE_TSAN)
    list(APPEND AZ_DASHBOARD_SANITIZERS thread)
  endif()
  if(AZ_DASHBOARD_SANITIZERS)
    list(JOIN AZ_DASHBOARD_SANITIZERS "," AZ_DASHBOARD_SANITIZER_FLAGS)
  endif()
endif()

function(az_dashboard_apply_sanitizers target_name)
  if(AZ_DASHBOARD_SANITIZER_FLAGS)
    target_compile_options(${target_name} PRIVATE -fsanitize=${AZ_DASHBOARD_SANITIZER_FLAGS})
    target_link_options(${target_name} PRIVATE -fsanitize=${AZ_DASHBOARD_SANITIZER_FLAGS})
  endif()
endfunction()

add_library(az_dashboard_lib STATIC
  src/analytics.cpp
  src/azure_cli.cpp
  src/cli.cpp
  src/process_runner.cpp
  src/render.cpp
  src/report.cpp
  src/subscription_aliases.cpp
)

target_compile_features(az_dashboard_lib PUBLIC cxx_std_23)
target_include_directories(az_dashboard_lib
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

find_package(ftxui CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

target_link_libraries(az_dashboard_lib
  PUBLIC
    ftxui::component
    ftxui::dom
    ftxui::screen
    nlohmann_json::nlohmann_json
)
az_dashboard_apply_sanitizers(az_dashboard_lib)

if(MSVC)
  target_compile_options(az_dashboard_lib PRIVATE /W4 /permissive-)
else()
  target_compile_options(az_dashboard_lib PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_compile_definitions(az_dashboard_lib
  PUBLIC
    AZ_DASHBOARD_VERSION="${PROJECT_VERSION}"
)

add_executable(azdash src/main.cpp)
target_link_libraries(azdash PRIVATE az_dashboard_lib)
az_dashboard_apply_sanitizers(azdash)

include(GNUInstallDirs)
install(TARGETS azdash az_dashboard_lib
  EXPORT az-dashboard-targets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT az-dashboard-targets
  NAMESPACE az-dashboard::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/az-dashboard
)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/az-dashboard-config-version.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)
configure_package_config_file(
  cmake/az-dashboard-config.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/az-dashboard-config.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/az-dashboard
)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/az-dashboard-config.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/az-dashboard-config-version.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/az-dashboard
)

if(AZ_DASHBOARD_BUILD_TESTS)
  enable_testing()
  find_package(GTest CONFIG REQUIRED)

  add_executable(az-dashboard-tests
    tests/analytics_test.cpp
    tests/azure_cli_test.cpp
    tests/cli_test.cpp
    tests/render_test.cpp
    tests/report_test.cpp
    tests/subscription_aliases_test.cpp
  )
  target_link_libraries(az-dashboard-tests PRIVATE az_dashboard_lib GTest::gtest_main)
  az_dashboard_apply_sanitizers(az-dashboard-tests)

  include(GoogleTest)
  gtest_discover_tests(az-dashboard-tests)
endif()
