#include "../includes.h"
#include "../template.h"
#include "functions.h"

#include <sys/statvfs.h>

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


void posix_functions_init(struct template_state *tmpl)
{
    tmpl->put(tmpl, "disk_info", "", disk_info);
}
