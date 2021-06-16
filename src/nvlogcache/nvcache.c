#include "nvcache.h"
#include "nvcache_ram.h"
#include "nvinfo.h"
#include "nvlog.h"
#include "nvcache_musl_wrapp.h"
#include "nvcache_config.h"
#ifdef INTERNAL_PROFILE
#include "internal_profile.h"
#endif

#ifdef IOSTATS
      long long write_total_size=0;
      long long read_total_size=0;
      long number_writes=0;
      long number_reads=0;
#endif

static int init_complete = 0;
//-----------------------------------------------
void nvcache_init() {
    nvcache_config_init();
    ramcache_init();
    nvlog_init();
    init_complete = 1;
}

//-----------------------------------------------
int nvcache_initiated() { return init_complete; }

//-----------------------------------------------
__attribute__((constructor)) void constructor(void) {
//    info_logmask(~0 ^ NVTRACE);
#ifdef AUTO_INIT
    if (!init_complete) {
      nvcache_init();

    }
    
#else
    printinfo(
        NVINFO,
        RED "CONSTRUCTOR : NVcache AUTO_INIT disabled. (see nvcache.h)\n" RST);
#endif
#ifdef INTERNAL_PROFILE
    perfs_init();
#endif
}

//-----------------------------------------------
__attribute__((destructor)) void destructor(void) {
    if (init_complete) {

#ifdef IOSTATS
      printinfo(NVINFO, GRN
		"\n\t----------------------------------\n"
		"\tREADS : %ld         AVERAGE_SIZE : %f\n\n"
		"\tWRITES : %ld        AVERAGE_SIZE : %f\n\n",
		number_reads, (float)read_total_size/number_reads,
		number_writes, (float)write_total_size/number_writes);
#endif
      printinfo(NVINFO, GRN
                  "\n\t----------------------------------\n"
                  "\tFlushing RAM cache..." RST);
        printinfo(NVINFO, "\tFlushing NVRAM log..." RST);
        nvlog_final_flush();  // Flush the log in NVRAM

        ramcache_print();
    } else {
        printinfo(NVINFO, RED "DESTRUCTOR : NVcache not initialized.\n" RST);
    }
#ifdef INTERNAL_PROFILE
    perfs_printall();
#endif
}
