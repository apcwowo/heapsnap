#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <dlfcn.h>
#include <inttypes.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/cdefs.h>
#include <android/log.h>
#include <malloc.h>
#include <malloc_debug/backtrace.h>

#if (PLATFORM_SDK_VERSION>=29)
//#include <bionic_malloc.h>
#endif

#define TAG                         "heapsnap"
#define VERION                      "v1.0"
#define DEFAULT_HEAPSNAP_SIG        SIGTTIN
#define HEPA_SNAP_PATH              "/sdcard/heap_snap"

#define HEAPSNAP_DEBUG

#define info_log(...)                                             \
    do {                                                          \
        __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__ ); \
    } while (0)
#define err_log(...)                                              \
    do {                                                          \
        __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__ ); \
    } while (0)

#ifdef HEAPSNAP_DEBUG
#define dbg_log(...)                                              \
    do {                                                          \
        __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__ ); \
    } while (0)
#else
#define dbg_log(...)
#endif

#define SIZE_FLAG_ZYGOTE_CHILD  (1<<31)

typedef struct _hs_malloc_leak_info_t {
    // Pointer to the buffer allocated by a call
    uint8_t* buffer;
    // The size of the "info" buffer.
    size_t overall_size;
    // The size of a single entry.
    size_t info_size;
    // The sum of all allocations that have been tracked. Does not include
    // any heap overhead.
    size_t total_memory;
    // The maximum number of backtrace entries.
    size_t backtrace_size;
} hs_malloc_leak_info_t;

static pid_t myPid = 0;
static size_t gNumBacktraceElements;

#if (PLATFORM_SDK_VERSION<29)
extern "C" void get_malloc_leak_info(uint8_t** info, size_t* overallSize,
        size_t* infoSize, size_t* totalMemory, size_t* backtraceSize);
extern "C" void free_malloc_leak_info(uint8_t* info);
#endif
std::string backtrace_string(const uintptr_t* frames, size_t frame_count);

static FILE* heapsnap_getfile()
{
    size_t FILENAME_SIZE = 1024;
    char fileName[FILENAME_SIZE];
    char filename_heap[FILENAME_SIZE];
    FILE* fp_heap = NULL;

    int fno = 0;
    if (access(HEPA_SNAP_PATH, 0) == -1) {
        info_log("create directory: %s\n", HEPA_SNAP_PATH);
        if (mkdir(HEPA_SNAP_PATH, 0775) < 0) {
            err_log("create directory failed: %s!\n", HEPA_SNAP_PATH);
            return NULL;
        }
    }
    while(1) {
        snprintf(fileName, FILENAME_SIZE-1, "%s/heap_%d_%04d", HEPA_SNAP_PATH, myPid, fno);
        snprintf(filename_heap, FILENAME_SIZE-1, "%s.txt", fileName);
        if (access(filename_heap, 0) < 0)
            break;
        ++fno;
    }
    fp_heap = fopen(filename_heap, "w+");
    if(fp_heap == NULL)
    {
        err_log("Fail to open file: %s!\n", filename_heap);
        return NULL;
    }
    info_log("Save process's heap to file: %s\n", filename_heap);

    return fp_heap;
}

static bool get_malloc_info(hs_malloc_leak_info_t* leak_info)
{
#if (PLATFORM_SDK_VERSION<29)
    get_malloc_leak_info(&leak_info->buffer, &leak_info->overall_size, &leak_info->info_size,
            &leak_info->total_memory, &leak_info->backtrace_size);
#else
    if (!android_mallopt(M_GET_MALLOC_LEAK_INFO, leak_info, sizeof(*leak_info))) {
      return false;
    }
#endif

    if (leak_info->buffer == nullptr || leak_info->overall_size == 0 || leak_info->info_size == 0
            || (leak_info->overall_size / leak_info->info_size) == 0) {
        return false;
    }

    return true;
}

static void free_malloc_info(hs_malloc_leak_info_t* info)
{
#if (PLATFORM_SDK_VERSION<29)
    free_malloc_leak_info(info->buffer);
#else
    android_mallopt(M_FREE_MALLOC_LEAK_INFO, info, sizeof(*info));
#endif
}

static int compareHeapRecords(const void* vrec1, const void* vrec2)
{
    const size_t* rec1 = (const size_t*) vrec1;
    const size_t* rec2 = (const size_t*) vrec2;
    size_t size1 = *rec1;
    size_t size2 = *rec2;

    if (size1 < size2) {
        return 1;
    } else if (size1 > size2) {
        return -1;
    }

    uintptr_t* bt1 = (uintptr_t*)(rec1 + 2);
    uintptr_t* bt2 = (uintptr_t*)(rec2 + 2);
    for (size_t idx = 0; idx < gNumBacktraceElements; idx++) {
        uintptr_t addr1 = bt1[idx];
        uintptr_t addr2 = bt2[idx];
        if (addr1 == addr2) {
            if (addr1 == 0)
                break;
            continue;
        }
        if (addr1 < addr2) {
            return -1;
        } else if (addr1 > addr2) {
            return 1;
        }
    }

    return 0;
}

static void merge_similar_entries(hs_malloc_leak_info_t *leak_info)
{
    size_t recordCount = leak_info->overall_size/leak_info->info_size;
    if (recordCount < 2) {
        return;
    }

    struct info_t {
        size_t size;
        size_t allocations;
        uintptr_t* backtrace;
    };

    const uint8_t* ptr_dst = leak_info->buffer;
    const uint8_t* ptr = ptr_dst+leak_info->info_size;
    struct info_t* info_dst = (struct info_t*)ptr_dst;
    info_dst->allocations = 1;
    size_t count = 1;

    for (size_t idx = 1; idx < recordCount; idx++) {
        struct info_t* info = (struct info_t*)ptr;
        if (!memcmp(info->backtrace, info_dst->backtrace, sizeof(uintptr_t)*leak_info->backtrace_size) &&
                info->size == info_dst->size) {
            info_dst->allocations++;
        } else {
            ptr_dst += leak_info->info_size;
            memcpy((void*)ptr_dst, (void*)ptr, leak_info->info_size);
            info_dst = (struct info_t*)ptr_dst;
            info_dst->allocations = 1;
            ++count;
        }
        ptr += leak_info->info_size;
    }
    leak_info->overall_size = count * leak_info->info_size;
}

static void demangle_and_save(hs_malloc_leak_info_t *leak_info, FILE* fp)
{
    size_t recordCount = leak_info->overall_size/leak_info->info_size;

    /* re-sort the entries */
    gNumBacktraceElements = leak_info->backtrace_size;
    qsort(leak_info->buffer, recordCount, leak_info->info_size, compareHeapRecords);

#if (PLATFORM_SDK_VERSION>=24 || PLATFORM_SDK_VERSION<=27)
    merge_similar_entries(leak_info);
    info_log("merge similar entries: %zu -> %zu\n", recordCount,
            leak_info->overall_size/leak_info->info_size);
    recordCount = leak_info->overall_size/leak_info->info_size;
#endif

    fprintf(fp, "Heap Snapshot %s\n\n", VERION);
    fprintf(fp, "Total memory: %zu\n", leak_info->total_memory);
    fprintf(fp, "Allocation records: %zd\n", recordCount);
    fprintf(fp, "Backtrace size: %zd\n", leak_info->backtrace_size);
    fprintf(fp, "\n");

    /* dump the entries to the file */
    const uint8_t* ptr = leak_info->buffer;
    for (size_t idx = 0; idx < recordCount; idx++) {
        size_t size = *(size_t*) ptr;
        size_t allocations = *(size_t*) (ptr + sizeof(size_t));
        uintptr_t* backtrace = (uintptr_t*) (ptr + sizeof(size_t) * 2);

        fprintf(fp, "z %d  sz %8zu  num %4zu  bt",
                (size & SIZE_FLAG_ZYGOTE_CHILD) != 0,
                size & ~SIZE_FLAG_ZYGOTE_CHILD,
                allocations);
        size_t bt = 0;
        for (bt = 0; bt < leak_info->backtrace_size; bt++) {
            if (backtrace[bt] == 0) {
                break;
            } else {
#ifdef __LP64__
                fprintf(fp, " %016" PRIxPTR, backtrace[bt]);
#else
                fprintf(fp, " %08" PRIxPTR, backtrace[bt]);
#endif
            }
        }
        fprintf(fp, "\n");

        std::string str = backtrace_string(backtrace, bt);
        fprintf(fp, "%s\n", str.c_str());

        ptr += leak_info->info_size;
    }

    fprintf(fp, "MAPS\n");
    const char* maps = "/proc/self/maps";
    FILE* fp_in = fopen(maps, "r");
    if (fp_in == NULL) {
        fprintf(fp, "Could not open %s\n", maps);
        return;
    }
    char buf[BUFSIZ];
    while (size_t n = fread(buf, sizeof(char), BUFSIZ, fp_in)) {
        fwrite(buf, sizeof(char), n, fp);
    }

    fprintf(fp, "END\n");
    fclose(fp_in);
}

extern "C" void heapsnap_save(void)
{
    hs_malloc_leak_info_t leak_info;
    FILE *fp = heapsnap_getfile();

    if (fp == NULL)
        return;

    if (!get_malloc_info(&leak_info)) {
        fprintf(fp, "Native heap dump not available. To enable, run these"
                    " commands (requires root):\n");
        fprintf(fp, "# adb shell stop\n");
#if (PLATFORM_SDK_VERSION<24)
        fprintf(fp, "# adb shell setprop libc.debug.malloc 1\n");
#else
        fprintf(fp, "# adb shell setprop libc.debug.malloc.options backtrace\n");
#endif
        fprintf(fp, "# adb shell start\n");
        fclose(fp);
        return;
    }

    demangle_and_save(&leak_info, fp);
    free_malloc_info(&leak_info);
    fclose(fp);

    info_log("PID(%d): Heap Save Done.\n", myPid);
}

static void heapsnap_signal_handler(int sig)
{
    dbg_log("PID(%d): catch SIG: %d\n", myPid, sig);
    switch (sig) {
    case DEFAULT_HEAPSNAP_SIG: {
        heapsnap_save();
        break;
    }
    default:
        break;
    }
}

extern "C" void heapsnap_init() {
    myPid = getpid();
    dbg_log("PID(%d): register snapshot SIG: %d\n", myPid, DEFAULT_HEAPSNAP_SIG);
    signal(DEFAULT_HEAPSNAP_SIG, &heapsnap_signal_handler);
    info_log("PID(%d): Heap Snap enabled\n", myPid);
}

extern "C" void heapsnap_deinit() {
    dbg_log("PID(%d): leaving heapsnap\n", myPid);
}

extern "C" void __attribute__((constructor)) prepare()
{
    dbg_log("prepare heapsnap\n");
    heapsnap_init();
}