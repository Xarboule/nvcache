#include "nvcache_musl_wrapp.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "../internal/stdio_impl.h"
#include "nvcache.h"
#include "nvcache_ram.h"
#include "nvinfo.h"
#include "nvlog.h"

#ifdef IOSTATS
extern long long write_total_size;
extern long number_writes;
extern long long read_total_size;
extern long number_reads;
#define ADD_WRITE(a) ++number_writes; write_total_size+=a;
#define ADD_READ(a) ++number_reads; read_total_size+=a;
#endif

#define TRACE_READ 0x1
#define TRACE_WRITE 0x2
#define TRACE_OPEN 0x4
#define TRACE_CLOSE 0x8
#define TRACE_SEEK 0x10
#define TRACE_FSTAT 0x20

static int tracemask = 0;//TRACE_READ | TRACE_WRITE | TRACE_OPEN | TRACE_CLOSE;

//-----------------------------------------------
const char *mode_to_string(mode_t mode);
const char *flags_to_string(int flags);
const char *whence_to_string(int whence);
int is_writeonly(int fd);
int is_ramcached(int fd);
//-----------------------------------------------
//            NOT EXPORTED
//-----------------------------------------------
#ifdef NVCACHE_DEBUG
static int trace(int bit);
#else
#define trace(x) 0
#endif
static int nvcache_managed(int fd);
static int is_readonly(int fd);
static int flags_test(int flags, int mask);
static void open_common(int fd);
static void init_file_cursor(int fd);
static void lock_metadata(int fd);
static void unlock_metadata(int fd);
static off_t set_cur(int fd, off_t offset);
static off_t advance_cur(int fd, off_t offset);
static off_t advance_end(int fd, off_t offset);
//-----------------------------------------------
static int file_flags[MAX_FILES];
static off_t curr_off[MAX_FILES];
static off_t end_off[MAX_FILES];
static pthread_mutex_t meta_lock[MAX_FILES];
//-----------------------------------------------
//                    OPEN
//-----------------------------------------------
void open_common(int fd) {
    if (!is_writeonly(fd)) {
        ramcache_newradix(fd);
    } else if (trace(TRACE_OPEN)) {
        printinfo(NVTRACE,
                  "Write only fd = %d. Not RAM cached, but logged in NVRAM.",
                  fd);
    }
    init_file_cursor(fd);
}

//-----------------------------------------------
int is_system_fd(int fd){
  return((fd==fileno(stdout))||(fd==fileno(stderr))||(fd==fileno(stdout)));
}

//-----------------------------------------------
int no_weird_flag(int flags){
  if ((flags & O_DIRECTORY)||(flags & O_CLOEXEC)){
    return 0;
  }
  return 1;
}


//-----------------------------------------------
int nvcache_open(const char *filename, int flags, mode_t mode) {
#ifdef DIRECT_IO  // Has to be enabled to guarantee persistence NVRAM => DISK
    flags |= O_DSYNC;
#endif  // DIRECT_IO

    if (no_weird_flag(flags)) {
      flags&=~O_DIRECT;
      flags&=~O_DSYNC;
      flags&=~O_SYNC;
    }
    
    int fd = __sys_open_cp(filename, flags, mode);


    
    if (trace(TRACE_OPEN)) {
        printinfo(NVTRACE, "nvcache_open(\"%s\", %X %s, %X %s): %d", filename,
                  flags, flags_to_string(flags), mode, mode_to_string(mode),
                  fd);
    }
    file_flags[fd] = flags;
    pthread_mutex_init(&meta_lock[fd], NULL);
    
#ifdef USE_LINUXCACHE
    printinfo(NVTRACE, "USE_LINUXCACHE fd %d not in NVcache", fd);
#else
    if (!is_system_fd(fd) && nvcache_initiated() && !is_readonly(fd)) {
        open_common(fd);
	nvlog_set_file_table(fd, filename, (flags&~O_CREAT), mode);
    } else if (trace(TRACE_OPEN)) {
        printinfo(NVTRACE, "Not cached: fd = %d", fd);
    }
#endif

    
    if (fd >= 0 && (flags & O_CLOEXEC)) {
        flags |= O_DSYNC;
        __syscall(SYS_fcntl, fd, F_SETFD, FD_CLOEXEC);
    }

    
    return __syscall_ret(fd);
}

//-----------------------------------------------
FILE *nvcache_fopen(const char *restrict filename, FILE *f, int flags) {
    file_flags[f->fd] = flags;
    if (!nvcache_initiated() || is_readonly(f->fd)) {
        return f;
    }
    if (trace(TRACE_OPEN)) {
        printinfo(NVTRACE, "nvcache_fopen(\"%s\", 0x%X [fd %d], %s)", filename,
                  f, f->fd, flags_to_string(flags));
    }
#ifndef USE_LINUXCACHE
    open_common(f->fd);
#endif
    return f;
}

//-----------------------------------------------
//                    CLOSE
//-----------------------------------------------
int nvcache_close(int fd) {
    if (!nvcache_managed(fd)) {
        return musl_close(fd);
    }
    if (trace(TRACE_CLOSE)) {
        printinfo(NVTRACE, "nvcache_close(%d)", fd);
    }
    nvlog_flush_file(fd);
    if (is_ramcached(fd)) {
        ramcache_file_clean(fd);
    }
    lock_metadata(fd);
    file_flags[fd] = -1;
    curr_off[fd] = end_off[fd] = 0;

    nvlog_reset_file_table(fd);
    unlock_metadata(fd);
    
    
    return musl_close(fd);
}

//-----------------------------------------------
//                     READ
//-----------------------------------------------
ssize_t nvcache_read(int fd, void *buf, size_t count) {
    if (buf == NULL || count == 0 || is_writeonly(fd)) {
        return 0;
    }

    ssize_t ret = 0;
#ifdef USE_LINUXCACHE
    ret = musl_read(fd, buf, count);
#else
    if (!is_ramcached(fd)) {
        return musl_read(fd, buf, count);
    }

    ret = nvcache_pread(fd, buf, count, nvcache_lseek(fd, 0, SEEK_CUR));
#endif
    if (trace(TRACE_READ)) {
        printinfo(NVTRACE, "nvcache_read(%d, 0x%X, %u) : %lu", fd, buf, count,
                  ret);
    }
#ifndef USE_LINUXCACHE
    if (ret > 0) {
        nvcache_lseek(fd, ret, SEEK_CUR);
    }
#endif
    return ret;
}

//-----------------------------------------------
ssize_t nvcache_pread(int fd, void *buf, size_t size, off_t offset) {
    if (buf == NULL || size == 0 || is_writeonly(fd)) {
        return 0;
    }

#ifdef IOSTATS
    ADD_READ(size);
#endif
    
    ssize_t ret;
#ifdef USE_LINUXCACHE
    ret = musl_pread(fd, buf, size, offset);
#else
    if (!is_ramcached(fd)) {
        return musl_pread(fd, buf, size, offset);
    }
    ret = ramcache_pread(fd, offset, buf, size);
#endif

    if (trace(TRACE_READ)) {
        printinfo(NVTRACE, "nvcache_pread(%d, 0x%X, %u, %u) : %lu", fd, buf,
                  size, offset, ret);
    }
    return ret;
}

//-----------------------------------------------
size_t nvcache_fread(FILE *f, unsigned char *buf, size_t len) {
    size_t ret = 0;
#ifdef USE_LINUXCACHE
    ret = __musl_stdio_read(f, buf, len);
#else
    if (!is_ramcached(f->fd)) {
        return __musl_stdio_read(f, buf, len);
    }

    ret = nvcache_read(f->fd, buf, len);
#endif

    if (trace(TRACE_READ)) {
        printinfo(NVTRACE, "nvcache_fread(0x%X [fd %d], 0x%X, %u) : %lu", f,
                  f->fd, buf, len, ret);
    }
    return ret;
}

//-----------------------------------------------
//                   WRITE
//-----------------------------------------------
ssize_t nvcache_write(int fd, const void *buf, size_t count) {
    if (count == 0 || buf == NULL || is_readonly(fd)) {
        return 0;
    }
#ifdef USE_LINUXCACHE
    nvlog_add_entry(fd, musl_lseek(fd, 0, SEEK_CUR), buf, count);
    return musl_write(fd, buf, count);  // advances file cursor
#else
    if (!nvcache_managed(fd)) {
        return musl_write(fd, buf, count);
    }
    off_t offset = nvcache_lseek(fd, 0, SEEK_CUR);
    ssize_t ret = nvcache_pwrite(fd, buf, count, offset);
    if (trace(TRACE_WRITE)) {
        printinfo(NVTRACE, "nvcache_write(%d, 0x%X, %u) : %lu", fd, buf, count,
                  ret);
    }

    if (ret > 0) {
        off_t end = advance_end(fd, 0);

        if (offset + ret > end) {
            nvcache_lseek(fd, offset + ret - end, SEEK_END);
        } else {
            nvcache_lseek(fd, ret, SEEK_CUR);
        }
    }
    return ret;
#endif
}

//-----------------------------------------------
ssize_t nvcache_pwrite(int fd, const void *buf, size_t size, off_t offset) {
    if (size == 0 || buf == NULL || is_readonly(fd)) {
        return 0;
    }

#ifdef IOSTATS
    ADD_WRITE(size);
#endif
    
    ssize_t ret = 0;
#ifndef USE_LINUXCACHE
    if (!nvcache_managed(fd)) {
        // Write to Linux cache, which may eventually flush it to disk
        return musl_pwrite(fd, buf, size, offset);
    }

    
#endif

    // Update the RAM cache
    if (!is_writeonly(fd)) {
        ret = ramcache_pwrite(fd, offset, buf, size);
    }
    
    nvlog_add_entry(fd, offset, buf, size);

#ifdef USE_LINUXCACHE
    ret = musl_pwrite(fd, buf, size, offset);
#endif

    if (trace(TRACE_WRITE)) {
        printinfo(NVTRACE,
                  "nvcache_pwrite(%d, 0x%X, %u, %u) : %lu RAM %lu NVRAM", fd,
                  buf, size, offset, ret, size);
    }

    // size is returned because it corresponds to the amount of data
    // written to nvlog, despite possibly not being written in the RAM cache
    return size;
}

//-----------------------------------------------
size_t nvcache_fwrite(FILE *f, const unsigned char *buf, size_t len) {
    if (!nvcache_managed(f->fd)) {
        return __fwritex(buf, len, f);
    }

    size_t ret = nvcache_write(f->fd, buf, len);
    if (trace(TRACE_WRITE)) {
        printinfo(NVTRACE, "nvcache_fwrite(0x%X [fd %d], 0x%X, %u) : %u", f,
                  f->fd, buf, len, ret);
    }
    return ret;
}

//-----------------------------------------------
size_t nvcache_stdio_write(FILE *f, const unsigned char *buf, size_t len) {
    // Too many prints breaks db_bench
    if (!nvcache_managed(f->fd)) {
        return __musl_stdio_write(f, buf, len);
    }

    size_t ret = nvcache_write(f->fd, buf, len);
    if (trace(TRACE_WRITE)) {
        printinfo(NVTRACE, "nvcache_stdio_write(0x%X [fd %d], 0x%X, %u) : %u",
                  f, f->fd, buf, len, ret);
    }
    return ret;
}

//-----------------------------------------------
//                 SEEK
//-----------------------------------------------
off_t nvcache_lseek(int fd, off_t offset, int whence) {
#ifdef USE_LINUXCACHE
    return musl_lseek(fd, offset, whence);
#else
    off_t ret = -1;

    if (!nvcache_managed(fd)) {
        return musl_lseek(fd, offset, whence);
    }

    if (whence == SEEK_CUR) {
        ret = advance_cur(fd, offset);
    } else if (whence == SEEK_END) {
        ret = set_cur(fd, advance_end(fd, offset));
    } else if (whence == SEEK_SET) {
        ret = set_cur(fd, offset);
    }
    if (trace(TRACE_SEEK)) {
        printinfo(NVTRACE, "nvcache_lseek(%d, %u, %s) : %d", fd, offset,
                  whence_to_string(whence), ret);
    }
    return ret;
#endif
}

//-----------------------------------------------
//        FILE CURSOR MANIPULATION
//-----------------------------------------------

void lock_metadata(int fd){
  pthread_mutex_lock(&meta_lock[fd]);
}

//-----------------------------------------------
void unlock_metadata(int fd){
  pthread_mutex_unlock(&meta_lock[fd]);
}

//-----------------------------------------------
void init_file_cursor(int fd) {
    off_t cur = musl_lseek(fd, 0, SEEK_CUR);
    end_off[fd] = musl_lseek(fd, 0, SEEK_END);
    curr_off[fd] = musl_lseek(fd, cur, SEEK_SET);
}

//-----------------------------------------------
off_t set_cur(int fd, off_t offset) {
  lock_metadata(fd);
  (curr_off[fd] = offset);
  off_t ret = curr_off[fd];
  unlock_metadata(fd);
  return ret;
}

//-----------------------------------------------
off_t advance_cur(int fd, off_t offset) {
  lock_metadata(fd);
  (curr_off[fd] += offset);
  off_t ret = curr_off[fd];
  unlock_metadata(fd);
  return ret;
}

//-----------------------------------------------
off_t advance_end(int fd, off_t offset) {
  lock_metadata(fd);
  (end_off[fd] += offset);
  off_t ret = end_off[fd];
  unlock_metadata(fd);
  return ret;
}

//-----------------------------------------------
//               STAT
//-----------------------------------------------
int nvcache_stat(const char *pathname, struct stat *statbuf) {
    int ret = musl_stat(pathname, statbuf);
    int fd = nvlog_find_fd_by_path(pathname);
    if (fd != -1){
      off_t before = statbuf->st_size;
      // (fake) size
      statbuf->st_size = advance_end(fd, 0);
      // (fake) number of 512B blocks allocated
      statbuf->st_blocks = (statbuf->st_blksize + statbuf->st_size)/512;
      if (trace(TRACE_FSTAT)) {
	printinfo(NVTRACE, "nvcache_stat(%d,%X) size %d -> %d", fd, statbuf, before,
		  statbuf->st_size);
      }
    }
    return ret;
}



//-----------------------------------------------
//              FSTAT
//-----------------------------------------------
int nvcache_fstat(int fd, struct stat *st) {
    int ret = musl_fstat(fd, st);
    off_t before = st->st_size;
    // (fake) size
    st->st_size = advance_end(fd, 0);
    // (fake) number of 512B blocks allocated
    st->st_blocks = (st->st_blksize + st->st_size)/512;
    if (trace(TRACE_FSTAT)) {
        printinfo(NVTRACE, "nvcache_fstat(%d,%X) size %d -> %d", fd, st, before,
                  st->st_size);
    }
    return ret;
}

//-----------------------------------------------
//              FSYNC
//-----------------------------------------------
int nvcache_fsync(int fd) {
#ifndef USE_LINUXCACHE
    if (!nvcache_managed(fd)) {
        return musl_fsync(fd);
    }
    return 0;
#else
    return musl_fsync(fd);
#endif
}


//-----------------------------------------------
//             FLOCK
//-----------------------------------------------

int nvcache_flock(int fd, int op){

    if (!nvcache_managed(fd)) {
      return musl_flock(fd, op);
    }
    
    nvlog_flush_file(fd);
    
    return musl_flock(fd, op);
  
}


//-----------------------------------------------
//             AUXILIARY
//-----------------------------------------------
int flags_test(int flags, int mask) {
    int twobits = flags & 03;
    return flags >= 0 && twobits == mask;
}

//-----------------------------------------------
int is_writeonly(int fd) { return flags_test(file_flags[fd], O_WRONLY); }

//-----------------------------------------------
int is_readonly(int fd) { return flags_test(file_flags[fd], O_RDONLY); }

//-----------------------------------------------
int is_ramcached(int fd) { return fd >= 3 && ramcache_exists(fd); }

//-----------------------------------------------
#ifdef NVCACHE_DEBUG
int trace(int bit) { return tracemask & bit; }
#endif

//-----------------------------------------------
int nvcache_managed(int fd) {
  if (is_system_fd(fd)) return 0;
    int ret = is_writeonly(fd);
#ifdef USE_LINUXCACHE
    ret = 1;
#else
    ret = ret || is_ramcached(fd);
#endif
    return ret;
}

//-----------------------------------------------
//         PRINTING FLAGS
//-----------------------------------------------
const char *flags_to_string(int flags) {
    static char flagstr[1024];
    flagstr[0] = 0;
    size_t free;
    do {
        free = sizeof(flagstr) - strlen(flagstr);
        if (O_RDONLY == (flags | O_RDONLY)) {
            strncat(flagstr, "RDONLY", free);
            flags &= O_RDONLY;
        } else if (flags & O_WRONLY) {
            strncat(flagstr, "WRONLY", free);
            flags &= ~O_WRONLY;
        } else if (flags & O_RDWR) {
            strncat(flagstr, "RDWR", free);
            flags &= ~O_RDWR;
        } else if (flags & O_APPEND) {
            strncat(flagstr, "APPEND", free);
            flags &= ~O_APPEND;
        } else if (flags & O_ASYNC) {
            strncat(flagstr, "ASYNC", free);
            flags &= ~O_ASYNC;
        } else if (flags & O_CLOEXEC) {
            strncat(flagstr, "CLOEXEC", free);
            flags &= ~O_CLOEXEC;
        } else if (flags & O_CREAT) {
            strncat(flagstr, "CREAT", free);
            flags &= ~O_CREAT;
        } else if (flags & O_DIRECT) {
            strncat(flagstr, "DIRECT", free);
            flags &= ~O_DIRECT;
        } else if (flags & O_DIRECTORY) {
            strncat(flagstr, "DIRECTORY", free);
            flags &= ~O_DIRECTORY;
        } else if (flags & O_DSYNC) {
            strncat(flagstr, "DSYNC", free);
            flags &= ~O_DSYNC;
        } else if (flags & O_EXCL) {
            strncat(flagstr, "EXCL", free);
            flags &= ~O_EXCL;
        } else if (flags & O_LARGEFILE) {
            strncat(flagstr, "LARGEFILE", free);
            flags &= ~O_LARGEFILE;
        } else if (flags & O_NOATIME) {
            strncat(flagstr, "NOATIME", free);
            flags &= ~O_NOATIME;
        } else if (flags & O_NOCTTY) {
            strncat(flagstr, "NOCTTY", free);
            flags &= ~O_NOCTTY;
        } else if (flags & O_NOFOLLOW) {
            strncat(flagstr, "NOFOLLOW", free);
            flags &= ~O_NOFOLLOW;
        } else if (flags & O_NONBLOCK) {
            strncat(flagstr, "NONBLOCK", free);
            flags &= ~O_NONBLOCK;
        } else if (flags & O_PATH) {
            strncat(flagstr, "PATH", free);
            flags &= ~O_PATH;
        } else if (O_SYNC == (flags & O_SYNC)) {
            strncat(flagstr, "SYNC", free);
            flags &= ~O_SYNC;
        } else if (O_TMPFILE == (flags & O_TMPFILE)) {
            strncat(flagstr, "TMPFILE", free);
            flags &= ~O_TMPFILE;
        } else if (flags & O_TRUNC) {
            strncat(flagstr, "TRUNC", free);
            flags &= ~O_TRUNC;
        } else {
            char left[10];
            snprintf(left, sizeof(left), "%X", flags);
            strncat(flagstr, left, free);
            flags = 0;
        }
        if (flags) {
            strncat(flagstr, "|", sizeof(flagstr) - strlen(flagstr));
        }
    } while (flags && free > 1);
    return flagstr;
}
//-----------------------------------------------
const char *mode_to_string(mode_t mode) {
    static char modestr[1024];
    modestr[0] = 0;
    size_t free;
    do {
        free = sizeof(modestr) - strlen(modestr);
        if (S_IRWXU == (mode & S_IRWXU)) {
            strncat(modestr, "IRWXU", free);
            mode &= ~S_IRWXU;
        } else if (S_IRWXG == (mode & S_IRWXG)) {
            strncat(modestr, "IRWXG", free);
            mode &= ~S_IRWXG;
        } else if (S_IRWXO == (mode & S_IRWXO)) {
            strncat(modestr, "IRWXO", free);
            mode &= ~S_IRWXO;
        } else if (mode & S_IRUSR) {
            strncat(modestr, "IRUSR", free);
            mode &= ~S_IRUSR;
        } else if (mode & S_IWUSR) {
            strncat(modestr, "IWUSR", free);
            mode &= ~S_IWUSR;
        } else if (mode & S_IXUSR) {
            strncat(modestr, "IXUSR", free);
            mode &= ~S_IXUSR;
        } else if (mode & S_IRGRP) {
            strncat(modestr, "IRGRP", free);
            mode &= ~S_IRGRP;
        } else if (mode & S_IWGRP) {
            strncat(modestr, "IWGRP", free);
            mode &= ~S_IWGRP;
        } else if (mode & S_IXGRP) {
            strncat(modestr, "IXGRP", free);
            mode &= ~S_IXGRP;
        } else if (mode & S_IROTH) {
            strncat(modestr, "IROTH", free);
            mode &= ~S_IROTH;
        } else if (mode & S_IWOTH) {
            strncat(modestr, "IWOTH", free);
            mode &= ~S_IWOTH;
        } else if (mode & S_IXOTH) {
            strncat(modestr, "IXOTH", free);
            mode &= ~S_IXOTH;
        } else if (mode & S_ISUID) {
            strncat(modestr, "ISUID", free);
            mode &= ~S_ISUID;
        } else if (mode & S_ISGID) {
            strncat(modestr, "ISGID", free);
            mode &= ~S_ISGID;
        } else if (mode & S_ISVTX) {
            strncat(modestr, "ISVTX", free);
            mode &= ~S_ISVTX;
        } else {
            char left[10];
            snprintf(left, sizeof(left), "%X", mode);
            strncat(modestr, left, free);
            mode = 0;
        }
        if (mode) {
            strncat(modestr, "|", sizeof(modestr) - strlen(modestr));
        }
    } while (mode && free > 1);
    return modestr;
}

//-----------------------------------------------
const char *whence_to_string(int whence) {
    if (whence == SEEK_SET) return "SET";
    if (whence == SEEK_CUR) return "CUR";
    if (whence == SEEK_END) return "END";
    return "unknown";
}
