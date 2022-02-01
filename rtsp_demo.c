/*************************************************************************
	> File Name: rtsp_demo.c
	> Author: bxq
	> Mail: 544177215@qq.com 
	> Created Time: Monday, November 23, 2015 AM12:34:09 CST
 ************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

#include "comm.h"
#include "rtsp_demo.h"
#include "rtsp_msg.h"
#include "rtp_enc.h"
#include "queue.h"
#include "stream_queue.h"
#include "utils.h"


#ifdef __WIN32__
#define MSG_DONTWAIT 0
#define SK_EAGAIN   (WSAEWOULDBLOCK)
#define SK_EINTR	(WSAEINTR)
typedef int SOCKLEN;
#endif

#ifdef __LINUX__
#define SOCKET_ERROR	(-1)
#define INVALID_SOCKET  (-1)
#define SK_EAGAIN   (EAGAIN)
#define SK_EINTR	(EINTR)
#define closesocket(x)  close(x)
typedef int SOCKET;
typedef socklen_t SOCKLEN;
#endif 

static int sk_errno (void)
{
#ifdef __WIN32__
	return WSAGetLastError();
#endif
#ifdef __LINUX__
	return (errno);
#endif
}

static const char *sk_strerror (int err)
{
#ifdef __WIN32__
	static char serr_code_buf[24];
	sprintf(serr_code_buf, "WSAE-%d", err);
	return serr_code_buf;
#endif
#ifdef __LINUX__
	return strerror(err);
#endif
}

struct rtsp_session;
struct rtsp_client_connection;
TAILQ_HEAD(rtsp_session_queue_head, rtsp_session);
TAILQ_HEAD(rtsp_client_connection_queue_head, rtsp_client_connection);

struct rtsp_session
{
	char path[64];
	int  vcodec_id;
	int  acodec_id;

	int  auth_type;
	char auth_user[16];
	char auth_passwd[16];

	union {
		struct codec_data_h264 h264;
		struct codec_data_h265 h265;
	} vcodec_data;

	union {
		struct codec_data_g726 g726;
		struct codec_data_aac aac;
	} acodec_data;

	rtp_enc vrtpe;
	rtp_enc artpe;
	struct stream_queue *vstreamq;
	struct stream_queue *astreamq;

	uint64_t video_ntptime_of_zero_ts;
	uint64_t audio_ntptime_of_zero_ts;
	
	struct rtsp_demo *demo;
	struct rtsp_client_connection_queue_head connections_qhead;
	TAILQ_ENTRY(rtsp_session) demo_entry;
};

struct rtp_connection
{
	int is_over_tcp;
	int tcp_interleaved[2];//if is_over_tcp=1. [0] is rtp interleaved, [1] is rtcp interleaved
	SOCKET udp_sockfd[2]; //if is_over_tcp=0. [0] is rtp socket, [1] is rtcp socket
	uint16_t udp_localport[2]; //if is_over_tcp=0. [0] is rtp local port, [1] is rtcp local port
	uint16_t udp_peerport[2]; //if is_over_tcp=0. [0] is rtp peer port, [1] is rtcp peer port
	struct in_addr peer_addr; //peer ipv4 addr
	int streamq_index;
	uint32_t ssrc;
	uint32_t rtcp_packet_count;
	uint32_t rtcp_octet_count;
	uint64_t rtcp_last_ts;
};

#define RTP_MAX_PKTSIZ	((1500-42)/4*4)
#define VRTP_MAX_NBPKTS	(400)
#define ARTP_MAX_NBPKTS	(10)
#define VRTP_PT_ID		(96)
#define ARTP_PT_ID		(97)
#define VRTSP_SUBPATH	"track1"
#define ARTSP_SUBPATH	"track2"

#define RTSP_REQBUF_MAX_SIZ	 (1024)
//#define RTSP_RESBUF_MAX_SIZ	 (1024)
#if (RTP_MAX_PKTSIZ+4 < RTSP_REQBUF_MAX_SIZ)
  #define RTSP_RESBUF_MAX_SIZ   (RTSP_REQ_BUF_MAX_SIZ)
#else
  #define RTSP_RESBUF_MAX_SIZ   (RTP_MAX_PKTSIZ+4)
#endif

struct rtsp_client_connection
{
	int state;	//session state
#define RTSP_CC_STATE_INIT		0
#define RTSP_CC_STATE_READY		1
#define RTSP_CC_STATE_PLAYING	2
#define RTSP_CC_STATE_RECORDING	3

	SOCKET sockfd;		//rtsp client socket
	struct in_addr peer_addr; //peer ipv4 addr
	unsigned int   peer_port; //peer ipv4 port
	unsigned long session_id;	//session id

	char reqbuf[RTSP_REQBUF_MAX_SIZ];
	int  reqlen;

	char resbuf[RTSP_RESBUF_MAX_SIZ];
	int  resoff;
	int  reslen;

	struct rtp_connection *vrtp;
	struct rtp_connection *artp;

	struct rtsp_demo *demo;
	struct rtsp_session *session;
	TAILQ_ENTRY(rtsp_client_connection) demo_entry;	
	TAILQ_ENTRY(rtsp_client_connection) session_entry;
};

struct rtsp_demo 
{
	SOCKET sockfd;	//rtsp server socket 0:invalid
	struct rtsp_session_queue_head sessions_qhead;
	struct rtsp_client_connection_queue_head connections_qhead;
};

static struct rtsp_demo *__alloc_demo (void)
{
	struct rtsp_demo *d = (struct rtsp_demo*) calloc(1, sizeof(struct rtsp_demo));
	if (NULL == d) {
		err("alloc memory for rtsp_demo failed\n");
		return NULL;
	}
	TAILQ_INIT(&d->sessions_qhead);
	TAILQ_INIT(&d->connections_qhead);
	return d;
}

static void __free_demo (struct rtsp_demo *d)
{
	if (d) {
		free(d);
	}
}

static struct rtsp_session *__alloc_session (struct rtsp_demo *d)
{
	struct rtsp_session *s = (struct rtsp_session*) calloc(1, sizeof(struct rtsp_session));
	if (NULL == s) {
		err("alloc memory for rtsp_session failed\n");
		return NULL;
	}

	s->demo = d;
	TAILQ_INIT(&s->connections_qhead);
	TAILQ_INSERT_TAIL(&d->sessions_qhead, s, demo_entry);
	return s;
}

static void __free_session (struct rtsp_session *s)
{
	if (s) {
		struct rtsp_demo *d = s->demo; 
		TAILQ_REMOVE(&d->sessions_qhead, s, demo_entry);
		free(s);
	}
}

static struct rtsp_client_connection *__alloc_client_connection (struct rtsp_demo *d)
{
	struct rtsp_client_connection *cc = (struct rtsp_client_connection*) calloc(1, sizeof(struct rtsp_client_connection));
	if (NULL == cc) {
		err("alloc memory for rtsp_session failed\n");
		return NULL;
	}

	cc->demo = d;
	TAILQ_INSERT_TAIL(&d->connections_qhead, cc, demo_entry);
	return cc;
}

static void __free_client_connection (struct rtsp_client_connection *cc)
{
	if (cc) {
		struct rtsp_demo *d = cc->demo;
		TAILQ_REMOVE(&d->connections_qhead, cc, demo_entry);
		free(cc);
	}
}

static void __client_connection_bind_session (struct rtsp_client_connection *cc, struct rtsp_session *s)
{
	if (cc->session == NULL) {
		cc->session = s;
		TAILQ_INSERT_TAIL(&s->connections_qhead, cc, session_entry);
	}
}

static void __client_connection_unbind_session (struct rtsp_client_connection *cc)
{
	struct rtsp_session *s = cc->session;
	if (s) {
		TAILQ_REMOVE(&s->connections_qhead, cc, session_entry);
		cc->session = NULL;
	}
}

rtsp_demo_handle rtsp_new_demo (int port)
{
	struct rtsp_demo *d = NULL;
	struct sockaddr_in inaddr;
	SOCKET sockfd;
	int ret;
	
#ifdef __WIN32__
	WSADATA ws;
	WSAStartup(MAKEWORD(2,2), &ws);
#endif

	d = __alloc_demo();
	if (NULL == d) {
		return NULL;
	}

	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd == INVALID_SOCKET) {
		err("create socket failed : %s\n", sk_strerror(sk_errno()));
		__free_demo(d);
		return NULL;
	}

	if (port <= 0)
		port = 554;

	memset(&inaddr, 0, sizeof(inaddr));
	inaddr.sin_family = AF_INET;
	inaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	inaddr.sin_port = htons(port);
	ret = bind(sockfd, (struct sockaddr*)&inaddr, sizeof(inaddr));
	if (ret == SOCKET_ERROR) {
		err("bind socket to address failed : %s\n", sk_strerror(sk_errno()));
		closesocket(sockfd);
		__free_demo(d);
		return NULL;
	}

	ret = listen(sockfd, 128); //XXX
	if (ret == SOCKET_ERROR) {
		err("listen socket failed : %s\n", sk_strerror(sk_errno()));
		closesocket(sockfd);
		__free_demo(d);
		return NULL;
	}

	d->sockfd = sockfd;

	info("rtsp server demo starting on port %d\n", port);
	return (rtsp_demo_handle)d;
}

#ifdef __WIN32__
#include <mstcpip.h>
#endif
#ifdef __LINUX__
#include <fcntl.h>
#include <netinet/tcp.h>
#endif

static int rtsp_set_client_socket (SOCKET sockfd)
{
		int ret;

#ifdef __WIN32__
		unsigned long nonblocked = 1;
		int sndbufsiz = 1024 * 256;
		int keepalive = 1;
		struct tcp_keepalive alive_in, alive_out;
		unsigned long alive_retlen;
		struct linger ling;

		ret = ioctlsocket(sockfd, FIONBIO, &nonblocked);
		if (ret == SOCKET_ERROR) {
			warn("ioctlsocket FIONBIO failed: %s\n", sk_strerror(sk_errno()));
		}

		ret = setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, (const char*)&sndbufsiz, sizeof(sndbufsiz));
		if (ret == SOCKET_ERROR) {
			warn("setsockopt SO_SNDBUF failed: %s\n", sk_strerror(sk_errno()));
		}

		ret = setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, (const char*)&keepalive, sizeof(keepalive));
		if (ret == SOCKET_ERROR) {
			warn("setsockopt setsockopt SO_KEEPALIVE failed: %s\n", sk_strerror(sk_errno()));
		}

		alive_in.onoff = TRUE;
		alive_in.keepalivetime = 60000;
		alive_in.keepaliveinterval = 30000;
		ret = WSAIoctl(sockfd, SIO_KEEPALIVE_VALS, &alive_in, sizeof(alive_in), 
			&alive_out, sizeof(alive_out), &alive_retlen, NULL, NULL);
		if (ret == SOCKET_ERROR) {
			warn("WSAIoctl SIO_KEEPALIVE_VALS failed: %s\n", sk_strerror(sk_errno()));
		}

		memset(&ling, 0, sizeof(ling));
		ling.l_onoff = 1;
		ling.l_linger = 0;
		ret = setsockopt(sockfd, SOL_SOCKET, SO_LINGER, (const char*)&ling, sizeof(ling)); //resolve too many TCP CLOSE_WAIT
		if (ret == SOCKET_ERROR) {
			warn("setsockopt SO_LINGER failed: %s\n", sk_strerror(sk_errno()));
		}
#endif

#ifdef __LINUX__
		int sndbufsiz = 1024 * 256;
		int keepalive = 1;
		int keepidle = 60;
		int keepinterval = 3;
		int keepcount = 5;
		struct linger ling;

		ret = fcntl(sockfd, F_GETFL, 0);
		if (ret < 0) {
			warn("fcntl F_GETFL failed: %s\n", strerror(errno));
		} else {
			ret |= O_NONBLOCK;
			ret = fcntl(sockfd, F_SETFL, ret);
			if (ret < 0) {
				warn("fcntl F_SETFL failed: %s\n", strerror(errno));
			}
		}

		ret = setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, (const char*)&sndbufsiz, sizeof(sndbufsiz));
		if (ret == SOCKET_ERROR) {
			warn("setsockopt SO_SNDBUF failed: %s\n", sk_strerror(sk_errno()));
		}

		ret = setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, (const char*)&keepalive, sizeof(keepalive));
		if (ret == SOCKET_ERROR) {
			warn("setsockopt SO_KEEPALIVE failed: %s\n", sk_strerror(sk_errno()));
		}

		ret = setsockopt(sockfd, SOL_TCP, TCP_KEEPIDLE, (const char*)&keepidle, sizeof(keepidle));
		if (ret == SOCKET_ERROR) {
			warn("setsockopt TCP_KEEPIDLE failed: %s\n", sk_strerror(sk_errno()));
		}

		ret = setsockopt(sockfd, SOL_TCP, TCP_KEEPINTVL, (const char*)&keepinterval, sizeof(keepinterval));
		if (ret == SOCKET_ERROR) {
			warn("setsockopt TCP_KEEPINTVL failed: %s\n", sk_strerror(sk_errno()));
		}

		ret = setsockopt(sockfd, SOL_TCP, TCP_KEEPCNT, (const char*)&keepcount, sizeof(keepcount));
		if (ret == SOCKET_ERROR) {
			warn("setsockopt TCP_KEEPCNT failed: %s\n", sk_strerror(sk_errno()));
		}

		memset(&ling, 0, sizeof(ling));
		ling.l_onoff = 1;
		ling.l_linger = 0;
		ret = setsockopt(sockfd, SOL_SOCKET, SO_LINGER, (const char*)&ling, sizeof(ling)); //resolve too many TCP CLOSE_WAIT
		if (ret == SOCKET_ERROR) {
			warn("setsockopt SO_LINGER failed: %s\n", sk_strerror(sk_errno()));
		}
#endif
		return 0;
}

static struct rtsp_client_connection *rtsp_new_client_connection (struct rtsp_demo *d)
{
	struct rtsp_client_connection *cc = NULL;
	struct sockaddr_in inaddr;
	SOCKET sockfd;
	SOCKLEN addrlen = sizeof(inaddr);

	sockfd = accept(d->sockfd, (struct sockaddr*)&inaddr, &addrlen);
	if (sockfd == INVALID_SOCKET) {
		err("accept failed : %s\n", sk_strerror(sk_errno()));
		return NULL;
	}

	rtsp_set_client_socket(sockfd);//XXX DEBUG

	info("new rtsp client comming [peer %s:%u]\n", 
			inet_ntoa(inaddr.sin_addr), ntohs(inaddr.sin_port));

	cc = __alloc_client_connection(d);
	if (cc == NULL) {
		closesocket(sockfd);
		return NULL;
	}

	cc->state = RTSP_CC_STATE_INIT;
	cc->sockfd = sockfd;
	cc->peer_addr = inaddr.sin_addr;
	cc->peer_port = ntohs(inaddr.sin_port);

	return cc;
}

static void rtsp_del_rtp_connection (struct rtsp_client_connection *cc, int isaudio);
static void rtsp_del_client_connection (struct rtsp_client_connection *cc)
{
	if (cc) {
		info("delete client socket %d [peer %s:%u]\n", cc->sockfd, inet_ntoa(cc->peer_addr), cc->peer_port);
		__client_connection_unbind_session(cc);
		rtsp_del_rtp_connection(cc, 0);
		rtsp_del_rtp_connection(cc, 1);
		closesocket(cc->sockfd);
		__free_client_connection(cc);
	}
}

static int rtsp_path_match (const char *main_path, const char *full_path)
{
	char path0[64] = {0};
	char path1[64] = {0};

	strncpy(path0, main_path, sizeof(path0) - 2);
	strncpy(path1, full_path, sizeof(path1) - 2);

	if (path0[strlen(path0) - 1] != '/')
		strcat(path0, "/");
	if (path1[strlen(path1) - 1] != '/')
		strcat(path1, "/");

	if (strncmp(path0, path1, strlen(path0)))
		return 0;
	return 1;
}

rtsp_session_handle rtsp_new_session (rtsp_demo_handle demo, const char *path)
{
	struct rtsp_demo *d = (struct rtsp_demo*)demo;
	struct rtsp_session *s = NULL;

	if (!d || !path || strlen(path) == 0) {
		err("param invalid\n");
		goto fail;
	}

	TAILQ_FOREACH(s, &d->sessions_qhead, demo_entry) {
		if (rtsp_path_match(s->path, path) || rtsp_path_match(path, s->path)) {
			err("path:%s (%s) is exist!!!\n", s->path, path);
			goto fail;
		}
	}

	s = __alloc_session(d);
	if (NULL == s) {
		goto fail;
	}

	strncpy(s->path, path, sizeof(s->path) - 1);
	s->vcodec_id = RTSP_CODEC_ID_NONE;
	s->acodec_id = RTSP_CODEC_ID_NONE;

	info("add session path: %s\n", s->path);
	return (rtsp_session_handle)s;
fail:
	if (s) {
		free(s);
	}
	return NULL;
}

int rtsp_set_auth (rtsp_session_handle session, int type, const char *user, const char *passwd)
{
	struct rtsp_session *s = (struct rtsp_session*)session;
	if (!s || (s->auth_type != RTSP_AUTH_TYPE_NONE && s->auth_type != type))
		return -1;
	
	switch(type) {
		case RTSP_AUTH_TYPE_NONE:
			s->auth_type = RTSP_AUTH_TYPE_NONE;
			memset(s->auth_user, 0, sizeof(s->auth_user));
			memset(s->auth_passwd, 0, sizeof(s->auth_passwd));
			break;
		case RTSP_AUTH_TYPE_BASIC:
			if (!user || !passwd) {
				err("please set user/passwd!\n");
				return -1;
			}
			if (strlen(user) < 1 || strlen(user) + 1 > sizeof(s->auth_user)) {
				err("user too short or too length!\n");
				return -1;
			}
			if (strlen(passwd) < 1 || strlen(passwd) + 1 > sizeof(s->auth_passwd)) {
				err("passwd too short or too length!\n");
				return -1;
			}
			s->auth_type = RTSP_AUTH_TYPE_BASIC;
			strcpy(s->auth_user, user);
			strcpy(s->auth_passwd, passwd);
			break;
		case RTSP_AUTH_TYPE_DIGEST:
		default:
			err("not support this authorization type %d\n", type);
			return -1;
	}

	return 0;
}

int rtsp_set_video (rtsp_session_handle session, int codec_id, const uint8_t *codec_data, int data_len)
{
	struct rtsp_session *s = (struct rtsp_session*)session;
	if (!s || (s->vcodec_id != RTSP_CODEC_ID_NONE && s->vcodec_id != codec_id))
		return -1;
	
	switch (codec_id) {
	case RTSP_CODEC_ID_VIDEO_H264:
	case RTSP_CODEC_ID_VIDEO_H265:
		break;
	default:
		err("not supported codec_id %d for video\n", codec_id);
		return -1;
	}

	s->vcodec_id = codec_id;
	s->vrtpe.pt = VRTP_PT_ID;
	s->vrtpe.seq = 0;
	s->vrtpe.ssrc = 0;
	s->vrtpe.sample_rate = 90000;
	memset(&s->vcodec_data, 0, sizeof(s->vcodec_data));

	if (codec_data && data_len > 0) {
		switch (codec_id) {
		case RTSP_CODEC_ID_VIDEO_H264:
			if (rtsp_codec_data_parse_from_user_h264(codec_data, data_len, &s->vcodec_data.h264) <= 0) {
				warn("parse codec_data failed\n");
				break;
			}
			break;
		case RTSP_CODEC_ID_VIDEO_H265:
			if (rtsp_codec_data_parse_from_user_h265(codec_data, data_len, &s->vcodec_data.h265) <= 0) {
				warn("parse codec_data failed\n");
				break;
			}
			break;
		}
	}

	if (!s->vstreamq) {
		//NOTE must VRTP_MAX_NBPKTS+1. Circular Queue requires at least 1 free elements for Empty element
		s->vstreamq = streamq_alloc(RTP_MAX_PKTSIZ, VRTP_MAX_NBPKTS + 1);
		if (!s->vstreamq) {
			err("alloc memory for video rtp queue failed\n");
			s->vcodec_id = RTSP_CODEC_ID_NONE;
			return -1;
		}
	}

	return 0;
}

int rtsp_set_audio (rtsp_session_handle session, int codec_id, const uint8_t *codec_data, int data_len)
{
	struct rtsp_session *s = (struct rtsp_session*)session;
	if (!s || (s->acodec_id != RTSP_CODEC_ID_NONE && s->acodec_id != codec_id))
		return -1;
	
	switch (codec_id) {
	case RTSP_CODEC_ID_AUDIO_G711A:
	case RTSP_CODEC_ID_AUDIO_G711U:
	case RTSP_CODEC_ID_AUDIO_G726:
	case RTSP_CODEC_ID_AUDIO_AAC:
		break;
	default:
		err("not supported codec_id %d for audio\n", codec_id);
		return -1;
	}

	s->acodec_id = codec_id;
	s->artpe.pt = ARTP_PT_ID;
	s->artpe.seq = 0;
	s->artpe.ssrc = 0;
	s->artpe.sample_rate = 8000;
	memset(&s->acodec_data, 0, sizeof(s->acodec_data));

	if (codec_data && data_len > 0) {
		switch (codec_id) {
		case RTSP_CODEC_ID_AUDIO_G726:
			if (rtsp_codec_data_parse_from_user_g726(codec_data, data_len, &s->acodec_data.g726) <= 0) {
				warn("parse codec_data failed\n");
				break;
			}
			break;
		case RTSP_CODEC_ID_AUDIO_AAC:
			if (rtsp_codec_data_parse_from_user_aac(codec_data, data_len, &s->acodec_data.aac) <= 0) {
				warn("parse codec_data failed\n");
				break;
			}
			s->artpe.sample_rate = s->acodec_data.aac.sample_rate;
			break;
		}
	}

	if (!s->astreamq) {
		//NOTE must VRTP_MAX_NBPKTS+1. Circular Queue requires at least 1 free elements for Empty element
		s->astreamq = streamq_alloc(RTP_MAX_PKTSIZ, ARTP_MAX_NBPKTS + 1);
		if (!s->astreamq) {
			err("alloc memory for audio rtp queue failed\n");
			s->acodec_id = RTSP_CODEC_ID_NONE;
			return -1;
		}
	}

	return 0;
}

void rtsp_del_session (rtsp_session_handle session)
{
	struct rtsp_session *s = (struct rtsp_session*)session;
	if (s) {
		struct rtsp_client_connection *cc;
		while ((cc = TAILQ_FIRST(&s->connections_qhead))) {
			rtsp_del_client_connection(cc);
		}
		info("del session path: %s\n", s->path);
		if (s->vstreamq)
			streamq_free(s->vstreamq);
		if (s->astreamq)
			streamq_free(s->astreamq);
		__free_session(s);
	}
}

void rtsp_del_demo (rtsp_demo_handle demo)
{
	struct rtsp_demo *d = (struct rtsp_demo*)demo;
	if (d) {
		struct rtsp_session *s;
		struct rtsp_client_connection *cc;
		
		while ((cc = TAILQ_FIRST(&d->connections_qhead))) {
			rtsp_del_client_connection(cc);
		}
		while ((s = TAILQ_FIRST(&d->sessions_qhead))) {
			rtsp_del_session(s);
		}

		closesocket(d->sockfd);
		__free_demo(d);
	}
}

static int build_simple_sdp (struct rtsp_session *s, const char *ip, const char *uri, char *sdpbuf, int maxlen)
{
	char *p = sdpbuf;

	p += sprintf(p, "v=0\r\n");
	p += sprintf(p, "o=- 0 0 IN IP4 %s\r\n", ip ? ip : "0.0.0.0");
	p += sprintf(p, "s=rtsp_demo\r\n");
	p += sprintf(p, "t=0 0\r\n");
	p += sprintf(p, "c=IN IP4 %s\r\n", ip ? ip : "0.0.0.0");
	p += sprintf(p, "a=control:%s\r\n", uri ? uri : "*");
	p += sprintf(p, "a=range:npt=0-\r\n");

	if (s->vcodec_id != RTSP_CODEC_ID_NONE) {
		switch (s->vcodec_id) {
		case RTSP_CODEC_ID_VIDEO_H264:
			p += rtsp_build_sdp_media_attr_h264(VRTP_PT_ID, s->vrtpe.sample_rate, 
					&s->vcodec_data.h264, p, maxlen - (p - sdpbuf));
			break;
		case RTSP_CODEC_ID_VIDEO_H265:
			p += rtsp_build_sdp_media_attr_h265(VRTP_PT_ID, s->vrtpe.sample_rate, 
					&s->vcodec_data.h265, p, maxlen - (p - sdpbuf));
			break;
		}
		if (uri)
			p += sprintf(p, "a=control:%s/%s\r\n", uri, VRTSP_SUBPATH);
		else
			p += sprintf(p, "a=control:%s\r\n", VRTSP_SUBPATH);
	}

	if (s->acodec_id != RTSP_CODEC_ID_NONE) {
		switch (s->acodec_id) {
		case RTSP_CODEC_ID_AUDIO_G711A:
			p += rtsp_build_sdp_media_attr_g711a(ARTP_PT_ID, s->artpe.sample_rate, 
					p, maxlen - (p - sdpbuf));
			break;
		case RTSP_CODEC_ID_AUDIO_G711U:
			p += rtsp_build_sdp_media_attr_g711u(ARTP_PT_ID, s->artpe.sample_rate, 
					p, maxlen - (p - sdpbuf));
			break;
		case RTSP_CODEC_ID_AUDIO_G726:
			p += rtsp_build_sdp_media_attr_g726(ARTP_PT_ID, s->artpe.sample_rate, 
					&s->acodec_data.g726, p, maxlen - (p - sdpbuf));
			break;
		case RTSP_CODEC_ID_AUDIO_AAC:
			p += rtsp_build_sdp_media_attr_aac(ARTP_PT_ID, s->artpe.sample_rate, 
					&s->acodec_data.aac, p, maxlen - (p - sdpbuf));
			break;
		}
		if (uri)
			p += sprintf(p, "a=control:%s/%s\r\n", uri, ARTSP_SUBPATH);
		else
			p += sprintf(p, "a=control:%s\r\n", ARTSP_SUBPATH);
	}

	return (p - sdpbuf);
}

static int rtsp_authorization_request (struct rtsp_client_connection *cc, const rtsp_msg_s *reqmsg, rtsp_msg_s *resmsg)
{
//	struct rtsp_demo *d = cc->demo;
	struct rtsp_session *s = cc->session;

	char basic_up[32] = "";
	char basic_b64a[64] = "";
	char basic_b64b[64] = "";

	switch (s->auth_type) {
		case RTSP_AUTH_TYPE_NONE:
			return 0;
		case RTSP_AUTH_TYPE_BASIC: 
			if (rtsp_msg_get_auth_basic(reqmsg, basic_b64a, sizeof(basic_b64a))) {
				dbg("authorization get fail. [peer %s:%u]\n", inet_ntoa(cc->peer_addr), cc->peer_port);
				rtsp_msg_set_www_auth_basic(resmsg, "rtsp_demo");
				rtsp_msg_set_response(resmsg, 401);//Unauthorized
				return 1;
			}
			sprintf(basic_up, "%s:%s", s->auth_user, s->auth_passwd);
			base64_encode(basic_b64b, sizeof(basic_b64b), (uint8_t*)basic_up, strlen(basic_up));
			if (strcmp(basic_b64a, basic_b64b)) {
				warn("authorization basic fail, my %s his %s. [peer %s:%u]\n", 
						basic_b64b, basic_b64a, inet_ntoa(cc->peer_addr), cc->peer_port);
				rtsp_msg_set_www_auth_basic(resmsg, "rtsp_demo");
				rtsp_msg_set_response(resmsg, 401);//Unauthorized
				return 1;
			}
			break;

		case RTSP_AUTH_TYPE_DIGEST:
			//TODO
		default:
			break;
	}
	return 0;
}

static int rtsp_handle_OPTIONS (struct rtsp_client_connection *cc, const rtsp_msg_s *reqmsg, rtsp_msg_s *resmsg)
{
//	struct rtsp_demo *d = cc->demo;
//	struct rtsp_session *s = cc->session;
	uint32_t public_ = 0;
	dbg("\n");
	public_ |= RTSP_MSG_PUBLIC_OPTIONS;
	public_ |= RTSP_MSG_PUBLIC_DESCRIBE;
	public_ |= RTSP_MSG_PUBLIC_SETUP;
	public_ |= RTSP_MSG_PUBLIC_PAUSE;
	public_ |= RTSP_MSG_PUBLIC_PLAY;
	public_ |= RTSP_MSG_PUBLIC_TEARDOWN;
	rtsp_msg_set_public(resmsg, public_);
	return 0;
}

static int rtsp_handle_DESCRIBE (struct rtsp_client_connection *cc, const rtsp_msg_s *reqmsg, rtsp_msg_s *resmsg)
{
//	struct rtsp_demo *d = cc->demo;
	struct rtsp_session *s = cc->session;
	char sdpbuf[1024] = "";
	int sdplen = 0;
	uint32_t accept = 0;
	const rtsp_msg_uri_s *puri = &reqmsg->hdrs.startline.reqline.uri;
	char uri[128] = "";

	dbg("\n");
	if (rtsp_msg_get_accept(reqmsg, &accept) < 0 && !(accept & RTSP_MSG_ACCEPT_SDP)) {
		rtsp_msg_set_response(resmsg, 406);
		warn("client not support accept SDP\n");
		return 0;
	}

	//build uri
	sprintf(uri, "rtsp://%s", puri->ipaddr);
	if (puri->port != 0)
		sprintf(uri + strlen(uri), ":%u", puri->port);
	strcat(uri, s->path);
	
	sdplen = build_simple_sdp(s, puri->ipaddr, uri, sdpbuf, sizeof(sdpbuf));

	rtsp_msg_set_content_type(resmsg, RTSP_MSG_CONTENT_TYPE_SDP);
	rtsp_msg_set_content_length(resmsg, sdplen);
	rtsp_msg_set_x_accept_dynamic_rate(resmsg, 1); //for QuickTime Player
	resmsg->body.body = rtsp_mem_dup(sdpbuf, sdplen);
	return 0;
}

static unsigned long __rtp_gen_ssrc (void)
{
	static unsigned long ssrc = 0x22345678;
	return ssrc++;
}

static int __rtp_udp_local_setup (struct rtp_connection *rtp)
{
	int i, ret;

	for (i = 65536/4*3/2*2; i < 65536; i += 2) {
		SOCKET rtpsock, rtcpsock;
		struct sockaddr_in inaddr;
		uint16_t port;

		rtpsock = socket(AF_INET, SOCK_DGRAM, 0);
		if (rtpsock == INVALID_SOCKET) {
			err("create rtp socket failed: %s\n", sk_strerror(sk_errno()));
			return -1;
		}

		rtcpsock = socket(AF_INET, SOCK_DGRAM, 0);
		if (rtcpsock == INVALID_SOCKET) {
			err("create rtcp socket failed: %s\n", sk_strerror(sk_errno()));
			closesocket(rtpsock);
			return -1;
		}

		port = i;
		memset(&inaddr, 0, sizeof(inaddr));
		inaddr.sin_family = AF_INET;
		inaddr.sin_addr.s_addr = htonl(INADDR_ANY);
		inaddr.sin_port = htons(port);
		ret = bind(rtpsock, (struct sockaddr*)&inaddr, sizeof(inaddr));
		if (ret == SOCKET_ERROR) {
			closesocket(rtpsock);
			closesocket(rtcpsock);
			continue;
		}

		port = i + 1;
		memset(&inaddr, 0, sizeof(inaddr));
		inaddr.sin_family = AF_INET;
		inaddr.sin_addr.s_addr = htonl(INADDR_ANY);
		inaddr.sin_port = htons(port);
		ret = bind(rtcpsock, (struct sockaddr*)&inaddr, sizeof(inaddr));
		if (ret == SOCKET_ERROR) {
			closesocket(rtpsock);
			closesocket(rtcpsock);
			continue;
		}

#ifdef __WIN32__
		{
			unsigned long nonblocked = 1;
			ret = ioctlsocket(rtpsock, FIONBIO, &nonblocked);
			if (ret == SOCKET_ERROR) {
				warn("ioctlsocket FIONBIO failed: %s\n", sk_strerror(sk_errno()));
			}
			ret = ioctlsocket(rtcpsock, FIONBIO, &nonblocked);
			if (ret == SOCKET_ERROR) {
				warn("ioctlsocket FIONBIO failed: %s\n", sk_strerror(sk_errno()));
			}
		}
#endif
#ifdef __LINUX__
		ret = fcntl(rtpsock, F_GETFL, 0);
		if (ret < 0) {
			warn("fcntl F_GETFL failed: %s\n", strerror(errno));
		} else {
			ret |= O_NONBLOCK;
			ret = fcntl(rtpsock, F_SETFL, ret);
			if (ret < 0) {
				warn("fcntl F_SETFL failed: %s\n", strerror(errno));
			}
		}
		ret = fcntl(rtcpsock, F_GETFL, 0);
		if (ret < 0) {
			warn("fcntl F_GETFL failed: %s\n", strerror(errno));
		} else {
			ret |= O_NONBLOCK;
			ret = fcntl(rtcpsock, F_SETFL, ret);
			if (ret < 0) {
				warn("fcntl F_SETFL failed: %s\n", strerror(errno));
			}
		}
#endif
		rtp->is_over_tcp = 0;
		rtp->udp_sockfd[0] = rtpsock;
		rtp->udp_sockfd[1] = rtcpsock;
		rtp->udp_localport[0] = i;
		rtp->udp_localport[1] = i + 1;

		return 0;
	}

	err("not found free local port for rtp/rtcp\n");
	return -1;
}

static int rtsp_new_rtp_connection (struct rtsp_client_connection *cc, int isaudio, int istcp, int peer_port, int peer_interleaved)
{
	struct rtp_connection *rtp;
	struct in_addr peer_addr = cc->peer_addr;

	rtp = (struct rtp_connection*) calloc(1, sizeof(struct rtp_connection));
	if (rtp == NULL) {
		err("alloc mem for rtp session failed: %s\n", strerror(errno));
		return -1;
	}

	rtp->is_over_tcp = !!istcp;
	rtp->peer_addr = peer_addr;
	rtp->ssrc = __rtp_gen_ssrc();

	if (istcp) {
		rtp->tcp_interleaved[0] = peer_interleaved;
		rtp->tcp_interleaved[1] = peer_interleaved + 1;
		dbg("new rtp over tcp for %s ssrc:%08x peer_addr:%s interleaved:%u-%u [peer %s:%u]\n",
			(isaudio ? "audio" : "video"), 
			rtp->ssrc, 
			inet_ntoa(peer_addr), 
			rtp->tcp_interleaved[0], rtp->tcp_interleaved[1], 
			inet_ntoa(cc->peer_addr), cc->peer_port);
	} else {
		if (__rtp_udp_local_setup(rtp) < 0) {
			free(rtp);
			return -1;
		}
		rtp->udp_peerport[0] = peer_port;
		rtp->udp_peerport[1] = peer_port + 1;
		dbg("new rtp over udp for %s ssrc:%08x local_port:%u-%u peer_addr:%s peer_port:%u-%u [peer %s:%u]\n",
			(isaudio ? "audio" : "video"), 
			rtp->ssrc, 
			rtp->udp_localport[0], rtp->udp_localport[1], 
			inet_ntoa(peer_addr), 
			rtp->udp_peerport[0], rtp->udp_peerport[1], 
			inet_ntoa(cc->peer_addr), cc->peer_port);
	}

	if (isaudio) {
		cc->artp = rtp;
	} else {
		cc->vrtp = rtp;
	}

	return 0;
}

static void rtsp_del_rtp_connection (struct rtsp_client_connection *cc, int isaudio)
{
	struct rtp_connection *rtp;

	if (isaudio) {
		rtp = cc->artp;
		cc->artp = NULL;
	} else {
		rtp = cc->vrtp;
		cc->vrtp = NULL;
	}

	if (rtp) {
		if (!(rtp->is_over_tcp)) {
			closesocket(rtp->udp_sockfd[0]);
			closesocket(rtp->udp_sockfd[1]);
		}
		free(rtp);
	}
}

static int rtsp_handle_SETUP (struct rtsp_client_connection *cc, const rtsp_msg_s *reqmsg, rtsp_msg_s *resmsg)
{
//	struct rtsp_demo *d = cc->demo;
	struct rtsp_session *s = cc->session;
	struct rtp_connection *rtp = NULL;
	int istcp = 0, isaudio = 0;
	char vpath[64] = "";
	char apath[64] = "";
	uint32_t x_dynamic_rate;
	int ret;

	dbg("\n");

	if (!reqmsg->hdrs.transport) {
		rtsp_msg_set_response(resmsg, 461);
		warn("rtsp no transport err\n");
		return 0;
	}

	if (reqmsg->hdrs.transport->type == RTSP_MSG_TRANSPORT_TYPE_RTP_AVP_TCP) {
		istcp = 1;
		if (!(reqmsg->hdrs.transport->flags & RTSP_MSG_TRANSPORT_FLAG_INTERLEAVED)) {
			warn("rtsp no interleaved err\n");
			rtsp_msg_set_response(resmsg, 461);
			return 0;
		}
	} else {
		if (!(reqmsg->hdrs.transport->flags & RTSP_MSG_TRANSPORT_FLAG_CLIENT_PORT)) {
			warn("rtsp no client_port err\n");
			rtsp_msg_set_response(resmsg, 461);
			return 0;
		}
	}

	snprintf(vpath, sizeof(vpath) - 1, "%s/%s", s->path, VRTSP_SUBPATH);
	snprintf(apath, sizeof(vpath) - 1, "%s/%s", s->path, ARTSP_SUBPATH);
	if (s->vcodec_id != RTSP_CODEC_ID_NONE && rtsp_path_match(vpath, reqmsg->hdrs.startline.reqline.uri.abspath)) {
		isaudio = 0;
	} else if (s->acodec_id != RTSP_CODEC_ID_NONE && rtsp_path_match(apath, reqmsg->hdrs.startline.reqline.uri.abspath)) {
		isaudio = 1;
	} else {
		warn("rtsp urlpath:%s err\n", reqmsg->hdrs.startline.reqline.uri.abspath);
		rtsp_msg_set_response(resmsg, 461);
		return 0;
	}

	rtsp_del_rtp_connection(cc, isaudio);

	ret = rtsp_new_rtp_connection(cc, isaudio, istcp, reqmsg->hdrs.transport->client_port, reqmsg->hdrs.transport->interleaved);
	if (ret < 0) {
		rtsp_msg_set_response(resmsg, 500);
		return 0;
	}
	
	rtp = isaudio ? cc->artp : cc->vrtp;

	if (istcp) {
		rtsp_msg_set_transport_tcp(resmsg, rtp->ssrc, rtp->tcp_interleaved[0]);
	} else {
		rtsp_msg_set_transport_udp(resmsg, rtp->ssrc, rtp->udp_peerport[0], rtp->udp_localport[0]);
	}

	//for QuickTime Player
	if (rtsp_msg_get_x_dynamic_rate(reqmsg, &x_dynamic_rate) < 0) {
		x_dynamic_rate = 0;
	}
	rtsp_msg_set_x_dynamic_rate(resmsg, x_dynamic_rate);

	if (cc->state == RTSP_CC_STATE_PLAYING) {
		rtp->streamq_index = streamq_tail((isaudio ? s->astreamq : s->vstreamq));
	}

	if (cc->state == RTSP_CC_STATE_INIT) {
		cc->state = RTSP_CC_STATE_READY;
		cc->session_id = rtsp_msg_gen_session_id();
		rtsp_msg_set_session(resmsg, cc->session_id);
	}

	return 0;
}

static int rtsp_handle_PAUSE (struct rtsp_client_connection *cc, const rtsp_msg_s *reqmsg, rtsp_msg_s *resmsg)
{
//	struct rtsp_demo *d = cc->demo;
//	struct rtsp_session *s = cc->session;

	dbg("\n");
	if (cc->state != RTSP_CC_STATE_READY && cc->state != RTSP_CC_STATE_PLAYING) 
	{
		rtsp_msg_set_response(resmsg, 455);
		warn("rtsp status err\n");
		return 0;
	}

	if (cc->state != RTSP_CC_STATE_READY)
		cc->state = RTSP_CC_STATE_READY;
	return 0;
}

static int rtsp_handle_PLAY (struct rtsp_client_connection *cc, const rtsp_msg_s *reqmsg, rtsp_msg_s *resmsg)
{
//	struct rtsp_demo *d = cc->demo;
	struct rtsp_session *s = cc->session;

	dbg("\n");
	if (cc->state != RTSP_CC_STATE_READY && cc->state != RTSP_CC_STATE_PLAYING) 
	{
		rtsp_msg_set_response(resmsg, 455);
		warn("rtsp status err\n");
		return 0;
	}

	if (cc->state != RTSP_CC_STATE_PLAYING) {
		if (cc->vrtp && s->vstreamq)
			cc->vrtp->streamq_index = streamq_tail(s->vstreamq);
		if (cc->artp && s->astreamq)
			cc->artp->streamq_index = streamq_tail(s->astreamq);
		cc->state = RTSP_CC_STATE_PLAYING;
	}
	return 0;
}

static int rtsp_handle_TEARDOWN (struct rtsp_client_connection *cc, const rtsp_msg_s *reqmsg, rtsp_msg_s *resmsg)
{
//	struct rtsp_demo *d = cc->demo;
	struct rtsp_session *s = cc->session;
	char vpath[64] = "";
	char apath[64] = "";
	dbg("\n");

	snprintf(vpath, sizeof(vpath) - 1, "%s/%s", s->path, VRTSP_SUBPATH);
	snprintf(apath, sizeof(vpath) - 1, "%s/%s", s->path, ARTSP_SUBPATH);

	if (rtsp_path_match(vpath, reqmsg->hdrs.startline.reqline.uri.abspath)) {
		rtsp_del_rtp_connection(cc, 0);
	} else if (rtsp_path_match(apath, reqmsg->hdrs.startline.reqline.uri.abspath)) {
		rtsp_del_rtp_connection(cc, 1);
	} else {
		rtsp_del_rtp_connection(cc, 0);
		rtsp_del_rtp_connection(cc, 1);
	}
	if (!cc->vrtp && !cc->artp) {
		cc->state = RTSP_CC_STATE_INIT;
	}
	return 0;
}

static int rtsp_process_request (struct rtsp_client_connection *cc, const rtsp_msg_s *reqmsg, rtsp_msg_s *resmsg)
{
	struct rtsp_demo *d = cc->demo;
	struct rtsp_session *s = cc->session;
	const char *path = reqmsg->hdrs.startline.reqline.uri.abspath;
	uint32_t cseq = 0, session = 0;

	rtsp_msg_set_response(resmsg, 200);
	rtsp_msg_set_date(resmsg, NULL);
	rtsp_msg_set_server(resmsg, "rtsp_demo (" __DATE__ " " __TIME__ ")");

	if (rtsp_msg_get_cseq(reqmsg, &cseq) < 0) {
		rtsp_msg_set_response(resmsg, 400);
		warn("No CSeq field\n");
		return 0;
	}
	rtsp_msg_set_cseq(resmsg, cseq);

	if (cc->state != RTSP_CC_STATE_INIT) {
		if (rtsp_msg_get_session(reqmsg, &session) < 0 || session != cc->session_id) {
			warn("Invalid Session field\n");
			rtsp_msg_set_response(resmsg, 454);
			return 0;
		}
		rtsp_msg_set_session(resmsg, session);
	}

	if (s) {
		if (rtsp_path_match(s->path, path) == 0) { // /live/chn0
			warn("path is not matched %s (old:%s)\n", path, s->path);
			rtsp_msg_set_response(resmsg, 451);
			return 0;
		}
	} else if (reqmsg->hdrs.startline.reqline.method != RTSP_MSG_METHOD_OPTIONS) {
		TAILQ_FOREACH(s, &d->sessions_qhead, demo_entry) {
			if (rtsp_path_match(s->path, path)) {
				break;
			}
		}
		if (NULL == s) {
			warn("Not found session path: %s\n", path);
			rtsp_msg_set_response(resmsg, 454);
			return 0;
		}
		__client_connection_bind_session(cc, s);
	}

	switch (reqmsg->hdrs.startline.reqline.method) {
		case RTSP_MSG_METHOD_OPTIONS:
			return rtsp_handle_OPTIONS(cc, reqmsg, resmsg);
		case RTSP_MSG_METHOD_DESCRIBE:
			if (rtsp_authorization_request(cc, reqmsg, resmsg))
				return 0;
			return rtsp_handle_DESCRIBE(cc, reqmsg, resmsg);
		case RTSP_MSG_METHOD_SETUP:
			if (rtsp_authorization_request(cc, reqmsg, resmsg))
				return 0;
			return rtsp_handle_SETUP(cc, reqmsg, resmsg);
		case RTSP_MSG_METHOD_PAUSE:
			if (rtsp_authorization_request(cc, reqmsg, resmsg))
				return 0;
			return rtsp_handle_PAUSE(cc, reqmsg, resmsg);
		case RTSP_MSG_METHOD_PLAY:
			if (rtsp_authorization_request(cc, reqmsg, resmsg))
				return 0;
			return rtsp_handle_PLAY(cc, reqmsg, resmsg);
		case RTSP_MSG_METHOD_TEARDOWN:
			if (rtsp_authorization_request(cc, reqmsg, resmsg))
				return 0;
			return rtsp_handle_TEARDOWN(cc, reqmsg, resmsg);
		default:
			break;
	}

	rtsp_msg_set_response(resmsg, 501);
	return 0;
}

static int rtsp_recv_msg (struct rtsp_client_connection *cc, rtsp_msg_s *msg)
{
	int ret;
	
	if (sizeof(cc->reqbuf) - cc->reqlen - 1 > 0) {
		ret = recv(cc->sockfd, cc->reqbuf + cc->reqlen, sizeof(cc->reqbuf) - cc->reqlen - 1, MSG_DONTWAIT);
		if (ret == 0) {
			dbg("peer closed [peer %s:%u]\n", inet_ntoa(cc->peer_addr), cc->peer_port);
			return -1;
		}
		if (ret == SOCKET_ERROR) {
			if (sk_errno() != SK_EAGAIN && sk_errno() != SK_EINTR) {
				err("recv data failed: %s\n", sk_strerror(sk_errno()));
				return -1;
			}
			ret = 0;
		}
		cc->reqlen += ret;
		cc->reqbuf[cc->reqlen] = 0;
	}

	if (cc->reqlen == 0) {
		return 0;
	}
	
	ret = rtsp_msg_parse_from_array(msg, cc->reqbuf, cc->reqlen);
	if (ret < 0) {
		err("Invalid frame\n");
		return -1;
	}
	if (ret == 0) {
		return 0;
	}

	//dbg("recv %d bytes rtsp message from %s\n", ret, inet_ntoa(cc->peer_addr));

	memmove(cc->reqbuf, cc->reqbuf + ret, cc->reqlen - ret);
	cc->reqlen -= ret;
	return ret;
}

static int rtsp_send_msg (struct rtsp_client_connection *cc, rtsp_msg_s *msg) 
{
	int size, ret;

	if (cc->reslen > 0) {
		ret = send(cc->sockfd, cc->resbuf + cc->resoff, cc->reslen, 0);
		if (ret != cc->reslen) {
			if (ret > 0 && ret < cc->reslen) {
				cc->resoff += ret;
				cc->reslen -= ret;
				warn("rtsp send message failed, buffer has %d bytes. [peer %s:%u]\n", 
						cc->reslen, inet_ntoa(cc->peer_addr), cc->peer_port);
				return 0;
			}
			if (ret == SOCKET_ERROR && sk_errno() != SK_EAGAIN && sk_errno() != SK_EINTR) {
				warn("rtsp send buffer fail: %s. [peer %s:%u]\n", 
						sk_strerror(sk_errno()), inet_ntoa(cc->peer_addr), cc->peer_port);
				return -1;
			}
			return 0;
		}
		cc->resoff = 0;
		cc->reslen = 0;
	}

	size = rtsp_msg_build_to_array(msg, cc->resbuf, sizeof(cc->resbuf));
	if (size < 0) {
		err("rtsp_msg_build_to_array failed\n");
		return -1;
	}

	ret = send(cc->sockfd, cc->resbuf, size, 0);
	if (ret != size) {
		if (ret > 0 && ret < size) {
			cc->resoff = ret;
			cc->reslen = size - ret;
			dbg("rtsp send message %d bytes, store %d bytes to buffer. [peer %s:%u]\n", 
					ret, cc->reslen, inet_ntoa(cc->peer_addr), cc->peer_port);
			return size;
		}
		if (ret == SOCKET_ERROR && sk_errno() != SK_EAGAIN && sk_errno() != SK_EINTR) {
			warn("rtsp send message fail: %s. [peer %s:%u]\n", 
					sk_strerror(sk_errno()), inet_ntoa(cc->peer_addr), cc->peer_port);
			return -1;
		}
		return 0;
	}

	//dbg("sent %d bytes rtsp message. [peer %s:%u]\n", ret, inet_ntoa(cc->peer_addr), cc->peer_port);
	return ret;
}

static int rtsp_send_interlaced_frame (struct rtsp_client_connection *cc, int channel, const uint8_t *data, int len)
{
	int ret;
	uint8_t szbuf[4];

	if (cc->reslen > 0) {
		ret = send(cc->sockfd, cc->resbuf + cc->resoff, cc->reslen, 0);
		if (ret != cc->reslen) {
			if (ret > 0 && ret < cc->reslen) {
				cc->resoff += ret;
				cc->reslen -= ret;
				warn("rtsp send interlaced frame failed, buffer has %d bytes. [peer %s:%u]\n", 
						cc->reslen, inet_ntoa(cc->peer_addr), cc->peer_port);
				return 0;
			}
			if (ret == SOCKET_ERROR && sk_errno() != SK_EAGAIN && sk_errno() != SK_EINTR) {
				warn("rtsp send buffer fail: %s. [peer %s:%u]\n", 
						sk_strerror(sk_errno()), inet_ntoa(cc->peer_addr), cc->peer_port);
				return -1;
			}
			return 0;
		}
		cc->resoff = 0;
		cc->reslen = 0;
	}

	if (len + 4 > (int)sizeof(cc->resbuf)) {
		err("interlaced frame too big %d bytes, more then sizeof resbuf %d bytes. "
				"please modify source code!!! [peer %s:%u]\n", len + 4, 
				sizeof(cc->resbuf), inet_ntoa(cc->peer_addr), cc->peer_port);
		return -1;
	}

	//XXX one times to send a complete interlaced frame
	szbuf[0] = '$';
	szbuf[1] = channel;
	*((uint16_t*)&szbuf[2]) = htons(len);
	ret = send(cc->sockfd, (const char*)szbuf, 4, 0);
	if (ret != 4) {
		if (ret > 0 && ret < 4) {
			cc->resoff = 0;
			cc->reslen = 4 - ret + len;
			memcpy(cc->resbuf, szbuf + ret, 4 - ret);
			memcpy(cc->resbuf + 4 - ret, data, len);
			dbg("rtsp send interlaced header %d bytes, store %d bytes to buffer. [peer %s:%u]\n", 
					ret, cc->reslen, inet_ntoa(cc->peer_addr), cc->peer_port);
			return len;
		}
		if (ret == SOCKET_ERROR && sk_errno() != SK_EAGAIN && sk_errno() != SK_EINTR) {
			warn("rtsp send interlaced header fail: %s. [peer %s:%u]\n", 
					sk_strerror(sk_errno()), inet_ntoa(cc->peer_addr), cc->peer_port);
			return -1;
		}
		return 0;
	}

	ret = send(cc->sockfd, (const char*)data, len, 0);
	if (ret != len) {
		if (ret > 0 && ret < len) {
			cc->resoff = 0;
			cc->reslen = len - ret;
			memcpy(cc->resbuf, data + ret, len - ret);
			dbg("rtsp send interlaced data %d bytes, store %d bytes to buffer. [peer %s:%u]\n", 
					ret, cc->reslen, inet_ntoa(cc->peer_addr), cc->peer_port);
		} else {
			cc->resoff = 0;
			cc->reslen = len;
			memcpy(cc->resbuf, data, len);
			dbg("rtsp send interlaced data fail: %s, store %d bytes to buffer. [peer %s:%u]\n", 
					sk_strerror(sk_errno()), cc->reslen, inet_ntoa(cc->peer_addr), cc->peer_port);
		}
	}

	return len;
}

static int rtsp_recv_rtp_over_udp (struct rtsp_client_connection *cc, int isaudio)
{
	struct rtp_connection *rtp = isaudio ? cc->artp : cc->vrtp;
	struct sockaddr_in inaddr;
	SOCKLEN addrlen = sizeof(inaddr);
	char szbuf[128];
	int  len;

	len = recvfrom(rtp->udp_sockfd[0], szbuf, sizeof(szbuf), MSG_DONTWAIT, (struct sockaddr*)&inaddr, &addrlen);
	if (len == SOCKET_ERROR) {
		if (sk_errno() != SK_EAGAIN && sk_errno() != SK_EINTR) {
			warn("rtp over udp recv failed: %s\n", sk_strerror(sk_errno()));
			return -1;
		}
		return 0;
	}

	//dbg("rtp over udp recv %d bytes [peer %s:%u]\n", len, inet_ntoa(cc->peer_addr), cc->peer_port);

	if (!memcmp(&inaddr.sin_addr, &rtp->peer_addr, sizeof(inaddr.sin_addr)) 
			&& rtp->udp_peerport[0] != ntohs(inaddr.sin_port)) 
	{
		dbg("rtp over udp port change %u to %u. [peer %s:%u]\n", rtp->udp_peerport[0], 
				ntohs(inaddr.sin_port), inet_ntoa(cc->peer_addr), cc->peer_port);
		rtp->udp_peerport[0] = ntohs(inaddr.sin_port);
	}

	//TODO process rtp frame
	return len;
}

static int rtsp_recv_rtcp_over_udp (struct rtsp_client_connection *cc, int isaudio)
{
	struct rtp_connection *rtp = isaudio ? cc->artp : cc->vrtp;
	struct sockaddr_in inaddr;
	SOCKLEN addrlen = sizeof(inaddr);
	char szbuf[128];
	int  len;

	len = recvfrom(rtp->udp_sockfd[1], szbuf, sizeof(szbuf), MSG_DONTWAIT, (struct sockaddr*)&inaddr, &addrlen);
	if (len == SOCKET_ERROR) {
		if (sk_errno() != SK_EAGAIN && sk_errno() != SK_EINTR) {
			warn("rtcp over udp recv failed: %s\n", sk_strerror(sk_errno()));
			return -1;
		}
		return 0;
	}

	//dbg("rtcp over udp recv %d bytes [peer %s:%u]\n", len, inet_ntoa(cc->peer_addr), cc->peer_port);

	if (!memcmp(&inaddr.sin_addr, &rtp->peer_addr, sizeof(inaddr.sin_addr)) 
			&& rtp->udp_peerport[1] != ntohs(inaddr.sin_port)) 
	{
		dbg("rtcp over udp port change %u to %u. [peer %s:%u]\n",rtp->udp_peerport[1], 
				ntohs(inaddr.sin_port), inet_ntoa(cc->peer_addr), cc->peer_port);
		rtp->udp_peerport[1] = ntohs(inaddr.sin_port);
	}

	//TODO process rtcp frame
	return len;
}

static int rtp_send_over_udp (struct rtp_connection *c, const uint8_t *data, int size)
{
	struct sockaddr_in inaddr;
	SOCKET sockfd = c->udp_sockfd[0];
	int ret = -1;

	memset(&inaddr, 0, sizeof(inaddr));
	inaddr.sin_family = AF_INET;
	inaddr.sin_addr = c->peer_addr;
	inaddr.sin_port = htons(c->udp_peerport[0]);

	ret = sendto(sockfd, (const char*)data, size, 0, (struct sockaddr*)&inaddr, sizeof(inaddr));
	if (ret != size) {
		if (sk_errno() != SK_EAGAIN && sk_errno() != SK_EINTR) {
			warn("rtp over udp send %d bytes to %s failed: %s\n", size, inet_ntoa(c->peer_addr), sk_strerror(sk_errno()));
			return -1;
		}
		return 0;
	}
	return size;
}

static int rtsp_tx_video_packet (struct rtsp_client_connection *cc)
{
//	struct rtsp_demo *d = cc->demo;
	struct rtsp_session *s = cc->session;
	struct stream_queue *q = s->vstreamq;
	struct rtp_connection *rtp = cc->vrtp;
	uint8_t *ppacket = NULL;
	int	 *ppktlen = NULL;
	int count = 0;

	/*dbg("index=%d head=%d tail=%d used=%d\n", 
		rtp->streamq_index, 
		streamq_head(q),
		streamq_tail(q),
		streamq_inused(q, rtp->streamq_index));*/

	while (streamq_inused(q, rtp->streamq_index) > 0) {
		streamq_query(q, rtp->streamq_index, (char**)&ppacket, &ppktlen);

		if (*ppktlen > 0) {
			*((uint32_t*)(&ppacket[8])) = htonl(rtp->ssrc); //modify ssrc
			if (rtp->is_over_tcp) {
				if (rtsp_send_interlaced_frame(cc, rtp->tcp_interleaved[0], ppacket, *ppktlen) != *ppktlen) {
					break;
				}
			} else {
				if (rtp_send_over_udp(rtp, ppacket, *ppktlen) != *ppktlen) {
					break;
				}
			}

			rtp->rtcp_packet_count ++;
			rtp->rtcp_octet_count += *ppktlen - 12;//XXX
		}
		rtp->streamq_index = streamq_next(q, rtp->streamq_index);
		count ++;
	}

	return count;
}

static int rtsp_tx_audio_packet (struct rtsp_client_connection *cc)
{
//	struct rtsp_demo *d = cc->demo;
	struct rtsp_session *s = cc->session;
	struct stream_queue *q = s->astreamq;
	struct rtp_connection *rtp = cc->artp;
	uint8_t *ppacket = NULL;
	int	 *ppktlen = NULL;
	int count = 0;

	while (streamq_inused(q, rtp->streamq_index) > 0) {
		streamq_query(q, rtp->streamq_index, (char**)&ppacket, &ppktlen);

		if (*ppktlen > 0) {
			*((uint32_t*)(&ppacket[8])) = htonl(rtp->ssrc); //modify ssrc
			if (rtp->is_over_tcp) {
				if (rtsp_send_interlaced_frame(cc, rtp->tcp_interleaved[0], ppacket, *ppktlen) != *ppktlen) {
					break;
				}
			} else {
				if (rtp_send_over_udp(rtp, ppacket, *ppktlen) != *ppktlen) {
					break;
				}
			}

			rtp->rtcp_packet_count ++;
			rtp->rtcp_octet_count += *ppktlen - 12;//XXX
		}
		rtp->streamq_index = streamq_next(q, rtp->streamq_index);
		count ++;
	}

	return count;
}

struct rtcp_sr {
#ifdef __BIG_ENDIAN__
	uint16_t pt:8;
	uint16_t v:2;
	uint16_t p:1;
	uint16_t rc:5;
#else
	uint16_t rc:5;
	uint16_t p:1;
	uint16_t v:2;
	uint16_t pt:8;
#endif
	uint16_t length;
	uint32_t ssrc;
	uint32_t ntpts_msw;
	uint32_t ntpts_lsw;
	uint32_t rtp_ts;
	uint32_t packet_count;
	uint32_t octet_count;
};

static int rtsp_try_tx_rtcp_sr (struct rtsp_client_connection *cc, int isaudio, uint64_t ts)
{
	struct rtsp_session *s = cc->session;
	struct rtp_connection *rtp = isaudio ? cc->artp : cc->vrtp;
	uint64_t ntptime_of_zero_ts = isaudio ? s->audio_ntptime_of_zero_ts : s->video_ntptime_of_zero_ts;
	uint32_t sample_rate = isaudio ? s->artpe.sample_rate : s->vrtpe.sample_rate;

	struct rtcp_sr sr = {0};
	uint32_t ntpts_msw = 0;
	uint32_t ntpts_lsw = 0;
	uint32_t rtp_ts = 0;
	int size = sizeof(sr);


	if (rtp->rtcp_last_ts && ts < rtp->rtcp_last_ts + 5000000ULL) {
		return 0;
	}

	ntpts_msw = (uint32_t)((ntptime_of_zero_ts + ts) / 1000000ULL);
	ntpts_lsw = (uint32_t)(((ntptime_of_zero_ts + ts) % 1000000ULL) * (1ULL << 32) / 1000000ULL);
	rtp_ts = (uint32_t)(ts * sample_rate / 1000000ULL);

	sr.v = 2;
	sr.p = 0;
	sr.rc = 0;
	sr.pt = 200;
	sr.length = htons(size / 4 - 1);
	sr.ssrc = htonl(rtp->ssrc);
	sr.ntpts_msw = htonl(ntpts_msw);
	sr.ntpts_lsw = htonl(ntpts_lsw);
	sr.rtp_ts = htonl(rtp_ts);
	sr.packet_count = htonl(rtp->rtcp_packet_count);
	sr.octet_count = htonl(rtp->rtcp_octet_count);

	if (rtp->is_over_tcp) {
		int ret = rtsp_send_interlaced_frame(cc, rtp->tcp_interleaved[1], (const uint8_t*)&sr, size);
		if (ret != size) {
			return ret;
		}
	} else {
		struct sockaddr_in inaddr;
		SOCKET sockfd = rtp->udp_sockfd[1];
		int ret = -1;

		memset(&inaddr, 0, sizeof(inaddr));
		inaddr.sin_family = AF_INET;
		inaddr.sin_addr = rtp->peer_addr;
		inaddr.sin_port = htons(rtp->udp_peerport[1]);

		ret = sendto(sockfd, (const char*)&sr, size, 0, (struct sockaddr*)&inaddr, sizeof(inaddr));
		if (ret == SOCKET_ERROR) {
			if (sk_errno() != SK_EAGAIN && sk_errno() != SK_EINTR) {
				warn("rtcp over udp send %d bytes to %s failed: %s. [peer %s:%u]\n", 
						size, inet_ntoa(rtp->peer_addr), sk_strerror(sk_errno()), 
						inet_ntoa(rtp->peer_addr), cc->peer_port);
				return -1;
			}
			return 0;
		}
	}

	rtp->rtcp_last_ts = ts;
	return size;
}

int rtsp_do_event (rtsp_demo_handle demo)
{
	struct rtsp_demo *d = (struct rtsp_demo*)demo;
	struct rtsp_client_connection *cc = NULL;
	struct timeval tv;
	fd_set rfds;
	fd_set wfds;
	SOCKET maxfd;
	int ret;

	if (NULL == d) {
		return -1;
	}

	FD_ZERO(&rfds);
	FD_ZERO(&wfds);

	FD_SET(d->sockfd, &rfds);

	maxfd = d->sockfd;
	TAILQ_FOREACH(cc, &d->connections_qhead, demo_entry) {
		struct rtsp_session *s = cc->session;
		struct rtp_connection *vrtp = cc->vrtp;
		struct rtp_connection *artp = cc->artp;

		FD_SET(cc->sockfd, &rfds);
		if (cc->sockfd > maxfd)
			maxfd = cc->sockfd;

		if (cc->state != RTSP_CC_STATE_PLAYING)
			continue;

		if (vrtp && streamq_inused(s->vstreamq, vrtp->streamq_index) > 0) {
			//add video rtp sock to wfds
			if (vrtp->is_over_tcp) {
				FD_SET(cc->sockfd, &wfds);
			} else {
				FD_SET(vrtp->udp_sockfd[0], &wfds);
				if (vrtp->udp_sockfd[0] > maxfd)
					maxfd = vrtp->udp_sockfd[0];
			}
		}

		if (artp && streamq_inused(s->astreamq, artp->streamq_index) > 0) {
			//add audio rtp sock to wfds
			if (artp->is_over_tcp) {
				FD_SET(cc->sockfd, &wfds);
			} else {
				FD_SET(artp->udp_sockfd[0], &wfds);
				if (artp->udp_sockfd[0] > maxfd)
					maxfd = artp->udp_sockfd[0];
			}
		}

		if (vrtp && (!vrtp->is_over_tcp)) {
			//add video rtp/rtcp sock to rfds
			FD_SET(vrtp->udp_sockfd[0], &rfds);
			FD_SET(vrtp->udp_sockfd[1], &rfds);
			if (vrtp->udp_sockfd[0] > maxfd)
				maxfd = vrtp->udp_sockfd[0];
			if (vrtp->udp_sockfd[1] > maxfd)
				maxfd = vrtp->udp_sockfd[1];
		}

		if (artp && (!artp->is_over_tcp)) {
			//add audio rtp/rtcp sock to rfds
			FD_SET(artp->udp_sockfd[0], &rfds);
			FD_SET(artp->udp_sockfd[1], &rfds);
			if (artp->udp_sockfd[0] > maxfd)
				maxfd = artp->udp_sockfd[0];
			if (artp->udp_sockfd[1] > maxfd)
				maxfd = artp->udp_sockfd[1];
		}
	}

	memset(&tv, 0, sizeof(tv));
	tv.tv_sec = 0;
	tv.tv_usec = 0;

	ret = select(maxfd + 1, &rfds, &wfds, NULL, &tv);
	if (ret < 0) {
		err("select failed : %s\n", strerror(errno));
		return -1;
	}
	if (ret == 0) {
		return 0;
	}

	if (FD_ISSET(d->sockfd, &rfds)) {
		//new client_connection
		rtsp_new_client_connection(d);
	}

	cc = TAILQ_FIRST(&d->connections_qhead); //NOTE do not use TAILQ_FOREACH
	while (cc) {
		struct rtsp_client_connection *cc1 = cc;
		struct rtsp_session *s = cc1->session;
		struct rtp_connection *vrtp = cc1->vrtp;
		struct rtp_connection *artp = cc1->artp;
		cc = TAILQ_NEXT(cc, demo_entry);

		if (FD_ISSET(cc1->sockfd, &rfds)) {
			do {
				rtsp_msg_s reqmsg, resmsg;
				rtsp_msg_init(&reqmsg);
				rtsp_msg_init(&resmsg);

				ret = rtsp_recv_msg(cc1, &reqmsg);
				if (ret == 0)
					break;
				if (ret < 0) {
					rtsp_del_client_connection(cc1);
					cc1 = NULL;
					break;
				}

				if (reqmsg.type == RTSP_MSG_TYPE_INTERLEAVED) {
					//TODO process RTCP over TCP frame
					rtsp_msg_free(&reqmsg);
					continue;
				}

				if (reqmsg.type != RTSP_MSG_TYPE_REQUEST) {
					err("not request frame.\n");
					rtsp_msg_free(&reqmsg);
					continue;
				}

				ret = rtsp_process_request(cc1, &reqmsg, &resmsg);
				if (ret < 0) {
					err("request internal err\n");
				} else {
					rtsp_send_msg(cc1, &resmsg);
				}

				rtsp_msg_free(&reqmsg);
				rtsp_msg_free(&resmsg);
			} while (cc1);

			if (cc1 == NULL)
				continue;
		}

		if (cc1->state != RTSP_CC_STATE_PLAYING)
			continue;

		if (vrtp && streamq_inused(s->vstreamq, vrtp->streamq_index) > 0) {
			//send rtp video packet
			if (vrtp->is_over_tcp && FD_ISSET(cc1->sockfd, &wfds)) {
				rtsp_tx_video_packet(cc1);
				//dbg("resend video packet for tcp [peer %s:%u]\n", inet_ntoa(cc1->peer_addr), cc1->peer_port);
			} else if ((!vrtp->is_over_tcp) && FD_ISSET(vrtp->udp_sockfd[0], &wfds)) {
				rtsp_tx_video_packet(cc1); //very useful for udp
				//dbg("resend video packet for udp [peer %s:%u]\n", inet_ntoa(cc1->peer_addr), cc1->peer_port);
			}
		}

		if (artp && streamq_inused(s->astreamq, artp->streamq_index) > 0) {
			//send rtp audio packet
			if (artp->is_over_tcp && FD_ISSET(cc1->sockfd, &wfds)) {
				rtsp_tx_audio_packet(cc1);
				//dbg("resend audio packet for tcp [peer %s:%u]\n", inet_ntoa(cc1->peer_addr), cc1->peer_port);
			} else if (0 == artp->is_over_tcp && FD_ISSET(artp->udp_sockfd[0], &wfds)) {
				rtsp_tx_audio_packet(cc1); //very useful for udp
				//dbg("resend audio packet for udp [peer %s:%u]\n", inet_ntoa(cc1->peer_addr), cc1->peer_port);
			}
		}

		if (vrtp && (!vrtp->is_over_tcp)) {
			//process video rtcp socket
			if (FD_ISSET(vrtp->udp_sockfd[0], &rfds)) {
				rtsp_recv_rtp_over_udp(cc1, 0);
			}
			if (FD_ISSET(vrtp->udp_sockfd[1], &rfds)) {
				rtsp_recv_rtcp_over_udp(cc1, 0);
			}
		}
		if (artp && (!artp->is_over_tcp)) {
			//process audio rtcp socket
			if (FD_ISSET(artp->udp_sockfd[0], &rfds)) {
				rtsp_recv_rtp_over_udp(cc1, 1);
			}
			if (FD_ISSET(artp->udp_sockfd[1], &rfds)) {
				rtsp_recv_rtcp_over_udp(cc1, 1);
			}
		}
	}

	return 1;
}

int rtsp_tx_video (rtsp_session_handle session, const uint8_t *frame, int len, uint64_t ts)
{
	struct rtsp_session *s = (struct rtsp_session*) session;
	struct stream_queue *q = NULL;
	struct rtsp_client_connection *cc = NULL;
	uint8_t *packets[VRTP_MAX_NBPKTS+1] = {NULL};
	int  pktsizs[VRTP_MAX_NBPKTS+1] = {0};
	int *pktlens[VRTP_MAX_NBPKTS] = {NULL};
	int i, index, count, start;

	if (!s || !frame || s->vcodec_id == RTSP_CODEC_ID_NONE)
		return -1;

	//XXX guess request the number of rtp packet for this frame. +4 for SPS+PPS+VPS+SEI+I frame
	count = (len + RTP_MAX_PKTSIZ - 12 - 4 - 1) / (RTP_MAX_PKTSIZ - 12 - 4) + 4;

	//get free buffer
	q = s->vstreamq;
	index = streamq_tail(q);
	for (i = 0; i < VRTP_MAX_NBPKTS && i < count; i++) {
		if (streamq_next(q, index) == streamq_head(q))
			streamq_pop(q);
		streamq_query(q, index, (char**)&packets[i], &pktlens[i]);
		pktsizs[i] = RTP_MAX_PKTSIZ;
		index = streamq_next(q, index);
	}
	packets[i] = NULL;
	pktsizs[i] = 0;

	//move all slow rtp connections streamq_index to queue tail
	TAILQ_FOREACH(cc, &s->connections_qhead, session_entry) {
		struct rtp_connection *rtp = cc->vrtp;
		if (cc->state != RTSP_CC_STATE_PLAYING || !rtp)
			continue;
		if (!streamq_inused(q, rtp->streamq_index) && rtp->streamq_index != streamq_tail(q)) {
			rtp->streamq_index = streamq_head(q);
			warn("client lost video packet [peer %s:%u]\n", inet_ntoa(cc->peer_addr), cc->peer_port);
		}
	}

	switch (s->vcodec_id) {
	case RTSP_CODEC_ID_VIDEO_H264:
		if (s->vcodec_data.h264.pps_len == 0 || s->vcodec_data.h264.pps_len == 0) {
			if (rtsp_codec_data_parse_from_frame_h264(frame, len, &s->vcodec_data.h264) < 0) {
				warn("rtsp_codec_data_parse_from_frame_h264 failed\n");
			} else {
//				dbg("rtsp_codec_data_parse_from_frame_h264 sps:%u pps:%u success\n", 
//					s->vcodec_data.h264.sps_len, s->vcodec_data.h264.pps_len);
			}
		}
		break;
	case RTSP_CODEC_ID_VIDEO_H265:
		if (s->vcodec_data.h265.pps_len == 0 || s->vcodec_data.h265.pps_len == 0 || s->vcodec_data.h265.vps_len == 0) {
			if (rtsp_codec_data_parse_from_frame_h265(frame, len, &s->vcodec_data.h265) < 0) {
				warn("rtsp_codec_data_parse_from_frame_h265 failed\n");
			} else {
//				dbg("rtsp_codec_data_parse_from_frame_h265 vps:%u sps:%u pps:%u success\n", 
//					s->vcodec_data.h265.vps, s->vcodec_data.h265.sps_len, s->vcodec_data.h265.pps_len);
			}
		}
		break;
	}

	start = 0;
	count = 0;
	while (start < len && packets[count] && pktsizs[count] > 0) {
		const uint8_t *p = NULL;
		int size = 0;
		int ret;

		p = rtsp_find_h264_h265_nalu(frame + start, len - start, &size);
		if (!p) {
			warn("not found nal header\n");
			break;
		}
		//dbg("size:%d\n", size);

		switch (s->vcodec_id) {
		case RTSP_CODEC_ID_VIDEO_H264:
			ret = rtp_enc_h264(&s->vrtpe, p, size, ts, &packets[count], &pktsizs[count]);
			if (ret <= 0) {
				err("rtp_enc_h264 ret = %d\n", ret);
				return -1;
			}
			break;
		case RTSP_CODEC_ID_VIDEO_H265:
			ret = rtp_enc_h265(&s->vrtpe, p, size, ts, &packets[count], &pktsizs[count]);
			if (ret <= 0) {
				err("rtp_enc_h265 ret = %d\n", ret);
				return -1;
			}
			break;
		}

		count += ret;
		start = p - frame + size;
	}

	for (i = 0; i < count; i++) {
		*pktlens[i] = pktsizs[i];
		streamq_push(q);
	}

	//first send
	TAILQ_FOREACH(cc, &s->connections_qhead, session_entry) {
		struct rtp_connection *rtp = cc->vrtp;
		if (cc->state != RTSP_CC_STATE_PLAYING || !rtp)
			continue;

		rtsp_try_tx_rtcp_sr(cc, 0, ts);
		rtsp_tx_video_packet(cc);
	}

	return len;
}

int rtsp_tx_audio (rtsp_session_handle session, const uint8_t *frame, int len, uint64_t ts)
{
	struct rtsp_session *s = (struct rtsp_session*) session;
	struct stream_queue *q = NULL;
	struct rtsp_client_connection *cc = NULL;
	uint8_t *packets[ARTP_MAX_NBPKTS+1] = {NULL};
	int  pktsizs[ARTP_MAX_NBPKTS+1] = {0};
	int *pktlens[ARTP_MAX_NBPKTS] = {NULL};
	int i, index, count;

	if (!s || !frame || s->acodec_id == RTSP_CODEC_ID_NONE)
		return -1;

	//XXX guess request the number of rtp packet for this frame
	count = (len + RTP_MAX_PKTSIZ - 12 - 4 - 1) / (RTP_MAX_PKTSIZ - 12 - 4);

	//get free buffer
	q = s->astreamq;
	index = streamq_tail(q);
	for (i = 0; i < ARTP_MAX_NBPKTS && i < count; i++) {
		if (streamq_next(q, index) == streamq_head(q))
			streamq_pop(q);
		streamq_query(q, index, (char**)&packets[i], &pktlens[i]);
		pktsizs[i] = RTP_MAX_PKTSIZ;
		index = streamq_next(q, index);
	}
	packets[i] = NULL;
	pktsizs[i] = 0;

	//move all slow rtp connections streamq_index to queue tail
	TAILQ_FOREACH(cc, &s->connections_qhead, session_entry) {
		struct rtp_connection *rtp = cc->artp;
		if (cc->state != RTSP_CC_STATE_PLAYING || !rtp)
			continue;
		if (!streamq_inused(q, rtp->streamq_index) && rtp->streamq_index != streamq_tail(q)) {
			rtp->streamq_index = streamq_head(q);
			warn("client lost audio packet [peer %s:%u]\n", inet_ntoa(cc->peer_addr), cc->peer_port);
		}
	}

	switch (s->acodec_id) {
	case RTSP_CODEC_ID_AUDIO_G711A:
	case RTSP_CODEC_ID_AUDIO_G711U:
		count = rtp_enc_g711(&s->artpe, frame, len, ts, packets, pktsizs);
		if (count <= 0) {
			err("rtp_enc_g711 ret = %d\n", count);
			return -1;
		}
		break;
	case RTSP_CODEC_ID_AUDIO_G726:
		count = rtp_enc_g726(&s->artpe, frame, len, ts, packets, pktsizs);
		if (count <= 0) {
			err("rtp_enc_g726 ret = %d\n", count);
			return -1;
		}
		break;
	case RTSP_CODEC_ID_AUDIO_AAC:
		if (s->acodec_data.aac.audio_specific_config_len == 0) {
			if (rtsp_codec_data_parse_from_frame_aac(frame, len, &s->acodec_data.aac) < 0) {
//				warn("rtsp_codec_data_parse_from_frame_aac failed\n");
			} else {
//				dbg("rtsp_codec_data_parse_from_frame_aac success\n");
				s->artpe.sample_rate = s->acodec_data.aac.sample_rate;
			}
		}
		count = rtp_enc_aac(&s->artpe, frame, len, ts, packets, pktsizs);
		if (count <= 0) {
			err("rtp_enc_aac ret = %d\n", count);
			return -1;
		}
		break;
	}

	for (i = 0; i < count; i++) {
		*pktlens[i] = pktsizs[i];
		streamq_push(q);
	}

	//first send
	TAILQ_FOREACH(cc, &s->connections_qhead, session_entry) {
		struct rtp_connection *rtp = cc->artp;
		if (cc->state != RTSP_CC_STATE_PLAYING || !rtp)
			continue;

		rtsp_try_tx_rtcp_sr(cc, 1, ts);
		rtsp_tx_audio_packet(cc);
	}

	return len;
}

//return us from system running
uint64_t rtsp_get_reltime (void)
{
#ifdef __WIN32__
	return (timeGetTime() * 1000ULL);
#endif
#ifdef __LINUX__
	struct timespec tp;
	clock_gettime(CLOCK_MONOTONIC, &tp);
	return (tp.tv_sec * 1000000ULL + tp.tv_nsec / 1000ULL);
#endif
}

//return us from 1970/1/1 00:00:00
static uint64_t rtsp_get_abstime (void)
{
#ifdef __WIN32__
	FILETIME ft;
	uint64_t t;
	GetSystemTimeAsFileTime(&ft);
	t = (uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
	return t / 10 - 11644473600000000ULL;
#endif
#ifdef __LINUX__
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (tv.tv_sec * 1000000ULL + tv.tv_usec);
#endif
}

//return us from 1900/1/1 00:00:00
uint64_t rtsp_get_ntptime (void)
{
#define NTP_OFFSET_US (2208988800000000ULL)
	return (rtsp_get_abstime() + NTP_OFFSET_US);
}

int rtsp_sync_video_ts (rtsp_session_handle session, uint64_t ts, uint64_t ntptime)
{
	struct rtsp_session *s = (struct rtsp_session*) session;

	if (!s || s->vcodec_id == RTSP_CODEC_ID_NONE)
		return -1;

	s->video_ntptime_of_zero_ts = ntptime - ts; //XXX
	return 0;
}

int rtsp_sync_audio_ts (rtsp_session_handle session, uint64_t ts, uint64_t ntptime)
{
	struct rtsp_session *s = (struct rtsp_session*) session;

	if (!s || s->acodec_id == RTSP_CODEC_ID_NONE)
		return -1;

	s->audio_ntptime_of_zero_ts = ntptime - ts; //XXX
	return 0;
}

