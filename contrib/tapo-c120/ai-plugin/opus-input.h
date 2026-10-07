#ifndef C120_OPUS_INPUT_H
#define C120_OPUS_INPUT_H
#include <stddef.h>
#include <stdint.h>

#define OPUS_INPUT_RATE 16000
struct opus_input;
typedef int (*opus_pcm_fn)(const int16_t *, size_t, void *);
struct opus_input *opus_input_create(opus_pcm_fn callback, void *user);
void opus_input_destroy(struct opus_input *s);
/* Mono Ogg Opus, decoded at 16 kHz. Nonzero means discard this connection. */
int opus_input_feed(struct opus_input *s, const void *data, size_t size);
#endif
