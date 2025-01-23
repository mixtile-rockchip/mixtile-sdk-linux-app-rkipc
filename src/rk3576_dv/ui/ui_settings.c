/*
 * Copyright (c) 2023 Rockchip, Inc. All Rights Reserved.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */
#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "ui_settings.c"

#include <linux/input.h>
#include "common.h"
#include "evdev.h"
#include "lvgl/porting/lv_port_indev.h"
#include "storage.h"
#include "video.h"
#include "ui_common.h"
#include "ui_player.h"
#include "ui_dialog.h"
#include "ui_dialog_format.h"
#include "ui_dialog_storage.h"
#include "ui_page_manager.h"
#include "ui_resource_manage.h"
#include "ui_settings.h"

/**********************
 *  STATIC PROTOTYPES
 **********************/
typedef struct {
	lv_obj_t *container_obj;

	lv_obj_t *exit_obj;
	lv_obj_t *exit_label_obj;

	lv_obj_t *mode_imgbtn_obj;
	lv_obj_t *mode_img_obj;
	lv_obj_t *mode_label_obj;

	lv_obj_t *fps_imgbtn_obj;
	lv_obj_t *fps_img_obj;
	lv_obj_t *fps_label_obj;

	lv_obj_t *cap_imgbtn_obj;
	lv_obj_t *cap_img_obj;
	lv_obj_t *cap_label_obj;

	lv_obj_t *mic_imgbtn_obj;
	lv_obj_t *mic_img_obj;
	lv_obj_t *mic_label_obj;

	lv_obj_t *store_imgbtn_obj;
	lv_obj_t *store_img_obj;
	lv_obj_t *store_label_obj;

	lv_obj_t *format_imgbtn_obj;
	lv_obj_t *format_img_obj;
	lv_obj_t *format_label_obj;

	lv_group_t *group;

} UI_SETTINGS_CONTROL_S;

/**********************
 *  STATIC VARIABLES
 **********************/
static UI_SETTINGS_CONTROL_S settings_ctrl;

static int32_t mic = 0;

extern lv_ft_info_t ttf_info_14;
extern lv_ft_info_t ttf_info_12;
extern lv_ft_info_t ttf_info_10;

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/**********************
 *   STATIC FUNCTIONS
 **********************/
static void setting_set_text(void);

static void mode_update(void) {
	if (rk_get_mode() == RK_VIDEO_MODE) {
		rk_set_mode(RK_PHOTO_MODE);
		lv_img_set_src(settings_ctrl.fps_img_obj, index_icon_fps_30);
		lv_img_set_src(settings_ctrl.mode_img_obj, index_icon_capture);
		lv_label_set_text(settings_ctrl.mode_label_obj, "拍照模式");
	} else if (rk_get_mode() == RK_PHOTO_MODE) {
		rk_set_mode(RK_SLOW_MOTION_MODE);
		lv_img_set_src(settings_ctrl.fps_img_obj, index_icon_fps_30);
		lv_img_set_src(settings_ctrl.mode_img_obj, index_icon_slow_motion);
		lv_label_set_text(settings_ctrl.mode_label_obj, "慢动作");
	} else {
		rk_set_mode(RK_VIDEO_MODE);
		lv_img_set_src(settings_ctrl.fps_img_obj, index_icon_fps_30);
		lv_img_set_src(settings_ctrl.mode_img_obj, index_icon_video);
		lv_label_set_text(settings_ctrl.mode_label_obj, "录像模式");
	}
}

static void fps_update(void) {
	int fps = 0;
	fps = rk_video_get_fps();
	if (fps == 30) {
		rk_video_set_fps(58);
		lv_img_set_src(settings_ctrl.fps_img_obj, index_icon_fps_60);
	} else {
		rk_video_set_fps(30);
		lv_img_set_src(settings_ctrl.fps_img_obj, index_icon_fps_30);
	}
	return;
}

static void capture_update(void) {
	if (rk_photo_get_max_num() == 5) {
		lv_img_set_src(settings_ctrl.cap_img_obj, index_icon_5p1s);
	} else if (rk_photo_get_max_num() == 10) {
		lv_img_set_src(settings_ctrl.cap_img_obj, index_icon_10p2s);
	} else {
		lv_img_set_src(settings_ctrl.cap_img_obj, index_icon_20p3s);
	}
}

static void mic_update(void) {
	if (mic)
		lv_img_set_src(settings_ctrl.mic_img_obj, index_icon_mic_02);
	else
		lv_img_set_src(settings_ctrl.mic_img_obj, index_icon_mic_01);
}

static void settings_disk_event_handler(rkipc_mount_status disk_state) {
	ui_format_dialog_destory();
	ui_storage_status_dialog_destroy();
	switch (disk_state) {
	case DISK_UNMOUNTED:
		ui_nosdcard_dialog_create();
		break;
	case DISK_FORMAT_ERR:
	case DISK_NOT_FORMATTED:
		ui_noformat_dialog_create();
		break;
	case DISK_SCANNING:
		ui_scansdcard_dialog_create();
		break;
	case DISK_MOUNTED:
		ui_nosdcard_dialog_destroy();
		ui_noformat_dialog_destroy();
		ui_scansdcard_dialog_destroy();
		break;

	default:
		break;
	}
}

static void format_extry(void) {}

static void format_exit(void) {}

static void mode_event_handler(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *obj = lv_event_get_target(e);

	if (code == LV_EVENT_CLICKED) {
		if (evdev_get_current_code() == KEY_POWER) {
			lv_event_send(settings_ctrl.exit_obj, LV_EVENT_CLICKED, NULL);
			return;
		}
		mode_update();
	} else if (code == LV_EVENT_FOCUSED) {
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_p, NULL);
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	} else if (code == LV_EVENT_DEFOCUSED) {
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_r, NULL);
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	}
}

static void fps_event_handler(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *obj = lv_event_get_target(e);

	if (code == LV_EVENT_CLICKED) {
		if (evdev_get_current_code() == KEY_POWER) {
			lv_event_send(settings_ctrl.exit_obj, LV_EVENT_CLICKED, NULL);
			return;
		}
		if (rk_get_mode() != RK_VIDEO_MODE) {
			ui_dialog_create("WARNING", "请先切换到录像模式!");
			return ;
		}
		fps_update();
	} else if (code == LV_EVENT_FOCUSED) {
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_p, NULL);
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	} else if (code == LV_EVENT_DEFOCUSED) {
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_r, NULL);
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	}
}

static void capture_event_handler(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *obj = lv_event_get_target(e);

	if (code == LV_EVENT_CLICKED) {
		if (evdev_get_current_code() == KEY_POWER) {
			lv_event_send(settings_ctrl.exit_obj, LV_EVENT_CLICKED, NULL);
			return;
		}
		if (rk_get_mode() != RK_PHOTO_MODE) {
			ui_dialog_create("WARNING", "请先切换到拍照模式!");
			return ;
		}
		if (rk_photo_get_max_num() == 5)
			rk_photo_set_max_num(10);
		else if (rk_photo_get_max_num() == 10)
			rk_photo_set_max_num(20);
		else
			rk_photo_set_max_num(5);
		capture_update();
	} else if (code == LV_EVENT_FOCUSED) {
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_p, NULL);
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	} else if (code == LV_EVENT_DEFOCUSED) {
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_r, NULL);
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	}
}

static void mic_event_handler(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *obj = lv_event_get_target(e);

	if (code == LV_EVENT_CLICKED) {
		if (evdev_get_current_code() == KEY_POWER) {
			lv_event_send(settings_ctrl.exit_obj, LV_EVENT_CLICKED, NULL);
			return;
		}
		mic = !mic;
		mic_update();
	} else if (code == LV_EVENT_FOCUSED) {
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_p, NULL);
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	} else if (code == LV_EVENT_DEFOCUSED) {
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_r, NULL);
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	}
}

static void store_event_handler(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *obj = lv_event_get_target(e);

	if (code == LV_EVENT_CLICKED) {
		if (evdev_get_current_code() == KEY_POWER) {
			lv_event_send(settings_ctrl.exit_obj, LV_EVENT_CLICKED, NULL);
			return;
		}
		switch (rkipc_storage_dev_mount_status_get()) {
		case DISK_UNMOUNTED:
			ui_nosdcard_dialog_create();
			break;
		case DISK_FORMAT_ERR:
		case DISK_NOT_FORMATTED:
			ui_noformat_dialog_create();
			break;
		case DISK_SCANNING:
			ui_scansdcard_dialog_create();
			break;
		case DISK_MOUNTED:
			ui_storage_status_dialog_create();
			break;
		default:
			break;
		}
	} else if (code == LV_EVENT_FOCUSED) {
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_p, NULL);
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	} else if (code == LV_EVENT_DEFOCUSED) {
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_r, NULL);
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	}
}

static void format_event_handler(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *obj = lv_event_get_target(e);

	if (code == LV_EVENT_CLICKED) {
		if (evdev_get_current_code() == KEY_POWER) {
			lv_event_send(settings_ctrl.exit_obj, LV_EVENT_CLICKED, NULL);
			return;
		}
		switch (rkipc_storage_dev_mount_status_get()) {
		case DISK_UNMOUNTED:
			ui_nosdcard_dialog_create();
			break;
		case DISK_FORMAT_ERR:
		case DISK_NOT_FORMATTED:
		case DISK_SCANNING:
		case DISK_MOUNTED:
			ui_format_dialog_create(format_extry, format_exit);
			break;
		default:
			break;
		}
	} else if (code == LV_EVENT_FOCUSED) {
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_p, NULL);
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	} else if (code == LV_EVENT_DEFOCUSED) {
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_r, NULL);
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	}
}

static void return_event_handler(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *obj = lv_event_get_target(e);

	if (code == LV_EVENT_CLICKED) {
		ui_page_pop_page();
	}
}

static void setting_layout(void) {
	static lv_coord_t col_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
	                               LV_GRID_TEMPLATE_LAST};
	static lv_coord_t row_dsc[] = {LV_GRID_FR(1), LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_FR(1),
	                               LV_GRID_TEMPLATE_LAST};

	lv_obj_set_layout(settings_ctrl.container_obj, LV_LAYOUT_GRID);

	lv_obj_set_style_grid_column_dsc_array(settings_ctrl.container_obj, col_dsc, 0);
	lv_obj_set_style_grid_row_dsc_array(settings_ctrl.container_obj, row_dsc, 0);

	lv_obj_set_grid_cell(settings_ctrl.exit_obj, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_CENTER, 0,
	                     1);

	lv_obj_set_grid_cell(settings_ctrl.mode_imgbtn_obj, LV_GRID_ALIGN_CENTER, 0, 1,
	                     LV_GRID_ALIGN_CENTER, 1, 1);

	lv_obj_set_grid_cell(settings_ctrl.fps_imgbtn_obj, LV_GRID_ALIGN_CENTER, 1, 1,
	                     LV_GRID_ALIGN_CENTER, 1, 1);

	lv_obj_set_grid_cell(settings_ctrl.cap_imgbtn_obj, LV_GRID_ALIGN_CENTER, 2, 1,
	                     LV_GRID_ALIGN_CENTER, 1, 1);

	lv_obj_set_grid_cell(settings_ctrl.mic_imgbtn_obj, LV_GRID_ALIGN_CENTER, 0, 1,
	                     LV_GRID_ALIGN_CENTER, 2, 1);

	lv_obj_set_grid_cell(settings_ctrl.store_imgbtn_obj, LV_GRID_ALIGN_CENTER, 1, 1,
	                     LV_GRID_ALIGN_CENTER, 2, 1);

	lv_obj_set_grid_cell(settings_ctrl.format_imgbtn_obj, LV_GRID_ALIGN_CENTER, 2, 1,
	                     LV_GRID_ALIGN_CENTER, 2, 1);
}

static void setting_create_ctrl(lv_obj_t *page_obj) {
	lv_color_t bg_color = lv_color_hex(0xFF04171D);
	lv_obj_t *cont_obj = NULL, *imgbtn_obj = NULL, *img_obj = NULL, *label_obj = NULL;

	lv_obj_set_style_bg_opa(page_obj, LV_OPA_TRANSP, 0);
	lv_disp_set_bg_opa(NULL, LV_OPA_TRANSP);

	settings_ctrl.container_obj = cont_obj = lv_obj_create(page_obj);
	lv_obj_set_size(cont_obj, lv_pct(100), lv_pct(100));
	lv_obj_align(cont_obj, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
	lv_obj_set_style_bg_color(cont_obj, bg_color, 0);
	lv_obj_set_style_border_color(cont_obj, bg_color, 0);
	lv_obj_set_style_radius(cont_obj, 0, 0);

	settings_ctrl.exit_obj = lv_btn_create(cont_obj);
	lv_obj_set_style_bg_opa(settings_ctrl.exit_obj, LV_OPA_TRANSP, 0);
	lv_obj_add_event_cb(settings_ctrl.exit_obj, return_event_handler, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(settings_ctrl.exit_obj, return_event_handler, LV_EVENT_FOCUSED, NULL);
	lv_obj_add_event_cb(settings_ctrl.exit_obj, return_event_handler, LV_EVENT_DEFOCUSED, NULL);
	settings_ctrl.exit_label_obj = lv_label_create(settings_ctrl.exit_obj);

	settings_ctrl.mode_imgbtn_obj = imgbtn_obj = lv_imgbtn_create(cont_obj);
	lv_imgbtn_set_src(imgbtn_obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_r, NULL);
	lv_imgbtn_set_src(imgbtn_obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	lv_obj_add_event_cb(imgbtn_obj, mode_event_handler, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(imgbtn_obj, mode_event_handler, LV_EVENT_FOCUSED, NULL);
	lv_obj_add_event_cb(imgbtn_obj, mode_event_handler, LV_EVENT_DEFOCUSED, NULL);
	lv_obj_set_size(imgbtn_obj, index_icon_boxbg_r->header.w, index_icon_boxbg_r->header.h);
	settings_ctrl.mode_img_obj = img_obj = lv_img_create(imgbtn_obj);
	lv_obj_align(img_obj, LV_ALIGN_CENTER, 0, 0);
	if (rk_get_mode() == RK_PHOTO_MODE)
		lv_img_set_src(settings_ctrl.mode_img_obj, index_icon_capture);
	else if (rk_get_mode() == RK_VIDEO_MODE)
		lv_img_set_src(settings_ctrl.mode_img_obj, index_icon_video);
	else
		lv_img_set_src(settings_ctrl.mode_img_obj, index_icon_slow_motion);
	settings_ctrl.mode_label_obj = label_obj = lv_label_create(imgbtn_obj);
	lv_obj_set_width(label_obj, lv_pct(100));
	lv_obj_align_to(label_obj, img_obj, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);

	settings_ctrl.fps_imgbtn_obj = imgbtn_obj = lv_imgbtn_create(cont_obj);
	lv_imgbtn_set_src(imgbtn_obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_r, NULL);
	lv_imgbtn_set_src(imgbtn_obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	lv_obj_add_event_cb(imgbtn_obj, fps_event_handler, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(imgbtn_obj, fps_event_handler, LV_EVENT_FOCUSED, NULL);
	lv_obj_add_event_cb(imgbtn_obj, fps_event_handler, LV_EVENT_DEFOCUSED, NULL);
	lv_obj_set_size(imgbtn_obj, index_icon_boxbg_r->header.w, index_icon_boxbg_r->header.h);
	settings_ctrl.fps_img_obj = img_obj = lv_img_create(imgbtn_obj);
	int fps = rk_param_get_int("isp.0.adjustment:fps", 30);
	if (fps == 30)
		lv_img_set_src(settings_ctrl.fps_img_obj, index_icon_fps_30);
	else
		lv_img_set_src(settings_ctrl.fps_img_obj, index_icon_fps_60);
	lv_obj_align(img_obj, LV_ALIGN_CENTER, 0, 0);
	settings_ctrl.fps_label_obj = label_obj = lv_label_create(imgbtn_obj);
	lv_obj_set_width(label_obj, lv_pct(100));
	lv_obj_align_to(label_obj, img_obj, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);

	settings_ctrl.cap_imgbtn_obj = imgbtn_obj = lv_imgbtn_create(cont_obj);
	lv_imgbtn_set_src(imgbtn_obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_r, NULL);
	lv_imgbtn_set_src(imgbtn_obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	lv_obj_add_event_cb(imgbtn_obj, capture_event_handler, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(imgbtn_obj, capture_event_handler, LV_EVENT_FOCUSED, NULL);
	lv_obj_add_event_cb(imgbtn_obj, capture_event_handler, LV_EVENT_DEFOCUSED, NULL);
	lv_obj_set_size(imgbtn_obj, index_icon_boxbg_r->header.w, index_icon_boxbg_r->header.h);
	settings_ctrl.cap_img_obj = img_obj = lv_img_create(imgbtn_obj);
	lv_obj_align(img_obj, LV_ALIGN_CENTER, 0, 0);
	capture_update();
	settings_ctrl.cap_label_obj = label_obj = lv_label_create(imgbtn_obj);
	lv_obj_set_width(label_obj, lv_pct(100));
	lv_obj_align_to(label_obj, img_obj, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);

	settings_ctrl.mic_imgbtn_obj = imgbtn_obj = lv_imgbtn_create(cont_obj);
	lv_imgbtn_set_src(imgbtn_obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_r, NULL);
	lv_imgbtn_set_src(imgbtn_obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	lv_obj_add_event_cb(imgbtn_obj, mic_event_handler, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(imgbtn_obj, mic_event_handler, LV_EVENT_FOCUSED, NULL);
	lv_obj_add_event_cb(imgbtn_obj, mic_event_handler, LV_EVENT_DEFOCUSED, NULL);
	lv_obj_set_size(imgbtn_obj, index_icon_boxbg_r->header.w, index_icon_boxbg_r->header.h);
	settings_ctrl.mic_img_obj = img_obj = lv_img_create(imgbtn_obj);
	mic_update();
	lv_obj_align(img_obj, LV_ALIGN_CENTER, 0, 0);
	settings_ctrl.mic_label_obj = label_obj = lv_label_create(imgbtn_obj);
	lv_obj_set_width(label_obj, lv_pct(100));
	lv_obj_align_to(label_obj, img_obj, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);

	settings_ctrl.store_imgbtn_obj = imgbtn_obj = lv_imgbtn_create(cont_obj);
	lv_imgbtn_set_src(imgbtn_obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_r, NULL);
	lv_imgbtn_set_src(imgbtn_obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	lv_obj_add_event_cb(imgbtn_obj, store_event_handler, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(imgbtn_obj, store_event_handler, LV_EVENT_FOCUSED, NULL);
	lv_obj_add_event_cb(imgbtn_obj, store_event_handler, LV_EVENT_DEFOCUSED, NULL);
	lv_obj_set_size(imgbtn_obj, index_icon_boxbg_r->header.w, index_icon_boxbg_r->header.h);
	settings_ctrl.store_img_obj = img_obj = lv_img_create(imgbtn_obj);
	lv_img_set_src(img_obj, index_icon_store_01);
	lv_obj_align(img_obj, LV_ALIGN_CENTER, 0, 0);
	settings_ctrl.store_label_obj = label_obj = lv_label_create(imgbtn_obj);
	lv_obj_set_width(label_obj, lv_pct(100));
	lv_obj_align_to(label_obj, img_obj, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);

	settings_ctrl.format_imgbtn_obj = imgbtn_obj = lv_imgbtn_create(cont_obj);
	lv_imgbtn_set_src(imgbtn_obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_r, NULL);
	lv_imgbtn_set_src(imgbtn_obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	lv_obj_add_event_cb(imgbtn_obj, format_event_handler, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(imgbtn_obj, format_event_handler, LV_EVENT_FOCUSED, NULL);
	lv_obj_add_event_cb(imgbtn_obj, format_event_handler, LV_EVENT_DEFOCUSED, NULL);
	lv_obj_set_size(imgbtn_obj, index_icon_boxbg_r->header.w, index_icon_boxbg_r->header.h);
	settings_ctrl.format_img_obj = img_obj = lv_img_create(imgbtn_obj);
	lv_img_set_src(img_obj, index_icon_format_01);
	lv_obj_align(img_obj, LV_ALIGN_CENTER, 0, 0);
	settings_ctrl.format_label_obj = label_obj = lv_label_create(imgbtn_obj);
	lv_obj_set_width(label_obj, lv_pct(100));
	lv_obj_align_to(label_obj, img_obj, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
}

static void setting_set_text() {
	static lv_style_t style, symbol_style;
	lv_color_t text_color = lv_color_hex(0xFFFFFF);

	lv_style_reset(&style);
	lv_style_init(&style);
	lv_style_set_text_font(&style, ttf_info_10.font);
	lv_style_set_text_color(&style, text_color);
	lv_style_set_text_align(&style, LV_TEXT_ALIGN_CENTER);

	lv_style_reset(&symbol_style);
	lv_style_init(&symbol_style);
	lv_style_set_text_font(&symbol_style, &lv_font_montserrat_20);
	lv_style_set_text_color(&symbol_style, text_color);
	lv_style_set_text_align(&symbol_style, LV_TEXT_ALIGN_CENTER);

	if (rk_get_mode() == RK_VIDEO_MODE)
		lv_label_set_text(settings_ctrl.mode_label_obj, "录像模式");
	else if (rk_get_mode() == RK_PHOTO_MODE)
		lv_label_set_text(settings_ctrl.mode_label_obj, "拍照模式");
	else
		lv_label_set_text(settings_ctrl.mode_label_obj, "慢动作");

	lv_obj_add_style(settings_ctrl.mode_label_obj, &style, 0);

	lv_label_set_text(settings_ctrl.fps_label_obj, "录像配置");
	lv_obj_add_style(settings_ctrl.fps_label_obj, &style, 0);

	lv_label_set_text(settings_ctrl.cap_label_obj, "连拍模式");
	lv_obj_add_style(settings_ctrl.cap_label_obj, &style, 0);

	lv_label_set_text(settings_ctrl.mic_label_obj, "录音");
	lv_obj_add_style(settings_ctrl.mic_label_obj, &style, 0);

	lv_label_set_text(settings_ctrl.store_label_obj, "存储状态");
	lv_obj_add_style(settings_ctrl.store_label_obj, &style, 0);

	lv_label_set_text(settings_ctrl.format_label_obj, "格式化");
	lv_obj_add_style(settings_ctrl.format_label_obj, &style, 0);

	lv_label_set_text(settings_ctrl.exit_label_obj, LV_SYMBOL_CLOSE);
	lv_obj_add_style(settings_ctrl.exit_label_obj, &symbol_style, 0);
}

static void setting_destroy_ctrl(void) {
	if (NULL != settings_ctrl.mode_imgbtn_obj) {
		ui_common_remove_stype_all(settings_ctrl.mode_imgbtn_obj);
		lv_obj_del(settings_ctrl.mode_imgbtn_obj);
		settings_ctrl.mode_imgbtn_obj = NULL;
	}
	if (NULL != settings_ctrl.fps_imgbtn_obj) {
		ui_common_remove_stype_all(settings_ctrl.fps_imgbtn_obj);
		lv_obj_del(settings_ctrl.fps_imgbtn_obj);
		settings_ctrl.fps_imgbtn_obj = NULL;
	}
	if (NULL != settings_ctrl.cap_imgbtn_obj) {
		ui_common_remove_stype_all(settings_ctrl.cap_imgbtn_obj);
		lv_obj_del(settings_ctrl.cap_imgbtn_obj);
		settings_ctrl.cap_imgbtn_obj = NULL;
	}
	if (NULL != settings_ctrl.mic_imgbtn_obj) {
		ui_common_remove_stype_all(settings_ctrl.mic_imgbtn_obj);
		lv_obj_del(settings_ctrl.mic_imgbtn_obj);
		settings_ctrl.mic_imgbtn_obj = NULL;
	}
	if (NULL != settings_ctrl.store_imgbtn_obj) {
		ui_common_remove_stype_all(settings_ctrl.store_imgbtn_obj);
		lv_obj_del(settings_ctrl.store_imgbtn_obj);
		settings_ctrl.exit_obj = NULL;
	}
	if (NULL != settings_ctrl.format_imgbtn_obj) {
		ui_common_remove_stype_all(settings_ctrl.format_imgbtn_obj);
		lv_obj_del(settings_ctrl.format_imgbtn_obj);
		settings_ctrl.format_imgbtn_obj = NULL;
	}
	if (NULL != settings_ctrl.exit_obj) {
		ui_common_remove_stype_all(settings_ctrl.exit_obj);
		lv_obj_del(settings_ctrl.exit_obj);
		settings_ctrl.exit_obj = NULL;
	}
	if (NULL != settings_ctrl.container_obj) {
		ui_common_remove_stype_all(settings_ctrl.container_obj);
		lv_obj_del(settings_ctrl.container_obj);
		settings_ctrl.container_obj = NULL;
	}
}

static void settings_page_create(lv_obj_t *page_obj) {

	setting_create_ctrl(page_obj);
	setting_set_text();
	setting_layout();
}

static void settings_add_indev(void) {
	lv_group_t *group = lv_port_indev_group_create();
	if (NULL == group)
		return;

	lv_group_add_obj(group, settings_ctrl.exit_obj);
	lv_group_add_obj(group, settings_ctrl.mode_imgbtn_obj);
	lv_group_add_obj(group, settings_ctrl.fps_imgbtn_obj);
	lv_group_add_obj(group, settings_ctrl.cap_imgbtn_obj);
	lv_group_add_obj(group, settings_ctrl.mic_imgbtn_obj);
	lv_group_add_obj(group, settings_ctrl.store_imgbtn_obj);
	lv_group_add_obj(group, settings_ctrl.format_imgbtn_obj);

	settings_ctrl.group = group;
}

static void settings_delete_indev(void) {
	if (NULL != settings_ctrl.group) {
		lv_port_indev_group_destroy(settings_ctrl.group);
		settings_ctrl.group = NULL;
	}
}

static void settings_page_enter(lv_obj_t *page_obj) { settings_add_indev(); }

static void settings_page_exit(lv_obj_t *page_obj) {}

static void settings_page_destroy(lv_obj_t *page_obj) {
	settings_delete_indev();
	setting_destroy_ctrl();
}

static UI_PAGE_HANDLER_T settings_page = {.name = "settings",
                                          .init = NULL,
                                          .create = settings_page_create,
                                          .enter = settings_page_enter,
                                          .destroy = settings_page_destroy,
                                          .exit = settings_page_exit};

UI_PAGE_REGISTER(settings_page)
