#pragma once

#include <kernel/compiler.h>

void smp_init(void);
uint32_t smp_cpu_count(void);
bool smp_active(void);

void smp_flush_tlb_others(void);
void smp_tlb_shootdown_sync(void);
void smp_halt_others(void);
void smp_report_cpu(uint32_t cpu, const char *why);
void smp_watchdog_tick(void);
void smp_dump(void);
