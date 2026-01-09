// Copyright 2025 Rockchip Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "uvc_mode.c"

#include <linux/media.h>
#include <linux/v4l2-mediabus.h>
#include <linux/v4l2-subdev.h>
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
#include <rk_mpi_vi.h>
#include <rk_mpi_vo.h>

#include "common.h"
#include "media_ctrl.h"
#include "uvc-gadget.h"
#include "uvc_control.h"
#include "uvc_video.h"

static pthread_t uvc_thread;
static bool uvc_thread_running = false;

static void *uvc_get_stream_loop(void *arg) {
	printf("#Start %s thread, arg:%p\n", __func__, arg);
	VENC_STREAM_S video_stream = {};
	VIDEO_FRAME_INFO_S video_frame = {};
	const char *uvc_format = rk_param_get_string("video.source:uvc_format", NULL);
	int loopCount = 0;
	int ret = 0;
	// FILE *fp = fopen("/data/venc.h265", "wb");
	video_stream.pstPack = malloc(sizeof(VENC_PACK_S));

	while (uvc_thread_running) {
		if (!strcmp(uvc_format, "NV12")) {
			ret = RK_MPI_VI_GetChnFrame(VI_DEV_ID, VI_UVC_CHN, &video_frame, 1000);
			if (ret == RK_SUCCESS) {
				void *data = RK_MPI_MB_Handle2VirAddr(video_frame.stVFrame.pMbBlk);
				size_t len = video_frame.stVFrame.u32Width * video_frame.stVFrame.u32Height * 3 / 2;
				if (data)
					uvc_read_camera_buffer(data, -1, len, NULL, 0);
				LOG_DEBUG("data %p, len %zu\n", data, len);
				RK_MPI_VI_ReleaseChnFrame(VI_DEV_ID, VI_UVC_CHN, &video_frame);
			} else {
				LOG_ERROR("RK_MPI_VI_GetChnFrame failed %#X\n", ret);
			}
		} else {
			// 5.get the frame
			ret = RK_MPI_VENC_GetStream(VENC_UVC_CHN, &video_stream, 1000);
			if (ret == RK_SUCCESS) {
				void *data = RK_MPI_MB_Handle2VirAddr(video_stream.pstPack->pMbBlk);
				// fwrite(data, 1, video_stream.pstPack->u32Len, fp);
				// fflush(fp);
				LOG_DEBUG("Count:%d, Len:%d, PTS is %" PRId64 ", enH264EType is %d\n", loopCount,
				          video_stream.pstPack->u32Len, video_stream.pstPack->u64PTS,
				          video_stream.pstPack->DataType.enH264EType);
				uvc_read_camera_buffer(data, -1, video_stream.pstPack->u32Len, NULL, 0);

				ret = RK_MPI_VENC_ReleaseStream(VENC_UVC_CHN, &video_stream);
				if (ret != RK_SUCCESS)
					LOG_ERROR("RK_MPI_VENC_ReleaseStream fail %x\n", ret);
			} else {
				LOG_ERROR("RK_MPI_VENC_GetStream timeout %x\n", ret);
			}
			loopCount++;
		}
	}
	if (video_stream.pstPack)
		free(video_stream.pstPack);
	return 0;
}

static int open_uvc(int width, int height, int fcc, int fps) {
	LOG_INFO("open uvc %dx%d %d %d\n", width, height, fcc, fps);
	return 0;
}

static void close_uvc(void) { LOG_INFO("close_uvc"); }

static int init_uvc(void) {
	char usb_config_cmd[128];
	const char *uvc_format = rk_param_get_string("video.source:uvc_format", NULL);
	int width = rk_param_get_int("video.source:uvc_width", 0);
	int height = rk_param_get_int("video.source:uvc_height", 0);
	uint32_t flags = UVC_CONTROL_CHECK_STRAIGHT; // UVC_CONTROL_LOOP_ONCE;
	int enable_uvc = 1;
	int enable_uac = rk_param_get_int("audio.0:enable_uac", 0);
	LOG_INFO("enter\n");
	if (uvc_format == NULL || !width || !height) {
		LOG_ERROR("bad uvc config: width %d height %d format %s!\n", width, height, uvc_format);
		return -1;
	}
	if (enable_uvc || enable_uac) {
		memset(usb_config_cmd, 0, sizeof(usb_config_cmd));
		system("/etc/init.d/S50usbdevice stop");
		sleep(2); // wait old usb change
		if (enable_uvc && enable_uac) {
			snprintf(usb_config_cmd, sizeof(usb_config_cmd),
			         "rkipc_usb_config.sh -f %s -w %d -h %d -a", uvc_format, width, height);
		} else if (enable_uvc) {
			snprintf(usb_config_cmd, sizeof(usb_config_cmd),
			         "rkipc_usb_config.sh -f %s -w %d -h %d", uvc_format, width, height);
		} else if (enable_uac) {
			snprintf(usb_config_cmd, sizeof(usb_config_cmd), "rkipc_usb_config.sh -a");
		}
		LOG_INFO("usb_config_cmd is %s\n", usb_config_cmd);
		system(usb_config_cmd);
		sleep(1);
		if (enable_uvc) {
			uvc_formats_init(uvc_format, width, height);
			register_uvc_open_camera(open_uvc);
			register_uvc_close_camera(close_uvc);
		}
		uvc_control_run(flags); // also monitor uac
	}
	LOG_INFO("exit\n");
	return 0;
}

static int deinit_uvc(void) {
	int ret = 0;
	int flags = UVC_CONTROL_CHECK_STRAIGHT;
	LOG_INFO("enter\n");
	uvc_control_join(flags);
	uvc_formats_deinit();
	LOG_INFO("exit\n");
	return ret;
}

static int create_uvc_pipe(void) {
	int ret;
	VI_CHN_ATTR_S vi_chn_attr = {};
	VENC_CHN_ATTR_S venc_chn_attr = {};

	LOG_INFO("enter\n");
	vi_chn_attr.stIspOpt.u32BufCount = 3;
	vi_chn_attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
	vi_chn_attr.stSize.u32Width = rk_param_get_int("video.source:uvc_width", 0);
	vi_chn_attr.stSize.u32Height = rk_param_get_int("video.source:uvc_height", 0);
	vi_chn_attr.enPixelFormat = RK_FMT_YUV420SP;
	vi_chn_attr.u32Depth = 1;
	ret = RK_MPI_VI_SetChnAttr(VI_DEV_ID, VI_UVC_CHN, &vi_chn_attr);
	ret |= RK_MPI_VI_EnableChn(VI_DEV_ID, VI_UVC_CHN);
	if (ret) {
		LOG_ERROR("ERROR: create VI uvc channel error! ret=%#X\n", ret);
		return ret;
	}

	const char *uvc_format = rk_param_get_string("video.source:uvc_format", NULL);
	if (!strcmp(uvc_format, "H.264")) {
		venc_chn_attr.stVencAttr.enType = RK_VIDEO_ID_AVC;
		venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
		venc_chn_attr.stRcAttr.stH264Cbr.u32Gop = 60;
		venc_chn_attr.stRcAttr.stH264Cbr.u32BitRate = 2000;
		venc_chn_attr.stRcAttr.stH264Cbr.fr32DstFrameRateDen = 1;
		venc_chn_attr.stRcAttr.stH264Cbr.fr32DstFrameRateNum = 30;
		venc_chn_attr.stRcAttr.stH264Cbr.u32SrcFrameRateDen = 1;
		venc_chn_attr.stRcAttr.stH264Cbr.u32SrcFrameRateNum = 30;
	} else if (!strcmp(uvc_format, "H.265")) {
		venc_chn_attr.stVencAttr.enType = RK_VIDEO_ID_HEVC;
		venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H265CBR;
		venc_chn_attr.stRcAttr.stH265Cbr.u32Gop = 60;
		venc_chn_attr.stRcAttr.stH265Cbr.u32BitRate = 2000;
		venc_chn_attr.stRcAttr.stH265Cbr.fr32DstFrameRateDen = 1;
		venc_chn_attr.stRcAttr.stH265Cbr.fr32DstFrameRateNum = 30;
		venc_chn_attr.stRcAttr.stH265Cbr.u32SrcFrameRateDen = 1;
		venc_chn_attr.stRcAttr.stH265Cbr.u32SrcFrameRateNum = 30;
	} else {
		LOG_INFO("exit\n");
		return ret;
	}

	venc_chn_attr.stGopAttr.enGopMode = VENC_GOPMODE_NORMALP;
	venc_chn_attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
	venc_chn_attr.stVencAttr.u32PicWidth = rk_param_get_int("video.source:uvc_width", 0);
	venc_chn_attr.stVencAttr.u32PicHeight = rk_param_get_int("video.source:uvc_height", 0);
	venc_chn_attr.stVencAttr.u32VirWidth = rk_param_get_int("video.source:uvc_width", 0);
	venc_chn_attr.stVencAttr.u32VirHeight = rk_param_get_int("video.source:uvc_height", 0);
	venc_chn_attr.stVencAttr.u32StreamBufCnt = 5;
	venc_chn_attr.stVencAttr.u32BufSize = 1 * 1024 * 1024;
	// venc_chn_attr.stVencAttr.u32Depth = 1;
	ret = RK_MPI_VENC_CreateChn(VENC_UVC_CHN, &venc_chn_attr);
	if (ret) {
		LOG_ERROR("ERROR: create UVC VENC error! ret=%d\n", ret);
		return -1;
	}
	VENC_RC_PARAM_S venc_rc_param;
	if (!strcmp(uvc_format, "H.264")) {
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
	} else {
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
	}
	RK_MPI_VENC_SetRcParam(VENC_UVC_CHN, &venc_rc_param);
	VENC_RECV_PIC_PARAM_S recv_param = {};
	recv_param.s32RecvPicNum = -1;
	ret = RK_MPI_VENC_StartRecvFrame(VENC_UVC_CHN, &recv_param);
	if (ret)
		LOG_ERROR("ERROR: start recv frame error! ret=%d\n", ret);
	LOG_INFO("exit\n");
	return ret;
}

static int destroy_uvc_pipe(void) {
	int ret = 0;
	LOG_INFO("enter\n");
	ret = RK_MPI_VI_DisableChn(VI_DEV_ID, VI_UVC_CHN);
	if (ret)
		LOG_ERROR("ERROR: RK_MPI_VI_DisableChn VI uvc channel error! ret=%x\n", ret);
	const char *uvc_format = rk_param_get_string("video.source:uvc_format", NULL);
	if (!strcmp(uvc_format, "NV12")) {
		LOG_INFO("exit\n");
		return ret;
	}
	ret = RK_MPI_VENC_StopRecvFrame(VENC_UVC_CHN);
	if (ret)
		LOG_ERROR("ERROR: stop recv frame error! ret=%d\n", ret);
	ret = RK_MPI_VENC_DestroyChn(VENC_UVC_CHN);
	if (ret)
		LOG_ERROR("ERROR: destroy VENC error! ret=%d\n", ret);
	LOG_INFO("exit\n");
	return ret;
}

static int bind_uvc_pipe(void) {
	MPP_CHN_S vi_uvc_chn;
	MPP_CHN_S venc_uvc_chn;
	int ret = 0;
	const char *uvc_format = rk_param_get_string("video.source:uvc_format", NULL);
	LOG_INFO("enter\n");
	vi_uvc_chn.enModId = RK_ID_VI;
	vi_uvc_chn.s32DevId = VI_DEV_ID;
	vi_uvc_chn.s32ChnId = VI_UVC_CHN;
	venc_uvc_chn.enModId = RK_ID_VENC;
	venc_uvc_chn.s32DevId = 0;
	venc_uvc_chn.s32ChnId = VENC_UVC_CHN;

	if (!strcmp(uvc_format, "H.264") || !strcmp(uvc_format, "H.265")) {
		ret = RK_MPI_SYS_Bind(&vi_uvc_chn, &venc_uvc_chn);
		if (ret != RK_SUCCESS) {
			LOG_ERROR("vi uvc chn bind venc uvc chn fail:%x\n", ret);
			return ret;
		}
	}
	uvc_thread_running = true;
	pthread_create(&uvc_thread, NULL, uvc_get_stream_loop, NULL);
	LOG_INFO("exit\n");
	return ret;
}

static int unbind_uvc_pipe(void) {
	MPP_CHN_S vi_uvc_chn;
	MPP_CHN_S venc_uvc_chn;
	int ret = 0;
	const char *uvc_format = rk_param_get_string("video.source:uvc_format", NULL);
	LOG_INFO("enter\n");
	vi_uvc_chn.enModId = RK_ID_VI;
	vi_uvc_chn.s32DevId = VI_DEV_ID;
	vi_uvc_chn.s32ChnId = VI_UVC_CHN;
	venc_uvc_chn.enModId = RK_ID_VENC;
	venc_uvc_chn.s32DevId = 0;
	venc_uvc_chn.s32ChnId = VENC_UVC_CHN;

	uvc_thread_running = false;
	pthread_join(uvc_thread, NULL);
	if (!strcmp(uvc_format, "H.264") || !strcmp(uvc_format, "H.265")) {
		ret = RK_MPI_SYS_UnBind(&vi_uvc_chn, &venc_uvc_chn);
		if (ret != RK_SUCCESS)
			LOG_ERROR("vi uvc chn unbind venc uvc chn fail:%x\n", ret);
	}
	LOG_INFO("exit\n");
	return ret;
}

int init_uvc_mode(void) {
	int ret = 0;
	LOG_INFO("enter\n");
	ret = init_uvc();
	if (ret) {
		LOG_ERROR("init uvc failed %d\n", ret);
		return ret;
	}
	ret = create_uvc_pipe();
	if (ret) {
		LOG_ERROR("create uvc pipe failed %d\n", ret);
		return ret;
	}
	ret = bind_uvc_pipe();
	if (ret) {
		LOG_ERROR("bind uvc pipe failed %d\n", ret);
		return ret;
	}
	LOG_INFO("exit\n");
	return ret;
}

int deinit_uvc_mode(void) {
	int ret = 0;
	LOG_INFO("enter\n");
	ret = unbind_uvc_pipe();
	if (ret) {
		LOG_ERROR("unbind uvc pipe failed %d\n", ret);
		return ret;
	}
	ret = destroy_uvc_pipe();
	if (ret) {
		LOG_ERROR("destroy uvc pipe failed %d\n", ret);
		return ret;
	}
	ret = deinit_uvc();
	if (ret) {
		LOG_ERROR("deinit uvc failed %d\n", ret);
		return ret;
	}
	LOG_INFO("exit\n");
	return ret;
}