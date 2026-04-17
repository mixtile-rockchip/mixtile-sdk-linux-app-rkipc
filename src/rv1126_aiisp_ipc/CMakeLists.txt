
#include(FindPkgConfig)
#pkg_check_modules (GLIB REQUIRED IMPORTED_TARGET glib-2.0)

# link_directories(${PROJECT_SOURCE_DIR}/lib)

include_directories(${PROJECT_SOURCE_DIR}/include
					${CMAKE_SYSROOT}/usr/include/rkaiq/
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
aux_source_directory(server SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/ SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/audio SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/isp/rv1126 SRCS)
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
aux_source_directory(${PROJECT_SOURCE_DIR}/common/rockiva SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/event SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/region_clip SRCS)

include_directories(video
					server
					${PROJECT_SOURCE_DIR}/common
					${PROJECT_SOURCE_DIR}/common/audio
					${PROJECT_SOURCE_DIR}/common/isp/rv1126
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
					${PROJECT_SOURCE_DIR}/common/rockiva
					${PROJECT_SOURCE_DIR}/common/event
					${PROJECT_SOURCE_DIR}/common/region_clip
					)

link_directories(${PROJECT_SOURCE_DIR}/lib/arm-rockchip830-linux-gnueabihf)

add_definitions(-DISP_HW_V20)
add_definitions(-g -ggdb)

add_executable(${PROJECT_NAME} ${SRCS})

if(RKIPC_WIFI_SUPPORT)
	target_link_libraries(${PROJECT_NAME}
							pthread rockit rkaiq rtsp freetype wpa_client rkmuxer m rksysutils iconv rockiva rga rkaudio)
else()
	target_link_libraries(${PROJECT_NAME}
							pthread rockit rkaiq rtsp freetype rkmuxer m rksysutils iconv rockiva rga rkaudio)
endif()

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
install(FILES rkipc-os04a10.ini DESTINATION share)
install(PROGRAMS RkLunch.sh DESTINATION bin)
install(PROGRAMS RkLunch-stop.sh DESTINATION bin)
install(FILES ${PROJECT_SOURCE_DIR}/common/osd/image.bmp DESTINATION share)
install(FILES ${PROJECT_SOURCE_DIR}/common/osd/rkipc_cn.ttf DESTINATION share)
if(RKIPC_WIFI_SUPPORT)
	install(FILES ${PROJECT_SOURCE_DIR}/lib/arm-rockchip830-linux-gnueabihf/libwpa_client.so DESTINATION lib)
endif()
