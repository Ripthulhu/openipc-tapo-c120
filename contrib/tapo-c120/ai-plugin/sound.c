/* Stock 8 kHz SED frontend: periodic Hann, power FFT, HTK mel, log10, int16. */
#include "sound.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "kiss_fft.h"

struct sound_dsp {
    kiss_fft_cfg fft;
    sound_tensor_fn callback;
    void *user;
    unsigned rate, decimate, used, frames, column, since;
    float level_dbfs, gain;
    int active[SOUND_FRAMES];
    float pcm[512], window[512];
    struct { unsigned first, count; float values[17]; } weights[64];
    int16_t history[SOUND_VALUES], tensor[SOUND_VALUES];
};

struct sound_dsp *sound_create(unsigned rate, float gain_db, sound_tensor_fn callback, void *user)
{
    if ((rate != 8000 && rate != 16000) || !isfinite(gain_db) || gain_db < 0 || gain_db > 24 || !callback) return NULL;
    struct sound_dsp *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->rate = rate; s->callback = callback; s->user = user; s->level_dbfs = -120;
    s->gain = powf(10, gain_db/20);
    s->fft = kiss_fft_alloc(512, 0, NULL, NULL);
    if (!s->fft) { sound_destroy(s); return NULL; }
    for (int n = 0; n < 512; ++n) s->window[n] = .5f - .5f * cos(2*M_PI*n/512);
    float points[66];
    for (int n = 0; n < 66; ++n) points[n] = 700*(pow(1+4000.0/700, n/65.0)-1);
    for (int m = 0; m < 64; ++m) for (int k = 0; k < 257; ++k) {
        float hz = k*(8000.0f/512);
        float w = fminf((hz-points[m])/(points[m+1]-points[m]),
                       (points[m+2]-hz)/(points[m+2]-points[m+1]));
        if (w < .0001f) continue;
        unsigned count = s->weights[m].count;
        if (!count) s->weights[m].first = k;
        if (count == 17 || (unsigned)k != s->weights[m].first + count) { sound_destroy(s); return NULL; }
        s->weights[m].values[count] = w;
        ++s->weights[m].count;
    }
    return s;
}

void sound_destroy(struct sound_dsp *s)
{
    if (!s) return;
    free(s->fft); free(s);
}

static int frame(struct sound_dsp *s)
{
    kiss_fft_cpx in[512], out[512];
    float power[257], mel[64], peak = -100, activity = 0, energy = 0;
    for (int n = 0; n < 512; ++n) {
        activity += fabsf(s->pcm[n]);
        energy += s->pcm[n]*s->pcm[n];
        in[n].r = s->pcm[n]*s->window[n]; in[n].i = 0;
    }
    kiss_fft(s->fft, in, out);
    s->level_dbfs = 10*log10f(fmaxf(energy/512, 1e-12f));
    for (int k = 0; k < 257; ++k) power[k] = out[k].r*out[k].r + out[k].i*out[k].i;
    /* The stock frontend uses magnitude, not squared magnitude, at the endpoints. */
    power[0] = fabsf(out[0].r); power[256] = fabsf(out[256].r);
    for (int m = 0; m < 64; ++m) {
        float sum = 0;
        for (unsigned k = 0; k < s->weights[m].count; ++k)
            sum += power[s->weights[m].first+k]*s->weights[m].values[k];
        mel[m] = 10*log10f(fmaxf(fabsf(sum), 1e-10f));
        peak = fmaxf(peak, mel[m]);
    }
    unsigned column = s->column;
    for (int m = 0; m < 64; ++m) {
        float v = fmaxf(mel[m], peak-80)/.0030518509447575f;
        s->history[m*101+column] = (int16_t)fmaxf(-32768, fminf(32767, v));
    }
    s->active[column] = activity > 9999.0f/32768;
    s->column = (column+1)%101;
    if (s->frames < 101) ++s->frames;
    ++s->since;
    if (s->frames < 101 || (s->since < 12 && s->frames == 101)) return 0;
    s->since = 0;
    int active = 0;
    for (unsigned n = 0; n < 101; ++n) active += s->active[n];
    for (unsigned m = 0; m < 64; ++m) for (unsigned n = 0; n < 101; ++n)
        s->tensor[m*101+n] = s->history[m*101+(s->column+n)%101];
    return s->callback(s->tensor, active >= 5, s->user);
}

static int samples(struct sound_dsp *s, const int16_t *pcm, size_t count, int decimate)
{
    for (size_t n = 0; n < count; ++n) {
        if (decimate && (s->decimate++ & 1)) continue;
        s->pcm[s->used++] = fmaxf(-1, fminf(1, pcm[n]/32768.0f*s->gain));
        if (s->used == 512) {
            if (frame(s)) return -1;
            memmove(s->pcm, s->pcm+160, 352*sizeof(float)); s->used = 352;
        }
    }
    return 0;
}

float sound_level_dbfs(const struct sound_dsp *s) { return s ? s->level_dbfs : -120; }

int sound_pcm(struct sound_dsp *s, const int16_t *pcm, size_t count)
{
    if (!s) return -1;
    return samples(s, pcm, count, s->rate != 8000);
}

#ifdef SOUND_TEST
#include <stdio.h>
static int emit(const int16_t *tensor, int active, void *user)
{
    (void)active; (void)user;
    return fwrite(tensor, sizeof(int16_t), SOUND_VALUES, stdout) != SOUND_VALUES;
}
int main(int argc, char **argv)
{
    unsigned rate = argc >= 2 ? strtoul(argv[1], NULL, 10) : 8000;
    float gain = argc == 3 ? strtof(argv[2], NULL) : 0;
    struct sound_dsp *s = sound_create(rate, gain, emit, NULL);
    if (!s) return 1;
    int16_t pcm[997]; size_t count; int rc = 0;
    while ((count = fread(pcm, sizeof(*pcm), 997, stdin))) if (sound_pcm(s, pcm, count)) { rc=1; break; }
    if (ferror(stdin)) rc = 1;
    sound_destroy(s); return rc;
}
#endif
