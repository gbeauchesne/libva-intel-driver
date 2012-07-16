/*
 * Copyright (C) 2012 Intel Corporation. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "config.h"
#include <assert.h>
#include <va/va_drm.h>
#include <va/va_backend_drm.h>
#include "intel_driver.h"
#include "i965_output_drm.h"
#include "i965_drv_video.h"
#include "i965_defines.h"

static VAStatus
va_GetSurfaceBufferDRM(
    VADriverContextP ctx,
    VASurfaceID      surface,
    VABufferInfoDRM *out_buffer_info
)
{
    struct i965_driver_data * const i965 = i965_driver_data(ctx);
    VABufferInfoDRM * const bi = out_buffer_info;
    struct object_surface *obj_surface;
    uint32_t name;

    obj_surface = SURFACE(surface);
    if (!obj_surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    if (!out_buffer_info)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    if (drm_intel_bo_flink(obj_surface->bo, &name) != 0)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    bi->handle = name;
    switch (obj_surface->fourcc) {
    case VA_FOURCC('N','V','1','2'):
        bi->format     = DRM_FORMAT_NV12;
        bi->num_planes = 2;
        bi->offsets[0] = 0;
        bi->pitches[0] = obj_surface->width;
        bi->offsets[1] = obj_surface->width * obj_surface->y_cb_offset;
        bi->pitches[1] = obj_surface->cb_cr_pitch;
        bi->offsets[2] = 0;
        bi->pitches[2] = 0;
        break;
    case VA_FOURCC('Y','V','1','2'):
    case VA_FOURCC('I','4','2','0'):
    case VA_FOURCC('I','M','C','1'):
        switch (obj_surface->subsampling) {
        case SUBSAMPLE_YUV411:
            bi->format = DRM_FORMAT_YUV411;
            break;
        case SUBSAMPLE_YUV420:
            bi->format = DRM_FORMAT_YUV420;
            break;
        case SUBSAMPLE_YUV422H:
        case SUBSAMPLE_YUV422V:
            bi->format = DRM_FORMAT_YUV422;
            break;
        case SUBSAMPLE_YUV444:
            bi->format = DRM_FORMAT_YUV444;
            break;
        default:
            assert(0 && "unsupported subsampling");
            return VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;
        }
        bi->offsets[0] = 0;
        bi->pitches[0] = obj_surface->width;
        bi->offsets[1] = obj_surface->width * obj_surface->y_cb_offset;
        bi->pitches[1] = obj_surface->cb_cr_pitch;
        bi->offsets[2] = obj_surface->width * obj_surface->y_cr_offset;
        bi->pitches[2] = obj_surface->cb_cr_pitch;
        break;
    default:
        assert(0 && "unsupported format");
        return VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus
va_GetImageBufferDRM(
    VADriverContextP ctx,
    VAImageID        image,
    VABufferInfoDRM *bi
)
{
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

bool
i965_output_drm_init(VADriverContextP ctx)
{
    struct VADriverVTableDRM * const vtable = ctx->vtable_drm;

    if (!vtable || vtable->version != VA_DRM_API_VERSION)
        return false;

    vtable->vaGetSurfaceBufferDRM       = va_GetSurfaceBufferDRM;
    vtable->vaGetImageBufferDRM         = va_GetImageBufferDRM;
    return true;
}

void
i965_output_drm_terminate(VADriverContextP ctx)
{
}
