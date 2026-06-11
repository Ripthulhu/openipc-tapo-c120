/*
 * SC430AI MIPI sensor driver for SigmaStar Infinity6C.
 *
 * This is an OpenIPC-style source driver built from the TP-Link Tapo C120
 * stock register table and the existing Infinity6C SmartSens drivers.
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

SENSOR_DRV_ENTRY_IMPL_BEGIN_EX(sc430ai);

#ifndef ARRAY_SIZE
#define ARRAY_SIZE CAM_OS_ARRAY_SIZE
#endif

#define SENSOR_PAD_GROUP_SET CUS_SENSOR_PAD_GROUP_A
#define SENSOR_CHANNEL_NUM (0)
#define SENSOR_CHANNEL_MODE_LINEAR CUS_SENSOR_CHANNEL_MODE_REALTIME_NORMAL

#define SENSOR_MIPI_LANE_NUM (2)
#define SENSOR_CSI_PAD_LANE_CFG (4)

#define SENSOR_DBG 0

#define SENSOR_ISP_TYPE ISP_EXT
#define SENSOR_IFBUS_TYPE CUS_SENIF_BUS_MIPI
#define SENSOR_MIPI_HSYNC_MODE PACKET_HEADER_EDGE1
#define SENSOR_DATAPREC CUS_DATAPRECISION_10
#define SENSOR_DATAMODE CUS_SEN_10TO12_9000
#define SENSOR_BAYERID CUS_BAYER_BG
#define SENSOR_RGBIRID CUS_RGBIR_NONE
#define SENSOR_ORIT CUS_ORIT_M0F0
#define SENSOR_MAXGAIN 160
#define Preview_MCLK_SPEED CUS_CMU_CLK_27MHZ

u32 Preview_line_period = 20202;
u32 vts_30fps = 1650;

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
    I2C_ARRAY tGain_reg[3];
    I2C_ARRAY tExpo_reg[3];
    I2C_ARRAY tMirror_reg[1];
    int sen_init;
    int still_min_fps;
    int video_min_fps;
    bool orient_dirty;
    bool reg_dirty;
} sc430ai_params;

const I2C_ARRAY Sensor_id_table[] = {
    { 0x3107, 0xce },
    { 0x3108, 0x39 },
};

const I2C_ARRAY Sensor_init_table_4M30fps[] = {
    { 0x0103, 0x01 },
    { 0x0100, 0x00 },
    { 0x36e9, 0x80 },
    { 0x37f9, 0x80 },
    { 0x301f, 0x81 },
    { 0x3203, 0x32 },
    { 0x3204, 0x0a },
    { 0x3205, 0xff },
    { 0x3206, 0x06 },
    { 0x3207, 0x29 },
    { 0x3208, 0x0a },
    { 0x3209, 0x80 },
    { 0x320a, 0x05 },
    { 0x320b, 0xf0 },
    { 0x3211, 0x1c },
    { 0x320e, 0x06 },
    { 0x320f, 0x72 },
    { 0x3250, 0x40 },
    { 0x3251, 0x98 },
    { 0x3253, 0x0c },
    { 0x325f, 0x20 },
    { 0x3301, 0x08 },
    { 0x3304, 0x50 },
    { 0x3306, 0x88 },
    { 0x3308, 0x14 },
    { 0x3309, 0x70 },
    { 0x330a, 0x00 },
    { 0x330b, 0xf8 },
    { 0x330d, 0x10 },
    { 0x331e, 0x41 },
    { 0x331f, 0x61 },
    { 0x3333, 0x10 },
    { 0x335d, 0x60 },
    { 0x335e, 0x06 },
    { 0x335f, 0x08 },
    { 0x3364, 0x56 },
    { 0x3366, 0x01 },
    { 0x337c, 0x02 },
    { 0x337d, 0x0a },
    { 0x3390, 0x01 },
    { 0x3391, 0x03 },
    { 0x3392, 0x07 },
    { 0x3393, 0x08 },
    { 0x3394, 0x08 },
    { 0x3395, 0x08 },
    { 0x3396, 0x40 },
    { 0x3397, 0x48 },
    { 0x3398, 0x4b },
    { 0x3399, 0x08 },
    { 0x339a, 0x08 },
    { 0x339b, 0x08 },
    { 0x339c, 0x1d },
    { 0x33a2, 0x04 },
    { 0x33ae, 0x30 },
    { 0x33af, 0x50 },
    { 0x33b1, 0x80 },
    { 0x33b2, 0x48 },
    { 0x33b3, 0x30 },
    { 0x349f, 0x02 },
    { 0x34a6, 0x48 },
    { 0x34a7, 0x4b },
    { 0x34a8, 0x30 },
    { 0x34a9, 0x18 },
    { 0x34f8, 0x5f },
    { 0x34f9, 0x08 },
    { 0x3632, 0x48 },
    { 0x3633, 0x32 },
    { 0x3637, 0x29 },
    { 0x3638, 0xc1 },
    { 0x363b, 0x20 },
    { 0x363d, 0x02 },
    { 0x3670, 0x09 },
    { 0x3674, 0x8b },
    { 0x3675, 0xc6 },
    { 0x3676, 0x8b },
    { 0x367c, 0x40 },
    { 0x367d, 0x48 },
    { 0x3690, 0x32 },
    { 0x3691, 0x43 },
    { 0x3692, 0x33 },
    { 0x3693, 0x40 },
    { 0x3694, 0x4b },
    { 0x3698, 0x85 },
    { 0x3699, 0x8f },
    { 0x369a, 0xa0 },
    { 0x369b, 0xc3 },
    { 0x36a2, 0x49 },
    { 0x36a3, 0x4b },
    { 0x36a4, 0x4f },
    { 0x36d0, 0x01 },
    { 0x36ec, 0x13 },
    { 0x370f, 0x01 },
    { 0x3722, 0x00 },
    { 0x3728, 0x10 },
    { 0x37b0, 0x03 },
    { 0x37b1, 0x03 },
    { 0x37b2, 0x83 },
    { 0x37b3, 0x48 },
    { 0x37b4, 0x49 },
    { 0x37fb, 0x24 },
    { 0x37fc, 0x01 },
    { 0x3901, 0x00 },
    { 0x3902, 0xc5 },
    { 0x3904, 0x08 },
    { 0x3905, 0x8c },
    { 0x3909, 0x00 },
    { 0x391d, 0x04 },
    { 0x391f, 0x51 },
    { 0x3926, 0x21 },
    { 0x3929, 0x18 },
    { 0x3933, 0x82 },
    { 0x3934, 0x0a },
    { 0x3937, 0x5f },
    { 0x3939, 0x00 },
    { 0x393a, 0x00 },
    { 0x39dc, 0x02 },
    { 0x3e01, 0xcd },
    { 0x3e02, 0xa0 },
    { 0x440e, 0x02 },
    { 0x4509, 0x20 },
    { 0x4837, 0x28 },
    { 0x5010, 0x10 },
    { 0x5780, 0x66 },
    { 0x578d, 0x40 },
    { 0x5799, 0x06 },
    { 0x57ad, 0x00 },
    { 0x5ae0, 0xfe },
    { 0x5ae1, 0x40 },
    { 0x5ae2, 0x30 },
    { 0x5ae3, 0x2a },
    { 0x5ae4, 0x24 },
    { 0x5ae5, 0x30 },
    { 0x5ae6, 0x2a },
    { 0x5ae7, 0x24 },
    { 0x5ae8, 0x3c },
    { 0x5ae9, 0x30 },
    { 0x5aea, 0x28 },
    { 0x5aeb, 0x3c },
    { 0x5aec, 0x30 },
    { 0x5aed, 0x28 },
    { 0x5aee, 0xfe },
    { 0x5aef, 0x40 },
    { 0x5af4, 0x30 },
    { 0x5af5, 0x2a },
    { 0x5af6, 0x24 },
    { 0x5af7, 0x30 },
    { 0x5af8, 0x2a },
    { 0x5af9, 0x24 },
    { 0x5afa, 0x3c },
    { 0x5afb, 0x30 },
    { 0x5afc, 0x28 },
    { 0x5afd, 0x3c },
    { 0x5afe, 0x30 },
    { 0x5aff, 0x28 },
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
    { 0x3e09, 0x00 },
};

const static I2C_ARRAY expo_reg[] = {
    { 0x3e00, 0x00 },
    { 0x3e01, 0xcd },
    { 0x3e02, 0xa0 },
};

const static I2C_ARRAY vts_reg[] = {
    { 0x320e, 0x06 },
    { 0x320f, 0x72 },
};

#if SENSOR_DBG == 1
#define SENSOR_DMSG(args...) SENSOR_DMSG(args)
#endif

#undef SENSOR_NAME
#define SENSOR_NAME sc430ai

#define SensorReg_Read(_reg, _data) (handle->i2c_bus->i2c_rx(handle->i2c_bus, &(handle->i2c_cfg), _reg, _data))
#define SensorReg_Write(_reg, _data) (handle->i2c_bus->i2c_tx(handle->i2c_bus, &(handle->i2c_cfg), _reg, _data))
#define SensorRegArrayW(_reg, _len) (handle->i2c_bus->i2c_array_tx(handle->i2c_bus, &(handle->i2c_cfg), (_reg), (_len)))
#define SensorRegArrayR(_reg, _len) (handle->i2c_bus->i2c_array_rx(handle->i2c_bus, &(handle->i2c_cfg), (_reg), (_len)))

static int pCus_poweron(ms_cus_sensor *handle, u32 idx)
{
    ISensorIfAPI *sensor_if = handle->sensor_if_api;

    sensor_if->Reset(idx, handle->reset_POLARITY);
    SENSOR_USLEEP(1000);
    sensor_if->PowerOff(idx, handle->pwdn_POLARITY);
    SENSOR_USLEEP(1000);

    /*
     * Tapo C120 needs the stock-compatible CSI pad/lane setup while keeping
     * the public MIPI lane count at 2 for the SigmaStar sensor interface.
     */
    sensor_if->SetIOPad(idx, handle->sif_bus, SENSOR_CSI_PAD_LANE_CFG);
    sensor_if->SetCSI_Clk(idx, CUS_CSI_CLK_216M);
    sensor_if->SetCSI_Lane(idx, SENSOR_CSI_PAD_LANE_CFG, 1);
    sensor_if->SetCSI_LongPacketType(idx, 0, 0x1C00, 0);
    sensor_if->MCLK(idx, 1, handle->mclk);

    sensor_if->PowerOff(idx, !handle->pwdn_POLARITY);
    CamOsMsSleep(1);
    sensor_if->Reset(idx, !handle->reset_POLARITY);
    CamOsMsSleep(1);

    return SUCCESS;
}

static int pCus_poweroff(ms_cus_sensor *handle, u32 idx)
{
    ISensorIfAPI *sensor_if = handle->sensor_if_api;

    sensor_if->PowerOff(idx, handle->pwdn_POLARITY);
    CamOsMsSleep(1);
    sensor_if->SetCSI_Clk(idx, CUS_CSI_CLK_DISABLE);
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

static int sc430ai_SetPatternMode(ms_cus_sensor *handle, u32 mode)
{
    return SUCCESS;
}

static int pCus_SetAEGain_cal(ms_cus_sensor *handle, u32 gain);
static int pCus_AEStatusNotify(ms_cus_sensor *handle, CUS_CAMSENSOR_AE_STATUS_NOTIFY status);

static int pCus_init_linear_4M30fps(ms_cus_sensor *handle)
{
    sc430ai_params *params = (sc430ai_params *)handle->private_data;
    int i, cnt;

    for (i = 0; i < ARRAY_SIZE(Sensor_init_table_4M30fps); i++) {
        if (Sensor_init_table_4M30fps[i].reg == 0xffff) {
            SENSOR_MSLEEP(Sensor_init_table_4M30fps[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_init_table_4M30fps[i].reg, Sensor_init_table_4M30fps[i].data) != SUCCESS) {
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
    sc430ai_params *params = (sc430ai_params *)handle->private_data;

    if (res_idx >= num_res)
        return FAIL;

    switch (res_idx) {
    case 0:
        handle->video_res_supported.ulcur_res = 0;
        handle->pCus_sensor_init = pCus_init_linear_4M30fps;
        vts_30fps = 1650;
        params->expo.vts = vts_30fps;
        params->expo.fps = 30;
        Preview_line_period = 20202;
        break;
    default:
        break;
    }

    return SUCCESS;
}

static int pCus_GetOrien(ms_cus_sensor *handle, CUS_CAMSENSOR_ORIT *orit)
{
    char sen_data;
    sc430ai_params *params = (sc430ai_params *)handle->private_data;

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
    sc430ai_params *params = (sc430ai_params *)handle->private_data;

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
    sc430ai_params *params = (sc430ai_params *)handle->private_data;
    u32 max_fps = handle->video_res_supported.res[handle->video_res_supported.ulcur_res].max_fps;
    u32 tVts = (params->tVts_reg[0].data << 8) | (params->tVts_reg[1].data << 0);

    if (params->expo.fps >= 1000)
        params->expo.preview_fps = (vts_30fps * max_fps * 1000) / tVts;
    else
        params->expo.preview_fps = (vts_30fps * max_fps) / tVts;

    return params->expo.preview_fps;
}

static int pCus_SetFPS(ms_cus_sensor *handle, u32 fps)
{
    u32 vts = 0;
    sc430ai_params *params = (sc430ai_params *)handle->private_data;
    u32 max_fps = handle->video_res_supported.res[handle->video_res_supported.ulcur_res].max_fps;
    u32 min_fps = handle->video_res_supported.res[handle->video_res_supported.ulcur_res].min_fps;

    if (fps >= min_fps && fps <= max_fps) {
        params->expo.fps = fps;
        params->expo.vts = (vts_30fps * max_fps) / fps;
    } else if ((fps >= (min_fps * 1000)) && (fps <= (max_fps * 1000))) {
        params->expo.fps = fps;
        params->expo.vts = (vts_30fps * (max_fps * 1000)) / fps;
    } else {
        SENSOR_DMSG("[%s] FPS %d out of range.\n", __FUNCTION__, fps);
        return FAIL;
    }

    if (params->expo.line > (2 * params->expo.vts) - 8)
        vts = (params->expo.line + 9) / 2;
    else
        vts = params->expo.vts;

    params->tVts_reg[0].data = (vts >> 8) & 0x00ff;
    params->tVts_reg[1].data = (vts >> 0) & 0x00ff;
    params->reg_dirty = true;

    return SUCCESS;
}

static int pCus_AEStatusNotify(ms_cus_sensor *handle, CUS_CAMSENSOR_AE_STATUS_NOTIFY status)
{
    sc430ai_params *params = (sc430ai_params *)handle->private_data;

    switch (status) {
    case CUS_FRAME_INACTIVE:
        break;
    case CUS_FRAME_ACTIVE:
        if (params->orient_dirty) {
            SensorRegArrayW((I2C_ARRAY *)params->tMirror_reg, sizeof(mirror_reg) / sizeof(I2C_ARRAY));
            params->orient_dirty = false;
        }
        if (params->reg_dirty) {
            SensorRegArrayW((I2C_ARRAY *)params->tExpo_reg, sizeof(expo_reg) / sizeof(I2C_ARRAY));
            SensorRegArrayW((I2C_ARRAY *)params->tGain_reg, sizeof(gain_reg) / sizeof(I2C_ARRAY));
            SensorRegArrayW((I2C_ARRAY *)params->tVts_reg, sizeof(vts_reg) / sizeof(I2C_ARRAY));
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
    sc430ai_params *params = (sc430ai_params *)handle->private_data;

    lines |= (u32)(params->tExpo_reg[0].data & 0x0f) << 16;
    lines |= (u32)(params->tExpo_reg[1].data & 0xff) << 8;
    lines |= (u32)(params->tExpo_reg[2].data & 0xf0) << 0;
    lines >>= 4;
    *us = (lines * Preview_line_period) / 1000 / 2;

    return rc;
}

static int pCus_SetAEUSecs(ms_cus_sensor *handle, u32 us)
{
    int i;
    u32 half_lines = 0, vts = 0;
    sc430ai_params *params = (sc430ai_params *)handle->private_data;
    I2C_ARRAY expo_reg_temp[] = {
        { 0x3e00, 0x00 },
        { 0x3e01, 0x00 },
        { 0x3e02, 0x10 },
    };

    memcpy(expo_reg_temp, params->tExpo_reg, sizeof(expo_reg));

    half_lines = (1000 * us * 2) / Preview_line_period;
    if (half_lines <= 3)
        half_lines = 3;
    if (half_lines > (2 * params->expo.vts) - 8)
        vts = (half_lines + 9) / 2;
    else
        vts = params->expo.vts;

    params->expo.line = half_lines;
    half_lines = half_lines << 4;
    params->tExpo_reg[0].data = (half_lines >> 16) & 0x0f;
    params->tExpo_reg[1].data = (half_lines >> 8) & 0xff;
    params->tExpo_reg[2].data = (half_lines >> 0) & 0xf0;
    params->tVts_reg[0].data = (vts >> 8) & 0x00ff;
    params->tVts_reg[1].data = (vts >> 0) & 0x00ff;

    for (i = 0; i < ARRAY_SIZE(expo_reg); i++) {
        if (params->tExpo_reg[i].data != expo_reg_temp[i].data) {
            params->reg_dirty = true;
            break;
        }
    }

    return SUCCESS;
}

static int pCus_GetAEGain(ms_cus_sensor *handle, u32 *gain)
{
    return SUCCESS;
}

static int pCus_SetAEGain_cal(ms_cus_sensor *handle, u32 gain)
{
    return SUCCESS;
}

static int pCus_SetAEGain(ms_cus_sensor *handle, u32 gain)
{
    sc430ai_params *params = (sc430ai_params *)handle->private_data;
    u8 i = 0, coarse_gain = 1, coarse_gain_reg = 0, analog_gain_reg = 0;
    u32 base_gain = 1024, fine_gain_reg;

    I2C_ARRAY gain_reg_temp[] = {
        { 0x3e06, 0x00 },
        { 0x3e07, 0x80 },
        { 0x3e09, 0x00 },
    };

    memcpy(gain_reg_temp, params->tGain_reg, sizeof(gain_reg_temp));

    if (gain <= 1024)
        gain = 1024;
    else if (gain > SENSOR_MAXGAIN * 1024)
        gain = SENSOR_MAXGAIN * 1024;

    if (gain < 2048) {
        base_gain = 1024;
        analog_gain_reg = 0x00;
    } else if (gain <= 2446) {
        base_gain = 2048;
        analog_gain_reg = 0x01;
    } else if (gain <= 4893) {
        base_gain = 2447;
        analog_gain_reg = 0x40;
    } else if (gain <= 9787) {
        base_gain = 4894;
        analog_gain_reg = 0x48;
    } else if (gain <= 19575) {
        base_gain = 9788;
        analog_gain_reg = 0x49;
    } else if (gain <= 39151) {
        base_gain = 19576;
        analog_gain_reg = 0x4b;
    } else if (gain <= 78303) {
        base_gain = 39152;
        analog_gain_reg = 0x4f;
    } else if (gain <= 156607) {
        base_gain = 78304;
        analog_gain_reg = 0x5f;
    } else {
        base_gain = 78304;
        coarse_gain = 2;
        analog_gain_reg = 0x5f;
    }

    fine_gain_reg = (gain << 7) / base_gain / coarse_gain;
    coarse_gain_reg = coarse_gain - 1;

    params->tGain_reg[0].data = coarse_gain_reg;
    params->tGain_reg[1].data = fine_gain_reg & 0xff;
    params->tGain_reg[2].data = analog_gain_reg;

    for (i = 0; i < ARRAY_SIZE(gain_reg); i++) {
        if (params->tGain_reg[i].data != gain_reg_temp[i].data) {
            params->reg_dirty = true;
            break;
        }
    }

    return SUCCESS;
}

static int pCus_GetAEMinMaxUSecs(ms_cus_sensor *handle, u32 *min, u32 *max)
{
    *min = 30;
    *max = 1000000 / Preview_MIN_FPS;
    return SUCCESS;
}

static int pCus_GetAEMinMaxGain(ms_cus_sensor *handle, u32 *min, u32 *max)
{
    *min = 1024;
    *max = SENSOR_MAXGAIN * 1024;
    return SUCCESS;
}

static int sc430ai_GetShutterInfo(struct __ms_cus_sensor *handle, CUS_SHUTTER_INFO *info)
{
    info->max = 1000000000 / Preview_MIN_FPS;
    info->min = (Preview_line_period * 3) / 2;
    info->step = Preview_line_period / 2;
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
    sc430ai_params *params;

    if (!handle) {
        SENSOR_DMSG("[%s] not enough memory!\n", __FUNCTION__);
        return FAIL;
    }
    if (handle->private_data == NULL) {
        SENSOR_EMSG("[%s] Private data is empty!\n", __FUNCTION__);
        return FAIL;
    }

    params = (sc430ai_params *)handle->private_data;
    memcpy(params->tVts_reg, vts_reg, sizeof(vts_reg));
    memcpy(params->tGain_reg, gain_reg, sizeof(gain_reg));
    memcpy(params->tExpo_reg, expo_reg, sizeof(expo_reg));
    memcpy(params->tMirror_reg, mirror_reg, sizeof(mirror_reg));

    sprintf(handle->model_id, "sc430ai_MIPI");

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
    handle->pCus_sensor_SetPatternMode = sc430ai_SetPatternMode;
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
    handle->pCus_sensor_GetShutterInfo = sc430ai_GetShutterInfo;

    params->expo.vts = vts_30fps;
    params->expo.fps = 30;
    params->expo.line = 1000;
    params->reg_dirty = false;
    params->orient_dirty = false;

    return SUCCESS;
}

static int cus_camsensor_release_handle(ms_cus_sensor *handle)
{
    return SUCCESS;
}

SENSOR_DRV_ENTRY_IMPL_END_EX(sc430ai,
                             cus_camsensor_init_handle,
                             NULL,
                             NULL,
                             sc430ai_params);
