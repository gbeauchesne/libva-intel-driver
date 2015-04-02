#ifndef _INTEL_MEMMAN_H_
#define _INTEL_MEMMAN_H_

Bool intel_memman_init(struct intel_driver_data *intel);
Bool intel_memman_terminate(struct intel_driver_data *intel);

bool
intel_memman_has_userptr(struct intel_driver_data *intel);

drm_intel_bo *
intel_memman_import_userptr(struct intel_driver_data *intel, const char *name,
    void *data, size_t data_size, uint32_t va_flags);

#endif /* _INTEL_MEMMAN_H_ */
