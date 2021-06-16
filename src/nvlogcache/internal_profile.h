#pragma once

#define CDF_NOT_SORTED

#ifdef INTERNAL_PROFILE
#define CHRONO_START(which) chrono_start(which)
#define CHRONO_STOP(which) chrono_stop(which)
#define CHRONO_TRANSFER(from,to) chrono_transfer(from,to)
#else
#define CHRONO_START(which)
#define CHRONO_STOP(which)
#define CHRONO_TRANSFER(from,to)
#endif

#ifdef INTERNAL_PROFILE
enum perfindex { PERF_CACHEHIT, PERF_CACHEMISS, PERF_DIRTYMISS, PERF_TOTAL };

void perfs_init();
void perfs_printall();
void perfs_print(enum perfindex e);
void chrono_start(enum perfindex e);
void chrono_stop(enum perfindex e);
void chrono_transfer(enum perfindex from, enum perfindex to);
#endif

