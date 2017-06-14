#include "../includes.h"
#include "../template.h"
#include "functions.h"

#include <sys/statvfs.h>

#include <sys/types.h>
#include <dirent.h>

static void sock_send_diskinfo(struct sock_buf *sock, const char *label, unsigned serial, unsigned total_clusters, unsigned free_clusters, unsigned cluster_size)
{
    sock_printf(sock,
                "{"
                "\"label\" : \"%s\", "
                "\"serial\" : %u, "
                "\"total_clusters\" : %u, "
                "\"free_clusters\" : %u, "
                "\"cluster_size\" : %u"
                "}",
                label, serial, total_clusters, free_clusters, cluster_size*512);
}

static void disk_info(struct template_state *tmpl, const char *name, const char *value, int argc, char **argv)
{
    unsigned long free_clusters = 0, total_clusters = 0, cluster_size = 0;
    const char label[] = "/";

    /* Get volume information and free clusters of root filesystem */
    struct statvfs buf;
    if (statvfs("/", &buf) != -1) {
        total_clusters = buf.f_blocks;
        free_clusters = buf.f_bfree;
        cluster_size = buf.f_bsize / 512;
    }

    const unsigned serial = 0;

    sock_send_diskinfo(tmpl->sock, label, serial, total_clusters, free_clusters, cluster_size);
}


static void sock_send_dirent(struct sock_buf *sock, bool first, int type, const char *name, unsigned year, unsigned month, unsigned day, unsigned hour, unsigned minute, unsigned second, unsigned size)
{
    if (!first) {
        sock_printf(sock, "%s", ",\n");
    }
    sock_printf(sock, "{"
                "\"type\" : %u, "
                "\"name\" : \"%s\", "
                "\"date\" : \"%04u-%02u-%02u %02u:%02u:%02u\", "
                "\"size\" : %u "
                "}",
                type, name, year, month, day, hour, minute, second, size);
}

static void file_listdir(struct template_state *tmpl, const char *name, const char *value, int argc, char **argv)
{
    if (argc < 1) {
        return;
    }
    const char *dirpath = argv[0];

    DIR *dh = opendir(dirpath);
    if (dh == NULL) {
        console_printf("Failed to open directory %s\n", dirpath);
        return;
    }

    sock_printf(tmpl->sock, "[ ");

    bool first = true;
    struct dirent *result;
    while ((result = readdir(dh)) != NULL) {
        if (strcmp(result->d_name, ".") == 0) {
            continue;
        }

        struct stat buf;
        char filepath[PATH_MAX] = {};
        snprintf(filepath, sizeof(filepath), "%s/%s", dirpath, result->d_name);
        if (stat(filepath, &buf) == -1) {
            continue;
        }

        struct tm t;
        if (localtime_r(&buf.st_mtim.tv_sec, &t) == NULL) {
            continue;
        }

        sock_send_dirent(tmpl->sock,
                         first,
                         (result->d_type==DT_DIR) ? 1 :0,
                         result->d_name,
                         t.tm_year+1900,
                         t.tm_mon+1,
                         t.tm_mday,
                         t.tm_hour,
                         t.tm_min,
                         t.tm_sec,
                         buf.st_size);
        first = false;
    }
    sock_printf(tmpl->sock, "]");
    closedir(dh);
}


void posix_functions_init(struct template_state *tmpl)
{
    tmpl->put(tmpl, "file_listdir", "", file_listdir);
    tmpl->put(tmpl, "disk_info", "", disk_info);
}
