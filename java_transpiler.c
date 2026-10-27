#include "java_transpiler.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} OutputBuffer;

static int append_text(OutputBuffer *buffer, const char *text) {
    size_t length = strlen(text);
    if (buffer->length + length + 1 > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity : 1024;
        while (capacity < buffer->length + length + 1) capacity *= 2;
        char *data = realloc(buffer->data, capacity);
        if (!data) return -1;
        buffer->data = data;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->length, text, length + 1);
    buffer->length += length;
    return 0;
}

static char *trim(char *text) {
    while (isspace((unsigned char)*text)) text++;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) *--end = '\0';
    return text;
}

static int is_type(const char *word) {
    return strcmp(word, "int") == 0 || strcmp(word, "float") == 0 ||
           strcmp(word, "double") == 0 || strcmp(word, "boolean") == 0 ||
           strcmp(word, "String") == 0;
}

static void replace_operators(char *text) {
    char result[4096];
    size_t output = 0;
    for (size_t i = 0; text[i] && output + 5 < sizeof(result); i++) {
        if (text[i] == '&' && text[i + 1] == '&') {
            memcpy(result + output, " and ", 5);
            output += 5;
            i++;
        } else if (text[i] == '|' && text[i + 1] == '|') {
            memcpy(result + output, " or ", 4);
            output += 4;
            i++;
        } else if (text[i] == '!' && text[i + 1] != '=') {
            memcpy(result + output, "not ", 4);
            output += 4;
        } else if (strncmp(text + i, "true", 4) == 0 &&
                   (i == 0 || !isalnum((unsigned char)text[i - 1])) &&
                   !isalnum((unsigned char)text[i + 4])) {
            memcpy(result + output, "true", 4);
            output += 4;
            i += 3;
        } else if (strncmp(text + i, "false", 5) == 0 &&
                   (i == 0 || !isalnum((unsigned char)text[i - 1])) &&
                   !isalnum((unsigned char)text[i + 5])) {
            memcpy(result + output, "false", 5);
            output += 5;
            i += 4;
        } else {
            result[output++] = text[i];
        }
    }
    result[output] = '\0';
    snprintf(text, 4096, "%s", result);
}

static void strip_modifiers(char **line) {
    static const char *modifiers[] = { "public ", "private ", "protected ",
                                       "static ", "final ", NULL };
    int changed;
    do {
        changed = 0;
        for (int i = 0; modifiers[i]; i++) {
            size_t length = strlen(modifiers[i]);
            if (strncmp(*line, modifiers[i], length) == 0) {
                *line += length;
                changed = 1;
                break;
            }
        }
    } while (changed);
}

static int translate_parameters(char *parameters, char *output, size_t output_size) {
    char *save = NULL;
    char *parameter = strtok_r(parameters, ",", &save);
    output[0] = '\0';
    while (parameter) {
        char *part = trim(parameter);
        char type[32] = "";
        char name[128] = "";
        if (sscanf(part, "%31s %127s", type, name) != 2 || !is_type(type)) return -1;
        if (output[0] && strlen(output) + 2 < output_size) strcat(output, ", ");
        if (strlen(output) + strlen(name) + 1 >= output_size) return -1;
        strcat(output, name);
        parameter = strtok_r(NULL, ",", &save);
    }
    return 0;
}

static int translate_method(char *line, char *output, size_t output_size) {
    char *open = strchr(line, '(');
    char *close = open ? strchr(open, ')') : NULL;
    if (!open || !close || !strchr(close, '{')) return 0;

    char name[128] = "";
    char declaration[256];
    size_t declaration_length = (size_t)(open - line);
    if (declaration_length >= sizeof(declaration)) return -1;
    memcpy(declaration, line, declaration_length);
    declaration[declaration_length] = '\0';
    char return_type[32] = "";
    if (sscanf(trim(declaration), "%31s %127s", return_type, name) != 2) return -1;
    if (!is_type(return_type) && strcmp(return_type, "void") != 0) return 0;

    char parameters[512];
    size_t parameter_length = (size_t)(close - open - 1);
    if (parameter_length >= sizeof(parameters)) return -1;
    memcpy(parameters, open + 1, parameter_length);
    parameters[parameter_length] = '\0';
    char translated_parameters[512];
    if (translate_parameters(parameters, translated_parameters, sizeof(translated_parameters)) != 0)
        return -1;
    return snprintf(output, output_size, "function %s(%s)", name, translated_parameters) >= (int)output_size ? -1 : 1;
}

static int translate_declaration(char *line, char *output, size_t output_size) {
    char type[32] = "";
    char name[128] = "";
    char *equals = strchr(line, '=');
    char *semicolon = strrchr(line, ';');
    if (sscanf(line, "%31s %127s", type, name) != 2 || !is_type(type)) return 0;
    char *name_end = name + strcspn(name, "=;");
    *name_end = '\0';
    if (!*name) return -1;
    if (!equals) {
        const char *default_value = strcmp(type, "boolean") == 0 ? "false" :
                                    strcmp(type, "String") == 0 ? "\"\"" : "0";
        return snprintf(output, output_size, "%s = %s", name, default_value) >= (int)output_size ? -1 : 1;
    }
    char value[2048];
    char *value_start = equals + 1;
    size_t value_length = semicolon ? (size_t)(semicolon - value_start) : strlen(value_start);
    if (value_length >= sizeof(value)) return -1;
    memcpy(value, value_start, value_length);
    value[value_length] = '\0';
    snprintf(output, output_size, "%s = %s", name, trim(value));
    return 1;
}

int lupi_java_transpile(const char *source_path, char **output, size_t *output_size,
                        char *error, size_t error_size) {
    FILE *source = fopen(source_path, "r");
    if (!source) {
        snprintf(error, error_size, "Nao foi possivel abrir %s", source_path);
        return -1;
    }

    OutputBuffer buffer = {0};
    char line_buffer[4096];
    int class_open = 0;
    int method_open = 0;
    int block_depth = 0;
    int line_number = 0;
    int failed = 0;

    while (fgets(line_buffer, sizeof(line_buffer), source)) {
        line_number++;
        char *line = trim(line_buffer);
        if (!*line || strncmp(line, "//", 2) == 0 || strncmp(line, "import ", 7) == 0 ||
            strncmp(line, "package ", 8) == 0) continue;
        if (strncmp(line, "class ", 6) == 0 && strchr(line, '{')) {
            class_open = 1;
            continue;
        }

        if (strcmp(line, "}") == 0) {
            if (block_depth > 0) {
                block_depth--;
                if (append_text(&buffer, "end\n") != 0) failed = 1;
                if (block_depth == 0) method_open = 0;
            } else if (class_open) {
                class_open = 0;
            }
            continue;
        }

        char translated[4096] = "";
        char *working = line;
        strip_modifiers(&working);

        if (strncmp(working, "if ", 3) == 0 || strncmp(working, "if(", 3) == 0) {
            char *open = strchr(working, '(');
            char *close = strrchr(working, ')');
            if (!open || !close || !strchr(close, '{')) { failed = 1; goto line_error; }
            *close = '\0';
            snprintf(translated, sizeof(translated), "if %s then", trim(open + 1));
            block_depth++;
        } else if (strncmp(working, "else if", 7) == 0) {
            char *open = strchr(working, '(');
            char *close = strrchr(working, ')');
            if (!open || !close || !strchr(close, '{')) { failed = 1; goto line_error; }
            *close = '\0';
            snprintf(translated, sizeof(translated), "elseif %s then", trim(open + 1));
            block_depth++;
        } else if (strncmp(working, "else", 4) == 0 && strchr(working, '{')) {
            snprintf(translated, sizeof(translated), "else");
            block_depth++;
        } else {
            int method_result = translate_method(working, translated, sizeof(translated));
            if (method_result < 0) { failed = 1; goto line_error; }
            if (method_result > 0) {
                method_open = 1;
                block_depth++;
            } else {
                int declaration_result = translate_declaration(working, translated, sizeof(translated));
                if (declaration_result < 0) { failed = 1; goto line_error; }
                if (declaration_result == 0) {
                    size_t length = strlen(working);
                    if (length && working[length - 1] == ';') working[length - 1] = '\0';
                    snprintf(translated, sizeof(translated), "%s", trim(working));
                    char *increment = strstr(translated, "++");
                    char *decrement = strstr(translated, "--");
                    if (increment) {
                        *increment = '\0';
                        snprintf(translated, sizeof(translated), "%s = %s + 1", trim(working), trim(working));
                    } else if (decrement) {
                        *decrement = '\0';
                        snprintf(translated, sizeof(translated), "%s = %s - 1", trim(working), trim(working));
                    }
                }
            }
        }

        replace_operators(translated);
        if (append_text(&buffer, translated) != 0 || append_text(&buffer, "\n") != 0) failed = 1;
        if (failed) goto line_error;
        continue;

line_error:
        snprintf(error, error_size, "Sintaxe Lupi Java invalida na linha %d", line_number);
        break;
    }

    if (!failed && (method_open || block_depth != 0 || class_open)) {
        snprintf(error, error_size, "Bloco Java sem fechamento no fim do arquivo");
        failed = 1;
    }
    fclose(source);
    if (failed) {
        free(buffer.data);
        return -1;
    }
    if (!buffer.data) buffer.data = calloc(1, 1);
    *output = buffer.data;
    *output_size = buffer.length;
    return 0;
}
