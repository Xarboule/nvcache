#pragma once

#define AUTO_INIT //Enables init by the libc constructor / destructor

#ifdef __cplusplus
extern "C" {
#endif

int nvcache_initiated();

#ifdef __cplusplus
}
#endif

