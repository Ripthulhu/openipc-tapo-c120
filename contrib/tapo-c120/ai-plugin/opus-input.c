/* Streaming demux/decoding uses the firmware's libogg/libopus, not a PCM endpoint. */
#include "opus-input.h"
#include <ogg/ogg.h>
#include <opus/opus.h>
#include <stdlib.h>
#include <string.h>

#define BUFFER_LIMIT 65536
struct opus_input {
    ogg_sync_state sync;
    ogg_stream_state stream;
    OpusDecoder *decoder;
    opus_pcm_fn callback;
    void *user;
    unsigned headers, skip;
    int started, ended, failed;
    ogg_int64_t decoded;
};

struct opus_input *opus_input_create(opus_pcm_fn callback, void *user)
{
    if (!callback) return NULL;
    struct opus_input *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    if (ogg_sync_init(&s->sync) || ogg_stream_init(&s->stream, 0)) {
        opus_input_destroy(s); return NULL;
    }
    s->callback = callback; s->user = user;
    return s;
}

void opus_input_destroy(struct opus_input *s)
{
    if (!s) return;
    if (s->decoder) opus_decoder_destroy(s->decoder);
    ogg_stream_clear(&s->stream); ogg_sync_clear(&s->sync); free(s);
}

static int packet(struct opus_input *s, ogg_packet *p)
{
    const unsigned char *b = p->packet;
    if (s->headers == 0) {
        /* Only the camera's mono mapping is supported; do not guess other layouts. */
        if (!p->b_o_s || p->bytes < 19 || memcmp(b, "OpusHead", 8) ||
            b[8] == 0 || b[8] > 15 || b[9] != 1 || b[18] != 0) return -1;
        int error;
        s->decoder = opus_decoder_create(OPUS_INPUT_RATE, 1, &error);
        if (!s->decoder) return -1;
        s->skip = b[10] | b[11] << 8;
        int gain = b[16] | b[17] << 8;
        if (gain >= 32768) gain -= 65536;
        if (opus_decoder_ctl(s->decoder, OPUS_SET_GAIN(gain))) return -1;
        ++s->headers; return 0;
    }
    if (s->headers == 1) {
        if (p->bytes < 16 || memcmp(b, "OpusTags", 8)) return -1;
        /* Tags are bounded by BUFFER_LIMIT and ignored, never parsed as commands. */
        ++s->headers; return 0;
    }
    int16_t pcm[5760]; /* Opus permits at most 120 ms per packet. */
    if (p->bytes <= 0 || p->bytes > BUFFER_LIMIT) return -1;
    int n = opus_decode(s->decoder, b, p->bytes, pcm, 5760, 0);
    if (n < 0) return -1;
    s->decoded += n;
    if (p->e_o_s) {
        if (p->granulepos < s->decoded - n || p->granulepos > s->decoded) return -1;
        n -= s->decoded - p->granulepos;
    }
    unsigned skip = s->skip < (unsigned)n ? s->skip : (unsigned)n;
    s->skip -= skip;
    return n > (int)skip ? s->callback(pcm + skip, n - skip, s->user) : 0;
}

int opus_input_feed(struct opus_input *s, const void *data, size_t size)
{
    if (!s || s->failed) return -1;
    if (!size) return 0;
    if (!data || s->ended || size > BUFFER_LIMIT ||
        s->sync.fill - s->sync.returned > BUFFER_LIMIT - (long)size) goto failed;
    char *buf = ogg_sync_buffer(&s->sync, size);
    if (!buf) goto failed;
    memcpy(buf, data, size);
    if (ogg_sync_wrote(&s->sync, size)) goto failed;
    ogg_page page;
    int rc;
    while ((rc = ogg_sync_pageout(&s->sync, &page)) > 0) {
        if (s->ended || ogg_page_version(&page) != 0) goto failed;
        if (!s->started) {
            if (!ogg_page_bos(&page) || ogg_page_continued(&page) || ogg_page_pageno(&page) != 0) goto failed;
            if (ogg_stream_reset_serialno(&s->stream, ogg_page_serialno(&page))) goto failed;
            s->started = 1;
        } else if (ogg_page_bos(&page)) goto failed;
        /* Bound continued packets before libogg grows its assembly buffer. */
        if (page.body_len > BUFFER_LIMIT ||
            s->stream.body_fill - s->stream.body_returned > BUFFER_LIMIT - page.body_len ||
            s->stream.lacing_fill - s->stream.lacing_returned > 1024 ||
            ogg_stream_pagein(&s->stream, &page)) goto failed;
        ogg_packet p;
        int result;
        while ((result = ogg_stream_packetout(&s->stream, &p)) > 0)
            if (packet(s, &p)) goto failed;
        if (result < 0) goto failed;
        if (ogg_page_eos(&page)) s->ended = 1;
    }
    if (rc < 0) goto failed; /* Reconnect on CRC errors or holes; never classify stale audio. */
    return 0;
failed:
    s->failed = 1;
    return -1;
}
