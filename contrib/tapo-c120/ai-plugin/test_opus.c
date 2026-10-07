/* gcc -Wall -Wextra -Werror -fsanitize=address,undefined test_opus.c opus-input.c -logg -lopus -lm -o /tmp/test-opus */
#include "opus-input.h"
#include <assert.h>
#include <math.h>
#include <ogg/ogg.h>
#include <opus/opus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct bytes { unsigned char data[131072]; size_t size; };
struct samples { int16_t pcm[48000]; size_t size; int reject; };
static int collect(const int16_t *pcm, size_t size, void *user)
{
    struct samples *s = user;
    if (s->reject) return -1;
    assert(s->size + size <= 48000);
    memcpy(s->pcm + s->size, pcm, size * sizeof(*pcm)); s->size += size;
    return 0;
}

static void append(struct bytes *b, const void *data, size_t size)
{
    assert(b->size + size <= sizeof(b->data));
    memcpy(b->data + b->size, data, size); b->size += size;
}

static void pages(struct bytes *b, ogg_stream_state *s)
{
    ogg_page p;
    while (ogg_stream_flush(s, &p)) {
        append(b, p.header, p.header_len); append(b, p.body, p.body_len);
    }
}

static void fixture(struct bytes *b, int channels, int gain, struct samples *expected)
{
    ogg_stream_state stream;
    assert(!ogg_stream_init(&stream, 42));
    unsigned char head[19] = "OpusHead";
    head[8] = 1; head[9] = channels; head[10] = 56; head[11] = 1; /* pre-skip 312 */
    head[12] = 128; head[13] = 187; /* informational 48000 Hz */
    head[16] = gain & 255; head[17] = (gain >> 8) & 255;
    ogg_packet p = {.packet=head, .bytes=19, .b_o_s=1};
    assert(!ogg_stream_packetin(&stream, &p)); pages(b, &stream);
    unsigned char tags[16] = "OpusTags";
    p = (ogg_packet){.packet=tags, .bytes=16, .packetno=1};
    assert(!ogg_stream_packetin(&stream, &p)); pages(b, &stream);
    int error;
    OpusEncoder *encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_AUDIO, &error);
    OpusDecoder *decoder = opus_decoder_create(48000, 1, &error);
    assert(encoder && decoder);
    assert(!opus_decoder_ctl(decoder, OPUS_SET_GAIN(gain)));
    for (int frame = 0; frame < 15; ++frame) {
        int16_t pcm[960], decoded[960]; unsigned char encoded[4000];
        for (int i = 0; i < 960; ++i) pcm[i] = 9000 * sin(2 * 3.141592653589793 * 440 * (frame*960+i)/48000);
        int n = opus_encode(encoder, pcm, 960, encoded, sizeof(encoded));
        assert(n > 0 && opus_decode(decoder, encoded, n, decoded, 960, 0) == 960);
        size_t skip = frame == 0 ? 312 : 0, count = 960 - skip - (frame == 14 ? 100 : 0);
        assert(!collect(decoded + skip, count, expected));
        p = (ogg_packet){.packet=encoded, .bytes=n, .packetno=frame+2,
            .granulepos=(frame+1)*960-(frame==14?100:0), .e_o_s=frame==14};
        assert(!ogg_stream_packetin(&stream, &p)); pages(b, &stream);
    }
    ogg_stream_clear(&stream); opus_encoder_destroy(encoder); opus_decoder_destroy(decoder);
}

static int feed(struct opus_input *s, const struct bytes *b, size_t chunk)
{
    for (size_t pos = 0; pos < b->size; ) {
        size_t n = b->size - pos < chunk ? b->size - pos : chunk;
        if (opus_input_feed(s, b->data + pos, n)) return -1;
        pos += n;
    }
    return 0;
}

static void reject(const struct bytes *b)
{
    struct samples out = {0}; struct opus_input *s = opus_input_create(collect, &out);
    assert(s && feed(s, b, 97));
    assert(opus_input_feed(s, "", 0)); /* Failed streams stay failed until recreated. */
    opus_input_destroy(s);
}

int main(void)
{
    assert(!opus_input_create(NULL, NULL)); opus_input_destroy(NULL);
    for (int gain = -512; gain <= 512; gain += 512) {
        struct bytes b = {0}; struct samples expected = {0}; fixture(&b, 1, gain, &expected);
        const size_t chunks[] = {1, 7, 97, 4096, 65536};
        for (unsigned i = 0; i < sizeof(chunks)/sizeof(*chunks); ++i) {
            struct samples out = {0}; struct opus_input *s = opus_input_create(collect, &out);
            assert(s && !opus_input_feed(s, NULL, 0) && !feed(s, &b, chunks[i]));
            assert(out.size == 15*960-312-100 && out.size == expected.size);
            assert(!memcmp(out.pcm, expected.pcm, out.size*sizeof(int16_t)));
            assert(opus_input_feed(s, b.data, b.size)); /* No chained streams. */
            opus_input_destroy(s);
        }
        struct samples out = {.reject=1}; struct opus_input *s = opus_input_create(collect, &out);
        assert(s && feed(s, &b, 31)); opus_input_destroy(s);
        b.data[b.size-1] ^= 1; reject(&b); /* Bad CRC. */
    }
    struct bytes b = {0}; struct samples out = {0}; fixture(&b, 2, 0, &out); reject(&b);
    b = (struct bytes){0}; out = (struct samples){0}; fixture(&b, 1, 0, &out);
    /* Skip the first audio page: libogg must surface a sequence hole. */
    size_t pos = 0;
    for (int i = 0; i < 2; ++i) {
        size_t n = 27 + b.data[pos+26];
        for (size_t j = 0; j < b.data[pos+26]; ++j) n += b.data[pos+27+j];
        pos += n;
    }
    size_t n = 27 + b.data[pos+26];
    for (size_t j = 0; j < b.data[pos+26]; ++j) n += b.data[pos+27+j];
    memmove(b.data+pos, b.data+pos+n, b.size-pos-n); b.size -= n; reject(&b);
    struct opus_input *s = opus_input_create(collect, &out);
    assert(s && opus_input_feed(s, b.data, 65537)); opus_input_destroy(s);
    /* Oversized continued tags cannot grow the packet assembly without bound. */
    b = (struct bytes){0}; ogg_stream_state stream; assert(!ogg_stream_init(&stream, 42));
    unsigned char head[19] = "OpusHead"; head[8]=1; head[9]=1;
    ogg_packet p = {.packet=head, .bytes=19, .b_o_s=1};
    assert(!ogg_stream_packetin(&stream, &p)); pages(&b, &stream);
    unsigned char *large = calloc(1, 100000); assert(large); memcpy(large, "OpusTags", 8);
    p = (ogg_packet){.packet=large, .bytes=100000, .packetno=1};
    assert(!ogg_stream_packetin(&stream, &p)); pages(&b, &stream);
    reject(&b); free(large); ogg_stream_clear(&stream);
    puts("PASS: Opus decoding, chunk boundaries, gain, pre-skip/end trim, CRC/sequence errors, callback failure, bounds and reconnect reset");
    return 0;
}
