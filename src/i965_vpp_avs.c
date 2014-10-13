/*
 * i965_vpp_avs.c - Adaptive Video Scaler (AVS) block
 *
 * Copyright (C) 2014 Intel Corporation
 *   Author: Gwenole Beauchesne <gwenole.beauchesne@intel.com>
 *
 * The source code contained or described herein and all documents related to
 * the source code ("Material") are owned by Intel Corporation or its suppliers
 * or licensors. Title to the Material remains with Intel Corporation or its
 * suppliers and licensors. The Material contains trade secrets and proprietary
 * and confidential information of Intel or its suppliers and licensors. The
 * Material is protected by worldwide copyright and trade secret laws and
 * treaty provisions. No part of the Material may be used, copied, reproduced,
 * modified, published, uploaded, posted, transmitted, distributed, or
 * disclosed in any way without Intel's prior express written permission.
 *
 * No license under any patent, copyright, trade secret or other intellectual
 * property right is granted to or conferred upon you by disclosure or delivery
 * of the Materials, either expressly, by implication, inducement, estoppel or
 * otherwise. Any license under such intellectual property rights must be
 * express and approved by Intel in writing.
 */

#include "sysdeps.h"
#include <math.h>
#include <va/va.h>
#include "i965_vpp_avs.h"

typedef void (*AVSGenCoeffsFunc)(float *coeffs, int num_coeffs, int phase,
    int num_phases, float f);

/* Initializes all coefficients to zero */
static void
avs_init_coeffs(float *coeffs, int num_coeffs)
{
#if defined(__STDC_IEC_559__) && (__STDC_IEC_559__ > 0)
    memset(coeffs, 0, num_coeffs * sizeof(*coeffs));
#else
    int i;

    for (i = 0; i < num_coeffs; i++)
        coeffs[i] = 0.0f;
#endif
}

/* Convolution kernel for linear interpolation */
static float
avs_kernel_linear(float x)
{
    const float abs_x = fabsf(x);

    return abs_x < 1.0f ? 1 - abs_x : 0.0f;
}

/* Convolution kernel for Lanczos-based interpolation */
static float
avs_kernel_lanczos(float x, float a)
{
    const float abs_x = fabsf(x);

    if (abs_x == 0.0f)
        return 1.0;
    if (abs_x < a)
        return a * sin(x * M_PI) * sin((x * M_PI) / a) / (M_PI * M_PI * x * x);
    return 0.0f;
}

/* Truncates floating-point value towards an epsilon factor */
static inline float
avs_trunc_coeff(float x, float epsilon)
{
    return floorf(x / epsilon) * epsilon;
}

/* Normalize coefficients for one sample/direction */
static void
avs_normalize_coeffs_1(float *coeffs, int num_coeffs, float epsilon)
{
    float s, sum = 0.0;
    int i, c, r, r1;

    for (i = 0; i < num_coeffs; i++)
        sum += coeffs[i];

    if (sum < epsilon)
        return;

    s = 0.0;
    for (i = 0; i < num_coeffs; i++)
        s += (coeffs[i] = avs_trunc_coeff(coeffs[i] / sum, epsilon));

    /* Distribute the remaining bits, while allocating more to the center */
    c = num_coeffs/2;
    c = c - (coeffs[c - 1] > coeffs[c]);

    r = (1.0f - s) / epsilon;
    r1 = r / 4;
    if (coeffs[c + 1] == 0.0f)
        coeffs[c] += r * epsilon;
    else {
        coeffs[c] += (r - 2*r1) * epsilon;
        coeffs[c - 1] += r1 * epsilon;
        coeffs[c + 1] += r1 * epsilon;
    }
}

/* Normalize all coefficients so that their sum yields 1.0f */
static void
avs_normalize_coeffs(AVSCoeffs *coeffs, const AVSConfig *config)
{
    avs_normalize_coeffs_1(coeffs->y_k_h, config->num_luma_coeffs,
        config->coeff_epsilon);
    avs_normalize_coeffs_1(coeffs->y_k_v, config->num_luma_coeffs,
        config->coeff_epsilon);
    avs_normalize_coeffs_1(coeffs->uv_k_h, config->num_chroma_coeffs,
        config->coeff_epsilon);
    avs_normalize_coeffs_1(coeffs->uv_k_v, config->num_chroma_coeffs,
        config->coeff_epsilon);
}

/* Generate coefficients for default quality (bilinear) */
static void
avs_gen_coeffs_linear(float *coeffs, int num_coeffs, int phase, int num_phases,
    float f)
{
    const int c = num_coeffs/2 - 1;
    const float p = (float)phase / (num_phases*2);

    avs_init_coeffs(coeffs, num_coeffs);
    coeffs[c] = avs_kernel_linear(p);
    coeffs[c + 1] = avs_kernel_linear(p - 1);
}

/* Generate coefficients for high quality (lanczos) */
static void
avs_gen_coeffs_lanczos(float *coeffs, int num_coeffs, int phase, int num_phases,
    float f)
{
    const int c = num_coeffs/2 - 1;
    const float p = (float)phase / (num_phases*2);
    int i, l = 2;

    l = num_coeffs > 4 ? 3 : 2;
    f = 1.0f / ceilf(1.0f/f);
    for (i = 0; i < num_coeffs; i++)
        coeffs[i] = avs_kernel_lanczos((i - (c + p)) * f, l);
}

/* Generate coefficients with the supplied scaler */
static void
avs_gen_coeffs(AVSState *avs, float sx, float sy, AVSGenCoeffsFunc gen_coeffs)
{
    const AVSConfig * const config = avs->config;
    int i;

    for (i = 0; i <= config->num_phases; i++) {
        AVSCoeffs * const coeffs = &avs->coeffs[i];

        gen_coeffs(coeffs->y_k_h, config->num_luma_coeffs,
            i, config->num_phases, sx);
        gen_coeffs(coeffs->uv_k_h, config->num_chroma_coeffs,
            i, config->num_phases, sx);
        gen_coeffs(coeffs->y_k_v, config->num_luma_coeffs,
            i, config->num_phases, sy);
        gen_coeffs(coeffs->uv_k_v, config->num_chroma_coeffs,
            i, config->num_phases, sy);

        avs_normalize_coeffs(coeffs, config);
    }
}

/* Initializes AVS state with the supplied configuration */
void
avs_init_state(AVSState *avs, const AVSConfig *config)
{
    avs->config = config;
}

/* Updates AVS coefficients for the supplied factors and quality level */
bool
avs_update_coefficients(AVSState *avs, float sx, float sy, uint32_t flags)
{
    AVSGenCoeffsFunc gen_coeffs;

    flags &= VA_FILTER_SCALING_MASK;
    switch (flags) {
    case VA_FILTER_SCALING_HQ:
        gen_coeffs = avs_gen_coeffs_lanczos;
        break;
    default:
        gen_coeffs = avs_gen_coeffs_linear;
        break;
    }
    avs_gen_coeffs(avs, sx, sy, gen_coeffs);
    return true;
}
