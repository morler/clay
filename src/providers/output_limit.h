#ifndef CLAY_PROVIDER_OUTPUT_LIMIT_H
#define CLAY_PROVIDER_OUTPUT_LIMIT_H

#include "clay/json.h"

void clay_provider_apply_output_limit(ClayJson *request, const char *field,
                                      long limit, long window, long minimum);

#endif
