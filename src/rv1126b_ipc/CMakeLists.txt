
#include(FindPkgConfig)
#pkg_check_modules (GLIB REQUIRED IMPORTED_TARGET glib-2.0)

# link_directories(${PROJECT_SOURCE_DIR}/lib)
include_directories(${PROJECT_SOURCE_DIR}/include
					${CMAKE_SYSROOT}/usr/include/rkaiq/
					${CMAKE_SYSROOT}/usr/include/rkaiq/isp
					${CMAKE_SYSROOT}/usr/include/rkaiq/uAPI
					${CMAKE_SYSROOT}/usr/include/rkaiq/uAPI2
					${CMAKE_SYSROOT}/usr/include/rkaiq/algos
					${CMAKE_SYSROOT}/usr/include/rkaiq/common
					${CMAKE_SYSROOT}/usr/include/rkaiq/xcore
					${CMAKE_SYSROOT}/usr/include/rkaiq/iq_parser
					${CMAKE_SYSROOT}/usr/include/rkaiq/iq_parser_v2
					${CMAKE_SYSROOT}/usr/include/libdrm
					${CMAKE_SYSROOT}/usr/include/freetype2
					${WPA_SUPPLICANT_INCLUDE_DIRS}
					)

aux_source_directory(./ SRCS)

aux_source_directory(video SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/ SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/audio SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/isp/rv1126b SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/socket_server SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/param SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/system SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/rtsp SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/rtmp SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/osd SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/osd/freetype2 SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/roi SRCS)
list(APPEND SRCS ${PROJECT_SOURCE_DIR}/common/network/network.c)
list(APPEND SRCS ${PROJECT_SOURCE_DIR}/common/network/ntp.c)
if(RKIPC_WIFI_SUPPORT)
	list(APPEND SRCS ${PROJECT_SOURCE_DIR}/common/network/Rk_wifi.c)
	list(APPEND SRCS ${PROJECT_SOURCE_DIR}/common/network/Hostapd.c)
	list(APPEND SRCS ${PROJECT_SOURCE_DIR}/common/network/RK_encode.c)
	list(APPEND SRCS ${PROJECT_SOURCE_DIR}/common/network/gbk_to_utf8.c)
	list(APPEND SRCS ${PROJECT_SOURCE_DIR}/common/network/utf8_to_gbk.c)
	list(APPEND SRCS ${PROJECT_SOURCE_DIR}/common/network/utility.c)
else()
	list(APPEND SRCS ${PROJECT_SOURCE_DIR}/common/network/wifi_stub.c)
endif()
aux_source_directory(${PROJECT_SOURCE_DIR}/common/storage SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/event SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/rockiva SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/region_clip SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/sysutil SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/venc SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/uvc SRCS)

include_directories(video
					${PROJECT_SOURCE_DIR}/common
					${PROJECT_SOURCE_DIR}/common/audio
					${PROJECT_SOURCE_DIR}/common/isp/rv1126b
					${PROJECT_SOURCE_DIR}/common/socket_server
					${PROJECT_SOURCE_DIR}/common/rtsp
					${PROJECT_SOURCE_DIR}/common/rtmp
					${PROJECT_SOURCE_DIR}/common/param
					${PROJECT_SOURCE_DIR}/common/system
					${PROJECT_SOURCE_DIR}/common/osd
					${PROJECT_SOURCE_DIR}/common/osd/freetype2
					${PROJECT_SOURCE_DIR}/common/roi
					${PROJECT_SOURCE_DIR}/common/network
					${PROJECT_SOURCE_DIR}/common/storage
					${PROJECT_SOURCE_DIR}/common/event
					${PROJECT_SOURCE_DIR}/common/rockiva
					${PROJECT_SOURCE_DIR}/common/region_clip
					${PROJECT_SOURCE_DIR}/common/sysutil
					${PROJECT_SOURCE_DIR}/common/venc
					${PROJECT_SOURCE_DIR}/common/uvc
					)

add_definitions(-DRKIPC_RV1126B)

# Disable WiFi support for rv1126b_ipc (override global RKIPC_WIFI_SUPPORT)
set(RKIPC_WIFI_SUPPORT OFF)

message(STATUS "RK_APP_ARCH_TYPE is ${RK_APP_ARCH_TYPE}")
link_directories(${PROJECT_SOURCE_DIR}/lib/${TOOLCHAIN_PREFIX})
if(RKIPC_WIFI_SUPPORT)
	if(EXISTS ${PROJECT_SOURCE_DIR}/lib/${TOOLCHAIN_PREFIX}/libwpa_client.so)
		install(FILES ${PROJECT_SOURCE_DIR}/lib/${TOOLCHAIN_PREFIX}/libwpa_client.so DESTINATION lib)
	endif()
endif()

add_definitions(-DISP_HW_V35)
add_definitions(-g -ggdb)

add_executable(${PROJECT_NAME} ${SRCS})

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-unused-result -Wno-unused-but-set-variable -Wno-unused-variable -Werror=format")

if(RKIPC_WIFI_SUPPORT)
	target_link_libraries(${PROJECT_NAME}
							pthread rockit rkaiq rtsp freetype wpa_client rkmuxer m rockiva rga rkaudio)
else()
	target_link_libraries(${PROJECT_NAME}
							pthread rockit rkaiq rtsp freetype rkmuxer m rockiva rga rkaudio)
endif()

if(COMPILE_FOR_BUILDROOT)
	install(FILES rkipc-2688x1520-buildroot.ini DESTINATION share RENAME rkipc-2688x1520.ini)
	install(PROGRAMS RkLunch-buildroot.sh DESTINATION bin RENAME RkLunch.sh)
else()
	install(FILES rkipc-2688x1520.ini DESTINATION share)
	install(FILES rkipc-3200x1800.ini DESTINATION share)
	install(FILES rkipc-3840x2160.ini DESTINATION share)
	install(PROGRAMS RkLunch.sh DESTINATION bin)
endif()
install(FILES ../../common/speaker_test.wav DESTINATION share)
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
install(PROGRAMS RkLunch-stop.sh DESTINATION bin)
install(PROGRAMS ${PROJECT_SOURCE_DIR}/common/uvc/rkipc_usb_config.sh DESTINATION bin)
install(FILES ${PROJECT_SOURCE_DIR}/common/osd/image.bmp DESTINATION share)
install(FILES ${PROJECT_SOURCE_DIR}/common/osd/rkipc_cn.ttf DESTINATION share)
install(FILES ${PROJECT_SOURCE_DIR}/common/osd/rkipc_en.ttf DESTINATION share)
