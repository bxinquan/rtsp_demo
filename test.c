/*************************************************************************
	> File Name: test.c
	> Author: bxq
	> Mail: 544177215@qq.com 
	> Created Time: Saturday, December 12, 2015 PM03:19:12 CST
 ************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include "rtsp_demo.h"

static int flag_run = 1;
static void sig_proc(int signo)
{
	flag_run = 0;
}

static int get_next_video_frame (FILE *fp, uint8_t **buff, int *size)
{
	uint8_t szbuf[1024];
	int szlen = 0;
	int ret;
	if (!(*buff)) {
		*buff = (uint8_t*)malloc(2*1024*1024);
		if (!(*buff))
			return -1;
	}

	*size = 0;

	while ((ret = fread(szbuf + szlen, 1, sizeof(szbuf) - szlen, fp)) > 0) {
		int i = 3;
		szlen += ret;
		while (i < szlen - 3 && !(szbuf[i] == 0 &&  szbuf[i+1] == 0 && (szbuf[i+2] == 1 || (szbuf[i+2] == 0 && szbuf[i+3] == 1)))) i++;
		memcpy(*buff + *size, szbuf, i);
		*size += i;
		memmove(szbuf, szbuf + i, szlen - i);
		szlen -= i;
		if (szlen > 3) {
			//printf("szlen %d\n", szlen);
			fseek(fp, -szlen, SEEK_CUR);
			break;
		}
	}
	if (ret > 0)
		return *size;
	return 0;
}

static int get_next_audio_frame (FILE *fp, uint8_t **buff, int *size)
{
	int ret;
#define AUDIO_FRAME_SIZE 320
	if (!(*buff)) {
		*buff = (uint8_t*)malloc(AUDIO_FRAME_SIZE);
		if (!(*buff))
			return -1;
	}

	ret = fread(*buff, 1, AUDIO_FRAME_SIZE, fp);
	if (ret > 0) {
		*size = ret;
		return ret;
	}
	return 0;
}

#define MAX_SESSION_NUM 64
#define DEMO_CFG_FILE "demo.cfg"

struct demo_cfg
{
	int session_count;
	struct {
		char path[64];
		char video_file[64];
		char audio_file[64];
	} session_cfg[MAX_SESSION_NUM];
};

static int load_cfg(struct demo_cfg *cfg, const char *cfg_file)
{
//cfgline: path=%s video=%s audio=%s
	FILE *fp = fopen(cfg_file, "r");
	char line[256];
	int count = 0;

	if (!fp) {
		fprintf(stderr, "open %s failed\n", cfg_file);
		return -1;
	}

	memset(cfg, 0, sizeof(*cfg));
	while (fgets(line, sizeof(line) - 1, fp)) {
		const char *p;
		memset(&cfg->session_cfg[count], 0, sizeof(cfg->session_cfg[count]));

		if (line[0] == '#')
			continue;

		p = strstr(line, "path=");
		if (!p)
			continue;
		if (sscanf(p, "path=%s", cfg->session_cfg[count].path) != 1)
			continue;
		if ((p = strstr(line, "video="))) {
			if (sscanf(p, "video=%s", cfg->session_cfg[count].video_file) != 1) {
				fprintf(stderr, "parse video file failed %s\n", p);
			}
		}
		if ((p = strstr(line, "audio="))) {
			if (sscanf(p, "audio=%s", cfg->session_cfg[count].audio_file) != 1) {
				fprintf(stderr, "parse audio file failed %s\n", p);
			}
		}
		if (strlen(cfg->session_cfg[count].video_file) || strlen(cfg->session_cfg[count].audio_file)) {
			count ++;
		} else {
			fprintf(stderr, "parse line %s failed\n", line);
		}
	}
	cfg->session_count = count;
	fclose(fp);
	return count;
}

#ifdef __LINUX__
#include <unistd.h>
#endif
#ifdef __WIN32__
#include <windows.h>
#define usleep(x) Sleep((x)/1000)
#endif

int main(int argc, char *argv[])
{
	const char *cfg_file = DEMO_CFG_FILE;
	struct demo_cfg cfg;
	FILE *fp[MAX_SESSION_NUM][2] = {{NULL}};
	rtsp_demo_handle demo;
	rtsp_session_handle session[MAX_SESSION_NUM] = {NULL};
	int session_count = 0;
	uint8_t *vbuf = NULL;
	uint8_t *abuf = NULL;
	uint64_t ts = 0;
	int vsize = 0, asize = 0;
	int ret, ch;

	if (argc > 1)
		cfg_file = argv[1];

	ret = load_cfg(&cfg, cfg_file);
	if (ret < 0) {
		fprintf(stderr, "Usage: %s [CFG_FILE]\n", argv[0]);
		getchar();
		return 0;
	}

	//rtsp_demo usage:
	//rtsp_new_demo
	//rtsp_new_session
	//rtsp_set_video
	//rtsp_set_audio
	//while(1){
	//  rtsp_tx_video
	//  rtsp_tx_audio
	//  rtsp_do_event
	//}
	//rtsp_del_session
	//rtsp_del_demo

	demo = rtsp_new_demo(8554);
	if (NULL == demo) {
		printf("rtsp_new_demo failed\n");
		return 0;
	}

	session_count = cfg.session_count;
	for (ch = 0; ch < session_count; ch++) {
		if (strlen(cfg.session_cfg[ch].video_file)) {
			fp[ch][0] = fopen(cfg.session_cfg[ch].video_file, "rb");
			if (!fp[ch][0]) {
				fprintf(stderr, "open %s failed\n", cfg.session_cfg[ch].video_file);
			}
		}

		if (strlen(cfg.session_cfg[ch].audio_file)) {
			fp[ch][1] = fopen(cfg.session_cfg[ch].audio_file, "rb");
			if (!fp[ch][1]) {
				fprintf(stderr, "open %s failed\n", cfg.session_cfg[ch].audio_file);
			}
		}
		
		if (fp[ch][0] == NULL && fp[ch][1] == NULL)
			continue;

		session[ch] = rtsp_new_session(demo, cfg.session_cfg[ch].path);
		if (NULL == session[ch]) {
			printf("rtsp_new_session failed\n");
			continue;
		}

		//rtsp_set_auth(session[ch], RTSP_AUTH_TYPE_BASIC, "admin", "123456");
		if (fp[ch][0]) {
			rtsp_set_video(session[ch], RTSP_CODEC_ID_VIDEO_H264, NULL, 0);
			rtsp_sync_video_ts(session[ch], rtsp_get_reltime(), rtsp_get_ntptime());
		}
		if (fp[ch][1]) {
			rtsp_set_audio(session[ch], RTSP_CODEC_ID_AUDIO_G711A, NULL, 0);
			rtsp_sync_audio_ts(session[ch], rtsp_get_reltime(), rtsp_get_ntptime());
		}

		printf("==========> rtsp://127.0.0.1:8554%s for %s %s <===========\n", cfg.session_cfg[ch].path, 
			fp[ch][0] ? cfg.session_cfg[ch].video_file : "", 
			fp[ch][1] ? cfg.session_cfg[ch].audio_file : "");
	}

	ts = rtsp_get_reltime();
	signal(SIGINT, sig_proc);
#ifdef __LINUX__
	signal(SIGPIPE, SIG_IGN);
#endif
	while (flag_run) {
		uint8_t type = 0;

		for (ch = 0; ch < session_count; ch++) {
			if (fp[ch][0]) {
			read_video_again:
				ret = get_next_video_frame(fp[ch][0], &vbuf, &vsize);
				if (ret < 0) {
					fprintf(stderr, "get_next_video_frame failed\n");
					flag_run = 0;
					break;
				}
				if (ret == 0) {
					fseek(fp[ch][0], 0, SEEK_SET);
					if (fp[ch][1])
						fseek(fp[ch][1], 0, SEEK_SET);
					goto read_video_again;
				}

				if (session[ch])
					rtsp_tx_video(session[ch], vbuf, vsize, ts);

				type = 0;
				if (vbuf[0] == 0 && vbuf[1] == 0 && vbuf[2] == 1) {
					type = vbuf[3] & 0x1f;
				}
				if (vbuf[0] == 0 && vbuf[1] == 0 && vbuf[2] == 0 && vbuf[3] == 1) {
					type = vbuf[4] & 0x1f;
				}
				if (type != 5 && type != 1)
					goto read_video_again;
			}

			if (fp[ch][1]) {
				ret = get_next_audio_frame(fp[ch][1], &abuf, &asize);
				if (ret < 0) {
					fprintf(stderr, "get_next_audio_frame failed\n");
					break;
				}
				if (ret == 0) {
					fseek(fp[ch][1], 0, SEEK_SET);
					if (fp[ch][0])
						fseek(fp[ch][0], 0, SEEK_SET);
					continue;
				}
				if (session[ch])
					rtsp_tx_audio(session[ch], abuf, asize, ts);
			}
		}

		do {
			ret = rtsp_do_event(demo);
			if (ret > 0)
				continue;
			if (ret < 0)
				break;
			usleep(20000);
		} while (rtsp_get_reltime() - ts < 1000000 / 25);
		if (ret < 0)
			break;
		ts += 1000000 / 25;
		printf(".");fflush(stdout);
	}

	free(vbuf);
	free(abuf);
	
	for (ch = 0; ch < session_count; ch++) {
		if (fp[ch][0])
			fclose(fp[ch][0]);
		if (fp[ch][1])
			fclose(fp[ch][1]);
		if (session[ch])
			rtsp_del_session(session[ch]);
	}

	rtsp_del_demo(demo);
	printf("Exit.\n");
	getchar();
	return 0;
}
