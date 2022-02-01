/*************************************************************************
	> File Name: stream_queue.h
	> Author: bxq
	> Mail: 544177215@qq.com 
	> Created Time: Sunday, May 22, 2016 PM09:35:22 CST
 ************************************************************************/

#ifndef __STREAM_QUEUE_H__
#define __STREAM_QUEUE_H__

#ifdef __cplusplus
extern "C" {
#endif

struct stream_queue {
	int pktsiz;
	int nbpkts;
	int head;
	int tail;
	int *pktlen;
	char *buf;
};

struct stream_queue *streamq_alloc (int pktsiz, int nbpkts);
int streamq_query (struct stream_queue *q, int index, char **ppacket, int **ppktlen);
int streamq_inused (struct stream_queue *q, int index);
int streamq_next (struct stream_queue *q, int index);
int streamq_head (struct stream_queue *q);
int streamq_tail (struct stream_queue *q);
int streamq_push (struct stream_queue *q);
int streamq_pop (struct stream_queue *q);
void streamq_free (struct stream_queue *q);

#ifdef __cplusplus
}
#endif
#endif

