/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HTTP_COMMON_H
#define HTTP_COMMON_H
#include <stddef.h>
#include <stdint.h>

#define HP_MAX_AUDIO (25u * 1024u * 1024u)
#define HP_MAX_TEXT (80u * 1024u)
typedef struct { unsigned char *data; size_t len, cap, limit; } hp_buffer;
typedef int (*hp_cancel_fn)(void *);

void hp_buffer_free(hp_buffer *b);
int hp_buffer_append(hp_buffer *b, const void *data, size_t len);
int hp_rate_valid(int rate);
int hp_wav_header(unsigned char header[44], size_t len, int rate);
int hp_wav_encode(const unsigned char *pcm, size_t len, int rate, hp_buffer *wav);
int hp_wav_decode(const unsigned char *wav, size_t len, int rate, hp_buffer *pcm);
char *hp_nlsml(const char *text, size_t len);
int hp_tts(const char *url, const char *text, size_t len, const char *request_id,
           int rate, long timeout_ms, hp_cancel_fn cancel, void *context, hp_buffer *pcm);
int hp_asr(const char *url, const unsigned char *pcm, size_t len, const char *request_id,
           int rate, long timeout_ms, hp_cancel_fn cancel, void *context, char **text, size_t *text_len);
#endif
