#include <getopt.h>

#include "audio.h"
#include "common.h"
#include "isp.h"
#include "log.h"
#include "network.h"
#include "osd.h"
#include "param.h"
#include "rockiva.h"
#include "server.h"
#include "storage.h"
#include "system.h"
#include "video.h"
#include <linux/input.h>

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "rkipc.c"

enum { LOG_ERROR, LOG_WARN, LOG_INFO, LOG_DEBUG };

int enable_minilog = 0;
int rkipc_log_level = LOG_DEBUG;
int rkipc_camera_id_ = 0;

int g_main_run_ = 1;
char *rkipc_ini_path_ = NULL;
char *rkipc_iq_file_path_ = NULL;

static void sig_proc(int signo) {
	LOG_INFO("received signo %d \n", signo);
	g_main_run_ = 0;
}

static const char short_options[] = "c:a:l:";
static const struct option long_options[] = {{"config", required_argument, NULL, 'c'},
                                             {"aiq_file", no_argument, NULL, 'a'},
                                             {"log_level", no_argument, NULL, 'l'},
                                             {"help", no_argument, NULL, 'h'},
                                             {0, 0, 0, 0}};

static void usage_tip(FILE *fp, int argc, char **argv) {
	fprintf(fp,
	        "Usage: %s [options]\n"
	        "Version %s\n"
	        "Options:\n"
	        "-c | --config      rkipc ini file, default is "
	        "/userdata/rkipc.ini, need to be writable\n"
	        "-a | --aiq_file    aiq file dir path, default is /etc/iqfiles\n"
	        "-l | --log_level   log_level [0/1/2/3], default is 2\n"
	        "-h | --help        for help \n\n"
	        "\n",
	        argv[0], "V1.0");
}

void rkipc_get_opt(int argc, char *argv[]) {
	for (;;) {
		int idx;
		int c;
		c = getopt_long(argc, argv, short_options, long_options, &idx);
		if (-1 == c)
			break;
		switch (c) {
		case 0: /* getopt_long() flag */
			break;
		case 'c':
			rkipc_ini_path_ = optarg;
			break;
		case 'a':
			rkipc_iq_file_path_ = optarg;
			break;
		case 'l':
			rkipc_log_level = atoi(optarg);
			break;
		case 'h':
			usage_tip(stdout, argc, argv);
			exit(EXIT_SUCCESS);
		default:
			usage_tip(stderr, argc, argv);
			exit(EXIT_FAILURE);
		}
	}
}

static void *wait_key_event(void *arg) {
	int key_fd;
	key_fd = open("/dev/input/event3", O_RDONLY);
	if (key_fd < 0) {
		LOG_ERROR("can't open /dev/input/event3\n");
		return NULL;
	}
	LOG_ERROR("success open /dev/input/event3\n");
	fd_set rfds;
	int nfds = key_fd + 1;
	struct timeval timeout;
	struct input_event key_event;

	while (g_main_run_) {
		// The rfds collection must be emptied every time,
		// otherwise the descriptor changes cannot be detected
		memset(&timeout, 0, sizeof(timeout));
		timeout.tv_sec = 1;
		FD_ZERO(&rfds);
		FD_SET(key_fd, &rfds);
		int retval = select(nfds, &rfds, NULL, NULL, &timeout);
		if (retval == -1) {
			LOG_ERROR("select() failed\n");
		} 
		// wait for the key event to occur
		if (FD_ISSET(key_fd, &rfds)) {
			read(key_fd, &key_event, sizeof(key_event));
			LOG_INFO("[timeval:sec:%d,usec:%d,type:%d,code:%d,value:%d]\n",
			         key_event.input_event_sec, key_event.input_event_usec, key_event.type,
			         key_event.code, key_event.value);
			if ((key_event.code == 114) && key_event.value) { //上面recovery是115，下面那个按键是114
				LOG_INFO("MENU, rkipc exit\n");
				raise(SIGINT);
			}
		}
	}

	if (key_fd) {
		close(key_fd);
		key_fd = 0;
	}
	LOG_DEBUG("wait key event out\n");
	return NULL;
}

int main(int argc, char **argv) {
	LOG_INFO("main begin\n");
	rkipc_version_dump();
	signal(SIGINT, sig_proc);
	signal(SIGTERM, sig_proc);

	rkipc_get_opt(argc, argv);
	LOG_INFO("rkipc_ini_path_ is %s, rkipc_iq_file_path_ is %s, rkipc_log_level "
	         "is %d\n",
	         rkipc_ini_path_, rkipc_iq_file_path_, rkipc_log_level);

	pthread_t key_chk;
	pthread_create(&key_chk, NULL, wait_key_event, NULL);

	// init
	rk_param_init(rkipc_ini_path_);
	rk_network_init(NULL);
	rk_system_init();
	if (rk_param_get_int("video.source:enable_npu", 0))
		rkipc_rockiva_init();
	rkipc_camera_id_ = rk_param_get_int("video.source:camera_id", 0); // need rk_param_init
	rk_isp_init(rkipc_camera_id_, rkipc_iq_file_path_);
	rk_isp_set_frame_rate_without_ini(0, rk_param_get_int("isp.0.adjustment:fps", 30));
	RK_MPI_SYS_Init();
	rk_storage_init();
	rk_video_init();
	if (rk_param_get_int("audio.0:enable", 0))
		rkipc_audio_init();
	rkipc_server_init();
	LOG_INFO("rkipc init over\n");

	while (g_main_run_) {
		usleep(1000 * 1000);
	}

	// deinit
	rk_storage_deinit();
	rkipc_server_deinit();
	rk_system_deinit();
	if (rk_param_get_int("audio.0:enable", 0))
		rkipc_audio_deinit();
	rk_video_deinit();
	RK_MPI_SYS_Exit();
	rk_isp_deinit(rkipc_camera_id_);
	if (rk_param_get_int("video.source:enable_npu", 0))
		rkipc_rockiva_deinit();
	rk_network_deinit();
	rk_param_deinit();
	LOG_INFO("rkipc deinit over\n");

	return 0;
}
