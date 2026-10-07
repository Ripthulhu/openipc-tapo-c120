/*
 * SC438HAI MIPI sensor driver for SigmaStar Infinity6C.
 *
 * This is an OpenIPC-style source driver built from the TP-Link Tapo C120
 * stock register table and the existing Infinity6C SmartSens drivers.
 * Stock reference: C120 v1, 1.4.4 Build 260106 Rel.62350n, sc438hai_2lane.ko
 * SHA256: cdc1a5acba82e168ec45ed0be917a591eacb974699abeb2817e4bda73d82b36d
 * Linear and experimental four-lane HDR modes. Linear remains the default;
 * HDR requires a compatible capture pipeline and board lane wiring.
 * The stock binary uses a different ms_cus_sensor ABI.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <drv_sensor_common.h>
#include <sensor_i2c_api.h>
#include <drv_sensor.h>

#ifdef __cplusplus
}
#endif

SENSOR_DRV_ENTRY_IMPL_BEGIN_EX(sc438hai);

#ifndef ARRAY_SIZE
#define ARRAY_SIZE CAM_OS_ARRAY_SIZE
#endif

#define SENSOR_PAD_GROUP_SET CUS_SENSOR_PAD_GROUP_A
#define SENSOR_CHANNEL_NUM (0)
#define SENSOR_CHANNEL_MODE_LINEAR CUS_SENSOR_CHANNEL_MODE_REALTIME_NORMAL

#define SENSOR_MIPI_LANE_NUM (2)
#define SENSOR_CSI_PAD_LANE_CFG (2)
#define SENSOR_HDR_LANES 4
#define HDR_LINE_PERIOD 10417
#define HDR_VTS_30FPS 3200

#define SENSOR_DBG 0

#define SENSOR_ISP_TYPE ISP_EXT
#define SENSOR_IFBUS_TYPE CUS_SENIF_BUS_MIPI
#define SENSOR_MIPI_HSYNC_MODE PACKET_HEADER_EDGE1
#define SENSOR_DATAPREC CUS_DATAPRECISION_10
#define SENSOR_DATAMODE CUS_SEN_10TO12_9000
#define SENSOR_BAYERID CUS_BAYER_BG
#define SENSOR_RGBIRID CUS_RGBIR_NONE
#define SENSOR_ORIT CUS_ORIT_M0F0
#define SENSOR_MAXGAIN 1255
#define Preview_MCLK_SPEED CUS_CMU_CLK_27MHZ

u32 Preview_line_period = 20834;
u32 vts_30fps = 1600;

#define Preview_WIDTH 2688
#define Preview_HEIGHT 1520
#define Preview_MAX_FPS 30
#define Preview_MIN_FPS 3
#define Preview_CROP_START_X 0
#define Preview_CROP_START_Y 0

#define SENSOR_I2C_ADDR 0x60
#define SENSOR_I2C_SPEED 240000
#define SENSOR_I2C_LEGACY I2C_NORMAL_MODE
#define SENSOR_I2C_FMT I2C_FMT_A16D8

#define SENSOR_PWDN_POL CUS_CLK_POL_NEG
#define SENSOR_RST_POL CUS_CLK_POL_NEG
#define SENSOR_VSYNC_POL CUS_CLK_POL_NEG
#define SENSOR_HSYNC_POL CUS_CLK_POL_POS
#define SENSOR_PCLK_POL CUS_CLK_POL_POS

#if defined(SENSOR_MODULE_VERSION)
#define TO_STR_NATIVE(e) #e
#define TO_STR_PROXY(m, e) m(e)
#define MACRO_TO_STRING(e) TO_STR_PROXY(TO_STR_NATIVE, e)
static char *sensor_module_version = MACRO_TO_STRING(SENSOR_MODULE_VERSION);
module_param(sensor_module_version, charp, S_IRUGO);
#endif

static int cus_camsensor_release_handle(ms_cus_sensor *handle);
static int pCus_SetAEGain(ms_cus_sensor *handle, u32 gain);
static int pCus_SetAEUSecs(ms_cus_sensor *handle, u32 us);
static int pCus_SetFPS(ms_cus_sensor *handle, u32 fps);
static int pCus_SetOrien(ms_cus_sensor *handle, CUS_CAMSENSOR_ORIT orit);
static int g_sensor_ae_min_gain = 1024;

CUS_MCLK_FREQ UseParaMclk(void);

typedef struct {
    struct {
        u32 sclk;
        u32 hts;
        u32 vts;
        u32 ho;
        u32 xinc;
        u32 line_freq;
        u32 us_per_line;
        u32 final_us;
        u32 final_gain;
        u32 back_pv_us;
        u32 fps;
        u32 preview_fps;
        u32 line;
    } expo;
    struct {
        bool bVideoMode;
        u16 res_idx;
        CUS_CAMSENSOR_ORIT orit;
    } res;
    I2C_ARRAY tVts_reg[2];
    I2C_ARRAY tGain_reg[4];
    I2C_ARRAY tExpo_reg[3];
    I2C_ARRAY tMirror_reg[1];
    I2C_ARRAY tShortGain_reg[4];
    I2C_ARRAY tShortExpo_reg[3];
    I2C_ARRAY tShortLimit_reg[2];
    u32 short_gain;
    u32 short_line;
    u32 max_short_exp;
    int sen_init;
    int still_min_fps;
    int video_min_fps;
    bool orient_dirty;
    bool reg_dirty;
} sc438hai_params;

const I2C_ARRAY Sensor_id_table[] = {
    { 0x3107, 0xce },
    { 0x3108, 0x78 },
};

const I2C_ARRAY Sensor_init_table_4M30fps[] = {
    { 0x0103, 0x01 },
    { 0x0100, 0x00 },
    { 0x36e9, 0x80 },
    { 0x37f9, 0x80 },
    { 0x23b0, 0x00 },
    { 0x23b1, 0x08 },
    { 0x23b2, 0x00 },
    { 0x23b3, 0x18 },
    { 0x23b4, 0x00 },
    { 0x23b5, 0x38 },
    { 0x23b6, 0x04 },
    { 0x23b7, 0x08 },
    { 0x23b8, 0x04 },
    { 0x23b9, 0x18 },
    { 0x23ba, 0x04 },
    { 0x23bb, 0x38 },
    { 0x23c0, 0x04 },
    { 0x23c1, 0x00 },
    { 0x23c2, 0x04 },
    { 0x23c3, 0x18 },
    { 0x23c4, 0x04 },
    { 0x23c5, 0x78 },
    { 0x23c6, 0x04 },
    { 0x23c7, 0x08 },
    { 0x23c8, 0x04 },
    { 0x23c9, 0x78 },
    { 0x3018, 0x3b },
    { 0x3019, 0x0c },
    { 0x301e, 0xf0 },
    { 0x301f, 0x07 },
    { 0x302c, 0x00 },
    { 0x30b8, 0x44 },
    { 0x3200, 0x00 },
    { 0x3201, 0x00 },
    { 0x3202, 0x00 },
    { 0x3203, 0xd4 },
    { 0x3204, 0x0a },
    { 0x3205, 0x87 },
    { 0x3206, 0x06 },
    { 0x3207, 0xcb },
    { 0x3208, 0x0a },
    { 0x3209, 0x80 },
    { 0x320a, 0x05 },
    { 0x320b, 0xf0 },
    { 0x320c, 0x07 },
    { 0x320d, 0x53 },
    { 0x320e, 0x06 },
    { 0x320f, 0x40 },
    { 0x3210, 0x00 },
    { 0x3211, 0x04 },
    { 0x3212, 0x00 },
    { 0x3213, 0x04 },
    { 0x3214, 0x11 },
    { 0x3215, 0x11 },
    { 0x3223, 0xc0 },
    { 0x3250, 0x40 },
    { 0x327f, 0x3f },
    { 0x32e0, 0x00 },
    { 0x3301, 0x12 },
    { 0x3302, 0x20 },
    { 0x3304, 0xc0 },
    { 0x3306, 0x88 },
    { 0x3309, 0xf0 },
    { 0x330a, 0x00 },
    { 0x330b, 0xf8 },
    { 0x330d, 0x10 },
    { 0x3310, 0x18 },
    { 0x331e, 0xb1 },
    { 0x331f, 0xe1 },
    { 0x3333, 0x10 },
    { 0x3334, 0x40 },
    { 0x3364, 0x56 },
    { 0x338f, 0x80 },
    { 0x3393, 0x1c },
    { 0x3394, 0x2c },
    { 0x3395, 0x3c },
    { 0x3399, 0x0c },
    { 0x339a, 0x10 },
    { 0x339b, 0x18 },
    { 0x339c, 0x80 },
    { 0x33ac, 0x10 },
    { 0x33ad, 0x2c },
    { 0x33ae, 0xb0 },
    { 0x33af, 0xe0 },
    { 0x33b0, 0x0f },
    { 0x33b2, 0x2c },
    { 0x33b3, 0x02 },
    { 0x349f, 0x03 },
    { 0x34a8, 0x02 },
    { 0x34a9, 0x08 },
    { 0x34aa, 0x00 },
    { 0x34ab, 0xf8 },
    { 0x34ac, 0x00 },
    { 0x34ad, 0xf8 },
    { 0x34f9, 0x12 },
    { 0x3631, 0x0f },
    { 0x3632, 0x8d },
    { 0x3633, 0x4d },
    { 0x363b, 0x58 },
    { 0x363c, 0xb4 },
    { 0x363d, 0x40 },
    { 0x3641, 0x08 },
    { 0x3670, 0x42 },
    { 0x3671, 0x44 },
    { 0x3672, 0x36 },
    { 0x3673, 0x04 },
    { 0x3674, 0x08 },
    { 0x3675, 0x04 },
    { 0x3676, 0x18 },
    { 0x367e, 0x49 },
    { 0x367f, 0x49 },
    { 0x3680, 0x49 },
    { 0x3681, 0x04 },
    { 0x3682, 0x08 },
    { 0x3683, 0x04 },
    { 0x3684, 0x38 },
    { 0x3685, 0x80 },
    { 0x3686, 0x81 },
    { 0x3687, 0x83 },
    { 0x3688, 0x86 },
    { 0x3689, 0x88 },
    { 0x368a, 0x8e },
    { 0x368b, 0xa3 },
    { 0x368c, 0xbb },
    { 0x368d, 0x00 },
    { 0x368e, 0x08 },
    { 0x368f, 0x00 },
    { 0x3690, 0x18 },
    { 0x3691, 0x04 },
    { 0x3692, 0x00 },
    { 0x3693, 0x04 },
    { 0x3694, 0x08 },
    { 0x3695, 0x04 },
    { 0x3696, 0x18 },
    { 0x3697, 0x04 },
    { 0x3698, 0x38 },
    { 0x3699, 0x04 },
    { 0x369a, 0x78 },
    { 0x36d0, 0x0d },
    { 0x36ea, 0x0a },
    { 0x36eb, 0x0c },
    { 0x36ec, 0x43 },
    { 0x36ed, 0xaa },
    { 0x370f, 0x13 },
    { 0x3721, 0x6c },
    { 0x3722, 0x8b },
    { 0x3724, 0xb1 },
    { 0x3729, 0x34 },
    { 0x37b0, 0x17 },
    { 0x37b1, 0x9b },
    { 0x37b2, 0x9b },
    { 0x37b3, 0x04 },
    { 0x37b4, 0x08 },
    { 0x37b5, 0x04 },
    { 0x37b6, 0x38 },
    { 0x37b7, 0x1f },
    { 0x37b8, 0x1f },
    { 0x37b9, 0x1f },
    { 0x37ba, 0x04 },
    { 0x37bb, 0x34 },
    { 0x37bc, 0x34 },
    { 0x37bd, 0x04 },
    { 0x37be, 0x08 },
    { 0x37bf, 0x04 },
    { 0x37c0, 0x38 },
    { 0x37c1, 0x04 },
    { 0x37c2, 0x08 },
    { 0x37c3, 0x04 },
    { 0x37c4, 0x38 },
    { 0x37fa, 0x19 },
    { 0x37fb, 0x14 },
    { 0x37fc, 0x10 },
    { 0x37fd, 0x16 },
    { 0x3901, 0x00 },
    { 0x3902, 0xc0 },
    { 0x3903, 0x40 },
    { 0x3905, 0x2d },
    { 0x391f, 0x41 },
    { 0x3933, 0x80 },
    { 0x3934, 0x03 },
    { 0x3937, 0x72 },
    { 0x3939, 0x0f },
    { 0x393a, 0xf8 },
    { 0x3e00, 0x00 },
    { 0x3e01, 0x63 },
    { 0x3e02, 0x80 },
    { 0x3e03, 0x0b },
    { 0x3e16, 0x01 },
    { 0x3e17, 0x40 },
    { 0x3e18, 0x01 },
    { 0x3e19, 0x40 },
    { 0x4509, 0x12 },
    { 0x450d, 0x07 },
    { 0x4800, 0x24 },
    { 0x480f, 0x03 },
    { 0x4837, 0x11 },
    { 0x5000, 0x06 },
    { 0x5780, 0x76 },
    { 0x5784, 0x0c },
    { 0x5785, 0x03 },
    { 0x5787, 0x16 },
    { 0x5788, 0x16 },
    { 0x5789, 0x15 },
    { 0x578a, 0x16 },
    { 0x578b, 0x16 },
    { 0x578c, 0x15 },
    { 0x578d, 0x40 },
    { 0x5790, 0x11 },
    { 0x5791, 0x0f },
    { 0x5792, 0x0f },
    { 0x5793, 0x11 },
    { 0x5794, 0x0f },
    { 0x5795, 0x0f },
    { 0x5799, 0x46 },
    { 0x579a, 0x77 },
    { 0x57a1, 0x04 },
    { 0x57a8, 0xd2 },
    { 0x57aa, 0x2a },
    { 0x57ab, 0x7f },
    { 0x57ac, 0x00 },
    { 0x57ad, 0x00 },
    { 0x36e9, 0x44 },
    { 0x37f9, 0x44 },
    { 0x0100, 0x01 },
    { 0xffff, 0x0a },
};

/* Stock .data+0x50, 227 register/value pairs; do not use the linear lane map. */
const static I2C_ARRAY Sensor_init_table_HDR[] = {
    { 0x0103, 0x01 },
    { 0x0100, 0x00 },
    { 0x36e9, 0x80 },
    { 0x37f9, 0x80 },
    { 0x23b0, 0x00 },
    { 0x23b1, 0x08 },
    { 0x23b2, 0x00 },
    { 0x23b3, 0x18 },
    { 0x23b4, 0x00 },
    { 0x23b5, 0x38 },
    { 0x23b6, 0x04 },
    { 0x23b7, 0x08 },
    { 0x23b8, 0x04 },
    { 0x23b9, 0x18 },
    { 0x23ba, 0x04 },
    { 0x23bb, 0x38 },
    { 0x23c0, 0x04 },
    { 0x23c1, 0x00 },
    { 0x23c2, 0x04 },
    { 0x23c3, 0x18 },
    { 0x23c4, 0x04 },
    { 0x23c5, 0x78 },
    { 0x23c6, 0x04 },
    { 0x23c7, 0x08 },
    { 0x23c8, 0x04 },
    { 0x23c9, 0x78 },
    { 0x3018, 0x7b },
    { 0x301e, 0xf0 },
    { 0x301f, 0x06 },
    { 0x302c, 0x00 },
    { 0x30b8, 0x44 },
    { 0x3200, 0x00 },
    { 0x3201, 0x00 },
    { 0x3202, 0x00 },
    { 0x3203, 0xd4 },
    { 0x3204, 0x0a },
    { 0x3205, 0x87 },
    { 0x3206, 0x06 },
    { 0x3207, 0xcb },
    { 0x3208, 0x0a },
    { 0x3209, 0x80 },
    { 0x320a, 0x05 },
    { 0x320b, 0xf0 },
    { 0x320c, 0x05 },
    { 0x320d, 0xdc },
    { 0x320e, 0x0c },
    { 0x320f, 0x80 },
    { 0x3210, 0x00 },
    { 0x3211, 0x04 },
    { 0x3212, 0x00 },
    { 0x3213, 0x04 },
    { 0x3214, 0x11 },
    { 0x3215, 0x11 },
    { 0x3223, 0xc0 },
    { 0x3250, 0xff },
    { 0x327f, 0x3f },
    { 0x3281, 0x01 },
    { 0x32e0, 0x00 },
    { 0x3301, 0x1a },
    { 0x3302, 0x20 },
    { 0x3304, 0xc0 },
    { 0x3306, 0xe0 },
    { 0x3309, 0xf0 },
    { 0x330a, 0x01 },
    { 0x330b, 0xe0 },
    { 0x330d, 0x10 },
    { 0x3310, 0x18 },
    { 0x331e, 0xa9 },
    { 0x331f, 0xd9 },
    { 0x3333, 0x10 },
    { 0x3334, 0x40 },
    { 0x3364, 0x56 },
    { 0x338f, 0x80 },
    { 0x3393, 0x24 },
    { 0x3394, 0x2c },
    { 0x3395, 0x3c },
    { 0x3399, 0x14 },
    { 0x339a, 0x20 },
    { 0x339b, 0x2c },
    { 0x339c, 0x50 },
    { 0x33ac, 0x10 },
    { 0x33ad, 0x2c },
    { 0x33ae, 0xb0 },
    { 0x33af, 0xe0 },
    { 0x33b0, 0x0f },
    { 0x33b2, 0x2c },
    { 0x33b3, 0x04 },
    { 0x349f, 0x03 },
    { 0x34a8, 0x06 },
    { 0x34a9, 0x08 },
    { 0x34aa, 0x01 },
    { 0x34ab, 0xe0 },
    { 0x34ac, 0x01 },
    { 0x34ad, 0xe0 },
    { 0x34f9, 0x0a },
    { 0x3631, 0x0f },
    { 0x3632, 0x8d },
    { 0x3633, 0x4d },
    { 0x363b, 0x58 },
    { 0x363c, 0xb4 },
    { 0x363d, 0x40 },
    { 0x3641, 0x08 },
    { 0x3670, 0x32 },
    { 0x3671, 0x34 },
    { 0x3672, 0x36 },
    { 0x3673, 0x04 },
    { 0x3674, 0x08 },
    { 0x3675, 0x04 },
    { 0x3676, 0x18 },
    { 0x367e, 0x6d },
    { 0x367f, 0x8d },
    { 0x3680, 0x8d },
    { 0x3681, 0x04 },
    { 0x3682, 0x08 },
    { 0x3683, 0x04 },
    { 0x3684, 0x38 },
    { 0x3685, 0x80 },
    { 0x3686, 0x81 },
    { 0x3687, 0x83 },
    { 0x3688, 0x86 },
    { 0x3689, 0x88 },
    { 0x368a, 0x8e },
    { 0x368b, 0xa3 },
    { 0x368c, 0xbb },
    { 0x368d, 0x00 },
    { 0x368e, 0x08 },
    { 0x368f, 0x00 },
    { 0x3690, 0x18 },
    { 0x3691, 0x04 },
    { 0x3692, 0x00 },
    { 0x3693, 0x04 },
    { 0x3694, 0x08 },
    { 0x3695, 0x04 },
    { 0x3696, 0x18 },
    { 0x3697, 0x04 },
    { 0x3698, 0x38 },
    { 0x3699, 0x04 },
    { 0x369a, 0x78 },
    { 0x36d0, 0x0d },
    { 0x36ea, 0x28 },
    { 0x36eb, 0x14 },
    { 0x36ec, 0x43 },
    { 0x36ed, 0x1a },
    { 0x370f, 0x13 },
    { 0x3721, 0x6c },
    { 0x3722, 0x8b },
    { 0x3724, 0xb1 },
    { 0x3729, 0x34 },
    { 0x37b0, 0x77 },
    { 0x37b1, 0x77 },
    { 0x37b2, 0x77 },
    { 0x37b3, 0x04 },
    { 0x37b4, 0x08 },
    { 0x37b5, 0x04 },
    { 0x37b6, 0x38 },
    { 0x37b7, 0x1f },
    { 0x37b8, 0x1f },
    { 0x37b9, 0x1f },
    { 0x37ba, 0x04 },
    { 0x37bb, 0x04 },
    { 0x37bc, 0x04 },
    { 0x37bd, 0x04 },
    { 0x37be, 0x08 },
    { 0x37bf, 0x04 },
    { 0x37c0, 0x38 },
    { 0x37c1, 0x04 },
    { 0x37c2, 0x08 },
    { 0x37c3, 0x04 },
    { 0x37c4, 0x38 },
    { 0x37fa, 0x20 },
    { 0x37fb, 0x03 },
    { 0x37fc, 0x30 },
    { 0x37fd, 0x16 },
    { 0x3901, 0x00 },
    { 0x3902, 0xc0 },
    { 0x3903, 0x40 },
    { 0x3905, 0x2d },
    { 0x391f, 0x41 },
    { 0x3933, 0x80 },
    { 0x3934, 0x03 },
    { 0x3937, 0x72 },
    { 0x3939, 0x0f },
    { 0x393a, 0xf8 },
    { 0x3e00, 0x00 },
    { 0x3e01, 0xbb },
    { 0x3e02, 0x00 },
    { 0x3e03, 0x0b },
    { 0x3e04, 0x0b },
    { 0x3e05, 0xb0 },
    { 0x3e16, 0x01 },
    { 0x3e17, 0x40 },
    { 0x3e18, 0x01 },
    { 0x3e19, 0x40 },
    { 0x3e23, 0x00 },
    { 0x3e24, 0xc4 },
    { 0x4509, 0x12 },
    { 0x450d, 0x07 },
    { 0x480f, 0x03 },
    { 0x5000, 0x06 },
    { 0x5780, 0x76 },
    { 0x5784, 0x0c },
    { 0x5785, 0x03 },
    { 0x5787, 0x16 },
    { 0x5788, 0x16 },
    { 0x5789, 0x15 },
    { 0x578a, 0x16 },
    { 0x578b, 0x16 },
    { 0x578c, 0x15 },
    { 0x578d, 0x40 },
    { 0x5790, 0x11 },
    { 0x5791, 0x0f },
    { 0x5792, 0x0f },
    { 0x5793, 0x11 },
    { 0x5794, 0x0f },
    { 0x5795, 0x0f },
    { 0x5799, 0x46 },
    { 0x579a, 0x77 },
    { 0x57a1, 0x04 },
    { 0x57a8, 0xd2 },
    { 0x57aa, 0x2a },
    { 0x57ab, 0x7f },
    { 0x57ac, 0x00 },
    { 0x57ad, 0x00 },
    { 0x36e9, 0x44 },
    { 0x37f9, 0x44 },
    { 0x0100, 0x01 },
    { 0xffff, 0x0a },
};

const static I2C_ARRAY mirror_reg[] = {
    { 0x3221, 0x00 },
};

const static I2C_ARRAY gain_reg[] = {
    { 0x3e06, 0x00 },
    { 0x3e07, 0x80 },
    { 0x3e08, 0x00 },
    { 0x3e09, 0x20 },
};

const static I2C_ARRAY expo_reg[] = {
    { 0x3e00, 0x00 },
    { 0x3e01, 0x63 },
    { 0x3e02, 0x80 },
};

const static I2C_ARRAY vts_reg[] = {
    { 0x320e, 0x06 },
    { 0x320f, 0x40 },
};

#if SENSOR_DBG == 1
#define SENSOR_DMSG(args...) SENSOR_DMSG(args)
#endif

#undef SENSOR_NAME
#define SENSOR_NAME sc438hai

#define SensorReg_Read(_reg, _data) (handle->i2c_bus->i2c_rx(handle->i2c_bus, &(handle->i2c_cfg), _reg, _data))
#define SensorReg_Write(_reg, _data) (handle->i2c_bus->i2c_tx(handle->i2c_bus, &(handle->i2c_cfg), _reg, _data))
#define SensorRegArrayW(_reg, _len) (handle->i2c_bus->i2c_array_tx(handle->i2c_bus, &(handle->i2c_cfg), (_reg), (_len)))
#define SensorRegArrayR(_reg, _len) (handle->i2c_bus->i2c_array_rx(handle->i2c_bus, &(handle->i2c_cfg), (_reg), (_len)))

static int sc438hai_is_hdr(ms_cus_sensor *handle)
{
    return handle->interface_attr.attr_mipi.mipi_hdr_mode == CUS_HDR_MODE_DCG;
}

static int sc438hai_is_short(ms_cus_sensor *handle)
{
    return sc438hai_is_hdr(handle) &&
        handle->interface_attr.attr_mipi.mipi_hdr_virtual_channel_num == 1;
}

static int pCus_poweron(ms_cus_sensor *handle, u32 idx)
{
    ISensorIfAPI *sensor_if = handle->sensor_if_api;
    u32 lanes = sc438hai_is_hdr(handle) ? SENSOR_HDR_LANES : SENSOR_CSI_PAD_LANE_CFG;

    sensor_if->Reset(idx, handle->reset_POLARITY);
    SENSOR_USLEEP(1000);
    sensor_if->PowerOff(idx, handle->pwdn_POLARITY);
    SENSOR_USLEEP(1000);

    sensor_if->SetIOPad(idx, handle->sif_bus, lanes);
    sensor_if->SetCSI_Clk(idx, CUS_CSI_CLK_216M);
    sensor_if->SetCSI_Lane(idx, lanes, 1);
    sensor_if->SetCSI_LongPacketType(idx, 0, 0x1C00, 0);
    if (sc438hai_is_hdr(handle))
        sensor_if->SetCSI_hdr_mode(idx, CUS_HDR_MODE_DCG, 2);
    sensor_if->MCLK(idx, 1, handle->mclk);
    SENSOR_USLEEP(2000);

    sensor_if->PowerOff(idx, !handle->pwdn_POLARITY);
    CamOsMsSleep(1);
    sensor_if->Reset(idx, !handle->reset_POLARITY);
    CamOsMsSleep(2);

    return SUCCESS;
}

static int pCus_poweroff(ms_cus_sensor *handle, u32 idx)
{
    ISensorIfAPI *sensor_if = handle->sensor_if_api;

    sensor_if->PowerOff(idx, handle->pwdn_POLARITY);
    sensor_if->Reset(idx, handle->reset_POLARITY);
    CamOsMsSleep(1);
    sensor_if->SetCSI_Clk(idx, CUS_CSI_CLK_DISABLE);
    if (sc438hai_is_hdr(handle))
        sensor_if->SetCSI_hdr_mode(idx, CUS_HDR_MODE_DCG, 0);
    sensor_if->MCLK(idx, 0, handle->mclk);

    return SUCCESS;
}

static int pCus_GetSensorID(ms_cus_sensor *handle, u32 *id)
{
    int i, n;
    int table_length = ARRAY_SIZE(Sensor_id_table);
    I2C_ARRAY id_from_sensor[ARRAY_SIZE(Sensor_id_table)];

    for (n = 0; n < table_length; ++n) {
        id_from_sensor[n].reg = Sensor_id_table[n].reg;
        id_from_sensor[n].data = 0;
    }

    *id = 0;
    if (table_length > 8)
        table_length = 8;

    for (n = 0; n < 4; ++n) {
        if (n > 2)
            return FAIL;
        if (SensorRegArrayR((I2C_ARRAY *)id_from_sensor, table_length) == SUCCESS)
            break;
        SENSOR_USLEEP(1000);
    }

    for (i = 0; i < table_length; ++i) {
        if (id_from_sensor[i].data != Sensor_id_table[i].data)
            return FAIL;
        *id = ((*id) + id_from_sensor[i].data) << 8;
    }

    *id >>= 8;
    SENSOR_DMSG("[%s] Read sensor id, get 0x%x Success\n", __FUNCTION__, (int)*id);

    return SUCCESS;
}

static int sc438hai_SetPatternMode(ms_cus_sensor *handle, u32 mode)
{
    return SensorReg_Write(0x4501, mode == 1 ? 0xcc : 0xc0);
}

static int pCus_SetAEGain_cal(ms_cus_sensor *handle, u32 gain);
static int pCus_AEStatusNotify(ms_cus_sensor *handle, CUS_CAMSENSOR_AE_STATUS_NOTIFY status);

static int pCus_init_linear_4M30fps(ms_cus_sensor *handle)
{
    sc438hai_params *params = (sc438hai_params *)handle->private_data;
    const I2C_ARRAY *table = sc438hai_is_hdr(handle) ?
        Sensor_init_table_HDR : Sensor_init_table_4M30fps;
    int count = sc438hai_is_hdr(handle) ? ARRAY_SIZE(Sensor_init_table_HDR) :
        ARRAY_SIZE(Sensor_init_table_4M30fps);
    int i, cnt;
    u16 revision = 0;

    SENSOR_MSLEEP(1);
    if (SensorReg_Read(0x8037, &revision) != SUCCESS || revision != 0x0f) {
        SENSOR_EMSG("SC438HAI: unsupported revision 0x%02x\n", revision);
        return FAIL;
    }

    for (i = 0; i < count; i++) {
        if (table[i].reg == 0xffff) {
            SENSOR_MSLEEP(table[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(table[i].reg, table[i].data) != SUCCESS) {
                cnt++;
                SENSOR_DMSG("Sensor_init_table -> Retry %d...\n", cnt);
                if (cnt >= 10) {
                    SENSOR_DMSG("[%s:%d] Sensor init fail\n", __FUNCTION__, __LINE__);
                    return FAIL;
                }
                SENSOR_MSLEEP(10);
            }
        }
    }

    pCus_SetOrien(handle, handle->orient);
    params->tVts_reg[0].data = (params->expo.vts >> 8) & 0x00ff;
    params->tVts_reg[1].data = (params->expo.vts >> 0) & 0x00ff;

    return SUCCESS;
}

static int pCus_GetVideoResNum(ms_cus_sensor *handle, u32 *ulres_num)
{
    *ulres_num = handle->video_res_supported.num_res;
    return SUCCESS;
}

static int pCus_GetVideoRes(ms_cus_sensor *handle, u32 res_idx, cus_camsensor_res **res)
{
    u32 num_res = handle->video_res_supported.num_res;

    if (res_idx >= num_res)
        return FAIL;

    *res = &handle->video_res_supported.res[res_idx];

    return SUCCESS;
}

static int pCus_GetCurVideoRes(ms_cus_sensor *handle, u32 *cur_idx, cus_camsensor_res **res)
{
    u32 num_res = handle->video_res_supported.num_res;

    *cur_idx = handle->video_res_supported.ulcur_res;
    if (*cur_idx >= num_res)
        return FAIL;

    *res = &handle->video_res_supported.res[*cur_idx];

    return SUCCESS;
}

static int pCus_SetVideoRes(ms_cus_sensor *handle, u32 res_idx)
{
    u32 num_res = handle->video_res_supported.num_res;
    sc438hai_params *params = (sc438hai_params *)handle->private_data;

    if (res_idx >= num_res)
        return FAIL;

    switch (res_idx) {
    case 0:
        handle->video_res_supported.ulcur_res = 0;
        handle->pCus_sensor_init = pCus_init_linear_4M30fps;
        params->expo.vts = sc438hai_is_hdr(handle) ? HDR_VTS_30FPS : vts_30fps;
        params->expo.fps = 30;
        break;
    default:
        break;
    }

    return SUCCESS;
}

static int pCus_GetOrien(ms_cus_sensor *handle, CUS_CAMSENSOR_ORIT *orit)
{
    char sen_data;
    sc438hai_params *params = (sc438hai_params *)handle->private_data;

    sen_data = params->tMirror_reg[0].data;
    switch (sen_data & 0x66) {
    case 0x00:
        *orit = CUS_ORIT_M0F0;
        break;
    case 0x06:
        *orit = CUS_ORIT_M1F0;
        break;
    case 0x60:
        *orit = CUS_ORIT_M0F1;
        break;
    case 0x66:
        *orit = CUS_ORIT_M1F1;
        break;
    default:
        break;
    }

    return SUCCESS;
}

static int pCus_SetOrien(ms_cus_sensor *handle, CUS_CAMSENSOR_ORIT orit)
{
    sc438hai_params *params = (sc438hai_params *)handle->private_data;

    switch (orit) {
    case CUS_ORIT_M0F0:
        params->tMirror_reg[0].data = 0x00;
        params->orient_dirty = true;
        break;
    case CUS_ORIT_M1F0:
        params->tMirror_reg[0].data = 0x06;
        params->orient_dirty = true;
        break;
    case CUS_ORIT_M0F1:
        params->tMirror_reg[0].data = 0x60;
        params->orient_dirty = true;
        break;
    case CUS_ORIT_M1F1:
        params->tMirror_reg[0].data = 0x66;
        params->orient_dirty = true;
        break;
    default:
        break;
    }

    handle->orient = orit;
    return SUCCESS;
}

static int pCus_GetFPS(ms_cus_sensor *handle)
{
    sc438hai_params *params = (sc438hai_params *)handle->private_data;
    u32 max_fps = handle->video_res_supported.res[handle->video_res_supported.ulcur_res].max_fps;
    u32 tVts = (params->tVts_reg[0].data << 8) | (params->tVts_reg[1].data << 0);
    u32 base_vts = sc438hai_is_hdr(handle) ? HDR_VTS_30FPS : vts_30fps;

    if (params->expo.fps >= 1000)
        params->expo.preview_fps = (base_vts * max_fps * 1000) / tVts;
    else
        params->expo.preview_fps = (base_vts * max_fps) / tVts;

    return params->expo.preview_fps;
}

static int sc438hai_set_hdr_exposure(sc438hai_params *params, u32 us, bool short_frame)
{
    I2C_ARRAY *regs = short_frame ? params->tShortExpo_reg : params->tExpo_reg;
    u32 limit = short_frame ? params->max_short_exp - 9 :
        params->expo.vts - params->max_short_exp - 11;
    u32 lines;

    if (us > 1000000 / Preview_MIN_FPS)
        us = 1000000 / Preview_MIN_FPS;
    lines = us * 1000 / HDR_LINE_PERIOD;
    if (lines < 2)
        lines = 2;
    if (lines > limit)
        lines = limit;
    regs[0].data = (lines >> 12) & 0x0f;
    regs[1].data = (lines >> 4) & 0xff;
    regs[2].data = (lines << 4) & 0xf0;
    if (short_frame)
        params->short_line = lines;
    else
        params->expo.line = lines;
    params->reg_dirty = true;
    return SUCCESS;
}

static int pCus_SetFPS(ms_cus_sensor *handle, u32 fps)
{
    u32 vts = 0;
    sc438hai_params *params = (sc438hai_params *)handle->private_data;
    u32 max_fps = handle->video_res_supported.res[handle->video_res_supported.ulcur_res].max_fps;
    u32 min_fps = handle->video_res_supported.res[handle->video_res_supported.ulcur_res].min_fps;
    u32 base_vts = sc438hai_is_hdr(handle) ? HDR_VTS_30FPS : vts_30fps;

    if (fps >= min_fps && fps <= max_fps) {
        params->expo.fps = fps;
        params->expo.vts = (base_vts * max_fps) / fps;
    } else if ((fps >= (min_fps * 1000)) && (fps <= (max_fps * 1000))) {
        params->expo.fps = fps;
        params->expo.vts = (base_vts * (max_fps * 1000)) / fps;
    } else {
        SENSOR_DMSG("[%s] FPS %d out of range.\n", __FUNCTION__, fps);
        return FAIL;
    }

    if (sc438hai_is_hdr(handle)) {
        vts = params->expo.vts;
        params->max_short_exp = ((vts / 17) - 1) & ~1U;
        params->tShortLimit_reg[0].data = params->max_short_exp >> 8;
        params->tShortLimit_reg[1].data = params->max_short_exp & 0xff;
        /* Re-clamp both exposures when the frame budget shrinks. */
        sc438hai_set_hdr_exposure(params,
            (params->expo.line * HDR_LINE_PERIOD + 999) / 1000, false);
        sc438hai_set_hdr_exposure(params,
            (params->short_line * HDR_LINE_PERIOD + 999) / 1000, true);
    } else if (params->expo.line > params->expo.vts - 8)
        vts = params->expo.line + 8;
    else
        vts = params->expo.vts;

    params->tVts_reg[0].data = (vts >> 8) & 0x00ff;
    params->tVts_reg[1].data = (vts >> 0) & 0x00ff;
    params->reg_dirty = true;

    return SUCCESS;
}

static int pCus_AEStatusNotify(ms_cus_sensor *handle, CUS_CAMSENSOR_AE_STATUS_NOTIFY status)
{
    sc438hai_params *params = (sc438hai_params *)handle->private_data;

    /* Both HDR planes share registers; only SEF commits once per frame. */
    if (sc438hai_is_hdr(handle) && !sc438hai_is_short(handle))
        return SUCCESS;
    switch (status) {
    case CUS_FRAME_INACTIVE:
        break;
    case CUS_FRAME_ACTIVE:
        if (params->orient_dirty) {
            if (SensorRegArrayW(params->tMirror_reg, ARRAY_SIZE(params->tMirror_reg)) != SUCCESS)
                return FAIL;
            params->orient_dirty = false;
        }
        if (params->reg_dirty) {
            if (SensorRegArrayW(params->tExpo_reg, ARRAY_SIZE(params->tExpo_reg)) != SUCCESS ||
                SensorRegArrayW(params->tGain_reg, ARRAY_SIZE(params->tGain_reg)) != SUCCESS ||
                SensorRegArrayW(params->tVts_reg, ARRAY_SIZE(params->tVts_reg)) != SUCCESS)
                return FAIL;
            if (sc438hai_is_hdr(handle) &&
                (SensorRegArrayW(params->tShortExpo_reg, ARRAY_SIZE(params->tShortExpo_reg)) != SUCCESS ||
                 SensorRegArrayW(params->tShortGain_reg, ARRAY_SIZE(params->tShortGain_reg)) != SUCCESS ||
                 SensorRegArrayW(params->tShortLimit_reg, ARRAY_SIZE(params->tShortLimit_reg)) != SUCCESS))
                return FAIL;
            params->reg_dirty = false;
        }
        break;
    default:
        break;
    }

    return SUCCESS;
}

static int pCus_GetAEUSecs(ms_cus_sensor *handle, u32 *us)
{
    int rc = 0;
    u32 lines = 0;
    sc438hai_params *params = (sc438hai_params *)handle->private_data;

    I2C_ARRAY *regs = sc438hai_is_short(handle) ? params->tShortExpo_reg : params->tExpo_reg;
    u32 period = sc438hai_is_hdr(handle) ? HDR_LINE_PERIOD : Preview_line_period;
    lines |= (u32)(regs[0].data & 0x0f) << 16;
    lines |= (u32)(regs[1].data & 0xff) << 8;
    lines |= (u32)(regs[2].data & 0xf0) << 0;
    lines >>= 4;
    *us = (lines * period) / 1000;

    return rc;
}

static int pCus_SetAEUSecs(ms_cus_sensor *handle, u32 us)
{
    sc438hai_params *params = (sc438hai_params *)handle->private_data;
    u32 lines, vts;

    if (sc438hai_is_hdr(handle))
        return sc438hai_set_hdr_exposure(params, us, sc438hai_is_short(handle));
    if (us > 1000000 / Preview_MIN_FPS)
        us = 1000000 / Preview_MIN_FPS;
    /* SC438HAI exposure uses full lines, unlike SC430AI's half-line units. */
    lines = (1000 * us) / Preview_line_period;
    if (lines < 2)
        lines = 2;
    vts = params->expo.vts;
    if (lines > vts - 8)
        vts = lines + 8;

    params->expo.line = lines;
    params->tExpo_reg[0].data = (lines >> 12) & 0x0f;
    params->tExpo_reg[1].data = (lines >> 4) & 0xff;
    params->tExpo_reg[2].data = (lines << 4) & 0xf0;
    params->tVts_reg[0].data = (vts >> 8) & 0xff;
    params->tVts_reg[1].data = vts & 0xff;
    params->reg_dirty = true;
    return SUCCESS;
}

static int pCus_GetAEGain(ms_cus_sensor *handle, u32 *gain)
{
    sc438hai_params *params = (sc438hai_params *)handle->private_data;

    *gain = sc438hai_is_short(handle) ? params->short_gain : params->expo.final_gain;
    return SUCCESS;
}

static int pCus_SetAEGain_cal(ms_cus_sensor *handle, u32 gain)
{
    return SUCCESS;
}

static int pCus_SetAEGain(ms_cus_sensor *handle, u32 gain)
{
    sc438hai_params *params = (sc438hai_params *)handle->private_data;
    u32 base, factor, fine;
    u16 analog_coarse, analog_fine, digital_coarse = 0, digital_fine = 0x80;
    I2C_ARRAY *regs = sc438hai_is_short(handle) ? params->tShortGain_reg : params->tGain_reg;

    if (gain < 1024)
        gain = 1024;
    if (gain > SENSOR_MAXGAIN * 1024)
        gain = SENSOR_MAXGAIN * 1024;

    /* Stock driver: 2.530x HCG, then analogue coarse/fine and digital gain. */
    if (gain < 2048) {
        base = 1000;
        analog_coarse = 0x00;
    } else if (gain <= 2590) {
        base = 2000;
        analog_coarse = 0x01;
    } else if (gain <= 5181) {
        base = 2530;
        analog_coarse = 0x80;
    } else if (gain <= 10362) {
        base = 2530 * 2;
        analog_coarse = 0x81;
    } else if (gain <= 20725) {
        base = 2530 * 4;
        analog_coarse = 0x83;
    } else if (gain <= 41451) {
        base = 2530 * 8;
        analog_coarse = 0x87;
    } else {
        base = 2530 * 16;
        analog_coarse = 0x8f;
    }

    if (gain <= 82903) {
        analog_fine = (gain * 1000 / base) >> 5;
    } else {
        factor = gain <= 164510 ? 1 : gain <= 329021 ? 2 :
                 gain <= 658042 ? 4 : 8;
        analog_fine = 0x3f;
        digital_coarse = factor - 1;
        fine = (u32)(((u64)gain * 8000) / (2530 * 16 * factor) / 127);
        digital_fine = fine & 0xfc;
    }

    if (regs[0].data != digital_coarse ||
        regs[1].data != digital_fine ||
        regs[2].data != analog_coarse ||
        regs[3].data != analog_fine)
        params->reg_dirty = true;
    regs[0].data = digital_coarse;
    regs[1].data = digital_fine;
    regs[2].data = analog_coarse;
    regs[3].data = analog_fine;
    if (sc438hai_is_short(handle))
        params->short_gain = gain;
    else
        params->expo.final_gain = gain;
    return SUCCESS;
}

static int pCus_GetAEMinMaxUSecs(ms_cus_sensor *handle, u32 *min, u32 *max)
{
    sc438hai_params *params = (sc438hai_params *)handle->private_data;
    if (sc438hai_is_hdr(handle)) {
        u32 limit = sc438hai_is_short(handle) ? params->max_short_exp - 9 :
            params->expo.vts - params->max_short_exp - 11;
        *min = (HDR_LINE_PERIOD * 2 + 999) / 1000;
        *max = limit * HDR_LINE_PERIOD / 1000;
        return SUCCESS;
    }
    *min = (Preview_line_period * 2 + 999) / 1000;
    *max = 1000000 / Preview_MIN_FPS;
    return SUCCESS;
}

static int pCus_GetAEMinMaxGain(ms_cus_sensor *handle, u32 *min, u32 *max)
{
    *min = 1024;
    *max = SENSOR_MAXGAIN * 1024;
    return SUCCESS;
}

static int sc438hai_GetShutterInfo(struct __ms_cus_sensor *handle, CUS_SHUTTER_INFO *info)
{
    sc438hai_params *params = (sc438hai_params *)handle->private_data;
    if (sc438hai_is_hdr(handle)) {
        u32 limit = sc438hai_is_short(handle) ? params->max_short_exp - 9 :
            params->expo.vts - params->max_short_exp - 11;
        info->max = limit * HDR_LINE_PERIOD;
        info->min = HDR_LINE_PERIOD * 2 + 999;
        info->step = HDR_LINE_PERIOD;
        return SUCCESS;
    }
    info->max = 1000000000 / Preview_MIN_FPS;
    info->min = Preview_line_period * 2 + 999;
    info->step = Preview_line_period;
    return SUCCESS;
}

static int pCus_setCaliData_gain_linearity(ms_cus_sensor *handle, CUS_GAIN_GAP_ARRAY *pArray, u32 num)
{
    return SUCCESS;
}

#define CMDID_I2C_READ (0x01)
#define CMDID_I2C_WRITE (0x02)

static int pCus_sensor_CustDefineFunction(ms_cus_sensor *handle, u32 cmd_id, void *param)
{
    if (param == NULL || handle == NULL) {
        SENSOR_EMSG("param/handle data NULL\n");
        return FAIL;
    }

    switch (cmd_id) {
    case CMDID_I2C_READ: {
        I2C_ARRAY *reg = (I2C_ARRAY *)param;
        SensorReg_Read(reg->reg, &reg->data);
        SENSOR_EMSG("reg %x, read data %x\n", reg->reg, reg->data);
        break;
    }
    case CMDID_I2C_WRITE: {
        I2C_ARRAY *reg = (I2C_ARRAY *)param;
        SENSOR_EMSG("reg %x, write data %x\n", reg->reg, reg->data);
        SensorReg_Write(reg->reg, reg->data);
        break;
    }
    default:
        SENSOR_EMSG("cmd id %d err\n", cmd_id);
        break;
    }

    return SUCCESS;
}

int cus_camsensor_init_handle(ms_cus_sensor *drv_handle)
{
    ms_cus_sensor *handle = drv_handle;
    sc438hai_params *params;

    if (!handle) {
        SENSOR_DMSG("[%s] not enough memory!\n", __FUNCTION__);
        return FAIL;
    }
    if (handle->private_data == NULL) {
        SENSOR_EMSG("[%s] Private data is empty!\n", __FUNCTION__);
        return FAIL;
    }

    params = (sc438hai_params *)handle->private_data;
    memcpy(params->tVts_reg, vts_reg, sizeof(vts_reg));
    memcpy(params->tGain_reg, gain_reg, sizeof(gain_reg));
    memcpy(params->tExpo_reg, expo_reg, sizeof(expo_reg));
    memcpy(params->tMirror_reg, mirror_reg, sizeof(mirror_reg));

    sprintf(handle->model_id, "sc438hai_MIPI");

    handle->isp_type = SENSOR_ISP_TYPE;
    handle->sif_bus = SENSOR_IFBUS_TYPE;
    handle->data_prec = SENSOR_DATAPREC;
    handle->data_mode = SENSOR_DATAMODE;
    handle->bayer_id = SENSOR_BAYERID;
    handle->RGBIR_id = SENSOR_RGBIRID;
    handle->orient = SENSOR_ORIT;
    handle->interface_attr.attr_mipi.mipi_lane_num = SENSOR_MIPI_LANE_NUM;
    handle->interface_attr.attr_mipi.mipi_data_format = CUS_SEN_INPUT_FORMAT_RGB;
    handle->interface_attr.attr_mipi.mipi_yuv_order = 0;
    handle->interface_attr.attr_mipi.mipi_hsync_mode = SENSOR_MIPI_HSYNC_MODE;
    handle->interface_attr.attr_mipi.mipi_hdr_mode = CUS_HDR_MODE_NONE;
    handle->interface_attr.attr_mipi.mipi_hdr_virtual_channel_num = 0;

    handle->video_res_supported.num_res = 1;
    handle->video_res_supported.ulcur_res = 0;
    handle->video_res_supported.res[0].width = Preview_WIDTH;
    handle->video_res_supported.res[0].height = Preview_HEIGHT;
    handle->video_res_supported.res[0].max_fps = Preview_MAX_FPS;
    handle->video_res_supported.res[0].min_fps = Preview_MIN_FPS;
    handle->video_res_supported.res[0].crop_start_x = Preview_CROP_START_X;
    handle->video_res_supported.res[0].crop_start_y = Preview_CROP_START_Y;
    handle->video_res_supported.res[0].nOutputWidth = Preview_WIDTH;
    handle->video_res_supported.res[0].nOutputHeight = Preview_HEIGHT;
    sprintf(handle->video_res_supported.res[0].strResDesc, "2688x1520@30fps");

    handle->i2c_cfg.mode = SENSOR_I2C_LEGACY;
    handle->i2c_cfg.fmt = SENSOR_I2C_FMT;
    handle->i2c_cfg.address = SENSOR_I2C_ADDR;
    handle->i2c_cfg.speed = SENSOR_I2C_SPEED;

    handle->mclk = Preview_MCLK_SPEED;

    handle->pwdn_POLARITY = SENSOR_PWDN_POL;
    handle->reset_POLARITY = SENSOR_RST_POL;
    handle->VSYNC_POLARITY = SENSOR_VSYNC_POL;
    handle->HSYNC_POLARITY = SENSOR_HSYNC_POL;
    handle->PCLK_POLARITY = SENSOR_PCLK_POL;

    handle->ae_gain_delay = 2;
    handle->ae_shutter_delay = 2;
    handle->ae_gain_ctrl_num = 1;
    handle->ae_shutter_ctrl_num = 1;
    handle->sat_mingain = g_sensor_ae_min_gain;

    handle->pCus_sensor_release = cus_camsensor_release_handle;
    handle->pCus_sensor_init = pCus_init_linear_4M30fps;
    handle->pCus_sensor_poweron = pCus_poweron;
    handle->pCus_sensor_poweroff = pCus_poweroff;
    handle->pCus_sensor_GetSensorID = pCus_GetSensorID;
    handle->pCus_sensor_GetVideoResNum = pCus_GetVideoResNum;
    handle->pCus_sensor_GetVideoRes = pCus_GetVideoRes;
    handle->pCus_sensor_GetCurVideoRes = pCus_GetCurVideoRes;
    handle->pCus_sensor_SetVideoRes = pCus_SetVideoRes;
    handle->pCus_sensor_GetOrien = pCus_GetOrien;
    handle->pCus_sensor_SetOrien = pCus_SetOrien;
    handle->pCus_sensor_GetFPS = pCus_GetFPS;
    handle->pCus_sensor_SetFPS = pCus_SetFPS;
    handle->pCus_sensor_SetPatternMode = sc438hai_SetPatternMode;
    handle->pCus_sensor_AEStatusNotify = pCus_AEStatusNotify;
    handle->pCus_sensor_GetAEUSecs = pCus_GetAEUSecs;
    handle->pCus_sensor_SetAEUSecs = pCus_SetAEUSecs;
    handle->pCus_sensor_GetAEGain = pCus_GetAEGain;
    handle->pCus_sensor_SetAEGain = pCus_SetAEGain;
    handle->pCus_sensor_GetAEMinMaxGain = pCus_GetAEMinMaxGain;
    handle->pCus_sensor_GetAEMinMaxUSecs = pCus_GetAEMinMaxUSecs;
    handle->pCus_sensor_CustDefineFunction = pCus_sensor_CustDefineFunction;
    handle->pCus_sensor_SetAEGain_cal = pCus_SetAEGain_cal;
    handle->pCus_sensor_setCaliData_gain_linearity = pCus_setCaliData_gain_linearity;
    handle->pCus_sensor_GetShutterInfo = sc438hai_GetShutterInfo;

    params->expo.vts = vts_30fps;
    params->expo.fps = 30;
    params->expo.line = 1592;
    params->expo.final_gain = 1024;
    params->reg_dirty = false;
    params->orient_dirty = false;

    return SUCCESS;
}

static int cus_camsensor_release_handle(ms_cus_sensor *handle)
{
    return SUCCESS;
}

static int sc438hai_hdr_lef_power(ms_cus_sensor *handle, u32 idx)
{
    /* SEF owns the physical sensor's power and initialization. */
    return SUCCESS;
}

static int sc438hai_hdr_lef_id(ms_cus_sensor *handle, u32 *id)
{
    *id = 0xce78;
    return SUCCESS;
}

static int cus_camsensor_init_handle_HDR_SEF(ms_cus_sensor *handle)
{
    sc438hai_params *params;
    const I2C_ARRAY short_gain[] = {
        {0x3e10, 0}, {0x3e11, 0x80}, {0x3e12, 0}, {0x3e13, 0x20}
    };
    const I2C_ARRAY short_expo[] = {
        {0x3e22, 0}, {0x3e04, 0x0b}, {0x3e05, 0xb0}
    };
    const I2C_ARRAY short_limit[] = {{0x3e23, 0}, {0x3e24, 0xc4}};

    if (cus_camsensor_init_handle(handle) != SUCCESS)
        return FAIL;
    params = (sc438hai_params *)handle->private_data;
    memcpy(params->tShortGain_reg, short_gain, sizeof(short_gain));
    memcpy(params->tShortExpo_reg, short_expo, sizeof(short_expo));
    memcpy(params->tShortLimit_reg, short_limit, sizeof(short_limit));
    params->short_gain = 1024;
    params->short_line = 187;
    params->max_short_exp = 196;
    params->expo.vts = HDR_VTS_30FPS;
    params->expo.line = 2992;
    params->tVts_reg[0].data = 0x0c;
    params->tVts_reg[1].data = 0x80;
    params->tExpo_reg[1].data = 0xbb;
    params->tExpo_reg[2].data = 0;

    sprintf(handle->model_id, "sc438hai_MIPI_HDR_SEF");
    sprintf(handle->video_res_supported.res[0].strResDesc, "2688x1520@30fps HDR");
    handle->interface_attr.attr_mipi.mipi_lane_num = SENSOR_HDR_LANES;
    handle->interface_attr.attr_mipi.mipi_hsync_mode = PACKET_FOOTER_EDGE;
    handle->interface_attr.attr_mipi.mipi_hdr_mode = CUS_HDR_MODE_DCG;
    handle->interface_attr.attr_mipi.mipi_hdr_virtual_channel_num = 1;
    handle->ae_shutter_ctrl_num = 2;
    return SUCCESS;
}

static int cus_camsensor_init_handle_HDR_LEF(ms_cus_sensor *handle)
{
    if (cus_camsensor_init_handle_HDR_SEF(handle) != SUCCESS)
        return FAIL;
    sprintf(handle->model_id, "sc438hai_MIPI_HDR_LEF");
    handle->interface_attr.attr_mipi.mipi_hdr_virtual_channel_num = 0;
    handle->pCus_sensor_poweron = sc438hai_hdr_lef_power;
    handle->pCus_sensor_poweroff = sc438hai_hdr_lef_power;
    handle->pCus_sensor_init = cus_camsensor_release_handle;
    handle->pCus_sensor_GetSensorID = sc438hai_hdr_lef_id;
    handle->pCus_sensor_GetVideoResNum = NULL;
    handle->pCus_sensor_GetVideoRes = NULL;
    handle->pCus_sensor_GetCurVideoRes = NULL;
    handle->pCus_sensor_SetVideoRes = NULL;
    return SUCCESS;
}

SENSOR_DRV_ENTRY_IMPL_END_EX(sc438hai,
                             cus_camsensor_init_handle,
                             cus_camsensor_init_handle_HDR_SEF,
                             cus_camsensor_init_handle_HDR_LEF,
                             sc438hai_params);
