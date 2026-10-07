#ifndef C120_SOUND_H
#define C120_SOUND_H
#include <stddef.h>
#include <stdint.h>

#define SOUND_BINS 64
#define SOUND_FRAMES 101
#define SOUND_VALUES (SOUND_BINS * SOUND_FRAMES)

struct sound_dsp;
typedef int (*sound_tensor_fn)(const int16_t *, int active, void *);
struct sound_dsp *sound_create(unsigned rate, float gain_db, sound_tensor_fn callback, void *user);
void sound_destroy(struct sound_dsp *s);
int sound_pcm(struct sound_dsp *s, const int16_t *pcm, size_t count);
float sound_level_dbfs(const struct sound_dsp *s);
#endif
