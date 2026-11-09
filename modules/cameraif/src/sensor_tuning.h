// SPDX-License-Identifier: MIT
//
// Each sensor's colour tuning, as data: see sensor_tuning.c.

#pragma once

#include <stdint.h>

// The most entries a colour-temperature table holds, built in or from Python.
#define CAMERAIF_CT_MAX 16

// A colour matrix measured under light of one colour temperature, in kelvin.
typedef struct {
    float ct;
    float m[9];     // row by row
} cameraif_ct_ccm_t;

// Where white falls on the sensor under light of one colour temperature: its
// raw red and blue over green.
typedef struct {
    float ct;
    float rg, bg;
} cameraif_ct_white_t;

typedef struct {
    uint16_t pid;                       // the sensor's product ID
    float gamma;                        // the tone curve's exponent
    const float *ccm;                   // one matrix for all light, or NULL
    const cameraif_ct_ccm_t *ccm_table; // matrices by colour temperature, ascending, or NULL
    uint8_t n_ccm;
    const cameraif_ct_white_t *white;   // white by colour temperature, ascending, or NULL
    uint8_t n_white;
    int16_t contrast, saturation;       // the ISP colour block's start, 0-255; -1 leaves it off
} cameraif_tuning_t;

// The built-in tuning for a sensor, or NULL when there is none.
const cameraif_tuning_t *cameraif_tuning_for(uint16_t pid);
