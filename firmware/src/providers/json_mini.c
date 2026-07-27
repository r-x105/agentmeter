#include "json_mini.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

// Locate the value position after `"key":`, skipping whitespace.
// Returns NULL if the quoted key never appears.
static const char* find_value(const char* body, const char* key) {
    size_t klen = strlen(key);
    const char* p = body;
    while ((p = strchr(p, '"')) != NULL) {
        p++;
        if (strncmp(p, key, klen) == 0 && p[klen] == '"') {
            const char* q = p + klen + 1;
            while (*q && isspace((unsigned char)*q)) q++;
            if (*q == ':') {
                q++;
                while (*q && isspace((unsigned char)*q)) q++;
                return q;
            }
        }
    }
    return NULL;
}

bool json_find_number(const char* body, const char* key, float* out) {
    const char* v = find_value(body, key);
    if (!v) return false;
    if (*v != '-' && !isdigit((unsigned char)*v)) return false;
    char* end = NULL;
    float f = strtof(v, &end);
    if (end == v) return false;
    *out = f;
    return true;
}

bool json_find_string(const char* body, const char* key, char* out, int n) {
    const char* v = find_value(body, key);
    if (!v || *v != '"') return false;
    v++;
    int i = 0;
    while (*v && *v != '"' && i < n - 1) {
        if (*v == '\\' && v[1]) v++;  // unescape the simple cases
        out[i++] = *v++;
    }
    out[i] = '\0';
    return true;
}

bool json_is_null(const char* body, const char* key) {
    const char* v = find_value(body, key);
    return v && strncmp(v, "null", 4) == 0;
}
