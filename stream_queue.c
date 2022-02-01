/*************************************************************************
	> File Name: stream_queue.c
	> Author: bxq
	> Mail: 544177215@qq.com 
	> Created Time: Sunday, May 22, 2016 PM10:25:44 CST
 ************************************************************************/

#include <stdlib.h>

#include "comm.h"
#include "stream_queue.h"

struct stream_queue *streamq_alloc (int pktsiz, int nbpkts)
{
	struct stream_queue *q;

	if (pktsiz <= 0 || nbpkts <= 0)
		return NULL;

	q = (struct stream_queue*) calloc(1, sizeof(struct stream_queue) + pktsiz * nbpkts + sizeof(int) * nbpkts);
	if (!q) {
		err("alloc memory failed for stream_queue\n");
		return NULL;
	}

	q->pktsiz = pktsiz;
	q->nbpkts = nbpkts;
	q->pktlen = (int*)(((char*)q) + sizeof(struct stream_queue));
	q->buf   = (char*)(((char*)q) + sizeof(struct stream_queue) + sizeof(int) * nbpkts);

	return q;
}

int streamq_query (struct stream_queue *q, int index, char **ppacket, int **ppktlen)
{
	if (!q || index >= q->nbpkts)
		return -1;
	if (ppacket)
		*ppacket = q->buf + index * q->pktsiz;
	if (ppktlen)
		*ppktlen = &q->pktlen[index];
	return 0;
}

int streamq_inused (struct stream_queue *q, int index)
{
	if (!q)
		return -1;
	if ((q->head <= index && index < q->tail) || (q->head > q->tail && (index >= q->head || index < q->tail)))
		return 1;
	return 0;
}

int streamq_next (struct stream_queue *q, int index)
{
	if (!q)
		return -1;
	
	index = (index + 1) % q->nbpkts;
	return index;
}

int streamq_head (struct stream_queue *q)
{
	if (!q)
		return -1;
	return q->head;
}

int streamq_tail (struct stream_queue *q)
{
	if (!q)
		return -1;
	return q->tail;
}

int streamq_push (struct stream_queue *q)
{
	if (!q)
		return -1;
	if ((q->tail + 1) % q->nbpkts == q->head)
		return -1;
	q->tail = (q->tail + 1) % q->nbpkts;
	return q->tail;
}

int streamq_pop (struct stream_queue *q)
{
	if (!q)
		return -1;
	if (q->head == q->tail)
		return -1;
	q->head = (q->head + 1) % q->nbpkts;
	return q->head;
}

void streamq_free (struct stream_queue *q)
{
	if (q) {
		free(q);
	}
}

