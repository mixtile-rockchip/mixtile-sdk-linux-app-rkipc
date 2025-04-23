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
#define LOG_TAG "ui_advanced_setup.c"

#include "audio.h"
#include "common.h"
#include "evdev.h"
#include "lvgl/porting/lv_port_indev.h"
#include "storage.h"
#include "ui_common.h"
#include "ui_dialog.h"
#include "ui_dialog_format.h"
#include "ui_dialog_storage.h"
#include "ui_page_manager.h"
#include "ui_player.h"
#include "ui_resource_manager.h"
#include "video.h"
#include <linux/input.h>

/**********************
 *  STATIC PROTOTYPES
 **********************/
typedef struct {
	lv_obj_t *container_obj;

	lv_obj_t *exit_obj;
	lv_obj_t *exit_label_obj;
	lv_obj_t *comp_enable_obj;
	lv_obj_t *rtsp_enable_obj;
	lv_obj_t *hdr_test_obj;
	lv_obj_t *hdr_test_label_obj;
	lv_obj_t *eis_test_obj;
	lv_obj_t *eis_test_label_obj;
	lv_group_t *group;

} UI_ADVANCED_SETUP_CONTROL_S;

static UI_ADVANCED_SETUP_CONTROL_S advanced_setup_ctrl;

extern lv_ft_info_t ttf_info_32;

static void return_event_handler(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *obj = lv_event_get_target(e);

	if (code == LV_EVENT_CLICKED) {
		ui_page_pop_page();
	}
}

static int comp_set_result = 0;
static void set_compress_func(void *btn) {
	bool comp_state = rk_get_compress();
	if (comp_state)
		comp_set_result = rk_set_compress(false);
	else
		comp_set_result = rk_set_compress(true);
	return ;
}

static void set_done_func(void *arg) {
	if (comp_set_result != 0)
		ui_dialog_create("WARNING", "切换压缩模式失败!");
	else
		ui_dialog_create("INFO", "切换压缩模式成功!");
}

static lv_timer_t *test_timer = NULL;
static lv_obj_t *test_dialog = NULL;
static lv_obj_t *test_status_obj = NULL;
static int test_total_count = 0;
static int test_success_count = 0;
static int test_fail_count = 0;

static pthread_t test_task_id;

static void create_test_dialog(const char *title) {
	lv_obj_t *obj, *obj_bg, *context_obj;
	lv_color_t color;
	lv_color_t text_color;
	lv_color_t dialog_color = {
		.full = 0xFF222D30,
	};
	static lv_style_t style;

	text_color.full = 0xffffffff;
	color.full = 0xFF000000;

	lv_style_reset(&style);
	lv_style_init(&style);
	lv_style_set_layout(&style, LV_LAYOUT_FLEX);
	lv_style_set_flex_flow(&style, LV_FLEX_FLOW_COLUMN_WRAP);
	lv_style_set_flex_main_place(&style, LV_FLEX_ALIGN_SPACE_EVENLY);
	lv_style_set_flex_cross_place(&style, LV_FLEX_ALIGN_CENTER);

	test_dialog = obj = lv_obj_create(lv_scr_act());
	lv_obj_set_size(obj, lv_pct(100), lv_pct(100));
	lv_obj_align(obj, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_style_bg_color(obj, color, 0);
	lv_obj_set_style_border_color(obj, color, 0);
	lv_obj_set_style_radius(obj, 0, 0);
	lv_obj_set_style_bg_opa(obj, LV_OPA_50, 0);

	obj_bg = obj = lv_obj_create(obj);
	lv_obj_set_size(obj, lv_pct(50), lv_pct(40));
	lv_obj_set_style_bg_color(obj, dialog_color, 0);
	lv_obj_set_style_border_color(obj, dialog_color, 0);
	lv_obj_set_style_radius(obj, 0, 0);
	lv_obj_set_style_bg_opa(obj, LV_OPA_80, 0);
	lv_obj_align(obj, LV_ALIGN_CENTER, 0, 0);
	lv_obj_add_style(obj, &style, 0);

	obj = lv_label_create(obj_bg);
	lv_obj_set_style_text_font(obj, ttf_info_32.font, 0);
	lv_obj_set_style_text_color(obj, text_color, 0);
	lv_obj_set_size(obj, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_text(obj, title);
	lv_label_set_long_mode(obj, LV_LABEL_LONG_SCROLL_CIRCULAR);

	obj = lv_label_create(obj_bg);
	lv_label_set_text(obj, LV_SYMBOL_WARNING);
	lv_obj_set_style_text_font(obj, &lv_font_montserrat_48, 0);
	lv_obj_set_style_text_color(obj, lv_palette_main(LV_PALETTE_BLUE), 0);
	lv_obj_add_flag(obj, LV_OBJ_FLAG_IGNORE_LAYOUT);
	lv_obj_align(obj, LV_ALIGN_TOP_RIGHT, 0, 0);

	context_obj = obj = lv_obj_create(obj_bg);
	lv_obj_set_width(obj, lv_pct(100));
	lv_obj_set_style_radius(obj, 0, 0);
	lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_TOP, 0);
	lv_obj_add_style(obj, &style, 0);
	lv_obj_set_flex_grow(obj, 1);

	text_color.full = 0xffffffff;
	test_status_obj = obj = lv_label_create(context_obj);
	lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_font(obj, ttf_info_32.font, 0);
	lv_obj_set_style_text_color(obj, text_color, 0);
	lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_width(obj, lv_pct(100));
}

static void test_timer_func(lv_timer_t *timer) {
	if (test_total_count == (test_success_count + test_fail_count) && test_total_count > 0) {
		if (test_status_obj) {
			lv_obj_del(test_status_obj);
			test_status_obj = NULL;
		}
		if (test_dialog) {
			lv_obj_del(test_dialog);
			test_dialog = NULL;
		}
		lv_timer_del(test_timer);
		pthread_join(test_task_id, NULL);
		test_timer = NULL;
		test_total_count = 0;
		test_success_count = 0;
		test_fail_count = 0;
		test_task_id = 0;
		ui_dialog_create("INFO", "测试完成!");
	}
	if (test_status_obj) {
		static char info[64];
		snprintf(info, sizeof(info), "TOTAL: %d, SUCCESS: %d, FAIL: %d", 
		         test_total_count, test_success_count, test_fail_count);
		lv_label_set_text(test_status_obj, info);
	}
}

static void* hdr_test_func(void *arg) {
	test_total_count = rk_param_get_int("video.source:hdr_test_loop_count", 30);
	int hdr_mode = 0;
	int ret = 0;
	for (int i = 0; i < test_total_count; i++) {
		hdr_mode = (hdr_mode + 1) % RK_HDR_MODE_NUM;
		ret = rk_set_hdr(hdr_mode);
		usleep(1000 * 1000);
		if (ret != 0) {
			LOG_ERROR("=============== loop %d, set hdr mode %d failed! ===============\n", i, hdr_mode);
			++test_fail_count;
		} else {
			LOG_INFO("=============== loop %d, set hdr mode %d success! ===============\n", i, hdr_mode);
			++test_success_count;
		}
	}
	return NULL;
}

static void* eis_test_func(void *arg) {
	test_total_count = rk_param_get_int("video.source:eis_test_loop_count", 20);
	int eis_mode = 0;
	int ret = 0;
	for (int i = 0; i < test_total_count; i++) {
		eis_mode = (eis_mode + 1) % RK_EIS_MODE_NUM;
		ret = rk_set_eis_mode(eis_mode);
		usleep(1000 * 1000);
		if (ret != 0) {
			LOG_ERROR("=============== loop %d, set eis mode %d failed! ===============\n", i, eis_mode);
			++test_fail_count;
		} else {
			LOG_INFO("=============== loop %d, set eis mode %d success! ===============\n", i, eis_mode);
			++test_success_count;
		}
	}
	return NULL;
}

static void event_handler(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *obj = lv_event_get_target(e);
	if (code == LV_EVENT_CLICKED) {
		if (obj == advanced_setup_ctrl.comp_enable_obj) {
			ui_start_async_task(set_compress_func, NULL, set_done_func, NULL);
		} else if (obj == advanced_setup_ctrl.rtsp_enable_obj) {
			if (lv_obj_has_state(obj, LV_STATE_CHECKED)) {
				rk_set_rtsp(true);
			} else {
				rk_set_rtsp(false);
			}
		} else if (obj == advanced_setup_ctrl.hdr_test_obj) {
			create_test_dialog("HDR切换测试");
			test_timer = lv_timer_create(test_timer_func, 500, NULL);
			pthread_create(&test_task_id, NULL, hdr_test_func, NULL);
		} else if (obj == advanced_setup_ctrl.eis_test_obj) {
			create_test_dialog("EIS切换测试");
			test_timer = lv_timer_create(test_timer_func, 500, NULL);
			pthread_create(&test_task_id, NULL, eis_test_func, NULL);
		} else {
			LOG_ERROR("unknown event\n");
		}
	}
}

static void advanced_setup_layout(lv_obj_t *page_obj) {
	lv_obj_set_flex_flow(advanced_setup_ctrl.container_obj, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(advanced_setup_ctrl.container_obj, LV_FLEX_ALIGN_START,
	                      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollbar_mode(advanced_setup_ctrl.container_obj, LV_SCROLLBAR_MODE_OFF);
}

static void advanced_setup_create_ctrl(lv_obj_t *page_obj) {
	lv_color_t bg_color = lv_color_hex(0xFF04171D);
	lv_obj_t *cont_obj = NULL, *obj = NULL;

	lv_disp_set_bg_opa(NULL, LV_OPA_TRANSP);
	lv_obj_set_style_bg_opa(page_obj, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(page_obj, bg_color, 0);

	advanced_setup_ctrl.exit_obj = lv_btn_create(page_obj);
	lv_obj_set_style_bg_opa(advanced_setup_ctrl.exit_obj, LV_OPA_TRANSP, 0);
	lv_obj_add_event_cb(advanced_setup_ctrl.exit_obj, return_event_handler, LV_EVENT_CLICKED, NULL);
	advanced_setup_ctrl.exit_label_obj = lv_label_create(advanced_setup_ctrl.exit_obj);

	advanced_setup_ctrl.container_obj = cont_obj = lv_obj_create(page_obj);
	lv_obj_set_size(cont_obj, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_opa(cont_obj, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_opa(cont_obj, LV_OPA_TRANSP, 0);
	lv_obj_set_style_radius(cont_obj, 0, 0);

	advanced_setup_ctrl.comp_enable_obj = obj = lv_checkbox_create(cont_obj);
	lv_obj_add_flag(obj, LV_OBJ_FLAG_CHECKABLE);
	lv_obj_set_size(obj, lv_pct(100), lv_pct(10));
	lv_obj_add_event_cb(obj, event_handler, LV_EVENT_CLICKED, NULL);

	advanced_setup_ctrl.rtsp_enable_obj = obj = lv_checkbox_create(cont_obj);
	lv_obj_add_flag(obj, LV_OBJ_FLAG_CHECKABLE);
	lv_obj_set_size(obj, lv_pct(100), lv_pct(10));
	lv_obj_add_event_cb(obj, event_handler, LV_EVENT_CLICKED, NULL);

	advanced_setup_ctrl.hdr_test_obj = obj = lv_obj_create(cont_obj);
	lv_obj_set_size(obj, lv_pct(100), lv_pct(10));
	lv_obj_add_event_cb(obj, event_handler, LV_EVENT_CLICKED, NULL);
	advanced_setup_ctrl.hdr_test_label_obj = lv_label_create(obj);

	advanced_setup_ctrl.eis_test_obj = obj = lv_obj_create(cont_obj);
	lv_obj_set_size(obj, lv_pct(100), lv_pct(10));
	lv_obj_add_event_cb(obj, event_handler, LV_EVENT_CLICKED, NULL);
	advanced_setup_ctrl.eis_test_label_obj = lv_label_create(obj);
}

static void advanced_setup_set_text() {
	static lv_style_t symbol_style, symbol_focus_style, option_style, option_focus_style;
	// INFO: make sure to get the right heigh of obj
	lv_obj_update_layout(advanced_setup_ctrl.comp_enable_obj);
	int font_height = lv_font_get_line_height(ttf_info_32.font);
	int option_height = lv_obj_get_height(advanced_setup_ctrl.comp_enable_obj);
	// INFO: adjust the padding of text, because set alignment function is not work
	int pad = (option_height - font_height) / 2;
	lv_style_reset(&option_style);
	lv_style_init(&option_style);
	lv_style_set_bg_opa(&option_style, LV_OPA_COVER);
	lv_style_set_bg_color(&option_style, lv_palette_lighten(LV_PALETTE_GREY, 3));
	lv_style_set_text_color(&option_style, lv_color_hex(0x000000));
	lv_style_set_text_font(&option_style, ttf_info_32.font);
	lv_style_set_text_align(&option_style, LV_TEXT_ALIGN_CENTER);
	lv_style_set_radius(&option_style, 5);
	lv_style_set_border_opa(&option_style, LV_OPA_TRANSP);
	lv_style_set_pad_top(&option_style, pad);

	lv_style_reset(&option_focus_style);
	lv_style_init(&option_focus_style);
	lv_style_set_bg_opa(&option_focus_style, LV_OPA_COVER);
	lv_style_set_bg_color(&option_focus_style, lv_palette_main(LV_PALETTE_GREY));
	lv_style_set_text_color(&option_focus_style, lv_color_hex(0x000000));
	lv_style_set_text_align(&option_focus_style, LV_TEXT_ALIGN_CENTER);
	lv_style_set_text_font(&option_focus_style, ttf_info_32.font);
	lv_style_set_radius(&option_focus_style, 5);
	lv_style_set_border_opa(&option_focus_style, LV_OPA_TRANSP);
	lv_style_set_outline_opa(&option_focus_style, LV_OPA_TRANSP);
	lv_style_set_outline_width(&option_focus_style, 0);
	lv_style_set_align(&option_focus_style, LV_ALIGN_CENTER);
	lv_style_set_pad_top(&option_focus_style, pad);

	lv_style_reset(&symbol_style);
	lv_style_init(&symbol_style);
	lv_style_set_bg_opa(&symbol_style, LV_OPA_TRANSP);
	lv_style_set_text_font(&symbol_style, &lv_font_montserrat_48);
	lv_style_set_text_color(&symbol_style, lv_color_hex(0xFFFFFF));
	lv_style_set_text_align(&symbol_style, LV_TEXT_ALIGN_LEFT);
	lv_style_set_outline_opa(&symbol_style, LV_OPA_TRANSP);
	lv_style_set_outline_width(&symbol_style, 0);
	lv_style_set_pad_all(&symbol_style, 30);

	lv_style_reset(&symbol_focus_style);
	lv_style_init(&symbol_focus_style);
	lv_style_set_bg_opa(&symbol_focus_style, LV_OPA_TRANSP);
	lv_style_set_text_font(&symbol_focus_style, &lv_font_montserrat_48);
	lv_style_set_text_color(&symbol_focus_style, lv_palette_main(LV_PALETTE_GREY));
	lv_style_set_text_align(&symbol_focus_style, LV_TEXT_ALIGN_LEFT);
	lv_style_set_outline_opa(&symbol_focus_style, LV_OPA_TRANSP);
	lv_style_set_outline_width(&symbol_focus_style, 0);
	lv_style_set_pad_all(&symbol_focus_style, 30);

	lv_obj_remove_style_all(advanced_setup_ctrl.exit_obj);
	lv_label_set_text(advanced_setup_ctrl.exit_label_obj, LV_SYMBOL_LEFT);
	lv_obj_add_style(advanced_setup_ctrl.exit_obj, &symbol_style, 0);
	lv_obj_add_style(advanced_setup_ctrl.exit_obj, &symbol_focus_style, LV_STATE_FOCUSED);

	// INFO: set align to the left of the parent after set text
	lv_obj_align_to(advanced_setup_ctrl.container_obj
		, advanced_setup_ctrl.exit_obj, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 0);

	lv_checkbox_set_text(advanced_setup_ctrl.comp_enable_obj, "开启录像数据流压缩功能");
	lv_obj_add_style(advanced_setup_ctrl.comp_enable_obj, &option_style, 0);
	lv_obj_add_style(advanced_setup_ctrl.comp_enable_obj, &option_focus_style, LV_PART_MAIN | LV_STATE_FOCUSED);
	lv_obj_set_style_radius(advanced_setup_ctrl.comp_enable_obj, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
	if (rk_get_compress())
		lv_obj_add_state(advanced_setup_ctrl.comp_enable_obj, LV_STATE_CHECKED);

	lv_checkbox_set_text(advanced_setup_ctrl.rtsp_enable_obj, "开启RTSP");
	lv_obj_add_style(advanced_setup_ctrl.rtsp_enable_obj, &option_style, 0);
	lv_obj_add_style(advanced_setup_ctrl.rtsp_enable_obj, &option_focus_style, LV_PART_MAIN | LV_STATE_FOCUSED);
	lv_obj_set_style_radius(advanced_setup_ctrl.rtsp_enable_obj, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
	if (rk_get_rtsp())
		lv_obj_add_state(advanced_setup_ctrl.rtsp_enable_obj, LV_STATE_CHECKED);

	lv_obj_add_style(advanced_setup_ctrl.hdr_test_obj, &option_style, 0);
	lv_obj_add_style(advanced_setup_ctrl.hdr_test_obj, &option_focus_style,
	                 LV_PART_MAIN | LV_STATE_FOCUSED);
	lv_label_set_text(advanced_setup_ctrl.hdr_test_label_obj, "HDR测试");

	lv_obj_add_style(advanced_setup_ctrl.eis_test_obj, &option_style, 0);
	lv_obj_add_style(advanced_setup_ctrl.eis_test_obj, &option_focus_style,
	                 LV_PART_MAIN | LV_STATE_FOCUSED);
	lv_label_set_text(advanced_setup_ctrl.eis_test_label_obj, "EIS测试");
}

static void advanced_setup_destroy_ctrl(void) {
	if (NULL != advanced_setup_ctrl.hdr_test_obj) {
		ui_common_remove_stype_all(advanced_setup_ctrl.hdr_test_obj);
		lv_obj_del(advanced_setup_ctrl.hdr_test_obj);
		advanced_setup_ctrl.hdr_test_obj = NULL;
	}
	if (NULL != advanced_setup_ctrl.eis_test_obj) {
		ui_common_remove_stype_all(advanced_setup_ctrl.eis_test_obj);
		lv_obj_del(advanced_setup_ctrl.eis_test_obj);
		advanced_setup_ctrl.eis_test_obj = NULL;
	}
	if (NULL != advanced_setup_ctrl.comp_enable_obj) {
		ui_common_remove_stype_all(advanced_setup_ctrl.comp_enable_obj);
		lv_obj_del(advanced_setup_ctrl.comp_enable_obj);
		advanced_setup_ctrl.comp_enable_obj = NULL;
	}
	if (NULL != advanced_setup_ctrl.rtsp_enable_obj) {
		ui_common_remove_stype_all(advanced_setup_ctrl.rtsp_enable_obj);
		lv_obj_del(advanced_setup_ctrl.rtsp_enable_obj);
		advanced_setup_ctrl.rtsp_enable_obj = NULL;
	}
	if (NULL != advanced_setup_ctrl.exit_obj) {
		ui_common_remove_stype_all(advanced_setup_ctrl.exit_obj);
		lv_obj_del(advanced_setup_ctrl.exit_obj);
		advanced_setup_ctrl.exit_obj = NULL;
	}
	if (NULL != advanced_setup_ctrl.container_obj) {
		ui_common_remove_stype_all(advanced_setup_ctrl.container_obj);
		lv_obj_del(advanced_setup_ctrl.container_obj);
		advanced_setup_ctrl.container_obj = NULL;
	}
}

static void advanced_setup_page_create(lv_obj_t *page_obj) {

	advanced_setup_create_ctrl(page_obj);
	advanced_setup_layout(page_obj);
	advanced_setup_set_text();
}

static void advanced_setup_add_indev(void) {
	lv_group_t *group = lv_port_indev_group_create();
	if (NULL == group)
		return;
	lv_group_add_obj(group, advanced_setup_ctrl.exit_obj);
	lv_group_add_obj(group, advanced_setup_ctrl.comp_enable_obj);
	lv_group_add_obj(group, advanced_setup_ctrl.rtsp_enable_obj);
	lv_group_add_obj(group, advanced_setup_ctrl.hdr_test_obj);
	lv_group_add_obj(group, advanced_setup_ctrl.eis_test_obj);
	advanced_setup_ctrl.group = group;
}

static void advanced_setup_delete_indev(void) {
	if (NULL != advanced_setup_ctrl.group) {
		lv_port_indev_group_destroy(advanced_setup_ctrl.group);
		advanced_setup_ctrl.group = NULL;
	}
}

static void advanced_setup_page_enter(lv_obj_t *page_obj) { advanced_setup_add_indev(); }

static void advanced_setup_page_exit(lv_obj_t *page_obj) {}

static void advanced_setup_page_destroy(lv_obj_t *page_obj) {
	advanced_setup_delete_indev();
	advanced_setup_destroy_ctrl();
}

static UI_PAGE_HANDLER_T advanced_setup_page = {.name = "advanced_setup",
                                                .init = NULL,
                                                .create = advanced_setup_page_create,
                                                .enter = advanced_setup_page_enter,
                                                .destroy = advanced_setup_page_destroy,
                                                .exit = advanced_setup_page_exit};

UI_PAGE_REGISTER(advanced_setup_page)
