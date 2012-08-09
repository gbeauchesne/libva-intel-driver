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

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <va/va_egl.h>
#include <va/va_backend_egl.h>
#include "i965_defines.h"
#include "i965_drv_video.h"
#include "i965_output_egl.h"

static inline void
swap_planes(struct va_egl_client_buffer *buf, int plane_a, int plane_b)
{
    uint32_t tmp_pitch    = buf->pitches[plane_a];
    intptr_t tmp_offset   = buf->offsets[plane_a];
    buf->pitches[plane_a] = buf->pitches[plane_b];
    buf->offsets[plane_a] = buf->offsets[plane_b];
    buf->pitches[plane_b] = tmp_pitch;
    buf->offsets[plane_b] = tmp_offset;
}

static void
va_egl_client_buffer_destroy(struct va_egl_client_buffer *buf)
{
    if (!buf)
        return;

    if (buf->private_data) {
        if (buf->destroy_private_data)
            buf->destroy_private_data(buf->private_data);
        buf->private_data = NULL;
    }
    free(buf);
}

static struct va_egl_client_buffer *
va_egl_client_buffer_new_from_surface(struct object_surface *obj_surface)
{
    struct va_egl_client_buffer *buf;
    uint32_t name;

    buf = calloc(1, sizeof(*buf));
    if (!buf)
        return NULL;

    if (drm_intel_bo_flink(obj_surface->bo, &name) != 0)
        goto error;

    buf->version = VA_EGL_CLIENT_BUFFER_VERSION;
    buf->handle  = name;
    buf->width   = obj_surface->orig_width;
    buf->height  = obj_surface->orig_height;

    switch (obj_surface->fourcc) {
    case VA_FOURCC('N','V','1','2'):
        buf->structure  = VA_EGL_BUFFER_STRUCTURE_Y_UV;
        buf->format     = VA_EGL_PIXEL_FORMAT_NV12;
        buf->num_planes = 2;
        buf->pitches[0] = obj_surface->width;           /* Y */
        buf->offsets[0] = 0;
        buf->pitches[1] = obj_surface->cb_cr_pitch;     /* UV */
        buf->offsets[1] = obj_surface->width * obj_surface->y_cb_offset;
        break;
    case VA_FOURCC('I','4','2','0'):
    case VA_FOURCC('Y','V','1','2'):
    case VA_FOURCC('I','M','C','1'):
    case VA_FOURCC('I','M','C','3'):
        buf->structure  = VA_EGL_BUFFER_STRUCTURE_Y_U_V;
        switch (obj_surface->subsampling) {
        case SUBSAMPLE_YUV400:
            buf->format = VA_EGL_PIXEL_FORMAT_GRAY8;
            break;
        case SUBSAMPLE_YUV411:
            buf->format = VA_EGL_PIXEL_FORMAT_YUV411P;
            break;
        case SUBSAMPLE_YUV420:
            buf->format = VA_EGL_PIXEL_FORMAT_YUV420P;
            break;
        case SUBSAMPLE_YUV422H:
        case SUBSAMPLE_YUV422V:
            buf->format = VA_EGL_PIXEL_FORMAT_YUV422P;
            break;
        case SUBSAMPLE_YUV444:
            buf->format = VA_EGL_PIXEL_FORMAT_YUV444P;
            break;
        default:
            assert(0 && "unsupported subsampling");
            abort();
        }
        buf->num_planes = 3;
        buf->pitches[0] = obj_surface->width;           /* Y */
        buf->offsets[0] = 0;
        buf->pitches[1] = obj_surface->cb_cr_pitch;     /* U */
        buf->offsets[1] = obj_surface->width * obj_surface->y_cb_offset;
        buf->pitches[2] = obj_surface->cb_cr_pitch;     /* V */
        buf->offsets[2] = obj_surface->width * obj_surface->y_cr_offset;
        break;
    default:
        assert(0 && "unsupported pixel format");
        abort();
        break;
    }
    return buf;

error:
    va_egl_client_buffer_destroy(buf);
    return NULL;
}

static struct va_egl_client_buffer *
va_egl_client_buffer_new_from_image(struct object_image *obj_image)
{
    const VAImage * const image = &obj_image->image;
    struct va_egl_client_buffer *buf;
    uint32_t name;
    unsigned int i;

    buf = calloc(1, sizeof(*buf));
    if (!buf)
        return NULL;

    if (drm_intel_bo_flink(obj_image->bo, &name) != 0)
        goto error;

    buf->version = VA_EGL_CLIENT_BUFFER_VERSION;
    buf->handle  = name;
    buf->width   = image->width;
    buf->height  = image->height;

    buf->num_planes = image->num_planes;
    for (i = 0; i < buf->num_planes; i++) {
        buf->pitches[i] = image->pitches[i];
        buf->offsets[i] = image->offsets[i];
    }

    /* Normalize plane info and format */
    switch (image->format.fourcc) {
    case VA_FOURCC('B','G','R','A'):
        buf->structure = VA_EGL_BUFFER_STRUCTURE_RGBA;
        buf->format    = VA_EGL_PIXEL_FORMAT_ARGB8888;
        break;
        // fall-through
    case VA_FOURCC('R','G','B','A'):
        buf->structure = VA_EGL_BUFFER_STRUCTURE_RGBA;
        buf->format    = VA_EGL_PIXEL_FORMAT_ABGR8888;
        break;
    case VA_FOURCC('N','V','1','2'):
        buf->structure = VA_EGL_BUFFER_STRUCTURE_Y_UV;
        buf->format    = VA_EGL_PIXEL_FORMAT_NV12;
        break;
    case VA_FOURCC('I','4','2','0'):
    case VA_FOURCC('Y','V','1','2'):
        /* XXX: only 4:2:0 subsampling is supported for VA images */
        buf->structure = VA_EGL_BUFFER_STRUCTURE_Y_U_V;
        buf->format    = VA_EGL_PIXEL_FORMAT_YUV420P;
        swap_planes(buf, 1, 2);
        break;
    default:
        assert(0 && "unsupported pixel format");
        abort();
        break;
    }
    return buf;

error:
    va_egl_client_buffer_destroy(buf);
    return NULL;
}

void
i965_destroy_egl_client_buffer(void *buffer)
{
    va_egl_client_buffer_destroy(buffer);
}

/* Hook to return an EGL client buffer associated with the VA surface */
static VAStatus
va_GetSurfaceBufferEGL(
    VADriverContextP    ctx,
    VASurfaceID         surface,
    EGLClientBuffer    *out_buffer
)
{
    struct i965_driver_data * const i965 = i965_driver_data(ctx);
    struct object_surface *obj_surface;
    struct va_egl_client_buffer *buf;

    obj_surface = SURFACE(surface);
    if (!obj_surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    if (!out_buffer)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    buf = obj_surface->egl_client_buffer;
    if (!buf) {
        buf = va_egl_client_buffer_new_from_surface(obj_surface);
        if (!buf)
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        obj_surface->egl_client_buffer = buf;
    }

    *out_buffer = buf;
    return VA_STATUS_SUCCESS;
}

/* Hook to return an EGL client buffer associated with the VA image */
VAStatus
va_GetImageBufferEGL(
    VADriverContextP    ctx,
    VAImageID           image,
    EGLClientBuffer    *out_buffer
)
{
    struct i965_driver_data * const i965 = i965_driver_data(ctx);
    struct object_image *obj_image;
    struct va_egl_client_buffer *buf;

    obj_image = IMAGE(image);
    if (!obj_image)
        return VA_STATUS_ERROR_INVALID_IMAGE;

    /* XXX: we don't support paletted formats yet */
    if (obj_image->palette)
        return VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;

    if (!out_buffer)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    buf = obj_image->egl_client_buffer;
    if (!buf) {
        buf = va_egl_client_buffer_new_from_image(obj_image);
        if (!buf)
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        obj_image->egl_client_buffer = buf;
    }

    *out_buffer = buf;
    return VA_STATUS_SUCCESS;
}

/* Hook to query VA/EGL buffer attributes */
VAStatus
va_GetBufferAttributeEGL(
    VADriverContextP    ctx,
    EGLClientBuffer     buffer,
    EGLenum             attribute,
    EGLint             *value
)
{
    struct va_egl_client_buffer * const buf = buffer;

    if (!buf || buf->version != VA_EGL_CLIENT_BUFFER_VERSION)
        return VA_STATUS_ERROR_INVALID_BUFFER;

    if (!value)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    switch (attribute) {
    case EGL_WIDTH:
        *value = buf->width;
        break;
    case EGL_HEIGHT:
        *value = buf->height;
        break;
    case EGL_TEXTURE_FORMAT:
        *value = buf->structure;
        break;
    default:
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    return VA_STATUS_SUCCESS;
}

bool
i965_output_egl_init(VADriverContextP ctx)
{
    struct VADriverVTableEGL *vtable;

    vtable = calloc(1, sizeof(*vtable));
    if (!vtable)
        return false;
    ctx->vtable_egl = vtable;

    vtable->version                     = VA_EGL_VTABLE_VERSION;
    vtable->vaGetSurfaceBufferEGL       = va_GetSurfaceBufferEGL;
    vtable->vaGetImageBufferEGL         = va_GetImageBufferEGL;
    vtable->vaGetBufferAttributeEGL     = va_GetBufferAttributeEGL;
    return true;
}

void
i965_output_egl_terminate(VADriverContextP ctx)
{
    free(ctx->vtable_egl);
    ctx->vtable_egl = NULL;
}
