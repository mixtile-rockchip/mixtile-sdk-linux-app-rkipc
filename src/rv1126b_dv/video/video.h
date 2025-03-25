// Copyright 2021 Rockchip Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "common.h"
#include "isp.h"
#include "osd.h"
#include "rtsp.h"
#include "storage.h"

#include <inttypes.h> // PRId64
#include <rk_debug.h>
#include <rk_mpi_mb.h>
#include <rk_mpi_mmz.h>
#include <rk_mpi_rgn.h>
#include <rk_mpi_sys.h>
#include <rk_mpi_tde.h>
#include <rk_mpi_venc.h>
#include <rk_mpi_vi.h>
#include <rk_mpi_vo.h>
#include <rk_mpi_vpss.h>

typedef enum {
	RK_VIDEO_MODE = 0,
	RK_PHOTO_MODE,
	RK_SLOW_MOTION_MODE,
	RK_TIME_LAPSE_MODE,
	RK_MODE_NUM,
} RK_MODE_E;

typedef enum {
	RK_EIS_OFF = 0,
	RK_NORMAL_STEADY,
	RK_HORIZON_STEADY,
	RK_DISTORTION_CORRECTION,
	RK_EIS_MODE_NUM,
} RK_EIS_MODE_E;

int rk_init_mode(void);
int rk_set_mode(RK_MODE_E mode);
RK_MODE_E rk_get_mode(void);

RK_EIS_MODE_E rk_get_eis_mode(void);
int rk_set_eis_mode(RK_EIS_MODE_E eis_mode);

static int rk_set_hdr(bool enable) { return 0; }
static int rk_get_hdr(void) { return 0; }

static int rk_set_eis_debug(bool enable) { return 0; }
static int rk_get_eis_debug(void) { return 0; }

int rk_video_start_record(void);
int rk_video_stop_record(void);
static int rk_video_get_fps(void) { return 0; }
static int rk_video_set_fps(int new_fps) { return 0; }

int rk_photo_set_max_num(int num);
int rk_photo_get_max_num(void);
int rk_photo_get_done_num(void);
int rk_photo_start(void); 
int rk_photo_stop(void); 
static int rk_enter_sleep(void) { return 0; }

int rk_video_init(void);
int rk_video_deinit(void);
int rk_video_restart(void);
int rk_video_reset_frame_rate(int stream_id);
