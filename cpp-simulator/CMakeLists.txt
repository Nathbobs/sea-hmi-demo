cmake_minimum_required(VERSION 3.16)
project(sea_plc_simulator CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(MODBUS REQUIRED IMPORTED_TARGET libmodbus)

add_executable(sea_plc_simulator src/main.cpp)
target_link_libraries(sea_plc_simulator PRIVATE PkgConfig::MODBUS)
