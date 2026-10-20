#pragma once

#include <kernel/compiler.h>

bool virtio_gpu_init(void);
bool virtio_gpu_present(void);
int virtio_gpu_set_mode(uint32_t width, uint32_t height);
uint64_t virtio_gpu_flushes(void);
