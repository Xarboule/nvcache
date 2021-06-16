#pragma once

#include <stdint.h>

//#define NVCACHE_DEBUG
#define NVNONE 0
#define NVCRIT 1
#define NVWARN 2
#define NVINFO 4
#define NVLOG 8
#define NVDEBG 16
#define NVTRACE 32

#define BLD "\x1B[1m"
#define RED "\x1B[31m"
#define GRN "\x1B[32m"
#define YEL "\x1B[33m"
#define BLU "\x1B[34m"
#define MAG "\x1B[35m"
#define CYN "\x1B[36m"
#define WHT "\x1B[37m"
#define RST "\x1B[0m"

#define IOSTATS

#ifdef __cplusplus
extern "C" {
#endif

void info_logmask(uint8_t l);
void printinfo(uint8_t level, const char *format, ...);

#ifdef __cplusplus
}
#endif
