// Copyright 2025 Rockchip Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "slowmotion_mode.c"

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
#include <rk_mpi_gdc.h>
#include <rk_mpi_mb.h>
#include <rk_mpi_mmz.h>
#include <rk_mpi_rgn.h>
#include <rk_mpi_sys.h>
#include <rk_mpi_venc.h>
#include <rk_mpi_vi.h>
#include <rk_mpi_vo.h>
#include <rk_mpi_vpss.h>

#include "common.h"
#include "isp.h"
#include "list.h"
#include "media_ctrl.h"
#include "osd.h"
#include "rk_sysfs.h"
#include "rtsp.h"
#include "storage.h"
#include "utils.h"

static struct {
	bool record_thread_running;
	pthread_t record_thread;
} rk_media_ctx;
static RK_MODE_PARAM_S rk_mode_param;

extern char *rkipc_iq_file_path_;

static void *slow_motion_loop(void *arg) {
	VENC_STREAM_S video_stream;
	uint64_t modified_pts = 0;
	uint64_t step = 33333; // isp 120fps output -> storage by 30fps
	int loop_count = 0;
	int ret = 0;
	FILE *fp = fopen("/tmp/venc.h265", "wb");
	video_stream.pstPack = malloc(sizeof(VENC_PACK_S));

	LOG_INFO("enter\n");
	prctl(PR_SET_NAME, __func__, 0, 0, 0);
	while (rk_media_ctx.record_thread_running) {
		// 5.get the frame
		ret = RK_MPI_VENC_GetStream(VENC_MAIN_CHN, &video_stream, 1000);
		if (ret == RK_SUCCESS) {
			if (modified_pts)
				modified_pts += step;
			else
				modified_pts = video_stream.pstPack->u64PTS;
			video_stream.pstPack->u64PTS = modified_pts;
			void *data = RK_MPI_MB_Handle2VirAddr(video_stream.pstPack->pMbBlk);
			LOG_DEBUG("Count:%d, Len:%d, PTS is %" PRId64 ", enH264EType is %d\n", loop_count,
			          video_stream.pstPack->u32Len, video_stream.pstPack->u64PTS,
			          video_stream.pstPack->DataType.enH264EType);
			if ((video_stream.pstPack->DataType.enH264EType == H264E_NALU_IDRSLICE) ||
			    (video_stream.pstPack->DataType.enH264EType == H264E_NALU_ISLICE) ||
			    (video_stream.pstPack->DataType.enH265EType == H265E_NALU_IDRSLICE) ||
			    (video_stream.pstPack->DataType.enH265EType == H265E_NALU_ISLICE)) {
				rk_storage_write_video_frame(STREAM_ID_FOR_SLOWMOTION, data,
				                             video_stream.pstPack->u32Len,
				                             video_stream.pstPack->u64PTS, 1);
			} else {
				rk_storage_write_video_frame(STREAM_ID_FOR_SLOWMOTION, data,
				                             video_stream.pstPack->u32Len,
				                             video_stream.pstPack->u64PTS, 0);
			}
			// 7.release the frame
			ret = RK_MPI_VENC_ReleaseStream(VENC_MAIN_CHN, &video_stream);
			if (ret != RK_SUCCESS)
				LOG_ERROR("RK_MPI_VENC_ReleaseStream fail %x\n", ret);
			loop_count++;
		} else {
			LOG_ERROR("RK_MPI_VENC_GetStream timeout %x\n", ret);
		}
	}
	if (video_stream.pstPack)
		free(video_stream.pstPack);
	if (fp)
		fclose(fp);
	LOG_INFO("exit\n");
	return 0;
}

static int vi_dev_init(void) {
	int ret = 0;
	VI_DEV_ATTR_S vi_dev_attr = {};
	VI_DEV_BIND_PIPE_S vi_dev_pipe = {};
	VI_PARAM_MOD_S vi_mod_param = {};
	// 0. get dev config status
	ret = RK_MPI_VI_GetDevAttr(VI_DEV_ID, &vi_dev_attr);
	if (ret == RK_ERR_VI_NOT_CONFIG) {
		// 0-1.config dev
		ret = RK_MPI_VI_SetDevAttr(VI_DEV_ID, &vi_dev_attr);
		if (ret != RK_SUCCESS) {
			LOG_ERROR("RK_MPI_VI_SetDevAttr %x\n", ret);
			return -1;
		}
	} else {
		LOG_ERROR("RK_MPI_VI_SetDevAttr already\n");
	}
	// 1.get dev enable status
	ret = RK_MPI_VI_GetDevIsEnable(VI_DEV_ID);
	if (ret != RK_SUCCESS) {
		// 1-2.enable dev
		ret = RK_MPI_VI_EnableDev(VI_DEV_ID);
		if (ret != RK_SUCCESS) {
			LOG_ERROR("RK_MPI_VI_EnableDev %x\n", ret);
			return -1;
		}
		// 1-3.bind dev/pipe
		vi_dev_pipe.u32Num = VI_DEV_ID;
		vi_dev_pipe.PipeId[0] = VI_DEV_ID;
		ret = RK_MPI_VI_SetDevBindPipe(VI_DEV_ID, &vi_dev_pipe);
		if (ret != RK_SUCCESS) {
			LOG_ERROR("RK_MPI_VI_SetDevBindPipe %x\n", ret);
			return -1;
		}
	} else {
		LOG_ERROR("RK_MPI_VI_EnableDev already\n");
	}
	LOG_INFO("vi device init success\n");
	return ret;
}

static int vi_dev_deinit(void) {
	RK_MPI_VI_DisableDev(VI_DEV_ID);
	return 0;
}

static int vi_chn_init(void) {
	int ret;
	int sensor_width = rk_mode_param.sensor_width;
	int sensor_height = rk_mode_param.sensor_height;
	int buf_cnt = rk_mode_param.vi_output_buf_cnt;
	int rotation = rk_mode_param.display_rotation;
	VI_CHN_ATTR_S vi_chn_attr = {};

	vi_chn_attr.stIspOpt.u32BufCount = buf_cnt;
	vi_chn_attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
	vi_chn_attr.stSize.u32Width = sensor_width;
	vi_chn_attr.stSize.u32Height = sensor_height;
	vi_chn_attr.enVideoFormat = VIDEO_FORMAT_LINEAR;
	vi_chn_attr.enCompressMode = COMPRESS_RFBC_64x4;
	vi_chn_attr.enPixelFormat = RK_FMT_YUV420SP;
	vi_chn_attr.u32Depth = 0;
	ret = RK_MPI_VI_SetChnAttr(VI_DEV_ID, VI_MAIN_CHN, &vi_chn_attr);
	ret |= RK_MPI_VI_EnableChn(VI_DEV_ID, VI_MAIN_CHN);
	if (ret) {
		LOG_ERROR("ERROR: create VI main channel error! ret=%#X\n", ret);
		return ret;
	}

	memset(&vi_chn_attr, 0, sizeof(vi_chn_attr));
	vi_chn_attr.stIspOpt.u32BufCount = 4;
	vi_chn_attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
	if (rotation == ROTATION_90 || rotation == ROTATION_270) {
		vi_chn_attr.stSize.u32Width = rk_mode_param.prev_height;
		vi_chn_attr.stSize.u32Height = rk_mode_param.prev_width;
	} else {
		vi_chn_attr.stSize.u32Width = rk_mode_param.prev_width;
		vi_chn_attr.stSize.u32Height = rk_mode_param.prev_height;
	}
	vi_chn_attr.enPixelFormat = RK_FMT_YUV420SP;
	vi_chn_attr.u32Depth = 0;
	if (rk_mode_param.enable_display) {
		ret = RK_MPI_VI_SetChnAttr(VI_DEV_ID, VI_DISPLAY_CHN, &vi_chn_attr);
		ret |= RK_MPI_VI_EnableChn(VI_DEV_ID, VI_DISPLAY_CHN);
	}
	if (ret) {
		LOG_ERROR("ERROR: create VI display channel error! ret=%#X\n", ret);
		return ret;
	}

	LOG_INFO("vi chn init success\n");
	return ret;
}

static int vi_chn_deinit(void) {
	int ret = 0;
	ret = RK_MPI_VI_DisableChn(VI_DEV_ID, VI_MAIN_CHN);
	if (ret)
		LOG_ERROR("ERROR: RK_MPI_VI_DisableChn VI main channel error! ret=%x\n", ret);
	if (rk_mode_param.enable_display)
		ret = RK_MPI_VI_DisableChn(VI_DEV_ID, VI_DISPLAY_CHN);
	if (ret)
		LOG_ERROR("ERROR: RK_MPI_VI_DisableChn VI display channel error! ret=%x\n", ret);
	if (ret == RK_SUCCESS)
		LOG_INFO("vi chn success\n");
	return ret;
}

static int vo_init(void) {
	int ret = 0;
	int vo_width = 1080;
	int vo_height = 1920;
	int vo_layer = VO_PREV_LAYER;
	int vo_dev_id = VO_DEV_ID;
	int vo_chn_id = VO_PREV_CHN;
	int vo_chn_priority = 0;
	int fps = 30;
	int vo_rotation = ROTATION_90;
	uint32_t disp_buf_len;
	const char *intf_type = "MIPI";
	VO_PUB_ATTR_S vo_pub_attr = {};
	VO_VIDEO_LAYER_ATTR_S vo_layer_attr = {};
	VO_CSC_S vo_csc = {};
	VO_CHN_ATTR_S vo_chn_attr = {};

	if (!rk_mode_param.enable_display) {
		LOG_INFO("display is off, no need to init vo\n");
		return ret;
	}

	ret = RK_MPI_VO_GetLayerDispBufLen(vo_layer, &disp_buf_len);
	LOG_INFO("Get vo_layer %d disp buf len is %d.\n", vo_layer, disp_buf_len);
	disp_buf_len = 3;
	ret = RK_MPI_VO_SetLayerDispBufLen(vo_layer, disp_buf_len);
	LOG_INFO("Agin Get vo_layer %d disp buf len is %d.\n", vo_layer, disp_buf_len);

	vo_layer_attr.stDispRect.s32X = rk_mode_param.prev_offset_x;
	vo_layer_attr.stDispRect.s32Y = rk_mode_param.prev_offset_y;
	vo_layer_attr.stDispRect.u32Width = rk_mode_param.prev_width;
	vo_layer_attr.stDispRect.u32Height = rk_mode_param.prev_height;
	vo_layer_attr.stImageSize.u32Width = rk_mode_param.prev_width;
	vo_layer_attr.stImageSize.u32Height = rk_mode_param.prev_height;
	LOG_INFO("vo_layer_attr W=%d, H=%d\n", vo_layer_attr.stDispRect.u32Width,
	         vo_layer_attr.stDispRect.u32Height);

	vo_layer_attr.u32DispFrmRt = fps;
	if (vo_rotation != ROTATION_0) {
		// INFO: Would introduced a delay include RGA hardware process time and schedule time of
		// rockit
		vo_layer_attr.bBypassFrame = false;
		// FIXME: low delay is not support
		// vo_layer_attr.bLowDelay = true;
	} else {
		vo_layer_attr.bBypassFrame = true;
	}
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
	vo_chn_attr.u32Priority = vo_chn_priority;
	vo_chn_attr.stRect.s32X = 0;
	vo_chn_attr.stRect.s32Y = 0;
	vo_chn_attr.stRect.u32Width = rk_mode_param.prev_width;
	vo_chn_attr.stRect.u32Height = rk_mode_param.prev_height;
	vo_chn_attr.enRotation = vo_rotation;
	ret = RK_MPI_VO_SetChnAttr(vo_layer, vo_chn_id, &vo_chn_attr);
	ret = RK_MPI_VO_EnableChn(vo_layer, vo_chn_id);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("create %d layer %d ch vo failed!\n", vo_layer, vo_chn_id);
		return ret;
	}
	RK_MPI_VO_SetLayerCleanMode(vo_layer, VO_LAYER_CLEAN_MODE_ONCE);
	LOG_INFO("vo init success\n");
	return ret;
}

static int vo_deinit(void) {
	int ret = 0;
	int vo_layer = VO_PREV_LAYER;
	int vo_dev_id = VO_DEV_ID;
	int vo_chn_id = VO_PREV_CHN;
	if (!rk_mode_param.enable_display) {
		LOG_INFO("display is off, no need to deinit vo\n");
		return ret;
	}
	ret = RK_MPI_VO_DisableChn(vo_layer, vo_chn_id);
	if (ret)
		LOG_ERROR("RK_MPI_VO_DisableChn failed, ret is %#x\n", ret);
	ret = RK_MPI_VO_DisableLayer(vo_layer);
	if (ret)
		LOG_ERROR("RK_MPI_VO_DisableLayer failed, ret is %#x\n", ret);
	ret = RK_MPI_VO_UnBindLayer(vo_layer, vo_dev_id);
	if (ret)
		LOG_ERROR("RK_MPI_VO_UnBindLayer failed, ret is %#x\n", ret);
	if (ret == RK_SUCCESS)
		LOG_INFO("vo deinit success\n");
	return ret;
}

static int venc_init(void) {
	VENC_CHN_ATTR_S venc_chn_attr;
	int ret = 0;
	int video_width = rk_mode_param.video_width;
	int video_height = rk_mode_param.video_height;
	const char *tmp_output_data_type = rk_param_get_string("video.1:output_data_type", NULL);
	const char *tmp_rc_mode = rk_param_get_string("video.1:rc_mode", NULL);
	const char *tmp_h264_profile = rk_param_get_string("video.1:h264_profile", NULL);

	memset(&venc_chn_attr, 0, sizeof(venc_chn_attr));
	if ((tmp_output_data_type == NULL) || (tmp_rc_mode == NULL)) {
		LOG_ERROR("tmp_output_data_type or tmp_rc_mode is NULL\n");
		return -1;
	}
	LOG_INFO("tmp_output_data_type is %s, tmp_rc_mode is %s, tmp_h264_profile is %s\n",
	         tmp_output_data_type, tmp_rc_mode, tmp_h264_profile);
	if (!strcmp(tmp_output_data_type, "H.264")) {
		venc_chn_attr.stVencAttr.enType = RK_VIDEO_ID_AVC;

		if (!strcmp(tmp_h264_profile, "high"))
			venc_chn_attr.stVencAttr.u32Profile = 100;
		else if (!strcmp(tmp_h264_profile, "main"))
			venc_chn_attr.stVencAttr.u32Profile = 77;
		else if (!strcmp(tmp_h264_profile, "baseline"))
			venc_chn_attr.stVencAttr.u32Profile = 66;
		else
			LOG_ERROR("tmp_h264_profile is %s\n", tmp_h264_profile);

		if (!strcmp(tmp_rc_mode, "CBR")) {
			venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
			venc_chn_attr.stRcAttr.stH264Cbr.u32Gop = rk_param_get_int("video.1:gop", -1);
			venc_chn_attr.stRcAttr.stH264Cbr.u32BitRate = rk_param_get_int("video.1:max_rate", 0);
			venc_chn_attr.stRcAttr.stH264Cbr.fr32DstFrameRateDen =
			    rk_param_get_int("video.1:dst_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH264Cbr.fr32DstFrameRateNum =
			    rk_param_get_int("video.1:dst_frame_rate_num", -1);
			venc_chn_attr.stRcAttr.stH264Cbr.u32SrcFrameRateDen =
			    rk_param_get_int("video.1:src_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH264Cbr.u32SrcFrameRateNum =
			    rk_param_get_int("video.1:src_frame_rate_num", -1);
		} else {
			venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H264VBR;
			venc_chn_attr.stRcAttr.stH264Vbr.u32Gop = rk_param_get_int("video.1:gop", -1);
			venc_chn_attr.stRcAttr.stH264Vbr.u32BitRate = rk_param_get_int("video.1:max_rate", 0);
			venc_chn_attr.stRcAttr.stH264Vbr.fr32DstFrameRateDen =
			    rk_param_get_int("video.1:dst_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH264Vbr.fr32DstFrameRateNum =
			    rk_param_get_int("video.1:dst_frame_rate_num", -1);
			venc_chn_attr.stRcAttr.stH264Vbr.u32SrcFrameRateDen =
			    rk_param_get_int("video.1:src_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH264Vbr.u32SrcFrameRateNum =
			    rk_param_get_int("video.1:src_frame_rate_num", -1);
		}
	} else if (!strcmp(tmp_output_data_type, "H.265")) {
		venc_chn_attr.stVencAttr.enType = RK_VIDEO_ID_HEVC;
		if (!strcmp(tmp_rc_mode, "CBR")) {
			venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H265CBR;
			venc_chn_attr.stRcAttr.stH265Cbr.u32Gop = rk_param_get_int("video.1:gop", -1);
			venc_chn_attr.stRcAttr.stH265Cbr.u32BitRate = rk_param_get_int("video.1:max_rate", 0);
			venc_chn_attr.stRcAttr.stH265Cbr.fr32DstFrameRateDen =
			    rk_param_get_int("video.1:dst_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH265Cbr.fr32DstFrameRateNum =
			    rk_param_get_int("video.1:dst_frame_rate_num", -1);
			venc_chn_attr.stRcAttr.stH265Cbr.u32SrcFrameRateDen =
			    rk_param_get_int("video.1:src_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH265Cbr.u32SrcFrameRateNum =
			    rk_param_get_int("video.1:src_frame_rate_num", -1);
		} else {
			venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H265VBR;
			venc_chn_attr.stRcAttr.stH265Vbr.u32Gop = rk_param_get_int("video.1:gop", -1);
			venc_chn_attr.stRcAttr.stH265Vbr.u32BitRate = rk_param_get_int("video.1:max_rate", 0);
			venc_chn_attr.stRcAttr.stH265Vbr.fr32DstFrameRateDen =
			    rk_param_get_int("video.1:dst_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH265Vbr.fr32DstFrameRateNum =
			    rk_param_get_int("video.1:dst_frame_rate_num", -1);
			venc_chn_attr.stRcAttr.stH265Vbr.u32SrcFrameRateDen =
			    rk_param_get_int("video.1:src_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH265Vbr.u32SrcFrameRateNum =
			    rk_param_get_int("video.1:src_frame_rate_num", -1);
		}
	} else {
		LOG_ERROR("tmp_output_data_type is %s, not support\n", tmp_output_data_type);
		return -1;
	}

	const char *tmp_smart = rk_param_get_string("video.1:smart", NULL);
	const char *tmp_gop_mode = rk_param_get_string("video.1:gop_mode", NULL);
	if (!strcmp(tmp_gop_mode, "normalP")) {
		venc_chn_attr.stGopAttr.enGopMode = VENC_GOPMODE_NORMALP;
	} else if (!strcmp(tmp_gop_mode, "smartP")) {
		venc_chn_attr.stGopAttr.enGopMode = VENC_GOPMODE_SMARTP;
		venc_chn_attr.stGopAttr.s32VirIdrLen = rk_param_get_int("video.1:smartp_viridrlen", 25);
	} else if (!strcmp(tmp_gop_mode, "TSVC4")) {
		venc_chn_attr.stGopAttr.enGopMode = VENC_GOPMODE_TSVC4;
	}
	// venc_chn_attr.stGopAttr.u32GopSize = rk_param_get_int("video.1:gop", -1);

	venc_chn_attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
	venc_chn_attr.stVencAttr.u32PicWidth = video_width;
	venc_chn_attr.stVencAttr.u32PicHeight = video_height;
	venc_chn_attr.stVencAttr.u32VirWidth = video_width;
	venc_chn_attr.stVencAttr.u32VirHeight = video_height;
	venc_chn_attr.stVencAttr.u32StreamBufCnt = rk_param_get_int("video.1:buffer_count", 5);
	venc_chn_attr.stVencAttr.u32BufSize = rk_param_get_int("video.1:buffer_size", 6291456);
	// venc_chn_attr.stVencAttr.u32Depth = 1;
	ret = RK_MPI_VENC_CreateChn(VENC_MAIN_CHN, &venc_chn_attr);
	if (ret) {
		LOG_ERROR("ERROR: create VENC error! ret=%d\n", ret);
		return -1;
	}

	const char *tmp_rc_quality = rk_param_get_string("video.1:rc_quality", NULL);
	VENC_RC_PARAM_S venc_rc_param;
	RK_MPI_VENC_GetRcParam(VENC_MAIN_CHN, &venc_rc_param);
	if (!strcmp(tmp_output_data_type, "H.264")) {
		venc_rc_param.s32FirstFrameStartQp = 26;
		venc_rc_param.stParamH264.u32StepQp = 4;
		venc_rc_param.stParamH264.u32MinQp = 10;
		venc_rc_param.stParamH264.u32MaxQp = 51;
		venc_rc_param.stParamH264.u32MinIQp = 10;
		venc_rc_param.stParamH264.u32MaxIQp = 51;
		venc_rc_param.stParamH264.s32DeltIpQp = -2;
		venc_rc_param.stParamH264.s32MaxReEncodeTimes = 2;
		venc_rc_param.stParamH264.u32FrmMinQp = 10;
		venc_rc_param.stParamH264.u32FrmMaxQp = 51;
		venc_rc_param.stParamH264.u32FrmMinIQp = 10;
		venc_rc_param.stParamH264.u32FrmMaxIQp = 51;
	} else if (!strcmp(tmp_output_data_type, "H.265")) {
		venc_rc_param.s32FirstFrameStartQp = 26;
		venc_rc_param.stParamH265.u32StepQp = 4;
		venc_rc_param.stParamH265.u32MinQp = 10;
		venc_rc_param.stParamH265.u32MaxQp = 51;
		venc_rc_param.stParamH265.u32MinIQp = 10;
		venc_rc_param.stParamH265.u32MaxIQp = 51;
		venc_rc_param.stParamH265.s32DeltIpQp = -2;
		venc_rc_param.stParamH265.s32MaxReEncodeTimes = 2;
		venc_rc_param.stParamH265.u32FrmMinQp = 10;
		venc_rc_param.stParamH265.u32FrmMaxQp = 51;
		venc_rc_param.stParamH265.u32FrmMinIQp = 10;
		venc_rc_param.stParamH265.u32FrmMaxIQp = 51;
	} else {
		LOG_ERROR("tmp_output_data_type is %s, not support\n", tmp_output_data_type);
		return -1;
	}
	RK_MPI_VENC_SetRcParam(VENC_MAIN_CHN, &venc_rc_param);
	LOG_INFO("venc init success\n");
	return ret;
}

static int venc_deinit(void) {
	int ret = 0;
	ret = RK_MPI_VENC_StopRecvFrame(VENC_MAIN_CHN);
	if (ret)
		LOG_ERROR("ERROR: stop recv frame error! ret=%d\n", ret);
	ret = RK_MPI_VENC_DestroyChn(VENC_MAIN_CHN);
	if (ret) {
		LOG_ERROR("ERROR: destroy VENC error! ret=%d\n", ret);
		return ret;
	}
	LOG_ERROR("venc deinit success\n");
	return ret;
}

static int bind_display_pipe(void) {
	MPP_CHN_S vi_display_chn;
	MPP_CHN_S vo_chn;
	MPP_CHN_S gdc_display_chn;
	int ret = 0;
	LOG_INFO("enter\n");
	vi_display_chn.enModId = RK_ID_VI;
	vi_display_chn.s32DevId = VI_DEV_ID;
	vi_display_chn.s32ChnId = VI_DISPLAY_CHN;
	vo_chn.enModId = RK_ID_VO;
	vo_chn.s32DevId = VO_PREV_LAYER;
	vo_chn.s32ChnId = VO_PREV_CHN;

	if (rk_mode_param.enable_display) {
		ret = RK_MPI_SYS_Bind(&vi_display_chn, &vo_chn);
		if (ret != RK_SUCCESS) {
			LOG_ERROR("vi display chn bind vo chn fail:%x\n", ret);
			return ret;
		}
	}
	LOG_INFO("exit\n");
	return ret;
}

static int unbind_display_pipe(void) {
	MPP_CHN_S vi_display_chn;
	MPP_CHN_S vo_chn;
	int ret = 0;
	LOG_INFO("enter\n");
	vi_display_chn.enModId = RK_ID_VI;
	vi_display_chn.s32DevId = VI_DEV_ID;
	vi_display_chn.s32ChnId = VI_DISPLAY_CHN;
	vo_chn.enModId = RK_ID_VO;
	vo_chn.s32DevId = VO_PREV_LAYER;
	vo_chn.s32ChnId = VO_PREV_CHN;

	if (rk_mode_param.enable_display) {
		ret = RK_MPI_SYS_UnBind(&vi_display_chn, &vo_chn);
		if (ret != RK_SUCCESS)
			LOG_ERROR("vi display chn unbind vo chn fail:%x\n", ret);
	}
	LOG_INFO("exit\n");
	return ret;
}

static int bind_record_pipe(void) {
	MPP_CHN_S vi_main_chn;
	MPP_CHN_S venc_chn;
	int ret = 0;
	LOG_INFO("enter\n");
	vi_main_chn.enModId = RK_ID_VI;
	vi_main_chn.s32DevId = VI_DEV_ID;
	vi_main_chn.s32ChnId = VI_MAIN_CHN;
	venc_chn.enModId = RK_ID_VENC;
	venc_chn.s32DevId = 0;
	venc_chn.s32ChnId = VENC_MAIN_CHN;

	ret = RK_MPI_SYS_Bind(&vi_main_chn, &venc_chn);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("vi main chn bind venc chn fail:%x\n", ret);
		return ret;
	}
	LOG_INFO("exit\n");
	return ret;
}

static int unbind_record_pipe(void) {
	MPP_CHN_S vi_main_chn;
	MPP_CHN_S venc_chn;
	int ret = 0;
	LOG_INFO("enter\n");
	vi_main_chn.enModId = RK_ID_VI;
	vi_main_chn.s32DevId = VI_DEV_ID;
	vi_main_chn.s32ChnId = VI_MAIN_CHN;
	venc_chn.enModId = RK_ID_VENC;
	venc_chn.s32DevId = 0;
	venc_chn.s32ChnId = VENC_MAIN_CHN;

	ret = RK_MPI_SYS_UnBind(&vi_main_chn, &venc_chn);
	if (ret != RK_SUCCESS)
		LOG_ERROR("vi main chn unbind venc chn fail:%x\n", ret);
	LOG_INFO("exit\n");
	return ret;
}

static int pause_preview(void) {
	int ret = 0;
	MPP_CHN_S vi_display_chn;
	MPP_CHN_S vo_chn;
	LOG_INFO("enter\n");
	vi_display_chn.enModId = RK_ID_VI;
	vi_display_chn.s32DevId = VI_DEV_ID;
	vi_display_chn.s32ChnId = VI_DISPLAY_CHN;
	vo_chn.enModId = RK_ID_VO;
	vo_chn.s32DevId = VO_PREV_LAYER;
	vo_chn.s32ChnId = VO_PREV_CHN;

	if (rk_mode_param.enable_display) {
		ret = RK_MPI_SYS_UnBind(&vi_display_chn, &vo_chn);
		if (ret != RK_SUCCESS)
			LOG_ERROR("vi display chn unbind vo chn fail:%x\n", ret);
		vo_deinit();
	}
	LOG_INFO("exit\n");
	return ret;
}

static int resume_preview(void) {
	int ret = 0;
	MPP_CHN_S vi_display_chn;
	MPP_CHN_S vo_chn;
	LOG_INFO("enter\n");
	ret = vo_init();
	if (ret) {
		LOG_ERROR("ERROR: vo_init failed! ret=%d\n", ret);
		return ret;
	}
	vi_display_chn.enModId = RK_ID_VI;
	vi_display_chn.s32DevId = VI_DEV_ID;
	vi_display_chn.s32ChnId = VI_DISPLAY_CHN;
	vo_chn.enModId = RK_ID_VO;
	vo_chn.s32DevId = VO_PREV_LAYER;
	vo_chn.s32ChnId = VO_PREV_CHN;

	if (rk_mode_param.enable_display) {
		ret = RK_MPI_SYS_Bind(&vi_display_chn, &vo_chn);
		if (ret != RK_SUCCESS) {
			LOG_ERROR("vi display chn bind vo chn fail:%x\n", ret);
			return ret;
		}
	}
	LOG_INFO("exit\n");
	return ret;
}

static int start_record(void) {
	int ret = 0;
	VENC_RECV_PIC_PARAM_S recv_param;
	if (rk_media_ctx.record_thread_running) {
		LOG_DEBUG("record is already started, no need to start record again\n");
		return 0;
	}
	memset(&recv_param, 0, sizeof(VENC_RECV_PIC_PARAM_S));
	recv_param.s32RecvPicNum = -1;
	ret = RK_MPI_VENC_StartRecvFrame(VENC_MAIN_CHN, &recv_param);
	if (!rk_param_get_int("video.source:enable_auto_record", 0))
		rk_storage_record_start(STREAM_ID_FOR_SLOWMOTION);
	rk_media_ctx.record_thread_running = true;
	pthread_create(&rk_media_ctx.record_thread, NULL, slow_motion_loop, NULL);

	if (ret) {
		LOG_ERROR("ERROR: start recv frame error! ret=%d\n", ret);
		goto __start_record_failed;
	}
	ret = bind_record_pipe();
	if (ret) {
		LOG_ERROR("ERROR: bind_record_pipe failed! ret=%d\n", ret);
		goto __start_record_failed;
	}
	return ret;

__start_record_failed:
	RK_MPI_VENC_StopRecvFrame(VENC_MAIN_CHN);
	rk_media_ctx.record_thread_running = false;
	pthread_join(rk_media_ctx.record_thread, NULL);
	unbind_record_pipe();
	return ret;
}

static int stop_record(void) {
	int ret = 0;
	if (rk_media_ctx.record_thread_running == false) {
		LOG_INFO("record is not started, no need to stop record\n");
		return 0;
	}
	rk_media_ctx.record_thread_running = false;
	pthread_join(rk_media_ctx.record_thread, NULL);
	ret = unbind_record_pipe();
	if (ret)
		LOG_ERROR("ERROR: unbind_record_pipe failed! ret=%d\n", ret);
	ret = RK_MPI_VENC_StopRecvFrame(VENC_MAIN_CHN);
	if (ret)
		LOG_ERROR("ERROR: stop recv frame error! ret=%d\n", ret);
	if (!rk_param_get_int("video.source:enable_auto_record", 0))
		rk_storage_record_stop(STREAM_ID_FOR_SLOWMOTION);
	return ret;
}

static int record_is_running(void) { return rk_media_ctx.record_thread_running; }

static int slowmotion_init(RK_MODE_PARAM_S param) {
	int ret = 0;
	LOG_INFO("enter\n");
	rk_mode_param = param;
	ret = set_camera_format(rk_mode_param.sensor_width, rk_mode_param.sensor_height,
	                        rk_mode_param.sensor_fps, rk_mode_param.sensor_format);
	ret = rk_isp_init(0, rkipc_iq_file_path_);
	if (ret) {
		LOG_ERROR("ERROR: rk_isp_init failed! ret=%d\n", ret);
		return ret;
	}
	rk_isp_set_frame_rate_without_ini(0, rk_mode_param.sensor_fps);

	ret = vi_dev_init();
	if (ret) {
		LOG_ERROR("ERROR: vi_dev_init failed! ret=%d\n", ret);
		return ret;
	}
	ret = vi_chn_init();
	if (ret) {
		LOG_ERROR("ERROR: vi_chn_init failed! ret=%d\n", ret);
		return ret;
	}
	ret = vo_init();
	if (ret) {
		LOG_ERROR("ERROR: vo_init failed! ret=%d\n", ret);
		return ret;
	}
	ret = venc_init();
	if (ret) {
		LOG_ERROR("ERROR: venc_init failed! ret=%d\n", ret);
		return ret;
	}
	ret = bind_display_pipe();
	if (ret) {
		LOG_ERROR("ERROR: bind_display_pipe failed! ret=%d\n", ret);
		return ret;
	}
	if (rk_param_get_int("video.source:enable_auto_record", 0)) {
		ret = start_record();
		if (ret)
			LOG_ERROR("ERROR: start_record failed! ret=%d\n", ret);
	}
	LOG_INFO("exit\n");
	return ret;
}

static int slowmotion_deinit(void) {
	int ret = 0;
	LOG_INFO("enter\n");
	ret = stop_record();
	if (ret)
		LOG_ERROR("ERROR: stop_record failed! ret=%d\n", ret);
	ret = unbind_display_pipe();
	if (ret)
		LOG_ERROR("ERROR: unbind_display_pipe failed! ret=%d\n", ret);
	ret = venc_deinit();
	if (ret)
		LOG_ERROR("ERROR: venc_deinit failed! ret=%d\n", ret);
	ret = vo_deinit();
	if (ret)
		LOG_ERROR("ERROR: vo_deinit failed! ret=%d\n", ret);
	ret = vi_chn_deinit();
	if (ret)
		LOG_ERROR("ERROR: vi_chn_deinit failed! ret=%d\n", ret);
	ret = vi_dev_deinit();
	if (ret)
		LOG_ERROR("ERROR: vi_dev_deinit failed! ret=%d\n", ret);
	ret = rk_isp_deinit(0);
	if (ret)
		LOG_ERROR("ERROR: rk_isp_deinit failed! ret=%d\n", ret);
	LOG_INFO("exit\n");
	return ret;
}

static RK_MODE_CALLBACK_S slowmotion_ops = {
    .init = slowmotion_init,
    .deinit = slowmotion_deinit,
    .start_record = start_record,
    .stop_record = stop_record,
    .record_is_running = record_is_running,
    .pause_preview = pause_preview,
    .resume_preview = resume_preview,
    .start_capture = NULL,
    .stop_capture = NULL,
    .get_capture_num = NULL,
};

static __attribute__((constructor)) void register_slowmotion_ops(void) {
	rk_media_register_callback(RK_SLOW_MOTION_MODE, &slowmotion_ops);
	const char *msg = "register slow motion operations\n";
	write(STDOUT_FILENO, msg, strlen(msg));
}
