#pragma once

#include <time.h>

static struct timespec begin, end;
long double average_time_sum = 0;
long int counter_avg_time = 0;
#define CHRONO_START clock_gettime(CLOCK_MONOTONIC_RAW, &begin);
#define CHRONO_TIMER_DIFFERENCE ((end.tv_sec*1.0e9+end.tv_nsec)-(begin.tv_sec*1.0e9+begin.tv_nsec))

#define CHRONO_STOP clock_gettime(CLOCK_MONOTONIC_RAW, &end); counter_avg_time++; average_time_sum += CHRONO_TIMER_DIFFERENCE;

#define CHRONO_PRINT_AVG(A) if(counter_avg_time){printf(A); printf(" : %ld ns/op   on %ld samples\n", (long int)(average_time_sum/counter_avg_time), counter_avg_time);} average_time_sum=0; counter_avg_time=0;
