// Copyright 2025 Rockchip Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#pragma once

#define IIO_IMU_DEVICE_NO 1

/* Video and encoder channel definitions */
#define VI_DEV_ID 0
#define VI_MAIN_CHN 3
#define VI_DISPLAY_CHN 2
#define VI_UVC_CHN 4
#define VENC_MAIN_CHN 0
#define JPEG_CHN 1
#define VENC_UVC_CHN 2

#define GDC_MAIN_CHN 0
#define GDC_DISPLAY_CHN 1
#define GDC_DUMMY_CHN 2
// INFO: In RV1126B, layer 0 support yuv and rgb, layer 1 just support rgb
// so we use layer 0 for camera preview, and layer 1 for drawing UI
#define VO_DEV_ID 0
#define VO_PREV_LAYER 0
#define VO_PREV_CHN 0
#define VO_PREV_LAYER_PRIORITY 0
#define VO_UI_LAYER 1
#define VO_UI_CHN 0
#define VO_UI_FPS 10
#define VO_UI_LAYER_PRIORITY 1

#define RGN_CHN 0

#define RTSP_URL_0 "/live/0"
#define RTSP_URL_1 "/live/1"
#define RTSP_URL_2 "/live/2"

#define STREAM_ID_FOR_VIDEO 0
#define STREAM_ID_FOR_SLOWMOTION 1
#define STREAM_ID_FOR_TIMELAPSE 2

#define STORAGE_SDCARD_DEV_PATH "/dev/mmcblk1p1"
#define STORAGE_SDCARD_MOUNT_PATH "/mnt/sdcard"
#define STORAGE_INTERNAL_DEV_PATH "/dev/mmcblk0p7"
#define STORAGE_INTERNAL_MOUNT_PATH "/userdata"

/**
 * Mode definitions

 * @RK_VIDEO_MODE: Video recording mode, EIS is supported
 * @RK_PHOTO_MODE: Photo capture mode
 * @RK_SLOW_MOTION_MODE: Slow motion video recording mode, just supports 4x slow-motion photography
 at 1080p
 * @RK_TIME_LAPSE_MODE: Time-lapse video recording mode
 */
typedef enum {
	RK_VIDEO_MODE = 0,
	RK_PHOTO_MODE,
	RK_SLOW_MOTION_MODE,
	RK_TIME_LAPSE_MODE,
	RK_MODE_NUM,
} RK_MODE_E;

/**
 * EIS mode definitions, just support in video mode
 *
 * @RK_EIS_OFF: EIS is off
 * @RK_NORMAL_STEADY: Normal steady mode,
 * @RK_HORIZON_STEADY: Horizon steady mode
 * @RK_DISTORTION_CORRECTION: Distortion correction mode (not supported now)
 */
typedef enum {
	RK_EIS_OFF = 0,
	RK_NORMAL_STEADY,
	RK_HORIZON_STEADY,
	RK_DISTORTION_CORRECTION,
	RK_EIS_MODE_NUM,
} RK_EIS_MODE_E;

/**
 * HDR mode definitions
 *
 * RK_LINEAR_MODE: non-HDR mode
 * RK_HDR_DAG_MODE: dag HDR mode(single frame HDR)
 * RK_HDR_STAGGERED_MODE: staggered HDR mode(multi-frame HDR)
 */
typedef enum {
	RK_LINEAR_MODE = 0,
	RK_HDR_DAG_MODE,
	RK_HDR_STAGGERED_MODE,
	RK_HDR_MODE_NUM,
} RK_HDR_MODE_E;

/**
 * USB mode definitions, each function is mutually exclusive
 *
 * @RK_NORMAL_MODE: normal mode, no usb function
 * @RK_UVC_MODE: UVC mode, support video and photo via UVC
 * @RK_STORAGE_MODE: Storage mode, support mass storage device via USB
 */
typedef enum {
	USB_NORMAL_MODE = 0,
	USB_UVC_MODE,
	USB_STORAGE_MODE,
	USB_MODE_MAX_NUM
} RK_USB_MODE_E;

/**
 * Preview mode definitions
 *
 * @PREVIEW_MODE_320x240: Preview at 320x240 resolution
 * @PREVIEW_MODE_1440x1080: Preview at 1440x1080 resolution
 * @PREVIEW_MODE_1920x1080: Preview at 1920x1080 resolution
 */
typedef enum {
	PREVIEW_MODE_320x240 = 0,
	PREVIEW_MODE_1440x1080,
	PREVIEW_MODE_1920x1080,
} RK_PREVIEW_MODE_E;

/**
 * Storage type definitions

 * @RK_STORAGE_EXTERNAL: use external sdcard storage
 * @RK_STORAGE_INTERNAL: use internal emmc storage
 */
typedef enum {
	RK_STORAGE_EXTERNAL = 0,
	RK_STORAGE_INTERNAL,
	RK_STORAGE_MAX_NUM,
} RK_STORAGE_TYPE_E;

/**
 * Mode parameters
 *
 * @eis_mode: EIS mode
 * @hdr_mode: HDR mode
 * @storage_type: use external storage or internal storage
 * @sensor_width: Sensor output width
 * @sensor_height: Sensor output height
 * @sensor_fps: Sensor output frame rate
 * @sensor_format: Sensor output pixel format
 * @video_width: Video output width
 * @video_height: Video output height
 * @prev_width: Preview display width
 * @prev_height: Preview display height
 * @prev_offset_x: Preview display X offset
 * @prev_offset_y: Preview display Y offset
 * @display_rotation: Display rotation angle
 * @vi_output_buf_cnt: Number of VI output buffers
 * @gdc_output_buf_cnt: Number of GDC output buffers
 * @max_photo_num: Maximum number of photos to capture
 * @enable_memc: Enable/disable MEMC
 * @enable_eis_debug: Enable/disable EIS debug
 * @enable_smart_ae: Enable/disable smart AE
 * @enable_display: Enable/disable display output
 * @enable_rtsp: Enable/disable RTSP streaming
 * @enable_osd: Enable/disable OSD overlay
 */
typedef struct {
	RK_EIS_MODE_E eis_mode;
	RK_HDR_MODE_E hdr_mode;
	// Sensor parameters
	uint32_t sensor_width;
	uint32_t sensor_height;
	uint32_t sensor_fps;
	const char *sensor_format;
	// Video parameters
	uint32_t video_width;
	uint32_t video_height;
	// Display parameters
	uint32_t prev_width;
	uint32_t prev_height;
	uint32_t prev_offset_x;
	uint32_t prev_offset_y;
	uint32_t display_rotation;
	// Other parameters
	uint32_t vi_output_buf_cnt;
	uint32_t gdc_output_buf_cnt;
	uint32_t max_photo_num;
	bool enable_memc;
	bool enable_eis_debug;
	bool enable_smart_ae;
	bool enable_display;
	bool enable_rtsp;
	bool enable_osd;
} RK_MODE_PARAM_S;

/**
 * Callback functions for each mode
 *
 * @init: Initialize resources for this mode
 * @pause_preview: Pause preview
 * @resume_preview: Resume preview
 * @start_record: Start video recording
 * @stop_record: Stop video recording
 * @record_is_running: Check if video recording is running
 * @start_capture: Start photo capture
 * @stop_capture: Stop photo capture
 * @get_capture_num: Get number of captured photos
 * @deinit: Deinitialize resources for this mode
 */
typedef struct {
	int (*init)(RK_MODE_PARAM_S);
	int (*pause_preview)(void);
	int (*resume_preview)(void);
	int (*start_record)(void);
	int (*stop_record)(void);
	int (*record_is_running)(void);
	int (*start_capture)(void);
	int (*stop_capture)(void);
	int (*get_capture_num)(void);
	int (*deinit)(void);
} RK_MODE_CALLBACK_S;

/**
 * Media control functions
 *
 * @rk_media_register_callback: Register callback functions for specified mode
 * @rk_media_init: Initialize media resources according to current mode
 * @rk_media_deinit: Deinitialize media resources according to current mode
 * @rk_media_reinit: Reinitialize media resources according to current mode
 * @rk_media_change_mode: Change current mode and reinitialize media resources
 * @rk_media_get_mode: Get current mode
 */
int rk_media_register_callback(RK_MODE_E mode, RK_MODE_CALLBACK_S *callback);
int rk_media_init(void);
int rk_media_deinit(void);
int rk_media_reinit(void);
int rk_media_change_mode(RK_MODE_E mode);
RK_MODE_E rk_media_get_mode(void);

/**
 * EIS control functions
 *
 * @rk_get_eis_mode: Get current EIS mode
 * @rk_set_eis_mode: Set EIS mode
 *
 * .. note:: EIS is only supported in video mode
 */
RK_EIS_MODE_E rk_get_eis_mode(void);
int rk_set_eis_mode(RK_EIS_MODE_E eis_mode);

/**
 * HDR control functions
 *
 * @rk_get_hdr: Get current HDR mode
 * @rk_set_hdr: Set HDR mode
 *
 * .. note:: staggered HDR and EIS are mutually exclusive, because staggered HDR
 *           would cause ghosting artifacts, and EIS can't work well with dual-exposure
 *           images. So when setting staggered HDR mode, EIS would be disabled automatically.
 *			 When setting EIS mode, staggered HDR would be disabled automatically.
 *			 DCG/Lofic HDR is not supported now, waiting for future implementation...
 */
int rk_set_hdr(RK_HDR_MODE_E hdr_mode);
RK_HDR_MODE_E rk_get_hdr(void);

int rk_venc_update_bitrate(void);

/**
 * MEMC control functions
 *
 * @rk_get_memc_mode: Get current MEMC mode
 * @rk_set_memc_mode: Set MEMC mode
 *
 * .. note:: MEMC is only supported in video mode, and MEMC would introduce additional CPU load.
 */
int rk_set_memc_mode(bool enable);
int rk_get_memc_mode(void);

/**
 * EIS debug control functions
 *
 * @rk_set_eis_debug: Enable/disable EIS debug
 * @rk_get_eis_debug: Get current EIS debug status
 */
int rk_set_eis_debug(bool enable);
int rk_get_eis_debug(void);

/**
 * Smart AE control functions
 *
 * @rk_set_smart_ae: Enable/disable smart AE
 * @rk_get_smart_ae: Get current smart AE status
 *
 * .. note:: Smart AE automatically adjusts AE using IMU data to reduce motion blur. This function
 *		   is only supported in video mode.
 */
int rk_set_smart_ae(bool enable);
int rk_get_smart_ae(void);

/**
 * RTSP control functions
 *
 * @rk_set_rtsp: Enable/disable RTSP streaming
 * @rk_get_rtsp: Get current RTSP streaming status
 */
int rk_set_rtsp(bool enable);
int rk_get_rtsp(void);

/**
 * Preview mode control functions
 *
 * @rk_set_prev_mode: Set preview mode
 * @rk_get_prev_mode: Get current preview mode
 */
int rk_set_prev_mode(RK_PREVIEW_MODE_E mode);
RK_PREVIEW_MODE_E rk_get_prev_mode(void);

/**
 * Video recording control functions
 *
 * @rk_video_start_record: Start video recording
 * @rk_video_stop_record: Stop video recording
 * @rk_video_record_is_running: Check if video recording is running
 */
int rk_video_start_record(void);
int rk_video_stop_record(void);
int rk_video_record_is_running(void);

/**
 * Photo capture control functions
 *
 * @rk_photo_set_max_num: Set maximum number of photos to capture
 * @rk_photo_get_max_num: Get maximum number of photos to capture
 * @rk_photo_get_done_num: Get number of captured photos
 * @rk_photo_start_capture: Start photo capture
 * @rk_photo_stop_capture: Stop photo capture
 */
int rk_photo_set_max_num(int num);
int rk_photo_get_max_num(void);
int rk_photo_get_done_num(void);
int rk_photo_start_capture(void);
int rk_photo_stop_capture(void);
int rk_enter_sleep(void);

static int rk_video_reset_frame_rate(int stream_id) { return 0; }

/**
 * Display control functions
 *
 * @rk_display_init: Initialize display device like LCD panel
 * @rk_display_deinit: Deinitialize display device like LCD panel
 * @rk_pause_preview: Pause camera preview stream
 * @rk_resume_preview: Resume camera preview stream
 */
int rk_display_init(void);
int rk_display_deinit(void);
int rk_pause_preview(void);
int rk_resume_preview(void);

/**
 * USB control functions
 *
 * @rk_enable_usb_function: Enable USB function according to current USB mode
 * @rk_disable_usb_function: Disable USB function
 * @rk_set_usb_mode: Set USB mode
 * @rk_get_usb_mode: Get current USB mode
 */
int rk_enable_usb_function(void);
int rk_disable_usb_function(void);
int rk_set_usb_mode(RK_USB_MODE_E mode);
RK_USB_MODE_E rk_get_usb_mode(void);

RK_STORAGE_TYPE_E rk_get_storage_type(void);
int rk_set_storage_type(RK_STORAGE_TYPE_E type);
