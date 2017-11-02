#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

bool config_integer_get(const char *category, const char *name, int * ret);
bool config_string_get(const char *category, const char *name, char *ret, uint8_t retlen);
