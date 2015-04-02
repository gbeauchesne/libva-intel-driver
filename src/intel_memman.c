/*
 * Copyright © 2009 Intel Corporation
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
 *
 * Authors:
 *    Xiang Haihao <haihao.xiang@intel.com>
 *    Zou Nan hai <nanhai.zou@intel.com>
 *
 */

#include "config.h"
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include "intel_driver.h"
#include "intel_memman.h"

Bool 
intel_memman_init(struct intel_driver_data *intel)
{
    intel->bufmgr = intel_bufmgr_gem_init(intel->fd, BATCH_SIZE);
    assert(intel->bufmgr);
    intel_bufmgr_gem_enable_reuse(intel->bufmgr);

    if (g_intel_debug_option_flags & VA_INTEL_DEBUG_OPTION_DUMP_AUB) {
	drm_intel_bufmgr_gem_set_aub_filename(intel->bufmgr,
					      "va.aub");
	drm_intel_bufmgr_gem_set_aub_dump(intel->bufmgr, 1);
    }

    /* Only check for userptr when needed, through intel_memman_has_userptr() */
    intel->userptr_disabled = 2;
    return True;
}

Bool 
intel_memman_terminate(struct intel_driver_data *intel)
{
    drm_intel_bufmgr_destroy(intel->bufmgr);
    return True;
}

drm_intel_bo *
do_import_userptr(struct intel_driver_data *intel, const char *name,
    void *data, size_t data_size, uint32_t va_flags)
{
#ifdef HAVE_DRM_INTEL_USERPTR
    uint32_t page_size, tiling_mode, flags = 0;
    drm_intel_bo *bo;

    /* XXX: userptr is only supported for page-aligned allocations */
    page_size = getpagesize();
    if ((uintptr_t)data & (page_size - 1))
        return NULL;

    tiling_mode = (va_flags & VA_SURFACE_EXTBUF_DESC_ENABLE_TILING) ?
        I915_TILING_Y : I915_TILING_NONE;

    bo = drm_intel_bo_alloc_userptr(intel->bufmgr, name, data, tiling_mode, 0,
        data_size, flags);
    if (bo)
        return bo;
#endif
    return NULL;
}

drm_intel_bo *
intel_memman_import_userptr(struct intel_driver_data *intel, const char *name,
    void *data, size_t data_size, uint32_t va_flags)
{
    if (!intel_memman_has_userptr(intel))
        return NULL;
    return do_import_userptr(intel, name, data, data_size, va_flags);
}

bool
intel_memman_has_userptr(struct intel_driver_data *intel)
{
    drm_intel_bo *bo;
    size_t page_size;
    void *page;

    if (intel->userptr_disabled > 1) {
        intel->userptr_disabled = 1;

        page_size = getpagesize();
        if (posix_memalign(&page, page_size, page_size) == 0) {
            bo = do_import_userptr(intel, "userptr test buffer",
                page, page_size, 0);
            if (bo) {
                drm_intel_bo_unreference(bo);
                intel->userptr_disabled  = 0;
            }
            free(page);
        }
    }
    return !intel->userptr_disabled;
}
