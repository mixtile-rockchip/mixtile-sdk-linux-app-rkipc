/*
 * Copyright (c) 2023 Rockchip, Inc. All Rights Reserved.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *	 http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#ifdef DRAW_UI_BY_VO
#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "rk_ui.c"

#define DRAW_USE_CPU 1

#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "common.h"
#include "disp.h"
#include <lvgl.h>

#include <rk_mpi_mb.h>
#include <rk_mpi_mmz.h>
#include <rk_mpi_rgn.h>
#include <rk_mpi_sys.h>
#include <rk_mpi_tde.h>
#include <rk_mpi_venc.h>
#include <rk_mpi_vi.h>
#include <rk_mpi_vo.h>
#include <rk_mpi_vpss.h>

/**********************
 *	  MACROS
 **********************/
#define DRM_CARD "/dev/dri/card0"
#define DRM_CONNECTOR_ID -1 /* -1 for the first connected one */

#define DBG_TAG "drm"

#define DIV_ROUND_UP(n, d) (((n) + (d)-1) / (d))

#define UPALIGNED_16(x) (((x) + 15) & ~15)

/**********************
 *  GLOBAL PROTOTYPES
 **********************/

/**********************
 *  GLOBAL VARIABLES
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
struct disp_dev {
	lv_disp_rot_t rot;
	VIDEO_FRAME_INFO_S display_bufs[2];
	VIDEO_FRAME_INFO_S *active_buf;
	VIDEO_FRAME_INFO_S *free_buf;
	uint32_t width, height;
	uint32_t mm_width, mm_height;
} disp_dev;

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *   STATIC FUNCTIONS
 **********************/
static int32_t drm_open(const char *path) {
	int32_t fd, flags;
	uint64_t has_dumb;
	int32_t ret;

	fd = open(path, O_RDWR);
	if (fd < 0) {
		LOG_ERROR("cannot open \"%s\" \n", path);
		return -1;
	}

	/* set FD_CLOEXEC flag */
	if ((flags = fcntl(fd, F_GETFD)) < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
		LOG_ERROR("fcntl FD_CLOEXEC failed\n");
		goto err;
	}

	/* check capability */
	ret = drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_dumb);
	if (ret < 0 || has_dumb == 0) {
		LOG_ERROR("drmGetCap DRM_CAP_DUMB_BUFFER failed or \"%s\" doesn't have dumb "
		          "buffer\n",
		          path);
		goto err;
	}

	return fd;
err:
	close(fd);
	return -1;
}

static int drm_find_connector(int32_t dev_fd) {
	drmModeConnector *conn = NULL;
	drmModeRes *res = NULL;
	int32_t i;

	if ((res = drmModeGetResources(dev_fd)) == NULL) {
		LOG_ERROR("drmModeGetResources() failed\n");
		return -1;
	}

	if (res->count_crtcs <= 0) {
		LOG_ERROR("no Crtcs\n");
		goto free_res;
	}

	/* find all available connectors */
	for (i = 0; i < res->count_connectors; i++) {
		conn = drmModeGetConnector(dev_fd, res->connectors[i]);
		if (!conn)
			continue;

#if DRM_CONNECTOR_ID >= 0
		if (conn->connector_id != DRM_CONNECTOR_ID) {
			drmModeFreeConnector(conn);
			continue;
		}
#endif

		if (conn->connection == DRM_MODE_CONNECTED) {
			LOG_DEBUG("drm: connector %d: connected\n", conn->connector_id);
		} else if (conn->connection == DRM_MODE_DISCONNECTED) {
			LOG_DEBUG("drm: connector %d: disconnected\n", conn->connector_id);
		} else if (conn->connection == DRM_MODE_UNKNOWNCONNECTION) {
			LOG_DEBUG("drm: connector %d: unknownconnection\n", conn->connector_id);
		} else {
			LOG_DEBUG("drm: connector %d: unknown\n", conn->connector_id);
		}

		if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0)
			break;

		drmModeFreeConnector(conn);
		conn = NULL;
	};

	if (!conn) {
		LOG_ERROR("suitable connector not found\n");
		goto free_res;
	}

	disp_dev.width = conn->modes[0].hdisplay;
	disp_dev.height = conn->modes[0].vdisplay;
	disp_dev.mm_width = conn->mmWidth;
	disp_dev.mm_height = conn->mmHeight;
	LOG_INFO("width %d, height %d, mm_width %d, mm_height %d\n", disp_dev.width, disp_dev.height,
	         disp_dev.mm_width, disp_dev.mm_height);

	return 0;

free_res:
	drmModeFreeResources(res);

	return -1;
}

static int32_t get_disp_info(void) {
	int32_t dev_fd, ret;
	const char *device_path = NULL;

	device_path = getenv("DRM_CARD");
	if (!device_path)
		device_path = DRM_CARD;

	dev_fd = drm_open(device_path);
	if (dev_fd < 0)
		return -1;

	ret = drm_find_connector(dev_fd);
	if (ret) {
		LOG_ERROR("available drm devices not found\n");
		close(dev_fd);
		return -1;
	}

	close(dev_fd);

	LOG_INFO("rk_disp: %dx%d (%dmm X% dmm)\n", disp_dev.width, disp_dev.height, disp_dev.mm_width,
	         disp_dev.mm_height);

	return 0;
}

static int32_t rk_disp_setup(void) {
	int32_t ret;
	VO_PUB_ATTR_S vo_pub_attr;
	VO_VIDEO_LAYER_ATTR_S vo_layer_attr;
	VO_CSC_S vo_csc;
	VO_CHN_ATTR_S vo_chn_attr;
	uint32_t disp_buf_len;
	uint32_t display_width = rk_param_get_int("display:width", 240);
	uint32_t display_height = rk_param_get_int("display:height", 320);
	uint32_t display_layer = rk_param_get_int("display:layer_id", 5);
	int display_dev_id = rk_param_get_int("display:dev_id", 1);
	int display_chn_id = rk_param_get_int("display:ui_chn_id", 2);
	uint32_t fps = rk_param_get_int("display:fps", 30);
	const char *intf_type = rk_param_get_string("display:intf_type", "LCD");

	memset(&vo_pub_attr, 0, sizeof(VO_PUB_ATTR_S));
	memset(&vo_layer_attr, 0, sizeof(VO_VIDEO_LAYER_ATTR_S));
	memset(&vo_csc, 0, sizeof(VO_CSC_S));
	memset(&vo_chn_attr, 0, sizeof(vo_chn_attr));
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
	RK_MPI_VO_GetLayerDispBufLen(display_layer, &disp_buf_len);
	LOG_INFO("Get display_layer %d disp buf len is %d.\n", display_layer, disp_buf_len);
	disp_buf_len = 3;
	ret = RK_MPI_VO_SetLayerDispBufLen(display_layer, disp_buf_len);
	LOG_INFO("Agin Get display_layer %d disp buf len is %d.\n", display_layer, disp_buf_len);

	ret = RK_MPI_VO_GetPubAttr(display_dev_id, &vo_pub_attr);
	if ((vo_pub_attr.stSyncInfo.u16Hact == 0) || (vo_pub_attr.stSyncInfo.u16Vact == 0)) {
		vo_pub_attr.stSyncInfo.u16Hact = display_width;
		vo_pub_attr.stSyncInfo.u16Vact = display_height;
	}

	vo_layer_attr.stDispRect.s32X = 0;
	vo_layer_attr.stDispRect.s32Y = 0;
	vo_layer_attr.stDispRect.u32Width = display_width;
	vo_layer_attr.stDispRect.u32Height = display_height;
	vo_layer_attr.stImageSize.u32Width = display_width;
	vo_layer_attr.stImageSize.u32Height = display_height;
	LOG_INFO("vo_layer_attr W=%d, H=%d\n", vo_layer_attr.stDispRect.u32Width,
	         vo_layer_attr.stDispRect.u32Height);

	vo_layer_attr.u32DispFrmRt = fps;
	vo_layer_attr.enPixFormat = RK_FMT_RGB888;
	vo_csc.enCscMatrix = VO_CSC_MATRIX_IDENTITY;
	vo_csc.u32Contrast = 50;
	vo_csc.u32Hue = 50;
	vo_csc.u32Luma = 50;
	vo_csc.u32Satuature = 50;

	/*bind layer0 to device hd0*/
	ret = RK_MPI_VO_BindLayer(display_layer, display_dev_id, VO_LAYER_MODE_GRAPHIC);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("failed to bind layer %#X\n", ret);
		return ret;
	}
	ret = RK_MPI_VO_SetLayerAttr(display_layer, &vo_layer_attr);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("failed to set layer attr\n");
		return ret;
	}
	ret = RK_MPI_VO_SetLayerSpliceMode(display_layer, VO_SPLICE_MODE_RGA);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("failed to set layer splice mode %#X\n", ret);
		return ret;
	}
	ret = RK_MPI_VO_EnableLayer(display_layer);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("failed to enable layer %#X\n", ret);
		return ret;
	}
	ret = RK_MPI_VO_SetLayerCSC(display_layer, &vo_csc);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("failed to set layer csc\n");
		return ret;
	}

	vo_chn_attr.bDeflicker = RK_FALSE;
	vo_chn_attr.u32Priority = rk_param_get_int("display:ui_chn_priority", 3);
	vo_chn_attr.stRect.s32X = 0;
	vo_chn_attr.stRect.s32Y = 0;
	vo_chn_attr.stRect.u32Width = vo_layer_attr.stDispRect.u32Width;
	vo_chn_attr.stRect.u32Height = vo_layer_attr.stDispRect.u32Height;
	vo_chn_attr.enRotation = 0; // force to no rotation
	ret = RK_MPI_VO_SetChnAttr(display_layer, display_chn_id, &vo_chn_attr);
	ret = RK_MPI_VO_EnableChn(display_layer, display_chn_id);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("create %d layer %d ch vo failed %#X!\n", display_layer, display_chn_id, ret);
		return ret;
	}
	LOG_INFO("rk_disp: ui created successfully.\n");

	return 0;
}

static void rk_disp_teardown(void) {
	int ret = 0;
	uint32_t display_layer = rk_param_get_int("display:layer_id", 5);
	int display_chn_id = rk_param_get_int("display:ui_chn_id", 2);
	int display_dev_id = rk_param_get_int("display:dev_id", 1);
	ret = RK_MPI_VO_DisableChn(display_layer, display_chn_id);
	if (ret) {
		LOG_ERROR("RK_MPI_VO_DisableChn failed, ret is %#x\n", ret);
		return;
	}
	ret = RK_MPI_VO_DisableLayer(display_layer);
	if (ret) {
		LOG_ERROR("RK_MPI_VO_DisableLayer failed, ret is %#x\n", ret);
		return;
	}
	ret = RK_MPI_VO_Disable(display_dev_id);
	if (ret) {
		LOG_ERROR("RK_MPI_VO_Disable failed, ret is %#x\n", ret);
		return;
	}
	ret = RK_MPI_VO_UnBindLayer(display_layer, display_dev_id);
	if (ret) {
		LOG_ERROR("RK_MPI_VO_UnBindLayer failed, ret is %#x\n", ret);
		return;
	}
	LOG_INFO("rk_disp: ui destroyed successfully.\n");
}

static int32_t rk_disp_setup_buffers(void) {
	int32_t ret;
	uint32_t i, size;
	void *blk = NULL, *viraddr = NULL;
	uint32_t format = 0;

	if (LV_COLOR_DEPTH == 32) {
		format = RK_FMT_BGRA8888;
	} else {
		format = 0;
		LOG_ERROR("drm_flush rga not supported format\n");
		return -1;
	}

	size = disp_dev.width * disp_dev.height * (LV_COLOR_SIZE / 8);

	for (int i = 0; i < 2; ++i) {
		ret = RK_MPI_MMZ_Alloc(&blk, size, RK_MMZ_ALLOC_TYPE_CMA);
		if (0 != ret) {
			LOG_ERROR("alloc buf size %d failed %#X!\n", size, ret);
			return ret;
		}
		disp_dev.display_bufs[i].stVFrame.enPixelFormat = format;
		disp_dev.display_bufs[i].stVFrame.pMbBlk = blk;
		disp_dev.display_bufs[i].stVFrame.u32Width = disp_dev.width;
		disp_dev.display_bufs[i].stVFrame.u32VirWidth = disp_dev.width;
		disp_dev.display_bufs[i].stVFrame.u32Height = disp_dev.height;
		disp_dev.display_bufs[i].stVFrame.u32VirHeight = disp_dev.height;
		viraddr = RK_MPI_MB_Handle2VirAddr(blk);
		memset(viraddr, 0x00, size);
		LOG_INFO("mb blk %p, width %d, height %d, viraddr %p\n",
		         disp_dev.display_bufs[i].stVFrame.pMbBlk,
		         disp_dev.display_bufs[i].stVFrame.u32Width,
		         disp_dev.display_bufs[i].stVFrame.u32Height,
		         viraddr);
	}
	disp_dev.active_buf = &disp_dev.display_bufs[0];
	disp_dev.free_buf = &disp_dev.display_bufs[1];
	return 0;
}

static void rk_disp_teardown_buffers(void) {
	for (int i = 0; i < 2; ++i) {
		LOG_INFO("free mb blk %p\n", disp_dev.display_bufs[i].stVFrame.pMbBlk);
		RK_MPI_MMZ_Free(disp_dev.display_bufs[i].stVFrame.pMbBlk);
		memset(&disp_dev.display_bufs[i], 0, sizeof(VIDEO_FRAME_INFO_S));
	}
}

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
int32_t rk_disp_init(lv_disp_rot_t rotate_disp) {
	int32_t ret;

	ret = get_disp_info();
	if (0 != ret) {
		LOG_ERROR("get display info failed\n");
		return -1;
	}

	ret = rk_disp_setup();
	if (0 != ret) {
		LOG_ERROR("rk_disp_setup failed");
		return -1;
	}

	ret = rk_disp_setup_buffers();
	if (0 != ret) {
		LOG_ERROR("Allocating display buffer failed\n");
		goto err;
	}

	disp_dev.rot = rotate_disp;

	return 0;
err:
	rk_disp_teardown();

	return -1;
}

void rk_disp_exit(void) {
	rk_disp_teardown_buffers();
	rk_disp_teardown();
}

static void draw_buf_rotate_90(lv_color_t *color_p, const lv_area_t *area, lv_color_t *dst_buf,
                               lv_coord_t canvas_w, lv_coord_t canvas_h) {
	lv_coord_t area_w = (area->x2 - area->x1 + 1);
	lv_coord_t area_h = (area->y2 - area->y1 + 1);
	uint32_t initial_i = area->x1 * canvas_w + (canvas_w - area->y1 - 1);
	for (lv_coord_t y = 0; y < area_h; y++) {
		uint32_t i = initial_i - y;
		for (lv_coord_t x = 0; x < area_w; x++) {
			dst_buf[i] = *(color_p++);
			i += canvas_w;
		}
	}
}

void rk_disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
	int ret = 0;
	lv_coord_t area_width = (area->x2 - area->x1 + 1);
	lv_coord_t area_height = (area->y2 - area->y1 + 1);
	uint32_t display_layer = rk_param_get_int("display:layer_id", 5);
	int display_chn_id = rk_param_get_int("display:ui_chn_id", 2);
	VIDEO_FRAME_INFO_S *dst_frame = disp_dev.free_buf;

	LOG_DEBUG("x %d:%d y %d:%d area_width %d area_height %d\n", area->x1, area->x2, area->y1,
	          area->y2, area_width, area_height);

#if DRAW_USE_CPU
	void *dst_virtual_addr =
	    RK_MPI_MB_Handle2VirAddr(dst_frame->stVFrame.pMbBlk);
	void *src_virtual_addr = RK_MPI_MB_Handle2VirAddr(disp_dev.active_buf->stVFrame.pMbBlk);
	memcpy(dst_virtual_addr, src_virtual_addr, disp_dev.width * disp_dev.height * (LV_COLOR_SIZE / 8));
	if (disp_dev.rot == LV_DISP_ROT_90 || disp_dev.rot == LV_DISP_ROT_270) {
		draw_buf_rotate_90(color_p, area, dst_virtual_addr, disp_dev.width, disp_dev.height);
	} else {
		for (uint32_t y = 0, i = area->y1; i <= area->y2; ++i, ++y) {
			memcpy((uint8_t *)dst_virtual_addr +
			           (area->x1 + disp_dev.width * i) * (LV_COLOR_SIZE / 8),
			       (uint8_t *)color_p + (area_width * (LV_COLOR_SIZE / 8) * y),
			       area_width * (LV_COLOR_SIZE / 8));
		}
	}
	RK_MPI_SYS_MmzFlushCache(dst_frame->stVFrame.pMbBlk, RK_FALSE);
#else
	if (disp_dev.rot == LV_DISP_ROT_90 || disp_dev.rot == LV_DISP_ROT_270) {
	} else {
	}
#endif
	ret = RK_MPI_VO_SendFrame(display_layer, display_chn_id, dst_frame, 1000);
	if (ret != 0) {
		LOG_ERROR("RK_MPI_VO_SendFrame [%d,%d] failed %#X\n", display_layer, display_chn_id, ret);
	}
	// swap buffer
	void *tmp = disp_dev.free_buf;
	disp_dev.free_buf = disp_dev.active_buf;
	disp_dev.active_buf = tmp;

	lv_disp_flush_ready(disp_drv);
}

void rk_disp_get_sizes(lv_coord_t *width, lv_coord_t *height, uint32_t *dpi) {
	if (width)
		*width = disp_dev.width;

	if (height)
		*height = disp_dev.height;

	if (dpi && disp_dev.mm_width)
		*dpi = DIV_ROUND_UP(disp_dev.width * 25400, disp_dev.mm_width * 1000);

	return;
}

disp_ops_t rk_ui_ops = {
    rk_disp_init,
    rk_disp_exit,
    rk_disp_get_sizes,
    rk_disp_flush,
};
#endif // DRAW_UI_BY_VO
