/* Frame ABI adapted from OpenIPC/waybeam, commit
 * 0c880d8bc874f803b569aa1bf028de82bb6e6045, src/maruko_ipu_yolo.c.
 * MIT License
 * Copyright (c) 2023 OpenIPC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include <stdint.h>

struct rect { uint16_t x, y, w, h; };
struct frame {
    int tile, format, compression, scan, field, layout;
    uint16_t width, height;
    void *virtual[3];
    uint64_t physical[3];
    uint32_t stride[3], size;
    uint16_t ring_start, ring_total;
    struct { int type; uint32_t global; } isp;
    struct rect crop;
};
struct frame_buffer {
    uint64_t pts, sideband;
    int type;
    uint8_t eos, user;
    uint32_t seq;
    uint8_t drop;
    union { struct frame frame; uint8_t pad[512]; };
    uint8_t custom;
};
struct port { unsigned module, device, channel, port; };
struct port_config {
    struct rect crop;
    uint16_t width, height;
    uint8_t mirror, flip;
    int format, compression;
};
_Static_assert(sizeof(struct port_config) == 24, "SCL port ABI");
_Static_assert(sizeof(struct frame_buffer) == 552, "SYS frame ABI");
