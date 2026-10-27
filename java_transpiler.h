#ifndef JAVA_TRANSPILER_H
#define JAVA_TRANSPILER_H

#include <stddef.h>

int lupi_java_transpile(const char *source_path, char **output, size_t *output_size,
                        char *error, size_t error_size);

#endif
