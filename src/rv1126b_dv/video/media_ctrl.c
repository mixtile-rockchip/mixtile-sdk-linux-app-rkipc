// Copyright 2025 Rockchip Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "media_ctrl.c"

#include <linux/media.h>
#include <linux/v4l2-mediabus.h>
#include <linux/v4l2-subdev.h>
#include <rk_aiq_user_api2_acsm.h>
#include <rk_aiq_user_api2_camgroup.h>
#include <rk_aiq_user_api2_imgproc.h>
#include <rk_aiq_user_api2_isp.h>
#include <rk_aiq_user_api2_sysctl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <rk_debug.h>
#include <rk_mpi_mb.h>
#include <rk_mpi_mmz.h>
#include <rk_mpi_rgn.h>
#include <rk_mpi_sys.h>
#include <rk_mpi_venc.h>
#include <rk_mpi_vo.h>

#include "audio.h"
#include "common.h"
#include "media_ctrl.h"
#include "rk_sysfs.h"
#include "storage.h"

static struct {
	RK_MODE_E cur_mode;
	RK_PREVIEW_MODE_E prev_mode;
	RK_USB_MODE_E usb_mode;
	RK_EIS_MODE_E eis_mode;
	RK_HDR_MODE_E hdr_mode;
	RK_STORAGE_TYPE_E storage_type;
	VO_CHN_ATTR_S vo_prev_chn_attr;
	VO_VIDEO_LAYER_ATTR_S vo_prev_layer_attr;
	uint32_t max_photo_num;
	bool enable_eis_debug;
	bool enable_smart_ae;
	bool enable_display;
	bool enable_rtsp;
	bool enable_osd;
	bool enable_memc;
} rk_ctrl_param;

static RK_MODE_CALLBACK_S rk_mode_ops[RK_MODE_NUM] = {NULL};

int rk_display_init(void) {
	int32_t ret;
	int display_dev_id = VO_DEV_ID;
	VO_PUB_ATTR_S vo_pub_attr;
	const char *intf_type = rk_param_get_string("display:intf_type", "LCD");

	memset(&vo_pub_attr, 0, sizeof(VO_PUB_ATTR_S));
	if (!strcmp(intf_type, "LCD"))
		vo_pub_attr.enIntfType = VO_INTF_LCD;
	else if (!strcmp(intf_type, "MIPI"))
		vo_pub_attr.enIntfType = VO_INTF_MIPI;
	else if (!strcmp(intf_type, "DP"))
		vo_pub_attr.enIntfType = VO_INTF_DP;
	else {
		LOG_ERROR("bad intf_type!\n");
		return -1;
	}
	vo_pub_attr.enIntfSync = VO_OUTPUT_DEFAULT;

	ret = RK_MPI_VO_SetPubAttr(display_dev_id, &vo_pub_attr);
	if (ret)
		LOG_ERROR("RK_MPI_VO_SetPubAttr failed %#X\n", ret);
	ret = RK_MPI_VO_Enable(display_dev_id);
	if (ret)
		LOG_ERROR("RK_MPI_VO_Enable failed %#X\n", ret);
	return ret;
}

int rk_display_deinit(void) {
	int ret = 0;
	int display_dev_id = VO_DEV_ID;
	ret = RK_MPI_VO_Disable(display_dev_id);
	if (ret) {
		LOG_ERROR("RK_MPI_VO_Disable failed, ret is %#x\n", ret);
		return ret;
	}
	return ret;
}

int rk_resume_preview(void) {
	int ret = 0;
	int vo_layer = VO_PREV_LAYER;
	int vo_dev_id = VO_DEV_ID;
	int vo_chn_id = VO_PREV_CHN;
	VO_CHN_ATTR_S vo_chn_attr = {};

	LOG_INFO("enter\n");
	// 1. Destroy current VO layer for video player
	ret = RK_MPI_VO_DisableChn(vo_layer, vo_chn_id);
	if (ret)
		LOG_ERROR("RK_MPI_VO_DisableChn failed, ret is %#x\n", ret);
	ret = RK_MPI_VO_DisableLayer(vo_layer);
	if (ret)
		LOG_ERROR("RK_MPI_VO_DisableLayer failed, ret is %#x\n", ret);
	ret = RK_MPI_VO_UnBindLayer(vo_layer, vo_dev_id);
	if (ret)
		LOG_ERROR("RK_MPI_VO_UnBindLayer failed, ret is %#x\n", ret);
	// 2. Resume preview stream
	RKIPC_ASSERT(rk_mode_ops[rk_ctrl_param.cur_mode].resume_preview != NULL,
	             "resume_preview callback is NULL");
	ret = rk_mode_ops[rk_ctrl_param.cur_mode].resume_preview();
	if (ret) {
		LOG_ERROR("ERROR: resume preview failed! ret=%d\n", ret);
		return ret;
	}

	LOG_INFO("exit\n");
	return ret;
}

int rk_pause_preview(void) {
	int ret = 0;
	int vo_width = rk_param_get_int("display:width", 1080);
	int vo_height = rk_param_get_int("display:height", 1920);
	int vo_layer = VO_PREV_LAYER;
	int vo_dev_id = VO_DEV_ID;
	int vo_chn_id = VO_PREV_CHN;
	int fps = 30;
	int vo_rotation = ROTATION_90;
	uint32_t disp_buf_len;
	const char *intf_type = "MIPI";
	VO_PUB_ATTR_S vo_pub_attr = {};
	VO_CSC_S vo_csc = {};
	VO_CHN_ATTR_S vo_chn_attr = {};
	VO_VIDEO_LAYER_ATTR_S vo_layer_attr = {};

	LOG_INFO("enter\n");
	// 1. Pause preview stream
	RKIPC_ASSERT(rk_mode_ops[rk_ctrl_param.cur_mode].pause_preview != NULL,
	             "pause_preview callback is NULL");
	ret = rk_mode_ops[rk_ctrl_param.cur_mode].pause_preview();
	if (ret) {
		LOG_ERROR("ERROR: pause preview failed! ret=%d\n", ret);
		return ret;
	}
	// 2. Create new VO layer with full screen size for video player
	ret = RK_MPI_VO_GetLayerDispBufLen(vo_layer, &disp_buf_len);
	LOG_INFO("Get vo_layer %d disp buf len is %d.\n", vo_layer, disp_buf_len);
	disp_buf_len = 3;
	ret = RK_MPI_VO_SetLayerDispBufLen(vo_layer, disp_buf_len);
	LOG_INFO("Agin Get vo_layer %d disp buf len is %d.\n", vo_layer, disp_buf_len);

	vo_layer_attr.stDispRect.s32X = 0;
	vo_layer_attr.stDispRect.s32Y = 0;
	vo_layer_attr.stDispRect.u32Width = vo_width;
	vo_layer_attr.stDispRect.u32Height = vo_height;
	vo_layer_attr.stImageSize.u32Width = vo_width;
	vo_layer_attr.stImageSize.u32Height = vo_height;
	LOG_INFO("vo_layer_attr W=%d, H=%d\n", vo_layer_attr.stDispRect.u32Width,
	         vo_layer_attr.stDispRect.u32Height);

	vo_layer_attr.u32DispFrmRt = fps;
	vo_layer_attr.enPixFormat = RK_FMT_YUV420SP;
	vo_csc.enCscMatrix = VO_CSC_MATRIX_IDENTITY;
	vo_csc.u32Contrast = 50;
	vo_csc.u32Hue = 50;
	vo_csc.u32Luma = 50;
	vo_csc.u32Satuature = 50;

	/*bind layer0 to device hd0*/
	ret = RK_MPI_VO_BindLayer(vo_layer, vo_dev_id, VO_LAYER_MODE_GRAPHIC);
	// ret = RK_MPI_VO_BindLayer(vo_layer, vo_dev_id, VO_LAYER_MODE_VIDEO);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("failed to bind layer %#X\n", ret);
		return ret;
	}
	ret = RK_MPI_VO_SetLayerAttr(vo_layer, &vo_layer_attr);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("failed to set layer attr\n");
		return ret;
	}
	ret = RK_MPI_VO_SetLayerSpliceMode(vo_layer, VO_SPLICE_MODE_RGA);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("failed to set layer splice mode %#X\n", ret);
		return ret;
	}
	ret = RK_MPI_VO_SetLayerPriority(vo_layer, VO_PREV_LAYER_PRIORITY);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("failed to set layer priority %#X\n", ret);
		return ret;
	}
	ret = RK_MPI_VO_EnableLayer(vo_layer);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("failed to enable layer %#X\n", ret);
		return ret;
	}
	ret = RK_MPI_VO_SetLayerCSC(vo_layer, &vo_csc);
	vo_chn_attr.bDeflicker = RK_FALSE;
	vo_chn_attr.u32Priority = 0;
	vo_chn_attr.stRect.s32X = 0;
	vo_chn_attr.stRect.s32Y = 0;
	vo_chn_attr.stRect.u32Width = vo_width;
	vo_chn_attr.stRect.u32Height = vo_height;
	vo_chn_attr.enRotation = vo_rotation;
	ret = RK_MPI_VO_SetChnAttr(vo_layer, vo_chn_id, &vo_chn_attr);
	ret = RK_MPI_VO_EnableChn(vo_layer, vo_chn_id);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("create %d layer %d ch vo failed!\n", vo_layer, vo_chn_id);
		return ret;
	}
	RK_MPI_VO_SetLayerCleanMode(vo_layer, VO_LAYER_CLEAN_MODE_ONCE);

	LOG_INFO("exit\n");
	return ret;
}

static void init_param_by_mode(RK_MODE_E mode, RK_MODE_PARAM_S *param) {
	memset(param, 0, sizeof(RK_MODE_PARAM_S));
	if (mode == RK_VIDEO_MODE) {
		param->eis_mode = rk_ctrl_param.eis_mode;
		param->hdr_mode = rk_ctrl_param.hdr_mode;
		param->enable_smart_ae = rk_ctrl_param.enable_smart_ae;
		param->enable_memc = rk_ctrl_param.enable_memc;
		param->enable_eis_debug = rk_ctrl_param.enable_eis_debug;
		param->enable_display = rk_ctrl_param.enable_display;
		param->enable_rtsp = rk_ctrl_param.enable_rtsp;
		param->enable_osd = rk_param_get_int("video.source:enable_osd", 0);
		// Sensor parameters
		param->sensor_width = rk_param_get_int("sensor.linear:width", -1);
		param->sensor_height = rk_param_get_int("sensor.linear:height", -1);
		param->sensor_fps = rk_param_get_int("sensor.linear:fps", -1);
		param->sensor_format = rk_param_get_string("sensor.linear:format", NULL);
		// Video parameters
		param->video_width = rk_param_get_int("video.0:width", -1);
		param->video_height = rk_param_get_int("video.0:height", -1);
		// Other parameters
		param->vi_output_buf_cnt = rk_param_get_int("video.source:vi_buf_count", 12);
		param->gdc_output_buf_cnt = rk_param_get_int("video.source:gdc_buf_count", 4);
	} else if (mode == RK_PHOTO_MODE) {
		param->hdr_mode = rk_ctrl_param.hdr_mode;
		param->enable_display = rk_ctrl_param.enable_display;
		// Sensor parameters
		param->sensor_width = rk_param_get_int("sensor.linear:width", -1);
		param->sensor_height = rk_param_get_int("sensor.linear:height", -1);
		param->sensor_format = rk_param_get_string("sensor.linear:format", NULL);
		// Video parameters
		param->video_width = param->sensor_width;
		param->video_height = param->sensor_height;
		// Other parameters
		param->vi_output_buf_cnt = 4;
		param->max_photo_num = rk_ctrl_param.max_photo_num;
	} else if (mode == RK_SLOW_MOTION_MODE) {
		// Sensor parameters
		param->sensor_width = rk_param_get_int("sensor.binning:width", -1);
		param->sensor_height = rk_param_get_int("sensor.binning:height", -1);
		param->sensor_fps = rk_param_get_int("sensor.binning:fps", -1);
		param->sensor_format = rk_param_get_string("sensor.binning:format", NULL);
		// Video parameters
		param->video_width = param->sensor_width;
		param->video_height = param->sensor_height;
		// Other parameters
		param->vi_output_buf_cnt = 4;
	} else if (mode == RK_TIME_LAPSE_MODE) {
		// Sensor parameters
		param->sensor_width = rk_param_get_int("sensor.linear:width", -1);
		param->sensor_height = rk_param_get_int("sensor.linear:height", -1);
		param->sensor_fps = rk_param_get_int("sensor.linear:fps", -1);
		param->sensor_format = rk_param_get_string("sensor.linear:format", NULL);
		// Video parameters
		param->video_width = param->sensor_width;
		param->video_height = param->sensor_height;
		// Other parameters
		param->vi_output_buf_cnt = 4;
	}
	// Display parameters
	if (rk_ctrl_param.prev_mode == PREVIEW_MODE_320x240) {
		param->prev_width = 240;
		param->prev_height = 320;
		param->prev_offset_x = 420;
		param->prev_offset_y = 800;
	} else if (rk_ctrl_param.prev_mode == PREVIEW_MODE_1440x1080) {
		param->prev_width = 1080;
		param->prev_height = 1440;
		param->prev_offset_x = 0;
		param->prev_offset_y = 240;
	} else {
		param->prev_width = 1080;
		param->prev_height = 1920;
		param->prev_offset_x = 0;
		param->prev_offset_y = 0;
	}
	param->display_rotation = rk_param_get_int("display:rotation", ROTATION_90);
	param->enable_display = rk_ctrl_param.enable_display;
}

int rk_media_init(void) {
	int ret = 0;
	const char* dev_path = rk_param_get_string("storage:dev_path", NULL);
	RK_MODE_PARAM_S mode_param;
	memset(&rk_ctrl_param, 0, sizeof(rk_ctrl_param));
	RKIPC_ASSERT(dev_path != NULL, "bad storage param!");
	rk_ctrl_param.cur_mode = rk_param_get_int("video.source:mode", RK_VIDEO_MODE);
	rk_ctrl_param.usb_mode = USB_NORMAL_MODE;
	if (!strncmp(dev_path, STORAGE_INTERNAL_DEV_PATH, strlen(STORAGE_INTERNAL_DEV_PATH)))
		rk_ctrl_param.storage_type = RK_STORAGE_INTERNAL;
	else
		rk_ctrl_param.storage_type = RK_STORAGE_EXTERNAL;
	rk_ctrl_param.eis_mode = rk_param_get_int("video.source:eis_mode", RK_EIS_OFF);
	rk_ctrl_param.enable_eis_debug = rk_param_get_int("video.source:enable_eis_debug", 0);
	rk_ctrl_param.enable_display = rk_param_get_int("video.source:enable_display", 1);
	rk_ctrl_param.enable_rtsp = rk_param_get_int("video.source:enable_rtsp", 0);
	rk_ctrl_param.enable_smart_ae = rk_param_get_int("isp:enable_smart_ae", 1);
	rk_ctrl_param.enable_osd = rk_param_get_int("video.source:enable_osd", 0);
	rk_ctrl_param.hdr_mode = rk_param_get_int("isp:hdr_mode", RK_LINEAR_MODE);
	rk_ctrl_param.enable_memc = rk_param_get_int("isp:enable_memc", 0);
	rk_ctrl_param.prev_mode = rk_param_get_int("video.source:prev_mode", PREVIEW_MODE_320x240);
	rk_ctrl_param.max_photo_num = 5;
	init_param_by_mode(rk_ctrl_param.cur_mode, &mode_param);

	rk_storage_init();
	if (rk_param_get_int("audio.0:enable", 0) && rk_ctrl_param.cur_mode == RK_VIDEO_MODE)
		rkipc_audio_init();

	RKIPC_ASSERT(rk_mode_ops[rk_ctrl_param.cur_mode].init != NULL, "no register init callback\n");
	if (rk_mode_ops[rk_ctrl_param.cur_mode].init) {
		return rk_mode_ops[rk_ctrl_param.cur_mode].init(mode_param);
	} else {
		LOG_ERROR("mode %d has no register init callback\n", rk_ctrl_param.cur_mode);
		return -1;
	}

	ret = rk_enable_usb_function();
	if (ret) {
		LOG_ERROR("enable usb function failed %d\n", ret);
		return ret;
	}
	return 0;
}

int rk_media_deinit(void) {
	int ret = 0;
	ret = rk_disable_usb_function();
	if (ret) {
		LOG_ERROR("disable usb function failed %d\n", ret);
		return ret;
	}
	RKIPC_ASSERT(rk_mode_ops[rk_ctrl_param.cur_mode].deinit != NULL,
	             "no register deinit callback\n");
	if (rk_mode_ops[rk_ctrl_param.cur_mode].deinit) {
		ret = rk_mode_ops[rk_ctrl_param.cur_mode].deinit();
		if (ret) {
			LOG_ERROR("mode %d deinit failed, ret=%d\n", rk_ctrl_param.cur_mode, ret);
			return ret;
		}
	} else {
		LOG_ERROR("mode %d has no register deinit callback\n", rk_ctrl_param.cur_mode);
		return -1;
	}
	if (rk_param_get_int("audio.0:enable", 0) && rk_ctrl_param.cur_mode == RK_VIDEO_MODE)
		rkipc_audio_deinit();

	rk_storage_deinit();
	return ret;
}

int rk_media_reinit(void) {
	int ret = 0;
	RK_MODE_PARAM_S mode_param;
	init_param_by_mode(rk_ctrl_param.cur_mode, &mode_param);

	// Deinit old mode
	ret = rk_media_deinit();
	if (ret) {
		LOG_ERROR("mode %d deinit failed, ret=%d\n", rk_ctrl_param.cur_mode, ret);
		return ret;
	}
	// Init new mode
	ret = rk_media_init();
	if (ret) {
		LOG_ERROR("mode %d init failed, ret=%d\n", rk_ctrl_param.cur_mode, ret);
		return ret;
	}
	LOG_INFO("reinit mode %d success\n", rk_ctrl_param.cur_mode);
	return 0;
}

int rk_media_change_mode(RK_MODE_E new_mode) {
	int ret = 0;
	long long start_time = rkipc_get_curren_time_ms();
	RK_MODE_PARAM_S new_mode_param;
	if (new_mode == rk_ctrl_param.cur_mode) {
		LOG_DEBUG("mode is already %d, no need to set mode again\n", new_mode);
		return 0;
	}
	RKIPC_ASSERT(rk_mode_ops[new_mode].init != NULL, "no register init callback\n");
	RKIPC_ASSERT(rk_mode_ops[rk_ctrl_param.cur_mode].deinit != NULL,
	             "no register deinit callback\n");
	// Deinit old mode
	if (rk_param_get_int("audio.0:enable", 0) && rk_ctrl_param.cur_mode == RK_VIDEO_MODE)
		rkipc_audio_deinit();
	rk_storage_deinit();
	ret = rk_mode_ops[rk_ctrl_param.cur_mode].deinit();
	if (ret) {
		LOG_ERROR("mode %d deinit failed, ret=%d\n", rk_ctrl_param.cur_mode, ret);
		return ret;
	}
	// Update new mode param
	if (new_mode != RK_VIDEO_MODE) {
		rk_ctrl_param.enable_memc = false;
		rk_ctrl_param.enable_smart_ae = false;
		rk_param_set_int("isp:enable_memc", 0);
		rk_param_set_int("isp:enable_smart_ae", 0);
	} else {
		rk_ctrl_param.enable_memc = true;
		rk_ctrl_param.enable_smart_ae = true;
		rk_param_set_int("isp:enable_memc", 1);
		rk_param_set_int("isp:enable_smart_ae", 1);
	}
	init_param_by_mode(new_mode, &new_mode_param);
	// Init new mode
	ret = rk_mode_ops[new_mode].init(new_mode_param);
	if (ret) {
		LOG_ERROR("mode %d init failed, ret=%d\n", new_mode, ret);
		return ret;
	}
	if (rk_param_get_int("audio.0:enable", 0) && new_mode == RK_VIDEO_MODE)
		rkipc_audio_init();
	rk_storage_init();
	// Change mode success
	rk_ctrl_param.cur_mode = new_mode;
	rk_param_set_int("video.source:mode", new_mode);
	LOG_INFO("set new mode %d, cost time %lld ms\n", new_mode,
	         rkipc_get_curren_time_ms() - start_time);
	return 0;
}

RK_MODE_E rk_media_get_mode(void) { return rk_ctrl_param.cur_mode; }

int rk_media_register_callback(RK_MODE_E mode, RK_MODE_CALLBACK_S *callback) {
	if (mode < 0 || mode >= RK_MODE_NUM) {
		return -1;
	}
	if (callback == NULL) {
		return -1;
	}
	rk_mode_ops[mode] = *callback;
	return 0;
}

int rk_video_start_record(void) {
	if (rk_mode_ops[rk_ctrl_param.cur_mode].start_record) {
		return rk_mode_ops[rk_ctrl_param.cur_mode].start_record();
	} else {
		LOG_ERROR("mode %d has no register start_record callback\n", rk_ctrl_param.cur_mode);
		return -1;
	}
}

int rk_video_stop_record(void) {
	if (rk_mode_ops[rk_ctrl_param.cur_mode].stop_record) {
		return rk_mode_ops[rk_ctrl_param.cur_mode].stop_record();
	} else {
		LOG_ERROR("mode %d has no register stop_record callback\n", rk_ctrl_param.cur_mode);
		return -1;
	}
}

int rk_video_record_is_running(void) {
	if (rk_mode_ops[rk_ctrl_param.cur_mode].record_is_running) {
		return rk_mode_ops[rk_ctrl_param.cur_mode].record_is_running();
	} else {
		return 0;
	}
}

int rk_photo_start_capture(void) {
	if (rk_mode_ops[rk_ctrl_param.cur_mode].start_capture) {
		return rk_mode_ops[rk_ctrl_param.cur_mode].start_capture();
	} else {
		LOG_ERROR("mode %d has no register start_capture callback\n", rk_ctrl_param.cur_mode);
		return -1;
	}
}

int rk_photo_stop_capture(void) {
	if (rk_mode_ops[rk_ctrl_param.cur_mode].stop_capture) {
		return rk_mode_ops[rk_ctrl_param.cur_mode].stop_capture();
	} else {
		LOG_ERROR("mode %d has no register stop_capture callback\n", rk_ctrl_param.cur_mode);
		return -1;
	}
}

int rk_photo_set_max_num(int max_num) {
	rk_ctrl_param.max_photo_num = max_num;
	return 0;
}

int rk_photo_get_max_num(void) { return rk_ctrl_param.max_photo_num; }

int rk_photo_get_done_num(void) {
	if (rk_mode_ops[rk_ctrl_param.cur_mode].get_capture_num) {
		return rk_mode_ops[rk_ctrl_param.cur_mode].get_capture_num();
	} else {
		LOG_ERROR("mode %d has no register get_capture_num callback\n", rk_ctrl_param.cur_mode);
		return 0;
	}
}

RK_EIS_MODE_E rk_get_eis_mode(void) { return rk_ctrl_param.eis_mode; }

int rk_set_eis_mode(RK_EIS_MODE_E eis_mode) {
	int ret = 0;
	long long start_time = rkipc_get_curren_time_ms();
	if (rk_ctrl_param.cur_mode != RK_VIDEO_MODE) {
		LOG_DEBUG("need set eis attributes in video mode\n");
		return -1;
	}
	if (eis_mode == rk_ctrl_param.eis_mode) {
		LOG_DEBUG("eis mode is already %d, no need to set eis mode again\n", eis_mode);
		return 0;
	}
	if (eis_mode == RK_DISTORTION_CORRECTION) {
		LOG_ERROR("eis mode %d is not support\n", eis_mode);
		return -1;
	}
	rk_ctrl_param.eis_mode = eis_mode;
	rk_param_set_int("video.source:eis_mode", eis_mode);
	return 0;
}

int rk_set_eis_debug(bool enable) {
	int ret = 0;
	long long start_time = rkipc_get_curren_time_ms();
	if (rk_ctrl_param.cur_mode != RK_VIDEO_MODE) {
		LOG_DEBUG("need set eis attributes in video mode\n");
		return -1;
	}
	if (enable == rk_ctrl_param.enable_eis_debug) {
		LOG_DEBUG("eis debug is already %d, no need to set eis debug again\n", enable);
		return 0;
	}
	if (enable && rk_ctrl_param.eis_mode == RK_EIS_OFF) {
		LOG_DEBUG("eis debug is %d, but eis mode is %d, no need to set eis debug\n", enable,
		          rk_ctrl_param.eis_mode);
		return -1;
	}
	rk_ctrl_param.enable_eis_debug = enable;
	rk_param_set_int("video.source:enable_eis_debug", enable);
	return 0;
}

int rk_get_eis_debug(void) { return rk_ctrl_param.enable_eis_debug; }

int rk_set_hdr(RK_HDR_MODE_E hdr_mode) {
	int ret = 0;
	uint32_t width, height, fps, bps;
	long long start_time = rkipc_get_curren_time_ms();
	const char *iq_name = NULL;
	if (hdr_mode == rk_ctrl_param.hdr_mode) {
		LOG_DEBUG("hdr is already %d, no need to set hdr again\n", hdr_mode);
		return 0;
	}
	if (hdr_mode < 0 || hdr_mode >= RK_HDR_MODE_NUM) {
		LOG_ERROR("hdr mode %d is not support\n", hdr_mode);
		return -1;
	}
	if (rk_ctrl_param.cur_mode == RK_SLOW_MOTION_MODE ||
	    rk_ctrl_param.cur_mode == RK_TIME_LAPSE_MODE) {
		LOG_DEBUG("hdr is %d, but mode is %d, no need to set hdr\n", hdr_mode,
		          rk_ctrl_param.cur_mode);
		return 0;
	}
	if (hdr_mode == RK_HDR_DAG_MODE && !rk_param_get_int("sensor.dag_hdr:width", 0)) {
		RK_LOGE("this sensor not support dag hdr");
		return -1;
	}
	if (hdr_mode == RK_HDR_STAGGERED_MODE && !rk_param_get_int("sensor.stagger_hdr:width", 0)) {
		RK_LOGE("this sensor not support stagger hdr");
		return -1;
	}
	rk_ctrl_param.hdr_mode = hdr_mode;
	rk_param_set_int("isp:hdr_mode", hdr_mode);
	return 0;
}

RK_HDR_MODE_E rk_get_hdr(void) { return rk_ctrl_param.hdr_mode; }

int rk_set_memc_mode(bool enable) {
	int ret = 0;
	long long start_time = rkipc_get_curren_time_ms();
	if (enable == rk_ctrl_param.enable_memc) {
		LOG_DEBUG("memc mode is already %d, no need to set memc mode again\n", enable);
		return 0;
	}
	if (rk_ctrl_param.cur_mode != RK_VIDEO_MODE) {
		LOG_DEBUG("memc mode is %d, but mode is %d, no need to set memc mode\n", enable,
		          rk_ctrl_param.cur_mode);
		return -1;
	}
	rk_ctrl_param.enable_memc = enable;
	rk_param_set_int("isp:enable_memc", enable);
	return 0;
}

int rk_get_memc_mode(void) { return rk_ctrl_param.enable_memc; }

int rk_set_smart_ae(bool enable) {
	int ret = 0;
	if (enable == rk_ctrl_param.enable_smart_ae) {
		LOG_DEBUG("smart ae is already %d, no need to set smart ae again\n", enable);
		return 0;
	}
	if (rk_ctrl_param.cur_mode != RK_VIDEO_MODE) {
		LOG_DEBUG("smart ae is %d, but mode is %d, no need to set smart ae\n", enable,
		          rk_ctrl_param.cur_mode);
		return -1;
	}
	rk_ctrl_param.enable_smart_ae = enable;
	rk_param_set_int("isp:enable_smart_ae", enable);
	return 0;
}

int rk_get_smart_ae(void) { return rk_ctrl_param.enable_smart_ae; }

int rk_set_rtsp(bool enable) {
	int ret = 0;
	if (enable == rk_ctrl_param.enable_rtsp) {
		LOG_DEBUG("rtsp is already %d, no need to set rtsp again\n", enable);
		return 0;
	}
	rk_ctrl_param.enable_rtsp = enable;
	rk_param_set_int("video.source:enable_rtsp", enable);
	return 0;
}

int rk_get_rtsp(void) { return rk_ctrl_param.enable_rtsp; }

int rk_enter_sleep(void) {
	LOG_INFO("enter\n");
	// enter sleep
	write_sysfs_string("state", "/sys/power", "mem");
	LOG_INFO("exit\n");
	return 0;
}

int rk_venc_update_bitrate(void) {
	VENC_CHN_ATTR_S venc_chn_attr;
	const char *tmp_output_data_type = rk_param_get_string("video.0:output_data_type", NULL);
	const char *tmp_rc_mode = rk_param_get_string("video.0:rc_mode", NULL);
	const char *tmp_h264_profile = rk_param_get_string("video.0:h264_profile", NULL);
	int ret = 0;
	ret = RK_MPI_VENC_GetChnAttr(VENC_MAIN_CHN, &venc_chn_attr);
	if (ret) {
		LOG_ERROR("ERROR: RK_MPI_VENC_GetChnAttr failed! ret=%d\n", ret);
		return ret;
	}
	if (!strcmp(tmp_output_data_type, "H.264")) {
		if (!strcmp(tmp_rc_mode, "CBR")) {
			venc_chn_attr.stRcAttr.stH264Cbr.u32BitRate = rk_param_get_int("video.0:max_rate", 0);
		} else {
			venc_chn_attr.stRcAttr.stH264Vbr.u32MaxBitRate =
			    rk_param_get_int("video.0:max_rate", 0);
		}
	} else if (!strcmp(tmp_output_data_type, "H.265")) {
		if (!strcmp(tmp_rc_mode, "CBR")) {
			venc_chn_attr.stRcAttr.stH265Cbr.u32BitRate = rk_param_get_int("video.0:max_rate", 0);
		} else {
			venc_chn_attr.stRcAttr.stH265Vbr.u32MaxBitRate =
			    rk_param_get_int("video.0:max_rate", 0);
		}
	} else {
		LOG_ERROR("ERROR: bad output_data_type %s\n", tmp_output_data_type);
		return -1;
	}
	ret = RK_MPI_VENC_SetChnAttr(VENC_MAIN_CHN, &venc_chn_attr);
	if (ret) {
		LOG_ERROR("ERROR: RK_MPI_VENC_SetChnAttr failed! ret=%d\n", ret);
		return ret;
	}
	return ret;
}

extern int init_uvc_mode(void);
extern int deinit_uvc_mode(void);
int rk_enable_usb_function(void) {
	int ret = 0;
	char usb_config_cmd[128];
	if (rk_ctrl_param.usb_mode == USB_UVC_MODE) {
		ret = init_uvc_mode();
		if (ret)
			return ret;
	} else if (rk_ctrl_param.usb_mode == USB_STORAGE_MODE) {
		// system("/etc/init.d/S50usbdevice stop");
		if (rkipc_storage_dev_mount_status_get() != DISK_MOUNTED) {
			LOG_ERROR("sd card is not exist!\n");
			return -1;
		}
		memset(usb_config_cmd, 0, sizeof(usb_config_cmd));
		if (rk_ctrl_param.storage_type == RK_STORAGE_EXTERNAL) {
			snprintf(usb_config_cmd, sizeof(usb_config_cmd) - 1,
					"echo \"ums_block=%s\" >> /tmp/.usb_config",
					STORAGE_SDCARD_DEV_PATH);
			system("echo usb_ums_en > /tmp/.usb_config");
			system(usb_config_cmd);
			system("echo \"ums_block_type=exfat\" >> /tmp/.usb_config");
		} else {
			snprintf(usb_config_cmd, sizeof(usb_config_cmd) - 1,
					"echo \"ums_block=%s\" >> /tmp/.usb_config",
					STORAGE_INTERNAL_DEV_PATH);
			system("echo usb_ums_en > /tmp/.usb_config");
			system(usb_config_cmd);
			system("echo \"ums_block_type=exfat\" >> /tmp/.usb_config");
		}
		system("/etc/init.d/S50usbdevice restart");
	} else {
		system("echo usb_adb_en > /tmp/.usb_config");
		system("/etc/init.d/S50usbdevice restart");
	}
	return ret;
}

int rk_disable_usb_function(void) {
	if (rk_ctrl_param.usb_mode == USB_UVC_MODE) {
		deinit_uvc_mode();
	} else if (rk_ctrl_param.usb_mode == USB_STORAGE_MODE) {
		// system("/etc/init.d/S50usbdevice stop");
		system("echo usb_adb_en > /tmp/.usb_config");
		system("/etc/init.d/S50usbdevice restart");
	}
	return 0;
}

int rk_set_usb_mode(RK_USB_MODE_E mode) {
	int ret = 0;
	if (mode < 0 || mode >= USB_MODE_MAX_NUM) {
		LOG_ERROR("error usb mode %d\n", mode);
		return -1;
	}
	rk_ctrl_param.usb_mode = mode;
	rk_enable_usb_function();
	return ret;
}

RK_USB_MODE_E rk_get_usb_mode(void) { return rk_ctrl_param.usb_mode; }

int rk_set_prev_mode(RK_PREVIEW_MODE_E mode) {
	if (mode == rk_ctrl_param.prev_mode)
		return 0;
	rk_ctrl_param.prev_mode = mode;
	rk_param_set_int("video.source:prev_mode", mode);
	return 0;
}

RK_PREVIEW_MODE_E rk_get_prev_mode(void) { return rk_ctrl_param.prev_mode; }

RK_STORAGE_TYPE_E rk_get_storage_type(void) {
	return rk_ctrl_param.storage_type;
}

int rk_set_storage_type(RK_STORAGE_TYPE_E type) {
	int ret = 0;
	if (type == rk_ctrl_param.storage_type)
		return ret;
	rk_storage_deinit();
	if (type == RK_STORAGE_EXTERNAL) {
		rk_param_set_string("storage:mount_path", STORAGE_SDCARD_MOUNT_PATH);
		rk_param_set_string("storage:dev_path", STORAGE_SDCARD_DEV_PATH);
	} else {
		rk_param_set_string("storage:mount_path", STORAGE_INTERNAL_MOUNT_PATH);
		rk_param_set_string("storage:dev_path", STORAGE_INTERNAL_DEV_PATH);
	}
	rk_ctrl_param.storage_type = type;
	rk_storage_init();
	return ret;
}
