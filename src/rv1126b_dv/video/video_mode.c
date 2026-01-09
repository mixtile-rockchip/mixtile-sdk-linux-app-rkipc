// Copyright 2025 Rockchip Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "video_mode.c"

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
	bool osd_thread_running;
	uint64_t start_record_pts;
	uint32_t debug_frame_cnt;
	pthread_t record_thread;
	pthread_t osd_thread;
	FILE *vi_debug_file;  // eis debug info
	FILE *imu_debug_file; // eis debug info
} rk_media_ctx;
static RK_MODE_PARAM_S rk_mode_param;

extern char *rkipc_iq_file_path_;

extern void *rk_isp_get_ctx(int cam_id);
static int gdc_aiq_imu_callback(RK_VOID *pUsr, GDC_SENSOR_INFO_S *pInfo) {
	LOG_VERBOSE("time stamp = %" PRId64 ", temp is %lf,"
	            "gyro data is %lf %lf %lf, acc data is %lf %lf %lf\n",
	            pInfo->s64Timestamp, pInfo->dTemp, pInfo->dGyroData[0], pInfo->dGyroData[1],
	            pInfo->dGyroData[2], pInfo->dAccData[0], pInfo->dAccData[1], pInfo->dAccData[2]);
	AiqImuData_t imu_data = {};
	if (pInfo == NULL) {
		LOG_ERROR("========== bad imu info! ===========\n");
		return GDC_INFO_CB_SUCCESS;
	}
	imu_data.s64Timestamp = pInfo->s64Timestamp;
	imu_data.dTemp = pInfo->dTemp;
	imu_data.dGyroData[0] = pInfo->dGyroData[0];
	imu_data.dGyroData[1] = pInfo->dGyroData[1];
	imu_data.dGyroData[2] = pInfo->dGyroData[2];
	imu_data.dAccData[0] = pInfo->dAccData[0];
	imu_data.dAccData[1] = pInfo->dAccData[1];
	imu_data.dAccData[2] = pInfo->dAccData[2];
	void *aiq_ctx = rk_isp_get_ctx(0);
	if (aiq_ctx == NULL) {
		LOG_ERROR("========== bad aiq ctx! ===========\n");
		return GDC_INFO_CB_SUCCESS;
	}
	if (rk_mode_param.enable_smart_ae) {
		int ret = rk_aiq_user_api2_amtd_setImuData(aiq_ctx, &imu_data);
		if (ret != 0 && ret != 0x1) // ret == 1, bypass
			LOG_ERROR("rk_aiq_user_api2_amtd_setImuData failed %#X\n", ret);
	}
	if (rk_mode_param.enable_memc) {
		rk_aiq_uapi2_setImuData(aiq_ctx, &imu_data);
	}
	return GDC_INFO_CB_SUCCESS;
}

static int gdc_aiq_frame_callback(RK_VOID *pUsr, GDC_VFAME_INFO_S *pInfo) {
	if (pInfo && pInfo->pMbBlk)
		RK_MPI_MB_ReleaseMB(pInfo->pMbBlk);
	return GDC_INFO_CB_SUCCESS;
}

static int gdc_sensor_callback(void *pUsr, GDC_SENSOR_INFO_S *pInfo) {
	char gdc_imu_info_buffer[256] = {};
	if (!rk_media_ctx.record_thread_running)
		return GDC_INFO_CB_SUCCESS;
	sprintf(gdc_imu_info_buffer, "%lf %lf %lf %lf %lf %lf %lf %" PRId64 "\n", pInfo->dAccData[0],
	        pInfo->dAccData[1], pInfo->dAccData[2], pInfo->dGyroData[0], pInfo->dGyroData[1],
	        pInfo->dGyroData[2], pInfo->dTemp, pInfo->s64Timestamp);

	fwrite(gdc_imu_info_buffer, sizeof(char), strlen(gdc_imu_info_buffer),
	       rk_media_ctx.imu_debug_file);
	fflush(rk_media_ctx.imu_debug_file);

	return GDC_INFO_CB_SUCCESS;
}

static int gdc_frame_callback(void *pUsr, GDC_VFAME_INFO_S *pInfo) {
	VIDEO_FRAME_INFO_S gdc_frame;
	char gdc_vi_info_buffer[256] = {};

	if (!rk_media_ctx.record_thread_running) {
		RK_MPI_MB_ReleaseMB(pInfo->pMbBlk);
		return GDC_INFO_CB_SUCCESS;
	}
	// 为保证imu数据记录早于vi数据，丢弃vi的前30帧
	rk_media_ctx.debug_frame_cnt++;
	if (rk_media_ctx.debug_frame_cnt < 30) {
		LOG_INFO("drop gdc frame cnt %d\n", rk_media_ctx.debug_frame_cnt);
		RK_MPI_MB_ReleaseMB(pInfo->pMbBlk);
		return GDC_INFO_CB_SUCCESS;
	}

	sprintf(gdc_vi_info_buffer, "%" PRId64 " %d %d %d %d %d %lf\n", pInfo->u64ExtraPts,
	        pInfo->u32RsSkew, pInfo->u32ExpTime, pInfo->u32Again, pInfo->u32Dgain,
	        pInfo->u32Ispgain, pInfo->dIso);
	fwrite(gdc_vi_info_buffer, sizeof(char), strlen(gdc_vi_info_buffer),
	       rk_media_ctx.vi_debug_file);
	fflush(rk_media_ctx.vi_debug_file);
	memset(&gdc_frame, 0, sizeof(VIDEO_FRAME_INFO_S));
	gdc_frame.stVFrame.pMbBlk = pInfo->pMbBlk;
	gdc_frame.stVFrame.u32Width = pInfo->u32Width;
	gdc_frame.stVFrame.u32Height = pInfo->u32Height;
	gdc_frame.stVFrame.u32VirWidth = pInfo->u32VirWidth;
	gdc_frame.stVFrame.u32VirHeight = pInfo->u32VirHeight;
	gdc_frame.stVFrame.enPixelFormat = pInfo->enPixelFormat;
	gdc_frame.stVFrame.enCompressMode = pInfo->enCompressMode;
	gdc_frame.stVFrame.u32TimeRef = pInfo->u32Seq;
	gdc_frame.stVFrame.u64PTS = pInfo->u64PTS;
	RK_MPI_VENC_SendFrame(VENC_MAIN_CHN, &gdc_frame, 1000);
	RK_MPI_MB_ReleaseMB(pInfo->pMbBlk);

	return GDC_INFO_CB_SUCCESS;
}

static void *normal_video_loop(void *arg) {
	VENC_STREAM_S video_stream;
	int loop_count = 0;
	int ret = 0;
	int cal_fps_step = 90;
	int64_t total_time = 0, total_cost_time = 0, total_write_time = 0;
	int64_t packet_cnt = 0;
	FILE *fp = fopen("/tmp/venc.h265", "wb");
	video_stream.pstPack = malloc(sizeof(VENC_PACK_S));

	LOG_INFO("enter\n");
	prctl(PR_SET_NAME, __func__, 0, 0, 0);
	rk_media_ctx.start_record_pts = 0;

	total_time = rkipc_get_curren_time_ms();
	while (rk_media_ctx.record_thread_running) {
		ret = RK_MPI_VENC_GetStream(VENC_MAIN_CHN, &video_stream, 1000);
		if (ret == RK_SUCCESS) {
			if (!rk_media_ctx.start_record_pts)
				rk_media_ctx.start_record_pts = video_stream.pstPack->u64PTS;
			void *data = RK_MPI_MB_Handle2VirAddr(video_stream.pstPack->pMbBlk);
			if (rk_mode_param.enable_eis_debug) {
				fwrite(data, 1, video_stream.pstPack->u32Len, fp);
				fflush(fp);
			}

			if (rk_mode_param.enable_rtsp) {
				rkipc_rtsp_write_video_frame(STREAM_ID_FOR_VIDEO, data,
				                             video_stream.pstPack->u32Len,
				                             video_stream.pstPack->u64PTS);
			} else {
				if ((video_stream.pstPack->DataType.enH264EType == H264E_NALU_IDRSLICE) ||
				    (video_stream.pstPack->DataType.enH264EType == H264E_NALU_ISLICE) ||
				    (video_stream.pstPack->DataType.enH265EType == H265E_NALU_IDRSLICE) ||
				    (video_stream.pstPack->DataType.enH265EType == H265E_NALU_ISLICE)) {
					rk_storage_write_video_frame(STREAM_ID_FOR_VIDEO, data,
					                             video_stream.pstPack->u32Len,
					                             video_stream.pstPack->u64PTS, 1);
				} else {
					rk_storage_write_video_frame(STREAM_ID_FOR_VIDEO, data,
					                             video_stream.pstPack->u32Len,
					                             video_stream.pstPack->u64PTS, 0);
				}
			}
			packet_cnt++;
			if ((packet_cnt % cal_fps_step) == 0) {
				total_time = rkipc_get_curren_time_ms() - total_time;
				float real_frame_interval = (float)total_time / (float)packet_cnt;
				float real_fps = 1000.0f / real_frame_interval;
				LOG_DEBUG("total_time %" PRId64 " ms, packet_cnt %" PRId64 ", real fps %.2f\n",
				          total_time, packet_cnt, real_fps);
				total_time = rkipc_get_curren_time_ms();
				packet_cnt = 0;
			}
			LOG_DEBUG("Count:%d, Len:%d, PTS is %" PRId64 ", enH264EType is %d, ref type %d\n",
			          loop_count, video_stream.pstPack->u32Len, video_stream.pstPack->u64PTS,
			          video_stream.pstPack->DataType.enH265EType,
			          video_stream.stH265Info.enRefType);

			ret = RK_MPI_VENC_ReleaseStream(VENC_MAIN_CHN, &video_stream);
			if (ret != RK_SUCCESS) {
				LOG_ERROR("RK_MPI_VENC_ReleaseStream fail %x\n", ret);
			}
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

#define OSD_TEXT_LEN 128
typedef struct {
	struct list_head list;
	ae_api_queryInfo_t ae_info;
	int64_t pts;
} ae_info_list_t;

static void *osd_update_loop(void *arg) {
	int ret = 0;
	int osd_width = rk_mode_param.video_width;
	int osd_height = 64;
	int osd_buf_size = osd_width * osd_height * 4; // BGRA8888 4byte
	// EIS would hold 11 yuv frames, so we also hold AE info in a list, to
	// better align AE information with the actual image
	int delay_show_loop = rk_mode_param.eis_mode == RK_EIS_OFF ? 0 : 9;
	wchar_t osd_text_buff[OSD_TEXT_LEN] = {};
	char *osd_draw_buff = NULL;
	void *aiq_ctx = rk_isp_get_ctx(0);
	// ae_api_queryInfo_t ae_info = {};
	BITMAP_S bit_map = {};
	LIST_HEAD(ae_info_list);

	prctl(PR_SET_NAME, __func__, 0, 0, 0);
	LOG_INFO("enter\n");
	osd_draw_buff = malloc(osd_buf_size);
	if (!osd_draw_buff) {
		LOG_ERROR("Failed to allocate memory for osd_draw_buff\n");
		return NULL;
	}
	memset(osd_draw_buff, 0, osd_buf_size);
	while (rk_media_ctx.osd_thread_running) {
		int64_t before_time = rkipc_get_curren_time_ms();
		ae_info_list_t *ae_info_node = NULL;
		ae_api_queryInfo_t *ae_info = NULL;
		struct timespec ts;
		memset(osd_text_buff, 0x00, sizeof(osd_text_buff));
		memset(osd_draw_buff, 0x00, osd_buf_size);

		if (!rk_media_ctx.start_record_pts) {
			goto __loop_sleep;
		}

		ae_info_node = malloc(sizeof(ae_info_list_t));
		if (!ae_info_node) {
			LOG_ERROR("Failed to allocate memory for ae_info_node\n");
			goto __loop_sleep;
		}
		memset(ae_info_node, 0, sizeof(ae_info_list_t));
		clock_gettime(CLOCK_BOOTTIME, &ts);
		ae_info_node->pts = (int64_t)ts.tv_sec * 1000000 + (int64_t)ts.tv_nsec / 1000;
		LOG_DEBUG("ae_info_node->pts is %" PRId64 ", start_record_pts is %" PRId64 "\n",
		          ae_info_node->pts, rk_media_ctx.start_record_pts);
		ae_info_node->pts = (ae_info_node->pts - rk_media_ctx.start_record_pts) / 1000;

		ret = rk_aiq_user_api2_ae_queryExpResInfo(aiq_ctx, &ae_info_node->ae_info);
		if (ret != 0) {
			LOG_ERROR("rk_aiq_user_api2_ae_queryExpResInfo failed %x\n", ret);
			free(ae_info_node);
			goto __loop_sleep;
		}
		list_add_tail(&ae_info_node->list, &ae_info_list);

		if (delay_show_loop > 0) {
			delay_show_loop--;
			goto __loop_sleep;
		}

		if (list_empty(&ae_info_list)) {
			LOG_ERROR("ae_info_list is empty\n");
			goto __loop_sleep;
		}
		ae_info_node = list_first_entry(&ae_info_list, ae_info_list_t, list);
		list_del(&ae_info_node->list);

		ae_info = &ae_info_node->ae_info;
		swprintf(osd_text_buff, OSD_TEXT_LEN,
		         L"exptime:%.4f again:%.3f dgain:%.3f isp_dgain:%.3f ae_info_ts:%" PRId64 "ms",
		         ae_info->linExpInfo.expParam.integration_time,
		         ae_info->linExpInfo.expParam.analog_gain,
		         ae_info->linExpInfo.expParam.digital_gain, ae_info->linExpInfo.expParam.isp_dgain,
		         ae_info_node->pts);
		free(ae_info_node);
		LOG_DEBUG("osd text: %ls\n", osd_text_buff);
		draw_argb8888_text(osd_draw_buff, osd_width, osd_height, osd_text_buff);
		bit_map.u32Width = osd_width;
		bit_map.u32Height = osd_height;
		bit_map.pData = osd_draw_buff;
		bit_map.enPixelFormat = RK_FMT_ARGB8888;
		ret = RK_MPI_RGN_SetBitMap(RGN_CHN, &bit_map);
		if (ret != RK_SUCCESS) {
			LOG_ERROR("RK_MPI_RGN_SetBitMap failed %x\n", ret);
		}
	__loop_sleep:
		int64_t cost_time = rkipc_get_curren_time_ms() - before_time;
		if (cost_time < 33) {
			usleep((33 - cost_time) * 1000); // sleep to make osd update 30fps
			LOG_DEBUG("osd update cost time is %" PRId64 "ms, sleep %" PRId64 "ms\n", cost_time,
			          (33 - cost_time));
		} else {
			LOG_ERROR("osd update cost time is %" PRId64 "ms, too long!\n", cost_time);
		}
	}
	ae_info_list_t *ae_info_node = NULL, *tmp = NULL;
	list_for_each_entry_safe(ae_info_node, tmp, &ae_info_list, list) {
		list_del(&ae_info_node->list);
		free(ae_info_node);
	}
	free(osd_draw_buff);
	LOG_INFO("exit\n");
	return NULL;
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
	if (rk_mode_param.eis_mode != RK_EIS_OFF && !rk_mode_param.enable_eis_debug) {
		vi_chn_attr.enVideoFormat = VIDEO_FORMAT_TILE_4x4;
		vi_chn_attr.enCompressMode = COMPRESS_MODE_NONE;
	} else {
		vi_chn_attr.enVideoFormat = VIDEO_FORMAT_LINEAR;
		vi_chn_attr.enCompressMode = COMPRESS_RFBC_64x4;
	}
	vi_chn_attr.enPixelFormat = RK_FMT_YUV420SP;
	vi_chn_attr.u32Depth = 0;
	vi_chn_attr.stIspOpt.bAttchFrmInfo = true;
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
	if (rk_mode_param.eis_mode != RK_EIS_OFF)
		vi_chn_attr.stIspOpt.bAttchFrmInfo = true;
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

static int gdc_display_init(void) {
	int ret = 0;
	int display_width = rk_mode_param.prev_width;
	int display_height = rk_mode_param.prev_height;
	int rotation = rk_mode_param.display_rotation;
	long long before_time = rkipc_get_curren_time_ms();
	GDC_CHN_ATTR_S gdc_attr = {0};
	if (rotation == ROTATION_90 || rotation == ROTATION_270) {
		int tmp = display_width;
		display_width = display_height;
		display_height = tmp;
	}
	if (rk_mode_param.eis_mode == RK_EIS_OFF || !rk_mode_param.enable_display) {
		LOG_DEBUG("EIS is off, no need to init GDC\n");
		return ret;
	}
	gdc_attr.u32MaxInQueue = 4;
	gdc_attr.u32MaxOutQueue = 4;
	gdc_attr.s32DstWidth = display_width;
	gdc_attr.s32DstHeight = display_height;
	gdc_attr.enDstPixelFormat = RK_FMT_YUV420SP;
	gdc_attr.enDstCompMode = COMPRESS_MODE_NONE;
	gdc_attr.stEisAttr.s32DevNo = IIO_IMU_DEVICE_NO;
	memset(gdc_attr.cfgFile, 0, sizeof(gdc_attr.cfgFile));
	if (rk_mode_param.eis_mode == RK_HORIZON_STEADY) {
#ifdef BUILDROOT
		snprintf(gdc_attr.cfgFile, sizeof(gdc_attr.cfgFile),
		         "/usr/share/rkeis_config/%s_horizon_%dx%d_%dx%d_30fps.json"
				 , rk_param_get_string("sensor:name", NULL)
				 , display_width, display_height, display_width, display_height);
#else
		snprintf(gdc_attr.cfgFile, sizeof(gdc_attr.cfgFile),
		         "/oem/usr/share/rkeis_config/%s_horizon_%dx%d_%dx%d_30fps.json"
				 , rk_param_get_string("sensor:name", NULL)
				 , display_width, display_height, display_width, display_height);
#endif
	} else if (rk_mode_param.eis_mode == RK_DISTORTION_CORRECTION) {
		LOG_ERROR("ERROR: distortion correction is not support now!\n");
		return -1;
	} else {
#ifdef BUILDROOT
		snprintf(gdc_attr.cfgFile, sizeof(gdc_attr.cfgFile),
		         "/usr/share/rkeis_config/%s_normal_%dx%d_%dx%d_30fps.json"
				 , rk_param_get_string("sensor:name", NULL)
				 , display_width, display_height, display_width, display_height);
#else
		snprintf(gdc_attr.cfgFile, sizeof(gdc_attr.cfgFile),
		         "/oem/usr/share/rkeis_config/%s_normal_%dx%d_%dx%d_30fps.json"
				 , rk_param_get_string("sensor:name", NULL)
				 , display_width, display_height, display_width, display_height);
#endif
	}
	ret = RK_MPI_GDC_CreateChn(GDC_DISPLAY_CHN, &gdc_attr);
	if (ret) {
		LOG_ERROR("ERROR: create GDC display channel error! ret=%#X\n", ret);
		return ret;
	}

	LOG_INFO("gdc init success, cost time %lld ms\n", rkipc_get_curren_time_ms() - before_time);
	return ret;
}

static int gdc_display_deinit(void) {
	int ret = 0;
	long long before_time = rkipc_get_curren_time_ms();
	if (rk_mode_param.eis_mode == RK_EIS_OFF || !rk_mode_param.enable_display) {
		LOG_DEBUG("EIS is off, no need to deinit GDC\n");
		return ret;
	}
	ret = RK_MPI_GDC_DestroyChn(GDC_DISPLAY_CHN);
	if (ret)
		LOG_ERROR("ERROR: destroy GDC display channel error! ret=%#X\n", ret);
	LOG_INFO("gdc deinit cost time %lld ms\n", rkipc_get_curren_time_ms() - before_time);
	return ret;
}

static int gdc_record_init(void) {
	int ret = 0;
	int video_width = rk_mode_param.video_width;
	int video_height = rk_mode_param.video_height;
	int sensor_width = rk_mode_param.sensor_width;
	int sensor_height = rk_mode_param.sensor_height;
	long long before_time = rkipc_get_curren_time_ms();
	GDC_CHN_ATTR_S gdc_attr = {0};
	if (rk_mode_param.eis_mode == RK_EIS_OFF) {
		LOG_DEBUG("EIS is off, no need to init GDC\n");
		return ret;
	}
	gdc_attr.u32MaxInQueue = rk_mode_param.vi_output_buf_cnt;
	gdc_attr.u32MaxOutQueue = rk_mode_param.gdc_output_buf_cnt;
	gdc_attr.s32DstWidth = video_width;
	gdc_attr.s32DstHeight = video_height;
	gdc_attr.s32Depth = 0;
	gdc_attr.enDstCompMode = COMPRESS_RFBC_64x4;
	gdc_attr.enDstPixelFormat = RK_FMT_YUV420SP;
	gdc_attr.enDstVideoFormat = VIDEO_FORMAT_LINEAR;
	gdc_attr.stEisAttr.s32DevNo = IIO_IMU_DEVICE_NO;
	memset(gdc_attr.cfgFile, 0, sizeof(gdc_attr.cfgFile));
	if (rk_mode_param.eis_mode == RK_HORIZON_STEADY) {
#ifdef BUILDROOT
		snprintf(gdc_attr.cfgFile, sizeof(gdc_attr.cfgFile),
		         "/usr/share/rkeis_config/%s_horizon_%dx%d_%dx%d_30fps.json"
				 , rk_param_get_string("sensor:name", NULL)
				 , sensor_width, sensor_height, video_width, video_height);
#else
		snprintf(gdc_attr.cfgFile, sizeof(gdc_attr.cfgFile),
		         "/oem/usr/share/rkeis_config/%s_horizon_%dx%d_%dx%d_30fps.json"
				 , rk_param_get_string("sensor:name", NULL)
				 , sensor_width, sensor_height, video_width, video_height);
#endif
	} else if (rk_mode_param.eis_mode == RK_DISTORTION_CORRECTION) {
		LOG_ERROR("ERROR: distortion correction is not support now!\n");
		return -1;
	} else {
#ifdef BUILDROOT
		snprintf(gdc_attr.cfgFile, sizeof(gdc_attr.cfgFile),
		         "/usr/share/rkeis_config/%s_normal_%dx%d_%dx%d_30fps.json"
				 , rk_param_get_string("sensor:name", NULL)
				 , sensor_width, sensor_height, video_width, video_height);
#else
		snprintf(gdc_attr.cfgFile, sizeof(gdc_attr.cfgFile),
		         "/oem/usr/share/rkeis_config/%s_normal_%dx%d_%dx%d_30fps.json"
				 , rk_param_get_string("sensor:name", NULL)
				 , sensor_width, sensor_height, video_width, video_height);
#endif
	}
	if (rk_mode_param.enable_eis_debug) {
		GDC_INFO_CB_S gdc_info_cb = {};
		rk_media_ctx.vi_debug_file = fopen("/tmp/vi_info.txt", "w");
		rk_media_ctx.imu_debug_file = fopen("/tmp/imu_info.txt", "w");
		gdc_info_cb.pfnSensorCB = gdc_sensor_callback;
		gdc_info_cb.pfnVframeCB = gdc_frame_callback;
		RK_MPI_GDC_Register_InfoCB(GDC_MAIN_CHN, NULL, &gdc_info_cb);
	}
	ret = RK_MPI_GDC_CreateChn(GDC_MAIN_CHN, &gdc_attr);
	if (ret) {
		LOG_ERROR("ERROR: create GDC main channel error! ret=%#X\n", ret);
		return ret;
	}
	LOG_INFO("gdc init success, cost time %lld ms\n", rkipc_get_curren_time_ms() - before_time);
	return ret;
}

static int gdc_record_deinit(void) {
	int ret = 0;
	long long before_time = rkipc_get_curren_time_ms();
	if (rk_mode_param.eis_mode == RK_EIS_OFF) {
		LOG_DEBUG("EIS is off, no need to deinit GDC\n");
		return ret;
	}
	ret = RK_MPI_GDC_DestroyChn(GDC_MAIN_CHN);
	if (ret)
		LOG_ERROR("ERROR: destroy GDC main channel error! ret=%#X\n", ret);
	if (rk_mode_param.enable_eis_debug) {
		fclose(rk_media_ctx.vi_debug_file);
		fclose(rk_media_ctx.imu_debug_file);
	}
	LOG_INFO("gdc deinit cost time %lld ms\n", rkipc_get_curren_time_ms() - before_time);
	return ret;
}

static int gdc_dummy_init(void) {
	int ret = 0;
	int width = 240;  // unused
	int height = 240; // unused
	GDC_CHN_ATTR_S gdc_attr = {0};
	if (!rk_mode_param.enable_smart_ae && !rk_mode_param.enable_memc) {
		LOG_DEBUG("smart ae is off, no need to init dummy GDC\n");
		return ret;
	}
	gdc_attr.u32MaxInQueue = 1;
	gdc_attr.u32MaxOutQueue = 1;
	gdc_attr.s32DstWidth = width;
	gdc_attr.s32DstHeight = height;
	gdc_attr.enDstPixelFormat = RK_FMT_YUV420SP;
	gdc_attr.enDstCompMode = COMPRESS_MODE_NONE;
	gdc_attr.stEisAttr.s32DevNo = IIO_IMU_DEVICE_NO;
	memset(gdc_attr.cfgFile, 0, sizeof(gdc_attr.cfgFile));
	GDC_INFO_CB_S gdc_info_cb = {};
	gdc_info_cb.pfnSensorCB = gdc_aiq_imu_callback;
	gdc_info_cb.pfnVframeCB = gdc_aiq_frame_callback;
	RK_MPI_GDC_Register_InfoCB(GDC_DUMMY_CHN, NULL, &gdc_info_cb);
	ret = RK_MPI_GDC_CreateChn(GDC_DUMMY_CHN, &gdc_attr);
	if (ret) {
		LOG_ERROR("ERROR: create GDC aiq channel error! ret=%#X\n", ret);
		return ret;
	}
	LOG_INFO("gdc dummy create success\n");
	return ret;
}

static int gdc_dummy_deinit(void) {
	int ret = 0;
	if (!rk_mode_param.enable_smart_ae && !rk_mode_param.enable_memc) {
		LOG_DEBUG("smart ae is off, no need to deinit dummy GDC\n");
		return ret;
	}
	ret = RK_MPI_GDC_DestroyChn(GDC_DUMMY_CHN);
	if (ret)
		LOG_ERROR("ERROR: destroy GDC aiq channel error! ret=%#X\n", ret);
	return ret;
}

static int venc_init(void) {
	VENC_CHN_ATTR_S venc_chn_attr;
	int ret = 0;
	int video_width = rk_mode_param.video_width;
	int video_height = rk_mode_param.video_height;
	const char *tmp_output_data_type = rk_param_get_string("video.0:output_data_type", NULL);
	const char *tmp_rc_mode = rk_param_get_string("video.0:rc_mode", NULL);
	const char *tmp_h264_profile = rk_param_get_string("video.0:h264_profile", NULL);

	if (rk_mode_param.eis_mode != RK_EIS_OFF && !rk_mode_param.enable_eis_debug) {
		video_width = rk_mode_param.video_width;
		video_height = rk_mode_param.video_height;
	} else {
		video_width = rk_mode_param.sensor_width;
		video_height = rk_mode_param.sensor_height;
	}
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
			venc_chn_attr.stRcAttr.stH264Cbr.u32Gop = rk_param_get_int("video.0:gop", -1);
			venc_chn_attr.stRcAttr.stH264Cbr.u32BitRate = rk_param_get_int("video.0:max_rate", 0);
			venc_chn_attr.stRcAttr.stH264Cbr.fr32DstFrameRateDen =
			    rk_param_get_int("video.0:dst_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH264Cbr.fr32DstFrameRateNum =
			    rk_param_get_int("video.0:dst_frame_rate_num", -1);
			venc_chn_attr.stRcAttr.stH264Cbr.u32SrcFrameRateDen =
			    rk_param_get_int("video.0:src_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH264Cbr.u32SrcFrameRateNum =
			    rk_param_get_int("video.0:src_frame_rate_num", -1);
		} else {
			venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H264VBR;
			venc_chn_attr.stRcAttr.stH264Vbr.u32Gop = rk_param_get_int("video.0:gop", -1);
			venc_chn_attr.stRcAttr.stH264Vbr.u32BitRate = rk_param_get_int("video.0:max_rate", 0);
			venc_chn_attr.stRcAttr.stH264Vbr.fr32DstFrameRateDen =
			    rk_param_get_int("video.0:dst_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH264Vbr.fr32DstFrameRateNum =
			    rk_param_get_int("video.0:dst_frame_rate_num", -1);
			venc_chn_attr.stRcAttr.stH264Vbr.u32SrcFrameRateDen =
			    rk_param_get_int("video.0:src_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH264Vbr.u32SrcFrameRateNum =
			    rk_param_get_int("video.0:src_frame_rate_num", -1);
		}
	} else if (!strcmp(tmp_output_data_type, "H.265")) {
		venc_chn_attr.stVencAttr.enType = RK_VIDEO_ID_HEVC;
		if (!strcmp(tmp_rc_mode, "CBR")) {
			venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H265CBR;
			venc_chn_attr.stRcAttr.stH265Cbr.u32Gop = rk_param_get_int("video.0:gop", -1);
			venc_chn_attr.stRcAttr.stH265Cbr.u32BitRate = rk_param_get_int("video.0:max_rate", 0);
			venc_chn_attr.stRcAttr.stH265Cbr.fr32DstFrameRateDen =
			    rk_param_get_int("video.0:dst_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH265Cbr.fr32DstFrameRateNum =
			    rk_param_get_int("video.0:dst_frame_rate_num", -1);
			venc_chn_attr.stRcAttr.stH265Cbr.u32SrcFrameRateDen =
			    rk_param_get_int("video.0:src_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH265Cbr.u32SrcFrameRateNum =
			    rk_param_get_int("video.0:src_frame_rate_num", -1);
		} else {
			venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H265VBR;
			venc_chn_attr.stRcAttr.stH265Vbr.u32Gop = rk_param_get_int("video.0:gop", -1);
			venc_chn_attr.stRcAttr.stH265Vbr.u32BitRate = rk_param_get_int("video.0:max_rate", 0);
			venc_chn_attr.stRcAttr.stH265Vbr.fr32DstFrameRateDen =
			    rk_param_get_int("video.0:dst_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH265Vbr.fr32DstFrameRateNum =
			    rk_param_get_int("video.0:dst_frame_rate_num", -1);
			venc_chn_attr.stRcAttr.stH265Vbr.u32SrcFrameRateDen =
			    rk_param_get_int("video.0:src_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH265Vbr.u32SrcFrameRateNum =
			    rk_param_get_int("video.0:src_frame_rate_num", -1);
		}
	} else {
		LOG_ERROR("tmp_output_data_type is %s, not support\n", tmp_output_data_type);
		return -1;
	}

	const char *tmp_smart = rk_param_get_string("video.0:smart", NULL);
	const char *tmp_gop_mode = rk_param_get_string("video.0:gop_mode", NULL);
	if (!strcmp(tmp_gop_mode, "normalP")) {
		venc_chn_attr.stGopAttr.enGopMode = VENC_GOPMODE_NORMALP;
	} else if (!strcmp(tmp_gop_mode, "smartP")) {
		venc_chn_attr.stGopAttr.enGopMode = VENC_GOPMODE_SMARTP;
		venc_chn_attr.stGopAttr.s32VirIdrLen = rk_param_get_int("video.0:smartp_viridrlen", 25);
	} else if (!strcmp(tmp_gop_mode, "TSVC4")) {
		venc_chn_attr.stGopAttr.enGopMode = VENC_GOPMODE_TSVC4;
	}
	// venc_chn_attr.stGopAttr.u32GopSize = rk_param_get_int("video.0:gop", -1);

	venc_chn_attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
	venc_chn_attr.stVencAttr.u32PicWidth = video_width;
	venc_chn_attr.stVencAttr.u32PicHeight = video_height;
	venc_chn_attr.stVencAttr.u32VirWidth = video_width;
	venc_chn_attr.stVencAttr.u32VirHeight = video_height;
	venc_chn_attr.stVencAttr.u32StreamBufCnt = rk_param_get_int("video.0:buffer_count", 5);
	venc_chn_attr.stVencAttr.u32BufSize = rk_param_get_int("video.0:buffer_size", 6291456);
	// venc_chn_attr.stVencAttr.u32Depth = 1;
	ret = RK_MPI_VENC_CreateChn(VENC_MAIN_CHN, &venc_chn_attr);
	if (ret) {
		LOG_ERROR("ERROR: create VENC error! ret=%d\n", ret);
		return -1;
	}

	const char *tmp_rc_quality = rk_param_get_string("video.0:rc_quality", NULL);
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

static int rgn_init(void) {
	int ret = 0;
	int font_size = rk_param_get_int("osd.common:font_size", 0);
	const char *font_path = rk_param_get_string("osd.common:font_path", NULL);
	RGN_ATTR_S rgn_attr = {};
	MPP_CHN_S mpp_chn = {};
	RGN_CHN_ATTR_S rgn_chn_attr = {};

	if (!rk_mode_param.enable_osd) {
		LOG_DEBUG("osd is off, no need to init rgn\n");
		return 0;
	}
	ret = create_font(font_path, font_size);
	if (ret != 0) {
		LOG_ERROR("create font failed %#X\n", ret);
		return ret;
	}
	set_font_color(0xfff799);
	// create overlay regions
	rgn_attr.enType = OVERLAY_RGN;
	rgn_attr.unAttr.stOverlay.enPixelFmt = RK_FMT_ARGB8888;
	rgn_attr.unAttr.stOverlay.u32CanvasNum = 3;
	rgn_attr.unAttr.stOverlay.stSize.u32Width = rk_mode_param.video_width;
	rgn_attr.unAttr.stOverlay.stSize.u32Height = 64;
	ret = RK_MPI_RGN_Create(RGN_CHN, &rgn_attr);
	if (RK_SUCCESS != ret) {
		LOG_ERROR("RK_MPI_RGN_Create (%d) failed with %#x\n", RGN_CHN, ret);
		RK_MPI_RGN_Destroy(RGN_CHN);
		return RK_FAILURE;
	}
	LOG_DEBUG("The handle: %d, create success\n", RGN_CHN);

	mpp_chn.enModId = RK_ID_VENC;
	mpp_chn.s32DevId = 0;
	mpp_chn.s32ChnId = VENC_MAIN_CHN;
	rgn_chn_attr.bShow = RK_TRUE;
	rgn_chn_attr.enType = OVERLAY_RGN;
	rgn_chn_attr.unChnAttr.stOverlayChn.stPoint.s32X = 0;
	rgn_chn_attr.unChnAttr.stOverlayChn.stPoint.s32Y = 0;
	rgn_chn_attr.unChnAttr.stOverlayChn.u32BgAlpha = 64;
	rgn_chn_attr.unChnAttr.stOverlayChn.u32FgAlpha = 64;
	// 1126 osd alpha not support multi layer, so use layer 0 to draw nn
	rgn_chn_attr.unChnAttr.stOverlayChn.u32Layer = 1;

	ret = RK_MPI_RGN_AttachToChn(RGN_CHN, &mpp_chn, &rgn_chn_attr);
	if (RK_SUCCESS != ret)
		LOG_ERROR("RK_MPI_RGN_AttachToChn (%d) to venc0 failed with %#x\n", RGN_CHN, ret);
	else
		LOG_DEBUG("RK_MPI_RGN_AttachToChn to venc0 success\n");
	rk_media_ctx.osd_thread_running = true;
	pthread_create(&rk_media_ctx.osd_thread, NULL, osd_update_loop, NULL);
	LOG_INFO("rgn init success\n");
	return ret;
}

static int rgn_deinit(void) {
	int ret = 0;
	MPP_CHN_S mpp_chn;
	mpp_chn.enModId = RK_ID_VENC;
	mpp_chn.s32DevId = 0;
	mpp_chn.s32ChnId = VENC_MAIN_CHN;
	if (!rk_mode_param.enable_osd) {
		LOG_DEBUG("osd is off, no need to deinit rgn\n");
		return 0;
	}
	rk_media_ctx.osd_thread_running = false;
	pthread_join(rk_media_ctx.osd_thread, NULL);
	ret = RK_MPI_RGN_DetachFromChn(RGN_CHN, &mpp_chn);
	if (RK_SUCCESS != ret) {
		LOG_ERROR("RK_MPI_RGN_DetachFromChn (%d) to venc0 failed with %#x\n", RGN_CHN, ret);
		return RK_FAILURE;
	}
	ret = RK_MPI_RGN_Destroy(RGN_CHN);
	if (RK_SUCCESS != ret) {
		LOG_ERROR("RK_MPI_RGN_Destroy (%d) failed with %#x\n", RGN_CHN, ret);
		return RK_FAILURE;
	}
	destroy_font();
	LOG_INFO("rgn deinit success\n");
	return ret;
}

static int bind_display_pipe(void) {
	MPP_CHN_S vi_display_chn;
	MPP_CHN_S vo_chn;
	MPP_CHN_S gdc_display_chn;
	int ret = 0;
	int delay_ms = rk_param_get_int("video.source:delay_ms", 0);
	LOG_INFO("enter\n");
	vi_display_chn.enModId = RK_ID_VI;
	vi_display_chn.s32DevId = VI_DEV_ID;
	vi_display_chn.s32ChnId = VI_DISPLAY_CHN;
	vo_chn.enModId = RK_ID_VO;
	vo_chn.s32DevId = VO_PREV_LAYER;
	vo_chn.s32ChnId = VO_PREV_CHN;
	gdc_display_chn.enModId = RK_ID_GDC;
	gdc_display_chn.s32DevId = 0;
	gdc_display_chn.s32ChnId = GDC_DISPLAY_CHN;
	// TODO: wait for imu data ready, do this in gdc is better
	if (delay_ms > 0 && rk_mode_param.eis_mode != RK_EIS_OFF) {
		LOG_INFO("delay %d ms\n", delay_ms);
		usleep(delay_ms * 1000);
	}

	if (rk_mode_param.enable_display) {
		if (rk_mode_param.eis_mode == RK_EIS_OFF) {
			ret = RK_MPI_SYS_Bind(&vi_display_chn, &vo_chn);
			if (ret != RK_SUCCESS) {
				LOG_ERROR("vi display chn bind vo chn fail:%x\n", ret);
				return ret;
			}
		} else {
			ret = RK_MPI_SYS_Bind(&vi_display_chn, &gdc_display_chn);
			if (ret != RK_SUCCESS) {
				LOG_ERROR("vi display chn bind gdc display chn fail:%x\n", ret);
				return ret;
			}
			ret = RK_MPI_SYS_Bind(&gdc_display_chn, &vo_chn);
			if (ret != RK_SUCCESS) {
				LOG_ERROR("gdc display chn bind vo chn fail:%x\n", ret);
				return ret;
			}
		}
	}
	LOG_INFO("exit\n");
	return ret;
}

static int unbind_display_pipe(void) {
	MPP_CHN_S vi_display_chn;
	MPP_CHN_S vo_chn;
	MPP_CHN_S gdc_display_chn;
	int ret = 0;
	LOG_INFO("enter\n");
	vi_display_chn.enModId = RK_ID_VI;
	vi_display_chn.s32DevId = VI_DEV_ID;
	vi_display_chn.s32ChnId = VI_DISPLAY_CHN;
	vo_chn.enModId = RK_ID_VO;
	vo_chn.s32DevId = rk_param_get_int("display:layer_id", 5);
	vo_chn.s32ChnId = rk_param_get_int("display:video_chn_id", 0);
	gdc_display_chn.enModId = RK_ID_GDC;
	gdc_display_chn.s32DevId = 0;
	gdc_display_chn.s32ChnId = GDC_DISPLAY_CHN;

	if (rk_mode_param.enable_display) {
		if (rk_mode_param.eis_mode == RK_EIS_OFF) {
			ret = RK_MPI_SYS_UnBind(&vi_display_chn, &vo_chn);
			if (ret != RK_SUCCESS)
				LOG_ERROR("vi display chn unbind vo chn fail:%x\n", ret);
		} else {
			ret = RK_MPI_SYS_UnBind(&gdc_display_chn, &vo_chn);
			if (ret != RK_SUCCESS)
				LOG_ERROR("gdc display chn unbind vo chn fail:%x\n", ret);
			ret = RK_MPI_SYS_UnBind(&vi_display_chn, &gdc_display_chn);
			if (ret != RK_SUCCESS)
				LOG_ERROR("vi display chn unbind gdc display chn fail:%x\n", ret);
		}
	}
	LOG_INFO("exit\n");
	return ret;
}

static int bind_record_pipe(void) {
	MPP_CHN_S vi_main_chn;
	MPP_CHN_S venc_chn;
	MPP_CHN_S gdc_main_chn;
	int ret = 0;
	int delay_ms = rk_param_get_int("video.source:delay_ms", 0);
	LOG_INFO("enter\n");
	vi_main_chn.enModId = RK_ID_VI;
	vi_main_chn.s32DevId = VI_DEV_ID;
	vi_main_chn.s32ChnId = VI_MAIN_CHN;
	gdc_main_chn.enModId = RK_ID_GDC;
	gdc_main_chn.s32DevId = 0;
	gdc_main_chn.s32ChnId = GDC_MAIN_CHN;
	venc_chn.enModId = RK_ID_VENC;
	venc_chn.s32DevId = 0;
	venc_chn.s32ChnId = VENC_MAIN_CHN;

	if (delay_ms > 0 && rk_mode_param.eis_mode != RK_EIS_OFF) {
		LOG_INFO("delay %d ms\n", delay_ms);
		usleep(delay_ms * 1000);
	}
	if (rk_mode_param.eis_mode == RK_EIS_OFF) {
		ret = RK_MPI_SYS_Bind(&vi_main_chn, &venc_chn);
		if (ret != RK_SUCCESS) {
			LOG_ERROR("vi main chn bind venc chn fail:%x\n", ret);
			return ret;
		}
	} else {
		ret = RK_MPI_SYS_Bind(&vi_main_chn, &gdc_main_chn);
		if (ret != RK_SUCCESS) {
			LOG_ERROR("vi main chn bind gdc main chn fail:%x\n", ret);
			return ret;
		}
		if (rk_mode_param.enable_eis_debug) {
			LOG_DEBUG("enable eis debug, no need to bind venc\n");
			return ret;
		}
		ret = RK_MPI_SYS_Bind(&gdc_main_chn, &venc_chn);
		if (ret != RK_SUCCESS) {
			LOG_ERROR("gdc main chn bind venc chn fail:%x\n", ret);
			return ret;
		}
	}
	LOG_INFO("exit\n");
	return ret;
}

static int unbind_record_pipe(void) {
	MPP_CHN_S vi_main_chn;
	MPP_CHN_S venc_chn;
	MPP_CHN_S gdc_main_chn;
	int ret = 0;
	LOG_INFO("enter\n");
	vi_main_chn.enModId = RK_ID_VI;
	vi_main_chn.s32DevId = VI_DEV_ID;
	vi_main_chn.s32ChnId = VI_MAIN_CHN;
	gdc_main_chn.enModId = RK_ID_GDC;
	gdc_main_chn.s32DevId = 0;
	gdc_main_chn.s32ChnId = GDC_MAIN_CHN;
	venc_chn.enModId = RK_ID_VENC;
	venc_chn.s32DevId = 0;
	venc_chn.s32ChnId = VENC_MAIN_CHN;

	if (rk_mode_param.eis_mode == RK_EIS_OFF) {
		ret = RK_MPI_SYS_UnBind(&vi_main_chn, &venc_chn);
		if (ret != RK_SUCCESS)
			LOG_ERROR("vi main chn unbind venc chn fail:%x\n", ret);
	} else {
		if (!rk_mode_param.enable_eis_debug) {
			ret = RK_MPI_SYS_UnBind(&gdc_main_chn, &venc_chn);
			if (ret != RK_SUCCESS)
				LOG_ERROR("gdc main chn unbind venc chn fail:%x\n", ret);
		}
		ret = RK_MPI_SYS_UnBind(&vi_main_chn, &gdc_main_chn);
		if (ret != RK_SUCCESS)
			LOG_ERROR("vi main chn unbind gdc main chn fail:%x\n", ret);
	}
	LOG_INFO("exit\n");
	return ret;
}

static int pause_preview(void) {
	int ret = 0;
	MPP_CHN_S vi_display_chn;
	MPP_CHN_S vo_chn;
	MPP_CHN_S gdc_display_chn;
	LOG_INFO("enter\n");
	vi_display_chn.enModId = RK_ID_VI;
	vi_display_chn.s32DevId = VI_DEV_ID;
	vi_display_chn.s32ChnId = VI_DISPLAY_CHN;
	vo_chn.enModId = RK_ID_VO;
	vo_chn.s32DevId = VO_PREV_LAYER;
	vo_chn.s32ChnId = VO_PREV_CHN;
	gdc_display_chn.enModId = RK_ID_GDC;
	gdc_display_chn.s32DevId = 0;
	gdc_display_chn.s32ChnId = GDC_DISPLAY_CHN;

	if (rk_mode_param.enable_display) {
		if (rk_mode_param.eis_mode == RK_EIS_OFF) {
			ret = RK_MPI_SYS_UnBind(&vi_display_chn, &vo_chn);
			if (ret != RK_SUCCESS)
				LOG_ERROR("vi display chn unbind vo chn fail:%x\n", ret);
		} else {
			ret = RK_MPI_SYS_UnBind(&gdc_display_chn, &vo_chn);
			if (ret != RK_SUCCESS)
				LOG_ERROR("gdc display chn unbind vo chn fail:%x\n", ret);
		}
		vo_deinit();
	}
	LOG_INFO("exit\n");
	return ret;
}

static int resume_preview(void) {
	int ret = 0;
	MPP_CHN_S vi_display_chn;
	MPP_CHN_S vo_chn;
	MPP_CHN_S gdc_display_chn;
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
	gdc_display_chn.enModId = RK_ID_GDC;
	gdc_display_chn.s32DevId = 0;
	gdc_display_chn.s32ChnId = GDC_DISPLAY_CHN;

	if (rk_mode_param.enable_display) {
		if (rk_mode_param.eis_mode == RK_EIS_OFF) {
			ret = RK_MPI_SYS_Bind(&vi_display_chn, &vo_chn);
			if (ret != RK_SUCCESS) {
				LOG_ERROR("vi display chn bind vo chn fail:%x\n", ret);
				return ret;
			}
		} else {
			ret = RK_MPI_SYS_Bind(&gdc_display_chn, &vo_chn);
			if (ret != RK_SUCCESS) {
				LOG_ERROR("gdc display chn bind vo chn fail:%x\n", ret);
				return ret;
			}
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
		rk_storage_record_start(STREAM_ID_FOR_VIDEO);
	rk_media_ctx.record_thread_running = true;
	pthread_create(&rk_media_ctx.record_thread, NULL, normal_video_loop, NULL);

	if (ret) {
		LOG_ERROR("ERROR: start recv frame error! ret=%d\n", ret);
		goto __start_record_failed;
	}
	ret = gdc_record_init();
	if (ret) {
		LOG_ERROR("ERROR: gdc_record_init failed! ret=%d\n", ret);
		goto __start_record_failed;
	}
	ret = rgn_init();
	if (ret) {
		LOG_ERROR("ERROR: rgn_init failed! ret=%d\n", ret);
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
	rgn_deinit();
	gdc_record_deinit();
	return ret;
}

static int stop_record(void) {
	int ret = 0;
	if (rk_media_ctx.record_thread_running == false) {
		LOG_DEBUG("record is not started, no need to stop record\n");
		return 0;
	}
	rk_media_ctx.record_thread_running = false;
	pthread_join(rk_media_ctx.record_thread, NULL);
	ret = unbind_record_pipe();
	if (ret)
		LOG_ERROR("ERROR: unbind_record_pipe failed! ret=%d\n", ret);
	ret = rgn_deinit();
	if (ret)
		LOG_ERROR("ERROR: rgn_deinit failed! ret=%d\n", ret);
	ret = gdc_record_deinit();
	if (ret)
		LOG_ERROR("ERROR: gdc_record_deinit failed! ret=%d\n", ret);
	ret = RK_MPI_VENC_StopRecvFrame(VENC_MAIN_CHN);
	if (ret)
		LOG_ERROR("ERROR: stop recv frame error! ret=%d\n", ret);
	if (!rk_param_get_int("video.source:enable_auto_record", 0))
		rk_storage_record_stop(STREAM_ID_FOR_VIDEO);
	return ret;
}

static int record_is_running(void) { return rk_media_ctx.record_thread_running; }

static int video_init(RK_MODE_PARAM_S param) {
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
	ret = gdc_display_init();
	if (ret) {
		LOG_ERROR("ERROR: gdc_display_init failed! ret=%d\n", ret);
		return ret;
	}
	ret = gdc_dummy_init();
	if (ret) {
		LOG_ERROR("ERROR: gdc_dummy_init failed! ret=%d\n", ret);
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
	if (rk_mode_param.enable_rtsp)
		rkipc_rtsp_init(RTSP_URL_0, RTSP_URL_1, RTSP_URL_2);
	if (rk_param_get_int("video.source:enable_auto_record", 0)) {
		ret = start_record();
		if (ret)
			LOG_ERROR("ERROR: start_record failed! ret=%d\n", ret);
	}
	LOG_INFO("exit\n");
	return ret;
}

static int video_deinit(void) {
	int ret = 0;
	LOG_INFO("enter\n");
	if (rk_mode_param.enable_rtsp)
		rkipc_rtsp_deinit();
	ret = stop_record();
	if (ret)
		LOG_ERROR("ERROR: stop_record failed! ret=%d\n", ret);
	ret = unbind_display_pipe();
	if (ret)
		LOG_ERROR("ERROR: unbind_display_pipe failed! ret=%d\n", ret);
	ret = venc_deinit();
	if (ret)
		LOG_ERROR("ERROR: venc_deinit failed! ret=%d\n", ret);
	ret = gdc_display_deinit();
	if (ret)
		LOG_ERROR("ERROR: gdc_display_deinit failed! ret=%d\n", ret);
	ret = gdc_dummy_deinit();
	if (ret)
		LOG_ERROR("ERROR: gdc_dummy_deinit failed! ret=%d\n", ret);
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

static RK_MODE_CALLBACK_S video_ops = {
    .init = video_init,
    .deinit = video_deinit,
    .start_record = start_record,
    .stop_record = stop_record,
    .record_is_running = record_is_running,
    .start_capture = NULL,
    .stop_capture = NULL,
    .get_capture_num = NULL,
    .pause_preview = pause_preview,
    .resume_preview = resume_preview,
};

static __attribute__((constructor)) void register_video_ops(void) {
	rk_media_register_callback(RK_VIDEO_MODE, &video_ops);
	const char *msg = "register video operations\n";
	write(STDOUT_FILENO, msg, strlen(msg));
}
