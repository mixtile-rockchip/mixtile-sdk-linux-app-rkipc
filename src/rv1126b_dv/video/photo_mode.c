// Copyright 2025 Rockchip Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "photo_mode.c"

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
#include <sys/stat.h>

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
	bool start_take_photo;
	pthread_t photo_thread;
	int remain_photo_num;
} rk_media_ctx;
static RK_MODE_PARAM_S rk_mode_param;

extern char *rkipc_iq_file_path_;
extern void *rk_isp_get_ctx(int cam_id);

// Insert 3A and module info into EXIF segment of JPEG image
static int insert_extra_info(VENC_STREAM_S *jpeg_stream, unsigned char **out_buf, size_t *out_len) {
	int ret = 0, brightness;
	void *aiq_ctx = rk_isp_get_ctx(0);
	unsigned char *data = RK_MPI_MB_Handle2VirAddr(jpeg_stream->pstPack->pMbBlk);
	size_t len = jpeg_stream->pstPack->u32Len;
	ae_api_queryInfo_t ae_info = {};
	ret = rk_aiq_user_api2_ae_queryExpResInfo(aiq_ctx, &ae_info);
	if (ret != 0) {
		LOG_ERROR("rk_aiq_user_api2_ae_queryExpResInfo failed %x\n", ret);
		return -1;
	}
	// Extract exposure time (seconds) and compute ISO from gains
	double exposure = ae_info.linExpInfo.expParam.integration_time;
	double analog = ae_info.linExpInfo.expParam.analog_gain;
	double digital = ae_info.linExpInfo.expParam.digital_gain;
	double isp_dgain = ae_info.linExpInfo.expParam.isp_dgain;
	double iso_d = 100.0 * analog * digital * isp_dgain;
	if (iso_d < 1.0)
		iso_d = 100.0;
	uint16_t iso = (uint16_t)round(iso_d);
	if (iso == 0)
		iso = 100;

	// Prepare rational for exposure time: numerator/denominator (use microseconds precision)
	uint32_t denom = 1000000U;
	uint32_t numer = (uint32_t)round(exposure * (double)denom);
	if (numer == 0)
		numer = 1;

	ae_api_linExpAttr_t lin_exp_attr = {};
	rk_aiq_user_api2_ae_getLinExpAttr(aiq_ctx, &lin_exp_attr);
	// Default values for additional EXIF fields
	// FIXME: read from actual camera parameters if available
	double exp_comp = lin_exp_attr.sw_aeT_evBias_strg; // exposure compensation (EV)
	double fnumber = 2.8;                              // aperture
	double focal_length = 3.6;                         // mm
	double max_aperture = 2.8;                         // max aperture

	const char *make_str = "RockChip";
	size_t make_len = strlen(make_str) + 1; // include null

	LOG_DEBUG("origin ae info: exposure %.6f s, analog %.2f, digital %.2f, isp_dgain %.2f, iso %d, "
	          "ev %f\n",
	          ae_info.linExpInfo.expParam.integration_time, ae_info.linExpInfo.expParam.analog_gain,
	          ae_info.linExpInfo.expParam.digital_gain, ae_info.linExpInfo.expParam.isp_dgain,
	          ae_info.linExpInfo.expParam.iso, lin_exp_attr.sw_aeT_evBias_strg);

	// Validate JPEG SOI
	if (len < 2 || data[0] != 0xFF || data[1] != 0xD8) {
		LOG_ERROR("Not a valid JPEG (missing SOI)\n");
		return -1;
	}

	// Build EXIF APP1 segment with TIFF little-endian
	// Layout:
	// 'Exif\0\0' + TIFF header (8 bytes) + 0th IFD (count + entries + next) + ExifIFD + rational
	// data + make string

	size_t tiff_header_size = 8;
	size_t zeroth_ifd_entries = 2;                            // Make + ExifIFDPointer
	size_t zeroth_ifd_size = 2 + 12 * zeroth_ifd_entries + 4; // count + entries + next
	size_t exif_ifd_entries = 6; // ExposureTime, ISOSpeedRatings, FNumber, FocalLength,
	                             // MaxApertureValue, ExposureBiasValue
	size_t exif_ifd_size = 2 + 12 * exif_ifd_entries + 4;
	size_t rational_data_size =
	    8 * 5; // exposure + fnumber + focal + max_ap + exposure bias (SRATIONAL)
	size_t rational_data_off = tiff_header_size + zeroth_ifd_size + exif_ifd_size;
	size_t make_offset = rational_data_off + rational_data_size;
	size_t exif_payload_len =
	    6 + tiff_header_size + zeroth_ifd_size + exif_ifd_size + rational_data_size + make_len;

	if (exif_payload_len + 2 > 0xFFFF) {
		LOG_ERROR("EXIF payload too large\n");
		return -1;
	}

	// find existing APP1
	size_t pos = 2;
	size_t app1_pos = SIZE_MAX;
	size_t app1_segment_len = 0;
	while (pos + 4 <= len && data[pos] == 0xFF) {
		unsigned char marker = data[pos + 1];
		if (marker == 0xE1) {
			if (pos + 4 > len)
				break;
			uint16_t seg_len = (data[pos + 2] << 8) | data[pos + 3];
			app1_pos = pos;
			app1_segment_len = 2 + seg_len;
			break;
		} else if (marker >= 0xE0 && marker <= 0xEF) {
			if (pos + 4 > len)
				break;
			uint16_t seg_len = (data[pos + 2] << 8) | data[pos + 3];
			pos += 2 + seg_len;
		} else {
			break;
		}
	}

	size_t new_segment_total = 2 + (exif_payload_len + 2);
	size_t new_len = 0;
	if (app1_pos != SIZE_MAX) {
		new_len = len - app1_segment_len + new_segment_total;
	} else {
		new_len = len + new_segment_total;
	}

	unsigned char *buf = malloc(new_len);
	if (!buf) {
		LOG_ERROR("Failed to allocate memory for new jpeg buffer\n");
		return -1;
	}

	size_t dst = 0;
	if (app1_pos != SIZE_MAX) {
		memcpy(buf + dst, data, app1_pos);
		dst += app1_pos;
	} else {
		memcpy(buf + dst, data, 2);
		dst += 2;
	}

	// APP1 marker
	buf[dst++] = 0xFF;
	buf[dst++] = 0xE1;
	uint16_t len_field = (uint16_t)(exif_payload_len + 2);
	buf[dst++] = (len_field >> 8) & 0xFF;
	buf[dst++] = len_field & 0xFF;
	// 'Exif\0\0'
	memcpy(buf + dst, "Exif\0\0", 6);
	dst += 6;
	// TIFF header (little endian)
	buf[dst++] = 'I';
	buf[dst++] = 'I';
	buf[dst++] = 0x2A;
	buf[dst++] = 0x00;
	// offset to 0th IFD = 8 (little endian 4 bytes)
	buf[dst++] = 0x08;
	buf[dst++] = 0x00;
	buf[dst++] = 0x00;
	buf[dst++] = 0x00;
	// 0th IFD count = 2 (Make + ExifIFDPointer)
	buf[dst++] = (uint8_t)(zeroth_ifd_entries & 0xFF);
	buf[dst++] = (uint8_t)((zeroth_ifd_entries >> 8) & 0xFF);
	// Entry 1: Make (0x010F) type ASCII (2) count = make_len -> value = offset to make string
	buf[dst++] = 0x0F;
	buf[dst++] = 0x01; // tag
	buf[dst++] = 0x02;
	buf[dst++] = 0x00; // type = ASCII
	buf[dst++] = (uint8_t)(make_len & 0xFF);
	buf[dst++] = (uint8_t)((make_len >> 8) & 0xFF);
	buf[dst++] = (uint8_t)((make_len >> 16) & 0xFF);
	buf[dst++] = (uint8_t)((make_len >> 24) & 0xFF); // count
	uint32_t make_off = (uint32_t)make_offset;
	buf[dst++] = (make_off)&0xFF;
	buf[dst++] = (make_off >> 8) & 0xFF;
	buf[dst++] = (make_off >> 16) & 0xFF;
	buf[dst++] = (make_off >> 24) & 0xFF;
	// Entry 2: Tag = 0x8769 (ExifIFDPointer)
	buf[dst++] = 0x69;
	buf[dst++] = 0x87; // tag
	buf[dst++] = 0x04;
	buf[dst++] = 0x00; // type = LONG
	buf[dst++] = 0x01;
	buf[dst++] = 0x00;
	buf[dst++] = 0x00;
	buf[dst++] = 0x00; // count =1
	uint32_t exif_ifd_off = (uint32_t)(tiff_header_size + zeroth_ifd_size);
	buf[dst++] = (exif_ifd_off)&0xFF;
	buf[dst++] = (exif_ifd_off >> 8) & 0xFF;
	buf[dst++] = (exif_ifd_off >> 16) & 0xFF;
	buf[dst++] = (exif_ifd_off >> 24) & 0xFF;
	// next IFD offset = 0
	buf[dst++] = 0x00;
	buf[dst++] = 0x00;
	buf[dst++] = 0x00;
	buf[dst++] = 0x00;

	// Exif IFD
	// count = 6 (ExposureTime, ISOSpeedRatings, FNumber, FocalLength, MaxApertureValue,
	// ExposureBiasValue)
	buf[dst++] = 0x06;
	buf[dst++] = 0x00;
	// Entry 1: ExposureTime (0x829A) type RATIONAL (5) count 1
	buf[dst++] = 0x9A;
	buf[dst++] = 0x82; // tag
	buf[dst++] = 0x05;
	buf[dst++] = 0x00; // type = RATIONAL
	buf[dst++] = 0x01;
	buf[dst++] = 0x00;
	buf[dst++] = 0x00;
	buf[dst++] = 0x00; // count =1
	uint32_t exposure_rational_offset = (uint32_t)rational_data_off;
	buf[dst++] = (exposure_rational_offset)&0xFF;
	buf[dst++] = (exposure_rational_offset >> 8) & 0xFF;
	buf[dst++] = (exposure_rational_offset >> 16) & 0xFF;
	buf[dst++] = (exposure_rational_offset >> 24) & 0xFF;
	// Entry 2: ISOSpeedRatings (0x8827) type SHORT (3) count 1
	buf[dst++] = 0x27;
	buf[dst++] = 0x88; // tag
	buf[dst++] = 0x03;
	buf[dst++] = 0x00; // type = SHORT
	buf[dst++] = 0x01;
	buf[dst++] = 0x00;
	buf[dst++] = 0x00;
	buf[dst++] = 0x00; // count =1
	// value (2 bytes) + pad
	buf[dst++] = (iso)&0xFF;
	buf[dst++] = (iso >> 8) & 0xFF;
	buf[dst++] = 0x00;
	buf[dst++] = 0x00;
	// Entry 3: FNumber (0x829D) type RATIONAL (5) count 1
	buf[dst++] = 0x9D;
	buf[dst++] = 0x82; // tag
	buf[dst++] = 0x05;
	buf[dst++] = 0x00; // type = RATIONAL
	buf[dst++] = 0x01;
	buf[dst++] = 0x00;
	buf[dst++] = 0x00;
	buf[dst++] = 0x00; // count =1
	uint32_t fnumber_offset = (uint32_t)(rational_data_off + 8);
	buf[dst++] = (fnumber_offset)&0xFF;
	buf[dst++] = (fnumber_offset >> 8) & 0xFF;
	buf[dst++] = (fnumber_offset >> 16) & 0xFF;
	buf[dst++] = (fnumber_offset >> 24) & 0xFF;
	// Entry 4: FocalLength (0x920A) type RATIONAL (5) count 1
	buf[dst++] = 0x0A;
	buf[dst++] = 0x92; // tag
	buf[dst++] = 0x05;
	buf[dst++] = 0x00; // type = RATIONAL
	buf[dst++] = 0x01;
	buf[dst++] = 0x00;
	buf[dst++] = 0x00;
	buf[dst++] = 0x00;
	uint32_t focal_offset = (uint32_t)(rational_data_off + 16);
	buf[dst++] = (focal_offset)&0xFF;
	buf[dst++] = (focal_offset >> 8) & 0xFF;
	buf[dst++] = (focal_offset >> 16) & 0xFF;
	buf[dst++] = (focal_offset >> 24) & 0xFF;
	// Entry 5: MaxApertureValue (0x9205) type RATIONAL (5) count 1
	buf[dst++] = 0x05;
	buf[dst++] = 0x92; // tag
	buf[dst++] = 0x05;
	buf[dst++] = 0x00; // type = RATIONAL
	buf[dst++] = 0x01;
	buf[dst++] = 0x00;
	buf[dst++] = 0x00;
	buf[dst++] = 0x00;
	uint32_t maxap_offset = (uint32_t)(rational_data_off + 24);
	buf[dst++] = (maxap_offset)&0xFF;
	buf[dst++] = (maxap_offset >> 8) & 0xFF;
	buf[dst++] = (maxap_offset >> 16) & 0xFF;
	buf[dst++] = (maxap_offset >> 24) & 0xFF;
	// Entry 6: ExposureBiasValue (0x9204) type SRATIONAL (10) count 1
	buf[dst++] = 0x04;
	buf[dst++] = 0x92; // tag
	buf[dst++] = 0x0A;
	buf[dst++] = 0x00; // type = SRATIONAL
	buf[dst++] = 0x01;
	buf[dst++] = 0x00;
	buf[dst++] = 0x00;
	buf[dst++] = 0x00;
	uint32_t expbias_offset = (uint32_t)(rational_data_off + 32);
	buf[dst++] = (expbias_offset)&0xFF;
	buf[dst++] = (expbias_offset >> 8) & 0xFF;
	buf[dst++] = (expbias_offset >> 16) & 0xFF;
	buf[dst++] = (expbias_offset >> 24) & 0xFF;
	// next IFD offset = 0
	buf[dst++] = 0x00;
	buf[dst++] = 0x00;
	buf[dst++] = 0x00;
	buf[dst++] = 0x00;
	// Rational data (numerator, denominator) little-endian
	// ExposureTime
	buf[dst++] = (numer)&0xFF;
	buf[dst++] = (numer >> 8) & 0xFF;
	buf[dst++] = (numer >> 16) & 0xFF;
	buf[dst++] = (numer >> 24) & 0xFF;
	buf[dst++] = (denom)&0xFF;
	buf[dst++] = (denom >> 8) & 0xFF;
	buf[dst++] = (denom >> 16) & 0xFF;
	buf[dst++] = (denom >> 24) & 0xFF;
	// FNumber (use denom = 100)
	uint32_t fnum_numer = (uint32_t)round(fnumber * 100.0);
	uint32_t fnum_denom = 100;
	buf[dst++] = (fnum_numer)&0xFF;
	buf[dst++] = (fnum_numer >> 8) & 0xFF;
	buf[dst++] = (fnum_numer >> 16) & 0xFF;
	buf[dst++] = (fnum_numer >> 24) & 0xFF;
	buf[dst++] = (fnum_denom)&0xFF;
	buf[dst++] = (fnum_denom >> 8) & 0xFF;
	buf[dst++] = (fnum_denom >> 16) & 0xFF;
	buf[dst++] = (fnum_denom >> 24) & 0xFF;
	// FocalLength (use denom = 100)
	uint32_t focal_numer = (uint32_t)round(focal_length * 100.0);
	uint32_t focal_denom = 100;
	buf[dst++] = (focal_numer)&0xFF;
	buf[dst++] = (focal_numer >> 8) & 0xFF;
	buf[dst++] = (focal_numer >> 16) & 0xFF;
	buf[dst++] = (focal_numer >> 24) & 0xFF;
	buf[dst++] = (focal_denom)&0xFF;
	buf[dst++] = (focal_denom >> 8) & 0xFF;
	buf[dst++] = (focal_denom >> 16) & 0xFF;
	buf[dst++] = (focal_denom >> 24) & 0xFF;
	// MaxApertureValue (use denom = 100)
	uint32_t maxap_numer = (uint32_t)round(max_aperture * 100.0);
	uint32_t maxap_denom = 100;
	buf[dst++] = (maxap_numer)&0xFF;
	buf[dst++] = (maxap_numer >> 8) & 0xFF;
	buf[dst++] = (maxap_numer >> 16) & 0xFF;
	buf[dst++] = (maxap_numer >> 24) & 0xFF;
	buf[dst++] = (maxap_denom)&0xFF;
	buf[dst++] = (maxap_denom >> 8) & 0xFF;
	buf[dst++] = (maxap_denom >> 16) & 0xFF;
	buf[dst++] = (maxap_denom >> 24) & 0xFF;
	// ExposureBiasValue (SRATIONAL) (use denom = 100)
	// FIEME: wrong value when EV is nagative
	int32_t expbias_numer = (int32_t)round(abs(exp_comp) / 100.0);
	int32_t expbias_denom = 100;
	buf[dst++] = (expbias_numer)&0xFF;
	buf[dst++] = ((uint32_t)expbias_numer >> 8) & 0xFF;
	buf[dst++] = ((uint32_t)expbias_numer >> 16) & 0xFF;
	buf[dst++] = ((uint32_t)expbias_numer >> 24) & 0xFF;
	buf[dst++] = (expbias_denom)&0xFF;
	buf[dst++] = (expbias_denom >> 8) & 0xFF;
	buf[dst++] = (expbias_denom >> 16) & 0xFF;
	buf[dst++] = (expbias_denom >> 24) & 0xFF;
	// Make string
	memcpy(buf + dst, make_str, make_len);
	dst += make_len;

	// copy rest (skip old APP1 if present)
	size_t src_rest = (app1_pos != SIZE_MAX) ? (app1_pos + app1_segment_len) : 2;
	size_t rest_len = len - src_rest;
	memcpy(buf + dst, data + src_rest, rest_len);
	dst += rest_len;

	if (dst != new_len) {
		LOG_ERROR("EXIF insertion size mismatch, expected %zu got %zu\n", new_len, dst);
		free(buf);
		return -1;
	}

	*out_buf = buf;
	*out_len = new_len;
	return 0;
}

static void *take_photo_loop(void *arg) {
	printf("#Start %s thread, arg:%p\n", __func__, arg);
	VENC_STREAM_S video_stream;
	int loop_count = 0;
	int ret = 0;
	char file_name[128] = {0};
	const char *mount_path = rk_param_get_string("storage:mount_path", "/mnt/sdcard");
	const char *photo_folder_name = rk_param_get_string("storage.3:folder_name", "photo");
	video_stream.pstPack = malloc(sizeof(VENC_PACK_S));
	char folder_path[256] = {0};
	char photo_type[8] = {0};

	prctl(PR_SET_NAME, __func__, 0, 0, 0);
	LOG_INFO("enter\n");
	snprintf(folder_path, sizeof(folder_path), "%s/%s", mount_path, photo_folder_name);
	struct stat st = {0};
	if (stat(folder_path, &st) == -1) {
		if (mkdir(folder_path, 0777) != 0) {
			LOG_ERROR("Failed to create folder: %s\n", folder_path);
			free(video_stream.pstPack);
			return NULL;
		}
	}
	if (rk_mode_param.hdr_mode == RK_HDR_DAG_MODE)
		snprintf(photo_type, sizeof(photo_type), "hdr1");
	else if (rk_mode_param.hdr_mode == RK_HDR_STAGGERED_MODE)
		snprintf(photo_type, sizeof(photo_type), "hdr2");
	else
		snprintf(photo_type, sizeof(photo_type), "normal");

	while (rk_media_ctx.start_take_photo) {
		if (rk_media_ctx.remain_photo_num <= 0) {
			usleep(300 * 1000);
			continue;
		}
		int64_t before_time = rkipc_get_curren_time_ms();
		// 1. Get JPEG stream
		ret = RK_MPI_VENC_GetStream(JPEG_CHN, &video_stream, 1000);
		if (ret == RK_SUCCESS) {
			// 2. Get data address and length of JPEG stream
			void *data = RK_MPI_MB_Handle2VirAddr(video_stream.pstPack->pMbBlk);
			LOG_INFO("Count:%d, Len:%d, PTS is %" PRId64 ", enH264EType is %d\n", loop_count,
			         video_stream.pstPack->u32Len, video_stream.pstPack->u64PTS,
			         video_stream.pstPack->DataType.enH264EType);
			time_t t = time(NULL);
			struct tm tm = *localtime(&t);
			snprintf(file_name, 128, "%s/%s/%d%02d%02d%02d%02d%02d_%s_%d.jpeg", mount_path,
			         photo_folder_name, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
			         tm.tm_min, tm.tm_sec, photo_type,
			         rk_mode_param.max_photo_num - rk_media_ctx.remain_photo_num);
			LOG_INFO("file_name is %s\n", file_name);
			FILE *fp = fopen(file_name, "wb");
			unsigned char *new_jpeg = NULL;
			size_t new_len = 0;
			// 3. Insert extra info into EXIF, and then save to file
			if (insert_extra_info(&video_stream, &new_jpeg, &new_len) == 0 && new_jpeg) {
				fwrite(new_jpeg, 1, new_len, fp);
				free(new_jpeg);
			} else {
				fwrite(data, 1, video_stream.pstPack->u32Len, fp);
			}
			fflush(fp);
			fsync(fileno(fp));
			fclose(fp);
			--rk_media_ctx.remain_photo_num;
			// 3. Release JPEG stream
			ret = RK_MPI_VENC_ReleaseStream(JPEG_CHN, &video_stream);
			if (ret != RK_SUCCESS) {
				LOG_ERROR("RK_MPI_VENC_ReleaseStream fail %x\n", ret);
			}
			loop_count++;
		} else {
			LOG_ERROR("RK_MPI_VENC_GetStream timeout %x\n", ret);
		}
		LOG_INFO("take photo cost time is %lldms\n", rkipc_get_curren_time_ms() - before_time);
	}
	if (video_stream.pstPack)
		free(video_stream.pstPack);
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
	vi_chn_attr.stFrameRate.s32SrcFrameRate = 30;
	vi_chn_attr.stFrameRate.s32DstFrameRate = 10;
	LOG_INFO("vi_chn_attr: W=%d, H=%d, SrcFrmRt=%d, DstFrmRt=%d\n", vi_chn_attr.stSize.u32Width,
	         vi_chn_attr.stSize.u32Height, vi_chn_attr.stFrameRate.s32SrcFrameRate,
	         vi_chn_attr.stFrameRate.s32DstFrameRate);
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

static int jpeg_init(void) {
	int ret = 0;
	int video_width = rk_mode_param.video_width;
	int video_height = rk_mode_param.video_height;
	VENC_CHN_ATTR_S jpeg_chn_attr;
	memset(&jpeg_chn_attr, 0, sizeof(jpeg_chn_attr));
	jpeg_chn_attr.stVencAttr.enType = RK_VIDEO_ID_JPEG;
	jpeg_chn_attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
	jpeg_chn_attr.stVencAttr.u32PicWidth = video_width;
	jpeg_chn_attr.stVencAttr.u32PicHeight = video_height;
	jpeg_chn_attr.stVencAttr.u32VirWidth = video_width;
	jpeg_chn_attr.stVencAttr.u32VirHeight = video_height;
	jpeg_chn_attr.stVencAttr.u32StreamBufCnt = 5;
	jpeg_chn_attr.stVencAttr.u32BufSize = video_width * video_height * 3;
	// jpeg_chn_attr.stVencAttr.u32Depth = 1;
	ret = RK_MPI_VENC_CreateChn(JPEG_CHN, &jpeg_chn_attr);
	if (ret) {
		LOG_ERROR("ERROR: create VENC error! ret=%d\n", ret);
		return -1;
	}
	VENC_JPEG_PARAM_S jpeg_param;
	memset(&jpeg_param, 0, sizeof(jpeg_param));
	jpeg_param.u32Qfactor = rk_param_get_int("video.0:jpeg_qfactor", 99);
	RK_MPI_VENC_SetJpegParam(JPEG_CHN, &jpeg_param);
	LOG_INFO("jpeg init success\n");
	return ret;
}

static int jpeg_deinit(void) {
	int ret = 0;
	ret = RK_MPI_VENC_DestroyChn(JPEG_CHN);
	if (ret)
		LOG_ERROR("ERROR: destroy VENC error! ret=%d\n", ret);
	if (ret == RK_SUCCESS)
		LOG_INFO("jpeg deinit success\n");
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

static int bind_photo_pipe(void) {
	MPP_CHN_S vi_main_chn;
	MPP_CHN_S venc_chn;
	int ret = 0;
	LOG_INFO("enter\n");
	vi_main_chn.enModId = RK_ID_VI;
	vi_main_chn.s32DevId = VI_DEV_ID;
	vi_main_chn.s32ChnId = VI_MAIN_CHN;
	venc_chn.enModId = RK_ID_VENC;
	venc_chn.s32DevId = 0;
	venc_chn.s32ChnId = JPEG_CHN;

	ret = RK_MPI_SYS_Bind(&vi_main_chn, &venc_chn);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("vi main chn bind venc chn fail:%x\n", ret);
		return ret;
	}
	LOG_INFO("exit\n");
	return ret;
}

static int unbind_photo_pipe(void) {
	MPP_CHN_S vi_main_chn;
	MPP_CHN_S venc_chn;
	int ret = 0;
	LOG_INFO("enter\n");
	vi_main_chn.enModId = RK_ID_VI;
	vi_main_chn.s32DevId = VI_DEV_ID;
	vi_main_chn.s32ChnId = VI_MAIN_CHN;
	venc_chn.enModId = RK_ID_VENC;
	venc_chn.s32DevId = 0;
	venc_chn.s32ChnId = JPEG_CHN;

	ret = RK_MPI_SYS_UnBind(&vi_main_chn, &venc_chn);
	if (ret != RK_SUCCESS)
		LOG_ERROR("vi main chn unbind venc chn fail:%x\n", ret);
	LOG_INFO("exit\n");
	return ret;
}

static int start_capture(void) {
	LOG_INFO("enter\n");
	int ret = 0;
	if (rk_media_ctx.remain_photo_num != 0) {
		LOG_ERROR("there have unfinished tasks!\n");
		return -1;
	}
	if (rk_media_ctx.start_take_photo) {
		LOG_ERROR("photo is already started, no need to start photo again\n");
		return -1;
	}
	VENC_RECV_PIC_PARAM_S recv_param;
	memset(&recv_param, 0, sizeof(VENC_RECV_PIC_PARAM_S));
	recv_param.s32RecvPicNum = rk_mode_param.max_photo_num;
	ret = RK_MPI_VENC_StartRecvFrame(JPEG_CHN, &recv_param);
	if (ret) {
		LOG_ERROR("ERROR: start recv frame error! ret=%d\n", ret);
		return ret;
	}
	ret = bind_photo_pipe();
	if (ret) {
		LOG_ERROR("ERROR: bind_photo_pipe failed! ret=%d\n", ret);
		RK_MPI_VENC_StopRecvFrame(JPEG_CHN);
		return ret;
	}
	rk_media_ctx.remain_photo_num = rk_mode_param.max_photo_num;
	rk_media_ctx.start_take_photo = true;
	pthread_create(&rk_media_ctx.photo_thread, NULL, take_photo_loop, NULL);
	LOG_INFO("exit\n");
	return ret;
}

static int stop_capture(void) {
	LOG_INFO("enter\n");
	int ret = 0;
	if (!rk_media_ctx.start_take_photo) {
		LOG_ERROR("photo is not started, no need to stop photo\n");
		return -1;
	}
	rk_media_ctx.start_take_photo = false;
	pthread_join(rk_media_ctx.photo_thread, NULL);
	ret = unbind_photo_pipe();
	if (ret)
		LOG_ERROR("ERROR: unbind_photo_pipe failed! ret=%d\n", ret);
	ret = RK_MPI_VENC_StopRecvFrame(JPEG_CHN);
	if (ret)
		LOG_ERROR("ERROR: stop recv frame error! ret=%d\n", ret);
	LOG_INFO("exit\n");
	return 0;
}

static int get_capture_num(void) {
	return rk_mode_param.max_photo_num - rk_media_ctx.remain_photo_num;
}

static int rk_capture_init(RK_MODE_PARAM_S param) {
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
	ret = jpeg_init();
	if (ret) {
		LOG_ERROR("ERROR: jpeg_init failed! ret=%d\n", ret);
		return ret;
	}
	ret = bind_display_pipe();
	if (ret) {
		LOG_ERROR("ERROR: bind_display_pipe failed! ret=%d\n", ret);
		return ret;
	}
	LOG_INFO("exit\n");
	return ret;
}

static int rk_capture_deinit(void) {
	int ret = 0;
	LOG_INFO("enter\n");
	ret = unbind_display_pipe();
	if (ret)
		LOG_ERROR("ERROR: unbind_display_pipe failed! ret=%d\n", ret);
	ret = jpeg_deinit();
	if (ret)
		LOG_ERROR("ERROR: jpeg_deinit failed! ret=%d\n", ret);
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

static RK_MODE_CALLBACK_S capture_ops = {
    .init = rk_capture_init,
    .deinit = rk_capture_deinit,
    .start_record = NULL,
    .stop_record = NULL,
    .pause_preview = pause_preview,
    .resume_preview = resume_preview,
    .start_capture = start_capture,
    .stop_capture = stop_capture,
    .get_capture_num = get_capture_num,
};

static __attribute__((constructor)) void register_capture_ops(void) {
	rk_media_register_callback(RK_PHOTO_MODE, &capture_ops);
	const char *msg = "register capture operations\n";
	write(STDOUT_FILENO, msg, strlen(msg));
}
