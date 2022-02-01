/*************************************************************************
	> File Name: rtp_enc.h
	> Author: bxq
	> Mail: 544177215@qq.com 
	> Created Time: Saturday, December 19, 2015 PM08:27:54 CST
 ************************************************************************/

#ifndef __RTP_ENC_H__
#define __RTP_ENC_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct __rtp_enc 
{
	uint8_t  pt;
	uint16_t seq;
	uint32_t ssrc;
	uint32_t sample_rate;
} rtp_enc;

int rtp_enc_h264 (rtp_enc *e, const uint8_t *frame, int len, uint64_t ts, uint8_t *packets[], int pktsizs[]);
int rtp_enc_h265 (rtp_enc *e, const uint8_t *frame, int len, uint64_t ts, uint8_t *packets[], int pktsizs[]);
int rtp_enc_aac (rtp_enc *e, const uint8_t *frame, int len, uint64_t ts, uint8_t *packets[], int pktsizs[]);
int rtp_enc_g711 (rtp_enc *e, const uint8_t *frame, int len, uint64_t ts, uint8_t *packets[], int pktsizs[]);
int rtp_enc_g726 (rtp_enc *e, const uint8_t *frame, int len, uint64_t ts, uint8_t *packets[], int pktsizs[]);

#ifdef __cplusplus
}
#endif
#endif

