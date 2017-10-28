#pragma once

#ifdef SYSTEM_FREERTOS
#include <FreeRTOS.h>
#include <bsp.h>
#include <task.h>
#include <nonstdlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <libmid_fatfs/ff.h>
#include "socket_ctrl.h"
#include <generated/snx_sdk_conf.h>
#include "../talloc.h"
#include "../dev_console.h"
#include "../util/print_vprintf.h"
#else
#include "linux/includes.h"
#include "mavlink_core.h"
#endif

#include "cgi.h"
#include "template.h"
#include "web_files.h"


