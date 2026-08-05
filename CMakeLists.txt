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

if(APPLE)
  # ---- macOS (Apple Silicon or Intel) -------------------------------------
  # OpenSSL is OPTIONAL here: Homebrew provides it when installed; a Mac
  # without Homebrew builds against Apple's CommonCrypto instead (everything
  # works except TLS to TVs -- wss:// falls back to the plaintext ports).
  if(NOT OPENSSL_ROOT_DIR)
    foreach(_bp
        /opt/homebrew/opt/openssl@3   # Homebrew, Apple Silicon
        /opt/homebrew/opt/openssl@1.1
        /usr/local/opt/openssl@3      # Homebrew, Intel
        /usr/local/opt/openssl@1.1
        /opt/local)                   # MacPorts
      if(EXISTS "${_bp}/include/openssl/ssl.h")
        set(OPENSSL_ROOT_DIR "${_bp}")
        break()
      endif()
    endforeach()
  endif()
  find_package(OpenSSL QUIET)
  if(OPENSSL_FOUND)
    set(ADMUFFS_HAVE_OPENSSL 1)
    message(STATUS "admuffs: OpenSSL found (${OPENSSL_VERSION}) - TLS/wss enabled")
  else()
    set(ADMUFFS_HAVE_OPENSSL 0)
    message(STATUS "admuffs: OpenSSL NOT found - using Apple CommonCrypto. "
                   "TLS to TVs (wss) is disabled; plaintext API ports still work. "
                   "To enable TLS: brew install openssl@3, then re-run cmake.")
  endif()

  # ncurses ships with the macOS SDK; CMake's built-in Curses module finds it
  # without pkg-config (which a no-Homebrew Mac doesn't have).
  set(CURSES_NEED_NCURSES TRUE)
  find_package(Curses REQUIRED)

  set(ADMUFFS_HAVE_ALSA 0)  # ALSA is Linux-only; audio capture disabled here
else()
  # ---- Linux (Raspberry Pi / x86) ------------------------------------------
  find_package(OpenSSL REQUIRED)
  set(ADMUFFS_HAVE_OPENSSL 1)

  # ncurses and ALSA via pkg-config (portable across Debian/RPi OS).
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(NCURSES REQUIRED ncurses)
  pkg_check_modules(ALSA REQUIRED alsa)
  set(ADMUFFS_HAVE_ALSA 1)
endif()

add_executable(admuffs
  src/main.cpp
  src/common.cpp
  src/config.cpp
  src/net/crypto_util.cpp
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

target_compile_definitions(admuffs PRIVATE
  ADMUFFS_HAVE_OPENSSL=${ADMUFFS_HAVE_OPENSSL}
  ADMUFFS_HAVE_ALSA=${ADMUFFS_HAVE_ALSA}
)

target_include_directories(admuffs PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/third_party
)

target_link_libraries(admuffs PRIVATE
  CURL::libcurl
  Threads::Threads
)

if(ADMUFFS_HAVE_OPENSSL)
  target_link_libraries(admuffs PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

if(APPLE)
  target_include_directories(admuffs PRIVATE ${CURSES_INCLUDE_DIRS})
  target_link_libraries(admuffs PRIVATE ${CURSES_LIBRARIES})
else()
  target_include_directories(admuffs PRIVATE
    ${NCURSES_INCLUDE_DIRS} ${ALSA_INCLUDE_DIRS})
  target_link_libraries(admuffs PRIVATE
    ${NCURSES_LIBRARIES} ${ALSA_LIBRARIES})
  target_link_directories(admuffs PRIVATE
    ${NCURSES_LIBRARY_DIRS} ${ALSA_LIBRARY_DIRS})
endif()

install(TARGETS admuffs RUNTIME DESTINATION bin)
# IR code database -> <prefix>/share/admuffs/ir_codes, one of the standard
# locations resolve_ir_db_dir() searches, so an installed admuffs works from
# any working directory.
install(DIRECTORY data/ir_codes DESTINATION share/admuffs)
