"""Host checks for the actual SC438HAI C gain/exposure callbacks (requires cc)."""
from pathlib import Path
import hashlib
import re
import struct
import subprocess
import tempfile


REPO = Path(__file__).resolve().parents[2]
source = (REPO / "sigmastar/infinity6c/sensor_sc438hai_mipi.c").read_text()

# Stock HDR register table, independently extracted from .data+0x50.
table = source.split("I2C_ARRAY Sensor_init_table_HDR[] = {", 1)[1].split("};", 1)[0]
pairs = re.findall(r"\{ (0x[0-9a-f]+), (0x[0-9a-f]+) \}", table)
assert len(pairs) == 227
assert hashlib.sha256(b"".join(struct.pack("<HH", int(a, 16), int(b, 16))
                             for a, b in pairs)).hexdigest() == \
    "e321e2798882e1bbc99b6191ef81bc8766399f27c2d406c24beb88546d5bfc35"


def function(name):
    start = source.index("static int " + name + "(")
    # Skip forward declarations, retaining the function body verbatim.
    while source.index(";", start) < source.index("{", start):
        start = source.index("static int " + name + "(", start + 1)
    return source[start:source.index("\n}", start) + 2]


header = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int CUS_CAMSENSOR_ORIT;
typedef struct { u16 reg, data; } I2C_ARRAY;
typedef struct __ms_cus_sensor {
    void *private_data;
    struct { struct { int mipi_hdr_mode, mipi_hdr_virtual_channel_num; } attr_mipi; } interface_attr;
    struct { unsigned ulcur_res; struct { u32 min_fps, max_fps; } res[1]; } video_res_supported;
} ms_cus_sensor;
typedef struct { u32 min, max, step; } CUS_SHUTTER_INFO;
typedef int CUS_CAMSENSOR_AE_STATUS_NOTIFY;
#define CUS_HDR_MODE_DCG 2
#define CUS_FRAME_ACTIVE 1
#define CUS_FRAME_INACTIVE 0
#define SUCCESS 0
#define FAIL -1
#define SENSOR_DMSG(...) ((void)0)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
static unsigned writes, fail_write;
static int write_regs(I2C_ARRAY *regs, unsigned len) {
    (void)regs; (void)len;
    return ++writes == fail_write ? FAIL : SUCCESS;
}
#define SensorRegArrayW(a, n) write_regs(a, n)
'''
constants = "\n".join(re.findall(
    r"^(?:#define (?:SENSOR_MAXGAIN|Preview_MIN_FPS|HDR_LINE_PERIOD|HDR_VTS_30FPS) .*|u32 (?:Preview_line_period|vts_30fps) = .*;)$",
    source, re.M))
params = source[source.index("typedef struct {"):source.index("} sc438hai_params;") + len("} sc438hai_params;")]
callbacks = "\n".join(function(name) for name in (
    "sc438hai_is_hdr", "sc438hai_is_short", "sc438hai_set_hdr_exposure",
    "pCus_SetAEGain", "pCus_GetAEGain", "pCus_SetAEUSecs", "pCus_GetAEUSecs",
    "pCus_SetFPS", "pCus_GetFPS", "pCus_GetAEMinMaxUSecs",
    "sc438hai_GetShutterInfo", "pCus_AEStatusNotify"))
check = r'''
int main(void) {
    sc438hai_params p = {0};
    ms_cus_sensor h = {.private_data = &p};
    double previous = 0;
    u32 actual;
    p.expo.vts = 1600;
    /* Exhaust the gain domain, including every analogue/digital stage boundary. */
    for (u32 gain = 1024; gain <= SENSOR_MAXGAIN * 1024; ++gain) {
        assert(pCus_SetAEGain(&h, gain) == SUCCESS);
        assert(pCus_GetAEGain(&h, &actual) == SUCCESS && actual == gain);
        u32 a = p.tGain_reg[2].data;
        double coarse = a & 0x80 ? 2.530 * ((a & 15) + 1) : a + 1;
        double decoded = coarse * p.tGain_reg[3].data / 32.0 *
            (p.tGain_reg[0].data + 1) * p.tGain_reg[1].data / 128.0;
        assert(decoded >= previous);
        assert(decoded >= gain / 1024.0 * 0.95 && decoded <= gain / 1024.0 * 1.02);
        assert(p.tGain_reg[1].data >= 128 && p.tGain_reg[1].data <= 252);
        assert(p.tGain_reg[3].data >= 32 && p.tGain_reg[3].data <= 63);
        previous = decoded;
    }
    pCus_SetAEGain(&h, 0);
    pCus_GetAEGain(&h, &actual);
    assert(actual == 1024);
    pCus_SetAEGain(&h, UINT32_MAX);
    pCus_GetAEGain(&h, &actual);
    assert(actual == SENSOR_MAXGAIN * 1024);
    const u32 exposures[] = {0, 1, 42, 1000, 33000, 200000, UINT32_MAX, 33000};
    for (unsigned i = 0; i < sizeof(exposures) / sizeof(exposures[0]); ++i) {
        p.reg_dirty = false;
        pCus_SetAEUSecs(&h, exposures[i]);
        pCus_GetAEUSecs(&h, &actual);
        assert(p.reg_dirty);
        assert(p.expo.line >= 2 && p.expo.line <= 16000);
        u32 vts = p.tVts_reg[0].data * 256 + p.tVts_reg[1].data;
        assert(vts >= 1600 && p.expo.line + 8 <= vts);
        assert(actual >= 41 && actual <= 333333);
        if (exposures[i] == 33000) {
            assert(vts == 1600 && actual >= 32979 && actual <= 33000);
        }
    }
    h.video_res_supported.res[0].min_fps = 3;
    h.video_res_supported.res[0].max_fps = 30;
    assert(pCus_SetFPS(&h, 30) == SUCCESS && pCus_GetFPS(&h) == 30);
    h.interface_attr.attr_mipi.mipi_hdr_mode = CUS_HDR_MODE_DCG;
    ms_cus_sensor short_h = h;
    short_h.interface_attr.attr_mipi.mipi_hdr_virtual_channel_num = 1;
    for (u32 fps = 3000; fps <= 30000; fps += 7) {
        assert(pCus_SetFPS(&h, fps) == SUCCESS);
        assert(p.expo.vts == HDR_VTS_30FPS * 30000 / fps);
        for (unsigned i = 0; i < ARRAY_SIZE(exposures); ++i) {
            pCus_SetAEUSecs(&h, exposures[i]);
            pCus_SetAEUSecs(&short_h, exposures[i]);
            assert(p.expo.line >= 2 && p.short_line >= 2);
            assert(p.expo.line + p.max_short_exp + 11 <= p.expo.vts);
            assert(p.short_line + 9 <= p.max_short_exp);
            assert(p.tVts_reg[0].data * 256U + p.tVts_reg[1].data == p.expo.vts);
            assert(p.tShortLimit_reg[0].data * 256U + p.tShortLimit_reg[1].data == p.max_short_exp);
            pCus_GetAEUSecs(&h, &actual);
            assert(actual == p.expo.line * HDR_LINE_PERIOD / 1000);
            pCus_GetAEUSecs(&short_h, &actual);
            assert(actual == p.short_line * HDR_LINE_PERIOD / 1000);
        }
    }
    pCus_SetFPS(&h, 3);
    pCus_SetAEUSecs(&h, UINT32_MAX);
    pCus_SetAEUSecs(&short_h, UINT32_MAX);
    pCus_SetFPS(&short_h, 30);
    assert(pCus_GetFPS(&h) == 30 && p.expo.vts == 3200);
    assert(p.max_short_exp == 186 && p.expo.line == 3003 && p.short_line == 177);
    assert(pCus_SetFPS(&h, 0) == FAIL && pCus_SetFPS(&h, 31) == FAIL);
    assert(pCus_SetFPS(&h, UINT32_MAX) == FAIL && p.expo.vts == 3200);
    u32 min, max;
    CUS_SHUTTER_INFO info;
    pCus_GetAEMinMaxUSecs(&short_h, &min, &max);
    sc438hai_GetShutterInfo(&short_h, &info);
    assert(min == 21 && max == 177 * HDR_LINE_PERIOD / 1000);
    assert(info.min == 2 * HDR_LINE_PERIOD + 999 && info.step == HDR_LINE_PERIOD);
    assert(info.max == 177 * HDR_LINE_PERIOD);
    pCus_SetAEGain(&h, 2048);
    pCus_SetAEGain(&short_h, 8192);
    pCus_GetAEGain(&h, &actual);
    assert(actual == 2048);
    pCus_GetAEGain(&short_h, &actual);
    assert(actual == 8192 && p.tGain_reg[2].data != p.tShortGain_reg[2].data);
    /* No duplicate commits, and a failed I2C transaction must be retried. */
    pCus_AEStatusNotify(&h, CUS_FRAME_ACTIVE);
    assert(writes == 0 && p.reg_dirty);
    fail_write = 4;
    assert(pCus_AEStatusNotify(&short_h, CUS_FRAME_ACTIVE) == FAIL && p.reg_dirty);
    writes = fail_write = 0;
    assert(pCus_AEStatusNotify(&short_h, CUS_FRAME_ACTIVE) == SUCCESS);
    assert(writes == 6 && !p.reg_dirty);
    puts("PASS SC438HAI: stock HDR table; linear gain/exposure; HDR planes, timing, bounds and I2C retry");
}
'''

if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="sc438hai-test-") as tmp:
        path = Path(tmp)
        (path / "test.c").write_text("\n".join((header, constants, params, callbacks, check)))
        subprocess.run(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-O2",
                        str(path / "test.c"), "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True)
