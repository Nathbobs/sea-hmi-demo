cmake_minimum_required(VERSION 3.16)
project(sea_plc_simulator CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    # The vcpkg libmodbus 3.1.10 port (pinned back from 3.1.11+, which switched
    # to an autoreconf-based build that's flaky on Windows) builds via its own
    # vendored CMakeLists.txt and installs a CMake package
    # config (libmodbusConfig.cmake) rather than a .pc file, so it must be
    # consumed via find_package(), not pkg-config.
    find_package(libmodbus CONFIG REQUIRED)
    set(MODBUS_TARGET modbus)
else()
    # On macOS/Linux (Homebrew, apt, etc.) libmodbus ships a pkg-config file.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MODBUS REQUIRED IMPORTED_TARGET libmodbus)
    set(MODBUS_TARGET PkgConfig::MODBUS)
endif()

add_executable(sea_plc_simulator src/main.cpp)
target_link_libraries(sea_plc_simulator PRIVATE ${MODBUS_TARGET})
