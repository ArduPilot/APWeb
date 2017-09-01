/*
  provide server side functions in C
 */

#include "../template.h"
#include "../includes.h"

void posix_functions_init(struct template_state *tmpl);
void download_filesystem(struct cgi_state *cgi, const char *fs_path);
