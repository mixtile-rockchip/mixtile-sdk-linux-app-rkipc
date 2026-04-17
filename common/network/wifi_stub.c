// Copyright 2026 Rockchip Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "Hostapd.h"
#include "Rk_wifi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef RKIPC_WIFI_SUPPORT

int RK_wifi_register_callback(RK_wifi_state_callback cb) { return -1; }

int RK_wifi_running_getState(RK_WIFI_RUNNING_State_e *pState) {
	if (pState)
		*pState = RK_WIFI_State_OFF;
	return 0;
}

int RK_wifi_running_getConnectionInfo(RK_WIFI_INFO_Connection_s *pInfo) {
	if (pInfo)
		pInfo->reason = -1;
	return -1;
}

int RK_wifi_enable_ap(char *ssid, const char *psk, const char *ip) {
	(void)ssid;
	(void)psk;
	(void)ip;
	return -1;
}

int RK_wifi_disable_ap() { return -1; }

int RK_wifi_enable(int enable) {
	(void)enable;
	return -1;
}

int RK_wifi_scan(void) { return -1; }

char *RK_wifi_scan_r(void) {
	char *scan_r = (char *)malloc(3 * sizeof(char));
	if (scan_r) {
		memset(scan_r, 0, 3);
		strcpy(scan_r, "[]");
	}
	return scan_r;
}

char *RK_wifi_scan_r_sec(const unsigned int cols) {
	(void)cols;
	char *scan_r = (char *)malloc(3 * sizeof(char));
	if (scan_r) {
		memset(scan_r, 0, 3);
		strcpy(scan_r, "[]");
	}
	return scan_r;
}

int RK_wifi_connect(const char *ssid, const char *psk) {
	(void)ssid;
	(void)psk;
	return -1;
}

int RK_wifi_connect1(const char *ssid, const char *psk, const RK_WIFI_CONNECTION_Encryp_e encryp,
                     const int hide) {
	(void)ssid;
	(void)psk;
	(void)encryp;
	(void)hide;
	return -1;
}

int RK_wifi_disconnect_network(void) { return -1; }

int RK_wifi_restart_network(void) { return -1; }

int RK_wifi_set_hostname(const char *name) {
	(void)name;
	return -1;
}

int RK_wifi_get_hostname(char *name, int len) {
	(void)name;
	(void)len;
	return -1;
}

int RK_wifi_get_mac(char *wifi_mac) {
	(void)wifi_mac;
	return -1;
}

int RK_wifi_has_config(void) { return 0; }

int RK_wifi_ping(char *address) {
	(void)address;
	return -1;
}

int RK_wifi_recovery(void) { return -1; }

int RK_wifi_airkiss_start(char *ssid, char *password) {
	(void)ssid;
	(void)password;
	return -1;
}

void RK_wifi_airkiss_stop(void) { return; }

int RK_wifi_forget_with_ssid(const char *ssid) {
	(void)ssid;
	return -1;
}

int RK_wifi_cancel(void) { return -1; }

int RK_wifi_getSavedInfo(RK_WIFI_SAVED_INFO *pSaveInfo) {
	(void)pSaveInfo;
	return -1;
}

int RK_wifi_connect_with_ssid(char *ssid) {
	(void)ssid;
	return -1;
}

int RK_wifi_reset(void) { return -1; }

int wifi_rtl_start_hostapd(const char *ssid, const char *psk, const char *ip) {
	(void)ssid;
	(void)psk;
	(void)ip;
	return -1;
}

int wifi_rtl_stop_hostapd() { return -1; }

#endif
