// SPDX-License-Identifier: MIT
//
// Each sensor's colour tuning, as data, chosen by its product ID when the
// camera opens. A raw sensor needs a colour matrix (its filters overlap, so
// its colour comes out pale and green-leaning) and a gamma curve (it counts
// light linearly, so mid-tones look dark), and both are measured per sensor.
// A sensor missing from this table gets an identity matrix and the default
// gamma; Python can override any of it (Camera(tuning=...), cam.tuning()).
//
// A matrix measured under one light is wrong under another, so a sensor can
// carry a table of them by colour temperature, plus where white falls on the
// sensor at each temperature. The camera then reads the temperature off its
// white balance and blends the two nearest matrices.

#include <stddef.h>

#include "sensor_tuning.h"

// The OV5647: Espressif's tuning (esp_cam_sensor's ov5647_default.json).
static const float ov5647_ccm[9] = {
    2.0f, -0.5459f, -0.4541f,
    -0.4751f, 1.7696f, -0.2945f,
    -0.2002f, -0.7998f, 2.0f,
};

// The OV2710: Espressif's factory tuning for the ESP32-P4-EYE
// (espressif/esp-dev-kits, examples/esp32-p4-eye/factory_demo,
// main/ov2710_custom.json, Apache-2.0).
//
// Its "acc.ccm.table": a matrix for each of 13 measured colour temperatures,
// with an identity at each end.
static const cameraif_ct_ccm_t ov2710_ccm_table[] = {
    { 1200, { 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f } },
    { 2314, { 2.2436f, -0.3347f, -0.9089f, -0.4129f, 1.3144f, 0.0985f, -0.5103f, -2.4895f, 3.9999f } },
    { 2882, { 2.3116f, -0.4091f, -0.9025f, -0.3807f, 1.5882f, -0.2075f, -0.3175f, -1.8562f, 3.1736f } },
    { 3372, { 2.0406f, -0.7137f, -0.3269f, -0.3717f, 1.4262f, -0.0544f, -0.4584f, -1.3831f, 2.8415f } },
    { 3880, { 2.0664f, -0.7524f, -0.3140f, -0.3392f, 1.4362f, -0.0970f, -0.3873f, -1.1604f, 2.5478f } },
    { 4310, { 2.0606f, -0.7608f, -0.2998f, -0.3150f, 1.4163f, -0.1012f, -0.3664f, -0.9836f, 2.3500f } },
    { 4679, { 2.1112f, -0.8504f, -0.2608f, -0.2974f, 1.4012f, -0.1038f, -0.3648f, -0.9099f, 2.2747f } },
    { 5097, { 2.1120f, -0.8299f, -0.2821f, -0.2857f, 1.3992f, -0.1135f, -0.3546f, -0.8452f, 2.1998f } },
    { 5470, { 2.1563f, -0.8270f, -0.3293f, -0.1982f, 1.3561f, -0.1579f, -0.2078f, -0.9811f, 2.1889f } },
    { 5780, { 2.0701f, -0.7070f, -0.3631f, -0.2310f, 1.4467f, -0.2157f, -0.2794f, -0.7587f, 2.0380f } },
    { 6086, { 2.0971f, -0.7310f, -0.3661f, -0.2285f, 1.4374f, -0.2090f, -0.2716f, -0.7361f, 2.0077f } },
    { 6670, { 2.0822f, -0.5657f, -0.5165f, -0.2036f, 1.4696f, -0.2661f, -0.1613f, -0.8585f, 2.0198f } },
    { 7200, { 2.0217f, -0.4805f, -0.5412f, -0.2007f, 1.5016f, -0.3009f, -0.1587f, -0.7368f, 1.8955f } },
    { 7847, { 2.0876f, -0.5671f, -0.5205f, -0.1967f, 1.4737f, -0.2770f, -0.1638f, -0.7154f, 1.8792f } },
    { 16000, { 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f } },
};

// Its "ian.color_temp.bp": 13 white points, red and blue over green, warmest
// first. The file gives them no temperatures; they pair one to one with the
// 13 measured matrices above, in the same order.
static const cameraif_ct_white_t ov2710_white[] = {
    { 2314, 1.4340f, 0.4717f },
    { 2882, 1.3019f, 0.5660f },
    { 3372, 1.1961f, 0.6078f },
    { 3880, 1.0943f, 0.6604f },
    { 4310, 1.0370f, 0.7037f },
    { 4679, 0.9630f, 0.7222f },
    { 5097, 0.9245f, 0.7547f },
    { 5470, 0.8750f, 0.7857f },
    { 5780, 0.8462f, 0.8077f },
    { 6086, 0.8200f, 0.8200f },
    { 6670, 0.7736f, 0.8491f },
    { 7200, 0.7143f, 0.8571f },
    { 7847, 0.6792f, 0.8868f },
};

#define N(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))

static const cameraif_tuning_t cameraif_tunings[] = {
    { .pid = 0x5647, .gamma = 0.72f, .ccm = ov5647_ccm, .contrast = -1, .saturation = -1 },
    // Gamma: the file steps between 0.51 and 0.56 by scene brightness
    // ("aen.gamma.table"); this takes its middle step. Contrast 130 and
    // saturation 128 are its "aen.contrast" and "acc.saturation" at the
    // lowest gain, in the ISP colour block's own units (128 is unchanged).
    { .pid = 0x2710, .gamma = 0.53f,
      .ccm_table = ov2710_ccm_table, .n_ccm = N(ov2710_ccm_table),
      .white = ov2710_white, .n_white = N(ov2710_white),
      .contrast = 130, .saturation = 128 },
};

const cameraif_tuning_t *cameraif_tuning_for(uint16_t pid) {
    for (size_t i = 0; i < sizeof(cameraif_tunings) / sizeof(cameraif_tunings[0]); i++) {
        if (cameraif_tunings[i].pid == pid) {
            return &cameraif_tunings[i];
        }
    }
    return NULL;
}
