/*
 * Copyright (C) 2006-2012 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "sysdeps.h"

#include <alloca.h>

#include "intel_batchbuffer.h"
#include "i965_drv_video.h"
#include "i965_decoder_utils.h"
#include "i965_defines.h"

/* Set reference surface if backing store exists */
static inline int
set_ref_frame(
    struct i965_driver_data *i965,
    GenFrameStore           *ref_frame,
    VASurfaceID              va_surface,
    struct object_surface   *obj_surface
)
{
    if (va_surface == VA_INVALID_ID)
        return 0;

    if (!obj_surface || !obj_surface->bo)
        return 0;

    ref_frame->surface_id = va_surface;
    ref_frame->obj_surface = obj_surface;
    return 1;
}

/* Check wether codec layer incorrectly fills in slice_vertical_position */
int
mpeg2_wa_slice_vertical_position(
    struct decode_state           *decode_state,
    VAPictureParameterBufferMPEG2 *pic_param
)
{
    unsigned int i, j, mb_height, vpos, last_vpos = 0;

    /* Assume progressive sequence if we got a progressive frame */
    if (pic_param->picture_coding_extension.bits.progressive_frame)
        return 0;

    /* Wait for a field coded picture */
    if (pic_param->picture_coding_extension.bits.picture_structure == MPEG_FRAME)
        return -1;

    assert(decode_state && decode_state->slice_params);

    mb_height = (pic_param->vertical_size + 31) / 32;

    for (j = 0; j < decode_state->num_slice_params; j++) {
        struct buffer_store * const buffer_store =
            decode_state->slice_params[j];

        for (i = 0; i < buffer_store->num_elements; i++) {
            VASliceParameterBufferMPEG2 * const slice_param =
                ((VASliceParameterBufferMPEG2 *)buffer_store->buffer) + i;

            vpos = slice_param->slice_vertical_position;
            if (vpos >= mb_height || vpos == last_vpos + 2) {
                WARN_ONCE("codec layer incorrectly fills in MPEG-2 slice_vertical_position. Workaround applied\n");
                return 1;
            }
            last_vpos = vpos;
        }
    }
    return 0;
}

/* Build MPEG-2 reference frames array */
void
mpeg2_set_reference_surfaces(
    VADriverContextP               ctx,
    GenFrameStore                  ref_frames[MAX_GEN_REFERENCE_FRAMES],
    struct decode_state           *decode_state,
    VAPictureParameterBufferMPEG2 *pic_param
)
{
    struct i965_driver_data * const i965 = i965_driver_data(ctx);
    VASurfaceID va_surface;
    unsigned pic_structure, is_second_field, n = 0;
    struct object_surface *obj_surface;

    pic_structure = pic_param->picture_coding_extension.bits.picture_structure;
    is_second_field = pic_structure != MPEG_FRAME &&
        !pic_param->picture_coding_extension.bits.is_first_field;

    ref_frames[0].surface_id = VA_INVALID_ID;
    ref_frames[0].obj_surface = NULL;

    /* Reference frames are indexed by frame store ID  (0:top, 1:bottom) */
    switch (pic_param->picture_coding_type) {
    case MPEG_P_PICTURE:
        if (is_second_field && pic_structure == MPEG_BOTTOM_FIELD) {
            va_surface = decode_state->current_render_target;
            obj_surface = decode_state->render_object;
            n += set_ref_frame(i965, &ref_frames[n], va_surface, obj_surface);
        }
        va_surface = pic_param->forward_reference_picture;
        obj_surface = decode_state->reference_objects[0];
        n += set_ref_frame(i965, &ref_frames[n], va_surface, obj_surface);
        break;

    case MPEG_B_PICTURE:
        va_surface = pic_param->forward_reference_picture;
        obj_surface = decode_state->reference_objects[0];
        n += set_ref_frame(i965, &ref_frames[n], va_surface, obj_surface);
        va_surface = pic_param->backward_reference_picture;
        obj_surface = decode_state->reference_objects[1];
        n += set_ref_frame(i965, &ref_frames[n], va_surface, obj_surface);
        break;
    }

    while (n != 2) {
        ref_frames[n].obj_surface = ref_frames[0].obj_surface;
        ref_frames[n++].surface_id = ref_frames[0].surface_id;
    }

    if (pic_param->picture_coding_extension.bits.frame_pred_frame_dct)
        return;

    ref_frames[2].surface_id = VA_INVALID_ID;
    ref_frames[2].obj_surface = NULL;

    /* Bottom field pictures used as reference */
    switch (pic_param->picture_coding_type) {
    case MPEG_P_PICTURE:
        if (is_second_field && pic_structure == MPEG_TOP_FIELD) {
            va_surface = decode_state->current_render_target;
            obj_surface = decode_state->render_object;
            n += set_ref_frame(i965, &ref_frames[n], va_surface, obj_surface);
        }
        va_surface = pic_param->forward_reference_picture;
        obj_surface = decode_state->reference_objects[0];
        n += set_ref_frame(i965, &ref_frames[n], va_surface, obj_surface);
        break;

    case MPEG_B_PICTURE:
        va_surface = pic_param->forward_reference_picture;
        obj_surface = decode_state->reference_objects[0];
        n += set_ref_frame(i965, &ref_frames[n], va_surface, obj_surface);
        va_surface = pic_param->backward_reference_picture;
        obj_surface = decode_state->reference_objects[1];
        n += set_ref_frame(i965, &ref_frames[n], va_surface, obj_surface);
        break;
    }

    while (n != 4) {
        ref_frames[n].obj_surface = ref_frames[2].obj_surface;
        ref_frames[n++].surface_id = ref_frames[2].surface_id;
    }
}

/* Ensure the supplied VA surface has valid storage for decoding the
   current picture */
VAStatus
avc_ensure_surface_bo(
    VADriverContextP                    ctx,
    struct decode_state                *decode_state,
    struct object_surface              *obj_surface,
    const VAPictureParameterBufferH264 *pic_param
)
{
    VAStatus va_status;
    uint32_t hw_fourcc, fourcc, subsample;

    /* Validate chroma format */
    switch (pic_param->seq_fields.bits.chroma_format_idc) {
    case 0: // Grayscale
        fourcc = VA_FOURCC_Y800;
        subsample = SUBSAMPLE_YUV400;
        break;
    case 1: // YUV 4:2:0
        fourcc = VA_FOURCC_NV12;
        subsample = SUBSAMPLE_YUV420;
        break;
    default:
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }

    /* XXX: always allocate NV12 (YUV 4:2:0) surfaces for now */
    hw_fourcc = VA_FOURCC_NV12;
    subsample = SUBSAMPLE_YUV420;

    /* (Re-)allocate the underlying surface buffer store, if necessary */
    if (!obj_surface->bo || obj_surface->fourcc != hw_fourcc) {
        i965_destroy_surface_storage(obj_surface);
        va_status = i965_check_alloc_surface_bo(ctx, obj_surface, 1,
            hw_fourcc, subsample);
        if (va_status != VA_STATUS_SUCCESS)
            return va_status;
    }

    /* Fake chroma components if grayscale is implemented on top of NV12 */
    if (fourcc == VA_FOURCC_Y800 && hw_fourcc == VA_FOURCC_NV12) {
        const uint32_t uv_offset = obj_surface->width * obj_surface->height;
        const uint32_t uv_size   = obj_surface->width * obj_surface->height / 2;

        drm_intel_gem_bo_map_gtt(obj_surface->bo);
        memset(obj_surface->bo->virtual + uv_offset, 0x80, uv_size);
        drm_intel_gem_bo_unmap_gtt(obj_surface->bo);
    }
    return VA_STATUS_SUCCESS;
}

/* Generate flat scaling matrices for H.264 decoding */
void
avc_gen_default_iq_matrix(VAIQMatrixBufferH264 *iq_matrix)
{
    /* Flat_4x4_16 */
    memset(&iq_matrix->ScalingList4x4, 16, sizeof(iq_matrix->ScalingList4x4));

    /* Flat_8x8_16 */
    memset(&iq_matrix->ScalingList8x8, 16, sizeof(iq_matrix->ScalingList8x8));
}

/* Get first macroblock bit offset for BSD, minus EPB count (AVC) */
/* XXX: slice_data_bit_offset does not account for EPB */
unsigned int
avc_get_first_mb_bit_offset(
    dri_bo                     *slice_data_bo,
    VASliceParameterBufferH264 *slice_param,
    unsigned int                mode_flag
)
{
    unsigned int slice_data_bit_offset = slice_param->slice_data_bit_offset;

    if (mode_flag == ENTROPY_CABAC)
        slice_data_bit_offset = ALIGN(slice_data_bit_offset, 0x8);
    return slice_data_bit_offset;
}

/* Get first macroblock bit offset for BSD, with EPB count (AVC) */
/* XXX: slice_data_bit_offset does not account for EPB */
unsigned int
avc_get_first_mb_bit_offset_with_epb(
    dri_bo                     *slice_data_bo,
    VASliceParameterBufferH264 *slice_param,
    unsigned int                mode_flag
)
{
    unsigned int in_slice_data_bit_offset = slice_param->slice_data_bit_offset;
    unsigned int out_slice_data_bit_offset;
    unsigned int i, j, n, buf_size, data_size, header_size;
    uint8_t *buf;
    int ret;

    header_size = slice_param->slice_data_bit_offset / 8;
    data_size   = slice_param->slice_data_size - slice_param->slice_data_offset;
    buf_size    = (header_size * 3 + 1) / 2; // Max possible header size (x1.5)

    if (buf_size > data_size)
        buf_size = data_size;

    buf = alloca(buf_size);
    ret = dri_bo_get_subdata(
        slice_data_bo, slice_param->slice_data_offset,
        buf_size, buf
    );
    assert(ret == 0);

    for (i = 2, j = 2, n = 0; i < buf_size && j < header_size; i++, j++) {
        if (buf[i] == 0x03 && buf[i - 1] == 0x00 && buf[i - 2] == 0x00)
            i += 2, j++, n++;
    }

    out_slice_data_bit_offset = in_slice_data_bit_offset + n * 8;

    if (mode_flag == ENTROPY_CABAC)
        out_slice_data_bit_offset = ALIGN(out_slice_data_bit_offset, 0x8);
    return out_slice_data_bit_offset;
}

static inline uint8_t
get_ref_idx_state_1(const VAPictureH264 *va_pic, unsigned int frame_store_id)
{
    const unsigned int is_long_term =
        !!(va_pic->flags & VA_PICTURE_H264_LONG_TERM_REFERENCE);
    const unsigned int is_top_field =
        !!(va_pic->flags & VA_PICTURE_H264_TOP_FIELD);
    const unsigned int is_bottom_field =
        !!(va_pic->flags & VA_PICTURE_H264_BOTTOM_FIELD);

    return ((is_long_term                         << 6) |
            ((is_top_field ^ is_bottom_field ^ 1) << 5) |
            (frame_store_id                       << 1) |
            ((is_top_field ^ 1) & is_bottom_field));
}

/* Fill in Reference List Entries (Gen5+: ILK, SNB, IVB) */
void
gen5_fill_avc_ref_idx_state(
    uint8_t             state[32],
    const VAPictureH264 ref_list[32],
    unsigned int        ref_list_count,
    const GenFrameStore frame_store[MAX_GEN_REFERENCE_FRAMES]
)
{
    unsigned int i, n, frame_idx;
    int found;

    for (i = 0, n = 0; i < ref_list_count; i++) {
        const VAPictureH264 * const va_pic = &ref_list[i];

        if (va_pic->flags & VA_PICTURE_H264_INVALID)
            continue;

        found = 0;
        for (frame_idx = 0; frame_idx < MAX_GEN_REFERENCE_FRAMES; frame_idx++) {
            const GenFrameStore * const fs = &frame_store[frame_idx];
            if (fs->surface_id != VA_INVALID_ID &&
                fs->surface_id == va_pic->picture_id) {
                found = 1;
                break;
            }
        }

        if (found) {
            state[n++] = get_ref_idx_state_1(va_pic, frame_idx);
        } else {
            WARN_ONCE("Invalid Slice reference frame list !!!. It is not included in DPB \n");
        }
    }

    for (; n < 32; n++)
        state[n] = 0xff;
}

/* Emit Reference List Entries (Gen6+: SNB, IVB) */
static void
gen6_send_avc_ref_idx_state_1(
    struct intel_batchbuffer         *batch,
    unsigned int                      list,
    const VAPictureH264              *ref_list,
    unsigned int                      ref_list_count,
    const GenFrameStore               frame_store[MAX_GEN_REFERENCE_FRAMES]
)
{
    uint8_t ref_idx_state[32];

    BEGIN_BCS_BATCH(batch, 10);
    OUT_BCS_BATCH(batch, MFX_AVC_REF_IDX_STATE | (10 - 2));
    OUT_BCS_BATCH(batch, list);
    gen5_fill_avc_ref_idx_state(
        ref_idx_state,
        ref_list, ref_list_count,
        frame_store
    );
    intel_batchbuffer_data(batch, ref_idx_state, sizeof(ref_idx_state));
    ADVANCE_BCS_BATCH(batch);
}

void
gen6_send_avc_ref_idx_state(
    struct intel_batchbuffer         *batch,
    const VASliceParameterBufferH264 *slice_param,
    const GenFrameStore               frame_store[MAX_GEN_REFERENCE_FRAMES]
)
{
    if (slice_param->slice_type == SLICE_TYPE_I ||
        slice_param->slice_type == SLICE_TYPE_SI)
        return;

    /* RefPicList0 */
    gen6_send_avc_ref_idx_state_1(
        batch, 0,
        slice_param->RefPicList0, slice_param->num_ref_idx_l0_active_minus1 + 1,
        frame_store
    );

    if (slice_param->slice_type != SLICE_TYPE_B)
        return;

    /* RefPicList1 */
    gen6_send_avc_ref_idx_state_1(
        batch, 1,
        slice_param->RefPicList1, slice_param->num_ref_idx_l1_active_minus1 + 1,
        frame_store
    );
}

void
intel_update_avc_frame_store_index(VADriverContextP ctx,
                                   struct decode_state *decode_state,
                                   VAPictureParameterBufferH264 *pic_param,
                                   GenFrameStore frame_store[MAX_GEN_REFERENCE_FRAMES])
{
    int i, j;

    assert(MAX_GEN_REFERENCE_FRAMES == ARRAY_ELEMS(pic_param->ReferenceFrames));

    for (i = 0; i < MAX_GEN_REFERENCE_FRAMES; i++) {
        int found = 0;

        if (frame_store[i].surface_id == VA_INVALID_ID ||
            frame_store[i].obj_surface == NULL)
            continue;

        assert(frame_store[i].frame_store_id != -1);

        for (j = 0; j < MAX_GEN_REFERENCE_FRAMES; j++) {
            VAPictureH264 *ref_pic = &pic_param->ReferenceFrames[j];
            if (ref_pic->flags & VA_PICTURE_H264_INVALID)
                continue;

            if (frame_store[i].surface_id == ref_pic->picture_id) {
                found = 1;
                break;
            }
        }

        /* remove it from the internal DPB */
        if (!found) {
            struct object_surface *obj_surface = frame_store[i].obj_surface;
            
            obj_surface->flags &= ~SURFACE_REFERENCED;

            if ((obj_surface->flags & SURFACE_ALL_MASK) == SURFACE_DISPLAYED) {
                obj_surface->flags &= ~SURFACE_REF_DIS_MASK;
                i965_destroy_surface_storage(obj_surface);
            }

            frame_store[i].surface_id = VA_INVALID_ID;
            frame_store[i].frame_store_id = -1;
            frame_store[i].obj_surface = NULL;
        }
    }

    for (i = 0; i < MAX_GEN_REFERENCE_FRAMES; i++) {
        VAPictureH264 *ref_pic = &pic_param->ReferenceFrames[i];
        int found = 0;

        if (ref_pic->flags & VA_PICTURE_H264_INVALID ||
            ref_pic->picture_id == VA_INVALID_SURFACE ||
            decode_state->reference_objects[i] == NULL)
            continue;

        for (j = 0; j < MAX_GEN_REFERENCE_FRAMES; j++) {
            if (frame_store[j].surface_id == ref_pic->picture_id) {
                found = 1;
                break;
            }
        }

        /* add the new reference frame into the internal DPB */
        if (!found) {
            int frame_idx;
            int slot_found;
            struct object_surface *obj_surface = decode_state->reference_objects[i];

            /* 
             * Sometimes a dummy frame comes from the upper layer library, call i965_check_alloc_surface_bo()
             * to ake sure the store buffer is allocated for this reference frame
             */
            avc_ensure_surface_bo(ctx, decode_state, obj_surface, pic_param);

            slot_found = 0;
            frame_idx = -1;
            /* Find a free frame store index */
            for (j = 0; j < MAX_GEN_REFERENCE_FRAMES; j++) {
                if (frame_store[j].surface_id == VA_INVALID_ID ||
                    frame_store[j].obj_surface == NULL) {
                    frame_idx = j;
                    slot_found = 1;
                    break;
                }
            }


	    if (slot_found) {
                frame_store[j].surface_id = ref_pic->picture_id;
                frame_store[j].frame_store_id = frame_idx;
                frame_store[j].obj_surface = obj_surface;
            } else {
                WARN_ONCE("Not free slot for DPB reference list!!!\n");
	    }
        }
    }

}

void
intel_update_vc1_frame_store_index(VADriverContextP ctx,
                                   struct decode_state *decode_state,
                                   VAPictureParameterBufferVC1 *pic_param,
                                   GenFrameStore frame_store[MAX_GEN_REFERENCE_FRAMES])
{
    struct object_surface *obj_surface;
    int i;

    obj_surface = decode_state->reference_objects[0];

    if (pic_param->forward_reference_picture == VA_INVALID_ID ||
        !obj_surface || 
        !obj_surface->bo) {
        frame_store[0].surface_id = VA_INVALID_ID;
        frame_store[0].obj_surface = NULL;
    } else {
        frame_store[0].surface_id = pic_param->forward_reference_picture;
        frame_store[0].obj_surface = obj_surface;
    }

    obj_surface = decode_state->reference_objects[1];

    if (pic_param->backward_reference_picture == VA_INVALID_ID ||
        !obj_surface || 
        !obj_surface->bo) {
        frame_store[1].surface_id = frame_store[0].surface_id;
        frame_store[1].obj_surface = frame_store[0].obj_surface;
    } else {
        frame_store[1].surface_id = pic_param->backward_reference_picture;
        frame_store[1].obj_surface = obj_surface;
    }
    for (i = 2; i < MAX_GEN_REFERENCE_FRAMES; i++) {
        frame_store[i].surface_id = frame_store[i % 2].surface_id;
        frame_store[i].obj_surface = frame_store[i % 2].obj_surface;
    }

}

void
intel_update_vp8_frame_store_index(VADriverContextP ctx,
                                   struct decode_state *decode_state,
                                   VAPictureParameterBufferVP8 *pic_param,
                                   GenFrameStore frame_store[MAX_GEN_REFERENCE_FRAMES])
{
    struct object_surface *obj_surface;
    int i;

    obj_surface = decode_state->reference_objects[0];

    if (pic_param->last_ref_frame == VA_INVALID_ID ||
        !obj_surface ||
        !obj_surface->bo) {
        frame_store[0].surface_id = VA_INVALID_ID;
        frame_store[0].obj_surface = NULL;
    } else {
        frame_store[0].surface_id = pic_param->last_ref_frame;
        frame_store[0].obj_surface = obj_surface;
    }

    obj_surface = decode_state->reference_objects[1];

    if (pic_param->golden_ref_frame == VA_INVALID_ID ||
        !obj_surface ||
        !obj_surface->bo) {
        frame_store[1].surface_id = frame_store[0].surface_id;
        frame_store[1].obj_surface = frame_store[0].obj_surface;
    } else {
        frame_store[1].surface_id = pic_param->golden_ref_frame;
        frame_store[1].obj_surface = obj_surface;
    }

    obj_surface = decode_state->reference_objects[2];

    if (pic_param->alt_ref_frame == VA_INVALID_ID ||
        !obj_surface ||
        !obj_surface->bo) {
        frame_store[2].surface_id = frame_store[0].surface_id;
        frame_store[2].obj_surface = frame_store[0].obj_surface;
    } else {
        frame_store[2].surface_id = pic_param->alt_ref_frame;
        frame_store[2].obj_surface = obj_surface;
    }

    for (i = 3; i < MAX_GEN_REFERENCE_FRAMES; i++) {
        frame_store[i].surface_id = frame_store[i % 2].surface_id;
        frame_store[i].obj_surface = frame_store[i % 2].obj_surface;
    }

}

static VAStatus
intel_decoder_check_avc_parameter(VADriverContextP ctx,
                                  VAProfile h264_profile,
                                  struct decode_state *decode_state)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    VAPictureParameterBufferH264 *pic_param = (VAPictureParameterBufferH264 *)decode_state->pic_param->buffer;
    struct object_surface *obj_surface;	
    int i;

    assert(!(pic_param->CurrPic.flags & VA_PICTURE_H264_INVALID));
    assert(pic_param->CurrPic.picture_id != VA_INVALID_SURFACE);

    if (pic_param->CurrPic.flags & VA_PICTURE_H264_INVALID ||
        pic_param->CurrPic.picture_id == VA_INVALID_SURFACE)
        goto error;

    assert(pic_param->CurrPic.picture_id == decode_state->current_render_target);

    if (pic_param->CurrPic.picture_id != decode_state->current_render_target)
        goto error;

    if ((h264_profile != VAProfileH264Baseline)) {
       if (pic_param->num_slice_groups_minus1 ||
           pic_param->pic_fields.bits.redundant_pic_cnt_present_flag) {
           WARN_ONCE("Unsupported the FMO/ASO constraints!!!\n");
           goto error;
       }
    }

    for (i = 0; i < 16; i++) {
        if (pic_param->ReferenceFrames[i].flags & VA_PICTURE_H264_INVALID ||
            pic_param->ReferenceFrames[i].picture_id == VA_INVALID_SURFACE)
            break;
        else {
            obj_surface = SURFACE(pic_param->ReferenceFrames[i].picture_id);
            assert(obj_surface);

            if (!obj_surface)
                goto error;

            if (!obj_surface->bo) { /* a reference frame  without store buffer */
                WARN_ONCE("Invalid reference frame!!!\n");
            }

            decode_state->reference_objects[i] = obj_surface;
        }
    }

    for ( ; i < 16; i++)
        decode_state->reference_objects[i] = NULL;

    return VA_STATUS_SUCCESS;

error:
    return VA_STATUS_ERROR_INVALID_PARAMETER;
}

static VAStatus
intel_decoder_check_mpeg2_parameter(VADriverContextP ctx,
                                    struct decode_state *decode_state)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    VAPictureParameterBufferMPEG2 *pic_param = (VAPictureParameterBufferMPEG2 *)decode_state->pic_param->buffer;
    struct object_surface *obj_surface;	
    int i = 0;
    
    if (pic_param->picture_coding_type == MPEG_I_PICTURE) {
    } else if (pic_param->picture_coding_type == MPEG_P_PICTURE) {
        obj_surface = SURFACE(pic_param->forward_reference_picture);

        if (!obj_surface || !obj_surface->bo)
            decode_state->reference_objects[i++] = NULL;
        else
            decode_state->reference_objects[i++] = obj_surface;
    } else if (pic_param->picture_coding_type == MPEG_B_PICTURE) {
        obj_surface = SURFACE(pic_param->forward_reference_picture);

        if (!obj_surface || !obj_surface->bo)
            decode_state->reference_objects[i++] = NULL;
        else
            decode_state->reference_objects[i++] = obj_surface;

        obj_surface = SURFACE(pic_param->backward_reference_picture);

        if (!obj_surface || !obj_surface->bo)
            decode_state->reference_objects[i++] = NULL;
        else
            decode_state->reference_objects[i++] = obj_surface;
    } else
        goto error;

    for ( ; i < 16; i++)
        decode_state->reference_objects[i] = NULL;

    return VA_STATUS_SUCCESS;

error:
    return VA_STATUS_ERROR_INVALID_PARAMETER;
}

static VAStatus
intel_decoder_check_vc1_parameter(VADriverContextP ctx,
                                  struct decode_state *decode_state)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    VAPictureParameterBufferVC1 *pic_param = (VAPictureParameterBufferVC1 *)decode_state->pic_param->buffer;
    struct object_surface *obj_surface;	
    int i = 0;
    
    if (pic_param->picture_fields.bits.picture_type == 0 ||
        pic_param->picture_fields.bits.picture_type == 3) {
    } else if (pic_param->picture_fields.bits.picture_type == 1 ||
               pic_param->picture_fields.bits.picture_type == 4) {
        obj_surface = SURFACE(pic_param->forward_reference_picture);

        if (!obj_surface || !obj_surface->bo)
            decode_state->reference_objects[i++] = NULL;
        else
            decode_state->reference_objects[i++] = obj_surface;
    } else if (pic_param->picture_fields.bits.picture_type == 2) {
        obj_surface = SURFACE(pic_param->forward_reference_picture);

        if (!obj_surface || !obj_surface->bo)
            decode_state->reference_objects[i++] = NULL;
        else
            decode_state->reference_objects[i++] = obj_surface;

        obj_surface = SURFACE(pic_param->backward_reference_picture);

        if (!obj_surface || !obj_surface->bo)
            decode_state->reference_objects[i++] = NULL;
        else
            decode_state->reference_objects[i++] = obj_surface;
    } else 
        goto error;

    for ( ; i < 16; i++)
        decode_state->reference_objects[i] = NULL;

    return VA_STATUS_SUCCESS;

error:
    return VA_STATUS_ERROR_INVALID_PARAMETER;
}

static VAStatus
intel_decoder_check_vp8_parameter(VADriverContextP ctx,
                                  struct decode_state *decode_state)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    VAPictureParameterBufferVP8 *pic_param = (VAPictureParameterBufferVP8 *)decode_state->pic_param->buffer;
    struct object_surface *obj_surface;	
    int i = 0;

    if (pic_param->last_ref_frame != VA_INVALID_SURFACE) {
        obj_surface = SURFACE(pic_param->last_ref_frame);

        if (obj_surface && obj_surface->bo)
            decode_state->reference_objects[i++] = obj_surface;
        else
            decode_state->reference_objects[i++] = NULL;
    }

    if (pic_param->golden_ref_frame != VA_INVALID_SURFACE) {
        obj_surface = SURFACE(pic_param->golden_ref_frame);

        if (obj_surface && obj_surface->bo)
            decode_state->reference_objects[i++] = obj_surface;
        else
            decode_state->reference_objects[i++] = NULL;
    }

    if (pic_param->alt_ref_frame != VA_INVALID_SURFACE) {
        obj_surface = SURFACE(pic_param->alt_ref_frame);

        if (obj_surface && obj_surface->bo)
            decode_state->reference_objects[i++] = obj_surface;
        else
            decode_state->reference_objects[i++] = NULL;
    }

    for ( ; i < 16; i++)
        decode_state->reference_objects[i] = NULL;

    return VA_STATUS_SUCCESS;
}

VAStatus
intel_decoder_sanity_check_input(VADriverContextP ctx,
                                 VAProfile profile,
                                 struct decode_state *decode_state)
{
    struct i965_driver_data *i965 = i965_driver_data(ctx);
    struct object_surface *obj_surface;
    VAStatus vaStatus = VA_STATUS_ERROR_INVALID_PARAMETER;

    if (decode_state->current_render_target == VA_INVALID_SURFACE)
        goto out;
        
    obj_surface = SURFACE(decode_state->current_render_target);

    if (!obj_surface)
        goto out;

    decode_state->render_object = obj_surface;

    switch (profile) {
    case VAProfileMPEG2Simple:
    case VAProfileMPEG2Main:
        vaStatus = intel_decoder_check_mpeg2_parameter(ctx, decode_state);
        break;
        
    case VAProfileH264ConstrainedBaseline:
    case VAProfileH264Main:
    case VAProfileH264High:
        vaStatus = intel_decoder_check_avc_parameter(ctx, profile, decode_state);
        break;

    case VAProfileVC1Simple:
    case VAProfileVC1Main:
    case VAProfileVC1Advanced:
        vaStatus = intel_decoder_check_vc1_parameter(ctx, decode_state);
        break;

    case VAProfileJPEGBaseline:
        vaStatus = VA_STATUS_SUCCESS;
        break;

    case VAProfileVP8Version0_3:
        vaStatus = intel_decoder_check_vp8_parameter(ctx, decode_state);
        break;

    default:
        vaStatus = VA_STATUS_ERROR_INVALID_PARAMETER;
        break;
    }

out:
    return vaStatus;
}

/* Ensure the segmentation buffer is large enough for the supplied
   number of MBs, or re-allocate it */
bool
intel_ensure_vp8_segmentation_buffer(VADriverContextP ctx, GenBuffer *buf,
    unsigned int mb_width, unsigned int mb_height)
{
    struct i965_driver_data * const i965 = i965_driver_data(ctx);
    /* The segmentation map is a 64-byte aligned linear buffer, with
       each cache line holding only 8 bits for 4 continuous MBs */
    const unsigned int buf_size = ((mb_width + 3) / 4) * 64 * mb_height;

    if (buf->valid) {
        if (buf->bo && buf->bo->size >= buf_size)
            return true;
        drm_intel_bo_unreference(buf->bo);
        buf->valid = false;
    }

    buf->bo = drm_intel_bo_alloc(i965->intel.bufmgr, "segmentation map",
        buf_size, 0x1000);
    buf->valid = buf->bo != NULL;
    return buf->valid;
}
