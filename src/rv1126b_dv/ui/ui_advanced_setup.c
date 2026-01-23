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
#include "media_ctrl.h"
#include "storage.h"
#include "ui_common.h"
#include "ui_dialog.h"
#include "ui_dialog_format.h"
#include "ui_dialog_storage.h"
#include "ui_page_manager.h"
#include "ui_player.h"
#include "ui_resource_manager.h"
#include <linux/input.h>
#include <stdio.h>
#include <time.h>

static struct {
	lv_obj_t *container_obj;
	lv_obj_t *exit_obj;
	lv_obj_t *exit_label_obj;
	lv_obj_t *rtsp_enable_obj;
	lv_obj_t *prev_mode_obj;
	lv_obj_t *prev_label_obj;
	lv_obj_t *storage_type_obj;
	lv_obj_t *storage_label_obj;
	lv_group_t *group;
	bool need_reinit;
} advanced_setup_ctrl;

extern lv_ft_info_t ttf_info_32;

static void update_setting_func(void *arg) {
	int ret = rk_media_reinit();
	if (ret < 0)
		LOG_ERROR("rk_media_reinit failed\n");
	else
		LOG_INFO("rk_media_reinit success\n");
}

static void exit_func(void *userdata) { ui_page_pop_page(); }

static void ui_dialog_create_func(void *msg) { ui_dialog_create("WARNING", (const char *)msg); }

static void prev_config_cb(void *userdata) {
	int ret = 0;
	RK_PREVIEW_MODE_E mode = (RK_PREVIEW_MODE_E)(size_t)userdata;
	ret = rk_set_prev_mode(mode);
	if (ret != 0) {
		lv_async_call(ui_dialog_create_func, "切换预览分辨率失败!");
	} else {
		lv_async_call(ui_dialog_create_func, "切换预览分辨率成功!");
		advanced_setup_ctrl.need_reinit = true;
	}
}

static const ui_option_entry_t prev_options[] = {
    {"320x240", prev_config_cb, (void *)PREVIEW_MODE_320x240},
    {"1440x1080", prev_config_cb, (void *)PREVIEW_MODE_1440x1080},
    {"1920x1080", prev_config_cb, (void *)PREVIEW_MODE_1920x1080},
};

static void storage_config_cb(void *userdata) {
	RK_STORAGE_TYPE_E type = (RK_STORAGE_TYPE_E)(size_t)userdata;
	rk_set_storage_type(type);
}

static const ui_option_entry_t storage_options[] = {
    {"SD卡", storage_config_cb, (void *)RK_STORAGE_EXTERNAL},
    {"内置存储", storage_config_cb, (void *)RK_STORAGE_INTERNAL},
};

static void event_handler(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *obj = lv_event_get_target(e);
	if (code == LV_EVENT_CLICKED) {
		if (obj == advanced_setup_ctrl.rtsp_enable_obj) {
			if (lv_obj_has_state(obj, LV_STATE_CHECKED)) {
				rk_set_rtsp(true);
			} else {
				rk_set_rtsp(false);
			}
			advanced_setup_ctrl.need_reinit = true;
		} else if (obj == advanced_setup_ctrl.exit_obj) {
			if (advanced_setup_ctrl.need_reinit)
				ui_start_async_task(update_setting_func, NULL, exit_func, NULL);
			else
				ui_page_pop_page();
		} else if (obj == advanced_setup_ctrl.prev_mode_obj) {
			create_select_box("预览分辨率配置", prev_options,
			                  sizeof(prev_options) / sizeof(prev_options[0]), rk_get_prev_mode());
		} else if (obj == advanced_setup_ctrl.storage_type_obj) {
			create_select_box("存储设置", storage_options,
			                  sizeof(storage_options) / sizeof(storage_options[0]), rk_get_storage_type());
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
	lv_obj_add_event_cb(advanced_setup_ctrl.exit_obj, event_handler, LV_EVENT_CLICKED, NULL);
	advanced_setup_ctrl.exit_label_obj = lv_label_create(advanced_setup_ctrl.exit_obj);

	advanced_setup_ctrl.container_obj = cont_obj = lv_obj_create(page_obj);
	lv_obj_set_size(cont_obj, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_opa(cont_obj, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_opa(cont_obj, LV_OPA_TRANSP, 0);
	lv_obj_set_style_radius(cont_obj, 0, 0);

	advanced_setup_ctrl.rtsp_enable_obj = obj = lv_checkbox_create(cont_obj);
	lv_obj_add_flag(obj, LV_OBJ_FLAG_CHECKABLE);
	lv_obj_set_size(obj, lv_pct(100), lv_pct(10));
	lv_obj_add_event_cb(obj, event_handler, LV_EVENT_CLICKED, NULL);

	advanced_setup_ctrl.prev_mode_obj = lv_btn_create(cont_obj);
	lv_obj_set_size(advanced_setup_ctrl.prev_mode_obj, lv_pct(100), lv_pct(10));
	lv_obj_add_event_cb(advanced_setup_ctrl.prev_mode_obj, event_handler, LV_EVENT_CLICKED, NULL);
	advanced_setup_ctrl.prev_label_obj = lv_label_create(advanced_setup_ctrl.prev_mode_obj);

	advanced_setup_ctrl.storage_type_obj = lv_btn_create(cont_obj);
	lv_obj_set_size(advanced_setup_ctrl.storage_type_obj, lv_pct(100), lv_pct(10));
	lv_obj_add_event_cb(advanced_setup_ctrl.storage_type_obj, event_handler, LV_EVENT_CLICKED, NULL);
	advanced_setup_ctrl.storage_label_obj = lv_label_create(advanced_setup_ctrl.storage_type_obj);
}

static void advanced_setup_set_text() {
	static lv_style_t symbol_style, symbol_focus_style, option_style, option_focus_style;
	// INFO: make sure to get the right heigh of obj
	lv_obj_update_layout(advanced_setup_ctrl.rtsp_enable_obj);
	int font_height = lv_font_get_line_height(ttf_info_32.font);
	int option_height = lv_obj_get_height(advanced_setup_ctrl.rtsp_enable_obj);
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
	lv_obj_align_to(advanced_setup_ctrl.container_obj, advanced_setup_ctrl.exit_obj,
	                LV_ALIGN_OUT_BOTTOM_LEFT, 0, 0);

	lv_checkbox_set_text(advanced_setup_ctrl.rtsp_enable_obj, "开启RTSP");
	lv_obj_add_style(advanced_setup_ctrl.rtsp_enable_obj, &option_style, 0);
	lv_obj_add_style(advanced_setup_ctrl.rtsp_enable_obj, &option_focus_style,
	                 LV_PART_MAIN | LV_STATE_FOCUSED);
	lv_obj_set_style_radius(advanced_setup_ctrl.rtsp_enable_obj, LV_RADIUS_CIRCLE,
	                        LV_PART_INDICATOR);
	if (rk_get_rtsp())
		lv_obj_add_state(advanced_setup_ctrl.rtsp_enable_obj, LV_STATE_CHECKED);

	lv_obj_add_style(advanced_setup_ctrl.prev_mode_obj, &option_style, 0);
	lv_obj_add_style(advanced_setup_ctrl.prev_mode_obj, &option_focus_style,
	                 LV_PART_MAIN | LV_STATE_FOCUSED);
	lv_label_set_text(advanced_setup_ctrl.prev_label_obj, "预览分辨率设置");

	lv_obj_add_style(advanced_setup_ctrl.storage_type_obj, &option_style, 0);
	lv_obj_add_style(advanced_setup_ctrl.storage_type_obj, &option_focus_style,
	                 LV_PART_MAIN | LV_STATE_FOCUSED);
	lv_label_set_text(advanced_setup_ctrl.storage_label_obj, "存储设置");
}

static void advanced_setup_destroy_ctrl(void) {
	if (NULL != advanced_setup_ctrl.storage_type_obj) {
		ui_common_remove_style_all(advanced_setup_ctrl.storage_type_obj);
		lv_obj_del(advanced_setup_ctrl.storage_type_obj);
		advanced_setup_ctrl.storage_type_obj = NULL;
	}
	if (NULL != advanced_setup_ctrl.prev_mode_obj) {
		ui_common_remove_style_all(advanced_setup_ctrl.prev_mode_obj);
		lv_obj_del(advanced_setup_ctrl.prev_mode_obj);
		advanced_setup_ctrl.prev_mode_obj = NULL;
	}
	if (NULL != advanced_setup_ctrl.rtsp_enable_obj) {
		ui_common_remove_style_all(advanced_setup_ctrl.rtsp_enable_obj);
		lv_obj_del(advanced_setup_ctrl.rtsp_enable_obj);
		advanced_setup_ctrl.rtsp_enable_obj = NULL;
	}
	if (NULL != advanced_setup_ctrl.exit_obj) {
		ui_common_remove_style_all(advanced_setup_ctrl.exit_obj);
		lv_obj_del(advanced_setup_ctrl.exit_obj);
		advanced_setup_ctrl.exit_obj = NULL;
	}
	if (NULL != advanced_setup_ctrl.container_obj) {
		ui_common_remove_style_all(advanced_setup_ctrl.container_obj);
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
	lv_group_add_obj(group, advanced_setup_ctrl.rtsp_enable_obj);
	lv_group_add_obj(group, advanced_setup_ctrl.prev_mode_obj);
	lv_group_add_obj(group, advanced_setup_ctrl.storage_type_obj);
	advanced_setup_ctrl.group = group;
}

static void advanced_setup_delete_indev(void) {
	if (NULL != advanced_setup_ctrl.group) {
		lv_port_indev_group_destroy(advanced_setup_ctrl.group);
		advanced_setup_ctrl.group = NULL;
	}
}

static void advanced_setup_page_enter(lv_obj_t *page_obj) {
	advanced_setup_ctrl.need_reinit = false;
	advanced_setup_add_indev();
}

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
