cmake_minimum_required(VERSION 3.13)
project(admuffs LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)

# ncurses and ALSA via pkg-config (portable across Debian/RPi OS).
find_package(PkgConfig REQUIRED)
pkg_check_modules(NCURSES REQUIRED ncurses)
pkg_check_modules(ALSA REQUIRED alsa)

add_executable(admuffs
  src/main.cpp
  src/common.cpp
  src/config.cpp
  src/net/http.cpp
  src/net/ws_client.cpp
  src/net/discovery.cpp
  src/ir/ir_database.cpp
  src/ir/ir_diag.cpp
  src/ir/ir_transmitter.cpp
  src/tv/factory.cpp
  src/tv/ir_controller.cpp
  src/tv/api_controllers.cpp
  src/detect/audio_bus.cpp
  src/detect/acr_client.cpp
  src/detect/fusion.cpp
  src/detect/sources.cpp
  src/tui/wizard.cpp
  src/web/web_server.cpp
  src/web/auth.cpp
  src/sensors/i2c_sensors.cpp
  src/sys/sysinfo.cpp
  src/ir/ir_record.cpp
  src/app.cpp
)

target_include_directories(admuffs PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party
  ${NCURSES_INCLUDE_DIRS}
  ${ALSA_INCLUDE_DIRS}
)

target_link_libraries(admuffs PRIVATE
  CURL::libcurl
  OpenSSL::SSL
  OpenSSL::Crypto
  ${NCURSES_LIBRARIES}
  ${ALSA_LIBRARIES}
  Threads::Threads
)

target_link_directories(admuffs PRIVATE ${NCURSES_LIBRARY_DIRS} ${ALSA_LIBRARY_DIRS})

install(TARGETS admuffs RUNTIME DESTINATION bin)
# IR code database -> <prefix>/share/admuffs/ir_codes, one of the standard
# locations resolve_ir_db_dir() searches, so an installed admuffs works from
# any working directory.
install(DIRECTORY data/ir_codes DESTINATION share/admuffs)
