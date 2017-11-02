#include "config_variables.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

char *config;
off_t config_size;
uint32_t config_file_modtime = 0;
const char *config_filename = "config.txt";

bool lock_initted;
static pthread_mutex_t lock;

bool lock_config()
{
    if (!lock_initted) {
        pthread_mutex_init(&lock, NULL);
        lock_initted = 1;
    }
    return pthread_mutex_lock(&lock);
}

bool unlock_config()
{
    return pthread_mutex_unlock(&lock);
}

bool read_config_file()
{
    int fd = open(config_filename, O_RDONLY);
    if (fd == -1) {
        goto OUT_BAD;
    }
    struct stat _stat;
    if (fstat(fd, &_stat) == -1) {
        goto OUT_BAD_FD;
    }
    const uint32_t modtime = _stat.st_atime;
    if (modtime == config_file_modtime) {
        close(fd);
        return true;
    }
    config_size = _stat.st_size;
    char *new_config = realloc(config, config_size+1);
    if (new_config == NULL) {
        goto OUT_BAD_FD;
    }
    config = new_config;
    memset(config, '\0', config_size+1);
    off_t total_read = 0;
    uint8_t max_loop_count = 10;
    while (total_read != config_size && max_loop_count--) {
        const off_t this_read = read(fd, &config[total_read], config_size-total_read);
        if (this_read == -1) {
            goto OUT_BAD_FD;
        }
        total_read += this_read;
    }
    if (total_read != config_size) {
        goto OUT_BAD_FD;
    }
    config_file_modtime = modtime;
    return true;
OUT_BAD_FD:
    close(fd);
OUT_BAD:
    free(config);
    config_size = 0;
    return false;
}

bool config_string_get(const char *category, const char *name, char *ret, uint8_t retlen)
{
    char fullname[50];
    snprintf(fullname, 50, "%s.%s=", category, name);
    lock_config();
    if (!read_config_file()) {
        goto OUT_DO_RELEASE;
    }
    const char *namepos = strstr(config, fullname);
    if (namepos == NULL) {
        goto OUT_DO_RELEASE;
    }
    const char *terminator = strstr(namepos, "\n");
    if (terminator == NULL) {
        goto OUT_DO_RELEASE;
    }
    const int16_t len = terminator - namepos - strlen(fullname);
    const char *value = namepos+strlen(fullname);
    memcpy(ret, value, (len <= retlen) ? len : retlen);
OUT_DO_RELEASE:
    unlock_config();
    return true;
}

bool config_integer_get(const char *category, const char *name, int *ret)
{
    char value[50] = {};
    if (!config_string_get(category, name, value, 50)) {
        return false;
    }
    *ret = atoi(value);
    fprintf(stderr, "value: (%s) (%d)\n", value, *ret);
    return true;
}

bool config_string_set(const char *category, const char *name, char *value)
{
    fprintf(stderr, "old config string: (%s) len=%lu\n", config, config_size);
    char fullname[50];
    snprintf(fullname, 50, "%s.%s=", category, name);
    lock_config();
    if (!read_config_file()) {
        return false;
    }
    const char *namepos = strstr(config, fullname);
    if (namepos == NULL) {
        // append
        const off_t new_config_size = config_size + strlen(fullname) + strlen(value) + 1;
        char *new_config = realloc(config, new_config_size+1);
        if (new_config == NULL) {
            goto OUT_DO_RELEASE;
        }
        config = new_config;
        config[new_config_size] = '\0';
        strcpy(&config[config_size], fullname);
        strcpy(&config[config_size+strlen(fullname)], value);
        config[config_size+strlen(fullname)+strlen(value)] = '\n';
        config_size = new_config_size;
    } else {
        // found existing entry
        const char *terminator = strstr(namepos, "\n");
        if (terminator == NULL) {
            goto OUT_DO_RELEASE;
        }
        const int name_offset = namepos-config;
        const int value_offset = name_offset + strlen(fullname);
        const off_t old_value_len = terminator - namepos - strlen(fullname);
        const int delta = old_value_len - strlen(value);
        const off_t new_config_size = config_size - old_value_len + strlen(value);
        if (new_config_size > config_size) {
            // make buffer bigger, move stuff to end of buffer using memmove
            char *new_config = realloc(config, new_config_size+1);
            if (new_config == NULL) {
                goto OUT_DO_RELEASE;
            }
            config = new_config;
            config[new_config_size] = '\0';
            memmove(&config[value_offset+strlen(value)],
                    &config[value_offset+old_value_len],
                    config_size-value_offset);
        } else if (new_config_size < config_size) {
            // move stuff to beginning of buffer using memmove, make
            // buffer smaller
            memmove(&config[value_offset],
                    &config[value_offset+delta],
                    config_size-value_offset-delta);
            char *new_config = realloc(config, new_config_size+1);
            if (new_config == NULL) {
                goto OUT_DO_RELEASE;
            }
            config = new_config;
            config[new_config_size] = '\0';
        }
        memcpy(&config[name_offset+strlen(fullname)], value, strlen(value));
        config_size = new_config_size;
    }
    fprintf(stderr, "new config string: (%s) len=%lu\n", config, config_size);
    unlock_config();
    return true;
OUT_DO_RELEASE:
    unlock_config();
    return false;
}

bool config_integer_set(const char *category, const char *name, int value)
{
    char value_string[50] = {};
    snprintf(value_string, 50, "%d", value);
    return config_string_set(category, name, value_string);
}



void test_config()
{
    int bob;
    if (!config_integer_get("FOO", "BAR", &bob)) {
        fprintf(stderr, "Failed to get FOO.BAR\n");
        abort();
    }
    if (bob != 37) {
        abort();
    }
    fprintf(stderr, "Got %d\n", bob);

    if (!config_integer_set("FOO", "BAR", 5)) {
        fprintf(stderr, "Failed to set FOO.BAR\n");
        abort();
    }
    if (!config_integer_get("FOO", "BAR", &bob)) {
        fprintf(stderr, "Failed to get FOO.BAR\n");
        abort();
    }
    if (bob != 5) {
        fprintf(stderr, "bob=%d\n", bob);
        abort();
    }

    if (!config_integer_set("FOO", "BAR", 713)) {
        fprintf(stderr, "Failed to set FOO.BAR\n");
        abort();
    }
    if (!config_integer_get("FOO", "BAR", &bob)) {
        fprintf(stderr, "Failed to get FOO.BAR\n");
        abort();
    }
    if (bob != 713) {
        fprintf(stderr, "bob=%d\n", bob);
        abort();
    }

    if (!config_integer_set("FOO", "BAR", 123)) {
        fprintf(stderr, "Failed to set FOO.BAR\n");
        abort();
    }
    if (!config_integer_get("FOO", "BAR", &bob)) {
        fprintf(stderr, "Failed to get FOO.BAR\n");
        abort();
    }
    if (bob != 123) {
        fprintf(stderr, "bob=%d\n", bob);
        abort();
    }

    if (!config_integer_set("FOO", "BAZ", 54)) {
        fprintf(stderr, "Failed to set FOO.BAR\n");
        abort();
    }
    if (!config_integer_get("FOO", "BAZ", &bob)) {
        fprintf(stderr, "Failed to get FOO.BAZ\n");
        abort();
    }
    if (bob != 54) {
        fprintf(stderr, "bob=%d\n", bob);
        abort();
    }
    if (!config_integer_get("FOO", "BAR", &bob)) {
        fprintf(stderr, "Failed to get FOO.BAR\n");
        abort();
    }
    if (bob != 123) {
        fprintf(stderr, "bob=%d\n", bob);
        abort();
    }
}

