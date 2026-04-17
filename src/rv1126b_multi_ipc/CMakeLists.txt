cmake_minimum_required(VERSION 3.5)

include_directories(${PROJECT_SOURCE_DIR}/./include
					${CMAKE_SYSROOT}/usr/include/rkaiq/
					${CMAKE_SYSROOT}/usr/include/rkaiq/uAPI
					${CMAKE_SYSROOT}/usr/include/rkaiq/uAPI2
					${CMAKE_SYSROOT}/usr/include/rkaiq/algos
					${CMAKE_SYSROOT}/usr/include/rkaiq/common
					${CMAKE_SYSROOT}/usr/include/rkaiq/xcore
					${CMAKE_SYSROOT}/usr/include/rkaiq/iq_parser
					${CMAKE_SYSROOT}/usr/include/rkaiq/iq_parser_v2
					${CMAKE_SYSROOT}/usr/include/libdrm
					${WPA_SUPPLICANT_INCLUDE_DIRS}
					)

aux_source_directory(./ SRCS)
aux_source_directory(video SRCS)
aux_source_directory(server SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/ SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/audio SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/isp/rv1126b SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/socket_server SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/param SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/system SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/rtsp SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/rtmp SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/osd SRCS)
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
aux_source_directory(${PROJECT_SOURCE_DIR}/common/sysutil SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/venc SRCS)
aux_source_directory(${PROJECT_SOURCE_DIR}/common/uvc SRCS)

include_directories(video
					server
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
					${PROJECT_SOURCE_DIR}/common/rockiva
					${PROJECT_SOURCE_DIR}/common/event
					${PROJECT_SOURCE_DIR}/common/region_clip
					${PROJECT_SOURCE_DIR}/common/sysutil
					${PROJECT_SOURCE_DIR}/common/venc
					${PROJECT_SOURCE_DIR}/common/uvc
					)

message(STATUS "RK_APP_CHIP is ${RK_APP_CHIP}")

add_definitions(-DRKIPC_RV1126B)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-unused-result -Wno-unused-but-set-variable -Wno-unused-variabl -Werror=format")

message(STATUS "RK_APP_ARCH_TYPE is ${RK_APP_ARCH_TYPE}")
link_directories(${PROJECT_SOURCE_DIR}/lib/${TOOLCHAIN_PREFIX})
if(RKIPC_WIFI_SUPPORT)
	if(EXISTS ${PROJECT_SOURCE_DIR}/lib/${TOOLCHAIN_PREFIX}/libwpa_client.so)
		install(FILES ${PROJECT_SOURCE_DIR}/lib/${TOOLCHAIN_PREFIX}/libwpa_client.so DESTINATION lib)
	endif()
endif()

add_definitions(-DISP_HW_V35)
# add_definitions(-Os)

# add_definitions(-fsanitize=address -g -ggdb -gdwarf -funwind-tables -rdynamic -O0)
# add_definitions(-fno-stack-protector -fno-omit-frame-pointer -fsanitize-recover=address)
# link_libraries(asan)

add_executable(${PROJECT_NAME} ${SRCS})
if(RKIPC_WIFI_SUPPORT)
	target_link_libraries(${PROJECT_NAME}
					pthread rockit rkaiq rtsp freetype wpa_client rkmuxer m rockiva rga rkaudio)
else()
	target_link_libraries(${PROJECT_NAME}
					pthread rockit rkaiq rtsp freetype rkmuxer m rockiva rga rkaudio)
endif()

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)

install(FILES rkipc-2x.ini DESTINATION share)
install(FILES rkipc-2x-no-blend-hor.ini DESTINATION share)
install(FILES rkipc-4x.ini DESTINATION share)
install(FILES ../../common/speaker_test.wav DESTINATION share)
install(PROGRAMS RkLunch.sh DESTINATION bin)
install(PROGRAMS RkLunch-stop.sh DESTINATION bin)
install(PROGRAMS ${PROJECT_SOURCE_DIR}/common/uvc/rkipc_usb_config.sh DESTINATION bin)
install(FILES ${PROJECT_SOURCE_DIR}/common/osd/image.bmp DESTINATION share)
install(FILES ${PROJECT_SOURCE_DIR}/common/osd/rkipc_en.ttf DESTINATION share)
