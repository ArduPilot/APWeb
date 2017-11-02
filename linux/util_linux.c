/*
  support functions when running on Linux. Many of these are stubs for now
 */
#include "../includes.h"


/*
 get upload progress as a percentage
*/
uint8_t get_upload_progress(void)
{
    return 0;
}

/*
 get upload progress message
*/
const char *get_upload_message(void)
{
    return "";
}

// get number of seconds since boot
long long get_sys_seconds_boot()
{
    return get_time_boot_ms()/1000;
}

// get number of milliseconds since boot
uint32_t get_time_boot_ms()
{
    struct timespec elapsed_from_boot;

    clock_gettime(CLOCK_BOOTTIME, &elapsed_from_boot);

    return elapsed_from_boot.tv_sec*1000 + elapsed_from_boot.tv_nsec/1000000;
}

void mdelay(uint32_t ms)
{
    uint32_t start = get_time_boot_ms();
    while (get_time_boot_ms() - start < ms) {
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 1000;
        select(0,NULL,NULL, NULL, &tv);
    }
}

bool toggle_recording(void)
{
    printf("toggle_recording not implemented\n");
    return false;
}

void __reboot(void)
{
    #if __APPLE__
        printf("reboot OSX implemented, but disabled for now.\n");
        //reboot(0);
    #elif __linux__
        system("/sbin/shutdown -t now -r");
    #else
        printf("reboot not implemented\n");
    #endif
}


/* compatibility with the sonix */
char *print_vprintf(void *ctx, const char *fmt, va_list ap)
{
    char *ret = talloc_vasprintf(ctx, fmt, ap);
    if (ret) {
        size_t size = talloc_get_size(ret);
        if (size > 0 && ret[size-1]==0) {
            ret = talloc_realloc_size(ctx, ret, size-1);
        }
    }
    return ret;
}

void *print_printf(void *ctx, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    void *ret = print_vprintf(ctx, fmt, ap);
    va_end(ap);
    return ret;
}



unsigned xPortGetFreeHeapSize()
{
    return 0;
}
