#pragma once

#include <kernel/compiler.h>

#define HDA_RATE_44100 (1u << 0)
#define HDA_RATE_48000 (1u << 1)

bool hda_init(void);
bool hda_present(void);
uint32_t hda_rates(void);
int hda_start(uint32_t rate, uint32_t channels);
int64_t hda_write(const void *buf, uint64_t len, bool nonblock);
uint64_t hda_space(void);
void hda_stop(bool drain);
size_t hda_report(char *buf, size_t cap);
int hda_tone(uint32_t ms, uint32_t hz);
