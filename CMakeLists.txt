cmake_minimum_required(VERSION 3.5)

project(rkipc)

# generate version info
include(cmake/Version.cmake)

option(COMPILE_FOR_RV1103_IPC "compile for rv1103 ipc" OFF)
option(COMPILE_FOR_RV1106_IPC "compile for rv1106 ipc" OFF)
option(COMPILE_FOR_RV1106_BATTERY_IPC_CLIENT "compile for rv1106 battery ipc client" OFF)
option(COMPILE_FOR_RV1106_DUAL_IPC "compile for rv1106 dual-ipc" OFF)
option(COMPILE_FOR_RV1106_WAKEUP_IPC "compile for rv1106 wakeup-ipc" OFF)
option(COMPILE_FOR_RV1106_AIISP "compile for rv1106 aiisp" OFF)

option(COMPILE_FOR_RV1126_RKMEDIA "compile for rv1126 ipc rkmedia" OFF)
option(COMPILE_FOR_RV1126_ROCKIT "compile for rv1126 ipc rockit" OFF)
option(COMPILE_FOR_RV1126_SNAPSHOT "compile for rv1126 snapshot" OFF)
option(COMPILE_FOR_RV1126_DUAL_IPC "compile for rv1126 dual-ipc" OFF)
option(COMPILE_FOR_RV1126_AIISP "compile for rv1126 aiisp" OFF)

option(COMPILE_FOR_RK3588 "compile for rk3588 ipc" OFF)
option(COMPILE_FOR_RK3588_MULTI_IPC "compile for rk3588 multi-ipc" OFF)

option(COMPILE_FOR_RK3576 "compile for rk3576 ipc" OFF)
option(COMPILE_FOR_RK3576_MULTI_IPC "compile for rk3576 multi-ipc" OFF)
option(COMPILE_FOR_RK3576_DV "compile for rk3576 dv" OFF)

option(COMPILE_FOR_RV1126B "compile for rv1126b ipc" OFF)
option(COMPILE_FOR_RV1126B_MULTI_IPC "compile for rv1126b multi-ipc" OFF)
option(COMPILE_FOR_RV1126B_DV "compile for rv1126b dv" OFF)

option(COMPILE_FOR_RK3572 "compile for rk3572 ipc" OFF)

if(${CMAKE_BUILD_TYPE} STREQUAL Debug)
	message(STATUS "CMAKE_BUILD_TYPE is ${CMAKE_BUILD_TYPE}")
	add_definitions(-g -ggdb -gdwarf -funwind-tables -rdynamic -O0)
	add_definitions(-fno-stack-protector -fno-omit-frame-pointer)
elseif(${CMAKE_BUILD_TYPE} STREQUAL Release)
	message(STATUS "CMAKE_BUILD_TYPE is ${CMAKE_BUILD_TYPE}")
elseif(${CMAKE_BUILD_TYPE} STREQUAL RelWithDebInfo)
	message(STATUS "CMAKE_BUILD_TYPE is ${CMAKE_BUILD_TYPE}")
elseif(${CMAKE_BUILD_TYPE} STREQUAL MinSizeRel)
	message(STATUS "CMAKE_BUILD_TYPE is ${CMAKE_BUILD_TYPE}")
	add_definitions(-Os)
endif()

if(RK_APP_CHIP)
  add_definitions(-DRK_APP_CHIP)
endif()
if(RK_APP_ARCH_TYPE)
  add_definitions(-DRK_APP_ARCH_TYPE)
endif()

if(RKIPC_CROSS_COMPILE)
message(STATUS "CROSS COMPILE TOOL: ${RKIPC_CROSS_COMPILE}")

# Extract toolchain prefix from CMAKE_C_COMPILER
# e.g. /path/to/arm-rockchip1240-linux-gnueabihf-gcc -> arm-rockchip1240-linux-gnueabihf
# e.g. /path/to/arm-vendor-linux-musl-clang -> arm-vendor-linux-musl
get_filename_component(TOOLCHAIN_PREFIX ${CMAKE_C_COMPILER} NAME)
string(REGEX REPLACE "-(gcc|clang)$" "" TOOLCHAIN_PREFIX ${TOOLCHAIN_PREFIX})
message(STATUS "TOOLCHAIN_PREFIX: ${TOOLCHAIN_PREFIX}")

set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fdata-sections -ffunction-sections -Wl,-rpath-link,${PROJECT_SOURCE_DIR}/lib/${TOOLCHAIN_PREFIX}")
set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fdata-sections -ffunction-sections -Wl,-rpath-link,${PROJECT_SOURCE_DIR}/lib/${TOOLCHAIN_PREFIX}")
set (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections -Wl,--as-needed")

# Check if time_t is 64-bit in the toolchain (for musl/uClibc/glibc compatibility)
include(CheckTypeSize)
set(CMAKE_REQUIRED_FLAGS "${CMAKE_C_FLAGS}")
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
check_type_size("time_t" TIME_T_SIZE LANGUAGE C)
if(TIME_T_SIZE GREATER_EQUAL 8)
    add_definitions(-DHAVE_TIME_T_64BIT)
    message(STATUS "time_t is 64-bit (${TIME_T_SIZE} bytes), defined HAVE_TIME_T_64BIT")
else()
    message(STATUS "time_t is 32-bit (${TIME_T_SIZE} bytes)")
endif()
endif()
endif()

if(COMPILE_FOR_RV1103_IPC)
  add_subdirectory(src/rv1103_ipc)
endif()

if(COMPILE_FOR_RV1106_IPC)
  add_subdirectory(src/rv1106_ipc)
endif()

if(COMPILE_FOR_RV1106_BATTERY_IPC_CLIENT)
  add_subdirectory(src/rv1106_battery_ipc_client)
endif()

if(COMPILE_FOR_RV1106_DUAL_IPC)
  add_subdirectory(src/rv1106_dual_ipc)
endif()

if(COMPILE_FOR_RV1106_WAKEUP_IPC)
  add_subdirectory(src/rv1106_wakeup_ipc)
endif()

if(COMPILE_FOR_RV1106_AIISP)
  add_subdirectory(src/rv1106_aiisp_ipc)
endif()

if(COMPILE_FOR_RV1126_RKMEDIA)
  add_subdirectory(src/rv1126_ipc_rkmedia)
endif()

if(COMPILE_FOR_RV1126_ROCKIT)
  add_subdirectory(src/rv1126_ipc_rockit)
endif()

if(COMPILE_FOR_RV1126_SNAPSHOT)
  add_subdirectory(src/rv1126_snapshot)
endif()

if(COMPILE_FOR_RV1126_DUAL_IPC)
  add_subdirectory(src/rv1126_dual_ipc)
endif()

if(COMPILE_FOR_RV1126_AIISP)
  add_subdirectory(src/rv1126_aiisp_ipc)
endif()

if(COMPILE_FOR_RK3588)
  add_subdirectory(src/rk3588_ipc)
endif()

if(COMPILE_FOR_RK3588_MULTI_IPC)
  add_subdirectory(src/rk3588_multi_ipc)
endif()

if(COMPILE_FOR_RK3576)
  add_subdirectory(src/rk3576_ipc)
endif()

if(COMPILE_FOR_RK3576_MULTI_IPC)
  add_subdirectory(src/rk3576_multi_ipc)
endif()

if(COMPILE_FOR_RK3576_DV)
  add_subdirectory(src/rk3576_dv)
endif()

if(COMPILE_FOR_RV1126B)
  add_subdirectory(src/rv1126b_ipc)
endif()

if(COMPILE_FOR_RV1126B_MULTI_IPC)
  add_subdirectory(src/rv1126b_multi_ipc)
endif()

if(COMPILE_FOR_RV1126B_DV)
  add_subdirectory(src/rv1126b_dv)
endif()

if(COMPILE_FOR_RK3572)
  add_subdirectory(src/rk3572_ipc)
endif()

if(COMPILE_FOR_BUILDROOT)
  add_definitions(-DCOMPILE_FOR_BUILDROOT)
endif()

option(RKIPC_WIFI_SUPPORT "Enable WiFi support (wpa_client, wifi functions)" ON)
if(RKIPC_WIFI_SUPPORT)
  add_definitions(-DRKIPC_WIFI_SUPPORT)
endif()
