#include "nvcache_config.h"
#include "nvinfo.h"
#include <string.h>

#ifndef NVCACHE_STATIC_CONF

  //default config
long __ram_cache_size = 250000; // Around 1GB

//long __log_size = 200000; // Around 800MB
//long __log_size = 2000000; // Around 8GB
long __log_size = 8000000; // Around 32GB

int __enable_recover = 0;
int __flush_thread = 1;

long __max_batch_size = 1000;
long __min_batch_size = 400;





long getenv_num(const char *name){
  char *str = getenv(name);
  if(str!=NULL){
    return atol(str);
  }
  else return -1;
}


void print_config(){
  printinfo(NVINFO, RED"== Config =="RST);
  printinfo(NVINFO,"RAM CACHE SIZE = %ld", __ram_cache_size);
  printinfo(NVINFO,"-------------------");
  printinfo(NVINFO,"LOG SIZE = %ld", __log_size);
  printinfo(NVINFO,"-------------------");
  printinfo(NVINFO,"MAX BATCH SIZE = %ld", __max_batch_size);
  printinfo(NVINFO,"MIN BATCH SIZE = %ld", __min_batch_size);
  printinfo(NVINFO,"-------------------");
  printinfo(NVINFO,"ENABLE RECOVER = %d", __enable_recover);
  printinfo(NVINFO,"FLUSH THREAD = %d", __flush_thread);
  
  printinfo(NVINFO,"============");
}


void configure_param_int(int *value, const char *env_var){
  int test = getenv_num(env_var);
  if(test!=-1){
    *value = test;
  }
  // Else : default value in nvcache_config.h
}


void configure_param_long(long *value, const char *env_var){
  long test = getenv_num(env_var);
  if(test!=-1){
    *value = test;
  }
  // Else : default value in nvcache_config.h
}


void nvcache_config_init(){

  
  //Read environment vars
  configure_param_long(&__ram_cache_size, "NVCACHE_RAM_CACHE_SIZE");
  
  configure_param_long(&__log_size, "NVCACHE_LOG_SIZE");
  
  configure_param_int(&__enable_recover, "NVCACHE_ENABLE_RECOVER");
  configure_param_int(&__flush_thread, "NVCACHE_FLUSH_THREAD");

  configure_param_long(&__max_batch_size, "NVCACHE_MAX_BATCH_SIZE");
  configure_param_long(&__min_batch_size, "NVCACHE_MIN_BATCH_SIZE");


  print_config();
  
}







#else

void nvcache_config_init(){
  ;
}

#endif //NVCACHE_STATIC_CONF
