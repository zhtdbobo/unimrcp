/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HTTP_STREAM_H
#define HTTP_STREAM_H
#include "http_common.h"

/* Callbacks and poll/close run only on the channel worker. PCM is host endian.
 * A sink must accept the whole block (1), request backpressure (0), or fail (-1).
 * A source returns bytes, SIZE_MAX on failure, and sets eof after its last byte.
 */
typedef int (*hp_pcm_sink)(void *, const unsigned char *, size_t);
typedef size_t (*hp_pcm_source)(void *, unsigned char *, size_t, int *eof);
typedef struct hp_stream hp_stream;

hp_stream *hp_stream_tts(const char *url, const char *text, size_t len,
                         const char *id, long timeout, hp_pcm_sink sink, void *context);
hp_stream *hp_stream_asr(const char *url, const char *id, long timeout,
                         long silence_ms, hp_pcm_source source, void *context);
/* 0: running, 1: successfully finished, -1: failed. Never waits for audio. */
int hp_stream_poll(hp_stream *stream);
const char *hp_stream_text(hp_stream *stream, size_t *len);
void hp_stream_close(hp_stream *stream);

#define HP_RING_CAP (2u * 1024u * 1024u)
typedef struct { unsigned char *data; size_t head, len, cap; } hp_ring;
int hp_ring_write(hp_ring *ring, const void *data, size_t len);
size_t hp_ring_read(hp_ring *ring, void *data, size_t len);
#endif
