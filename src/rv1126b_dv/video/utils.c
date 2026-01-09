// Copyright 2025 Rockchip Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "utils.c"

#include <fcntl.h>
#include <linux/media.h>
#include <linux/v4l2-mediabus.h>
#include <linux/v4l2-subdev.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "common.h"

#define FIXED_PAD 0
#define PATH_MAX 256

static int find_sensor_device(const char *sensor_name /* like imx586 */, char *sensor_device_path,
                              size_t path_size) {
	if (!sensor_name) {
		LOG_ERROR("sensor_name is NULL\n");
		return -1;
	}

	char media_dev[64] = {0};
	char cmd[256] = {0};
	char line[512] = {0};
	int found = 0;

	for (int i = 0; i < 64; ++i) {
		snprintf(media_dev, sizeof(media_dev), "/dev/media%d", i);
		if (access(media_dev, F_OK) != 0)
			continue; // not exist

		snprintf(cmd, sizeof(cmd), "media-ctl -d %s -p 2>/dev/null", media_dev);
		FILE *fp = popen(cmd, "r");
		if (!fp)
			continue;

		while (fgets(line, sizeof(line), fp)) {
			if (strstr(line, sensor_name)) {
				// search for device node in subsequent lines (or this line)
				char *p = NULL;
				// look in current line first
				p = strstr(line, "/dev/v4l-subdev");
				if (!p) {
					// check next lines (peek a few)
					long pos = ftell(fp);
					for (int j = 0; j < 8 && fgets(line, sizeof(line), fp); ++j) {
						p = strstr(line, "/dev/v4l-subdev");
						if (p)
							break;
					}
					// restore (not strictly necessary since we'll pclose next)
					fseek(fp, pos, SEEK_SET);
				}
				if (p) {
					// extract token
					char node[PATH_MAX] = {0};
					int k = 0;
					// stop at newline, carriage return, space, tab, comma or end
					while (*p && *p != '\n' && *p != '\r' && *p != ' ' && *p != '\t' && *p != ',' &&
					       k < (int)(sizeof(node) - 1))
						node[k++] = *p++;
					node[k] = '\0';
					// trim trailing whitespace if any
					size_t nlen = strlen(node);
					while (nlen > 0 && (node[nlen - 1] == '\n' || node[nlen - 1] == '\r' ||
					                    node[nlen - 1] == ' ' || node[nlen - 1] == '\t')) {
						node[--nlen] = '\0';
					}
					snprintf(sensor_device_path, path_size, "%s", node);
					found = 1;
					break;
				}
			}
		}
		pclose(fp);
		if (found)
			break;
	}

	if (found) {
		LOG_INFO("Found subdev path: %s for sensor %s\n", sensor_device_path, sensor_name);
		return 0;
	}

	LOG_ERROR("Failed to find sensor '%s' node\n", sensor_name);
	return -1;
}

int set_camera_format(uint32_t width, uint32_t height, uint32_t fps, const char *format) {
	uint32_t code = 0;
	int fd = -1, ret = 0;
	char sensor_device_path[PATH_MAX] = {'\0'};
	ret = find_sensor_device(rk_param_get_string("sensor:name", NULL), sensor_device_path,
	                         sizeof(sensor_device_path));
	if (ret) {
		LOG_ERROR("Failed to find sensor device\n");
		return ret;
	}
	if (!format) {
		LOG_ERROR("empty raw format!\n");
		return -1;
	} else if (!strcmp(format, "RGGB10"))
		code = MEDIA_BUS_FMT_SRGGB10_1X10;
	else if (!strcmp(format, "GBRG10"))
		code = MEDIA_BUS_FMT_SGBRG10_1X10;
	else if (!strcmp(format, "RGGB12"))
		code = MEDIA_BUS_FMT_SRGGB12_1X12;
	else if (!strcmp(format, "BGGR14"))
		code = MEDIA_BUS_FMT_SBGGR14_1X14;
	else if (!strcmp(format, "RGGB14"))
		code = MEDIA_BUS_FMT_SRGGB14_1X14;
	else if (!strcmp(format, "GBRG14"))
		code = MEDIA_BUS_FMT_SGBRG14_1X14;
	else if (!strcmp(format, "GRBG14"))
		code = MEDIA_BUS_FMT_SGRBG14_1X14;
	else {
		LOG_ERROR("unknown raw format!\n");
		return -1;
	}
	fd = open(sensor_device_path, O_RDWR);
	if (fd < 0) {
		LOG_ERROR("Failed to open subdev\n");
		return -1;
	}

	LOG_INFO("width %d, height %d, fps %d, code %#X\n", width, height, fps, code);
	struct v4l2_subdev_format fmt = {.which = V4L2_SUBDEV_FORMAT_ACTIVE,
	                                 .pad = FIXED_PAD,
	                                 .format = {.width = width,
	                                            .height = height,
	                                            .code = code,
	                                            .field = V4L2_FIELD_NONE,
	                                            .colorspace = V4L2_COLORSPACE_RAW}};
	struct v4l2_subdev_frame_interval ival = {.pad = FIXED_PAD,
	                                          .interval = {.numerator = 1, .denominator = fps}};

	if (ioctl(fd, VIDIOC_SUBDEV_S_FMT, &fmt) < 0) {
		LOG_ERROR("Failed to set format\n");
		close(fd);
		return -1;
	}
	if (ioctl(fd, VIDIOC_SUBDEV_S_FRAME_INTERVAL, &ival) < 0) {
		LOG_ERROR("Failed to set FPS %d\n", fps);
		close(fd);
		return -1;
	}
	close(fd);
	LOG_INFO("update sensor setting width %d height %d fps %d format %s\n", width, height, fps,
	         format);
	return 0;
}