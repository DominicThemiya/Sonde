#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "text.h"

// make room for extra more bytes plus the terminator, -1 if out of memory
static int text_reserve(struct text* text, size_t extra) {
    if (text->failed) {
        return -1;
    }
    if (text->len + extra + 1 <= text->cap) {
        return 0;
    }

    size_t cap = text->cap ? text->cap : 256;
    while (cap < text->len + extra + 1) {
        cap *= 2;
    }

    char* grown = realloc(text->data, cap);

    if (grown == NULL) {
        text->failed = 1;
        return -1;
    }
    text->data = grown;
    text->cap = cap;
    return 0;
}

int text_append(struct text* text, const char* data, size_t len) {
    if (text_reserve(text, len) == -1) {
        return -1;
    }
    memcpy(text->data + text->len, data, len);
    text->len += len;
    text->data[text->len] = '\0';
    return 0;
}

int text_puts(struct text* text, const char* str) {
    return text_append(text, str, strlen(str));
}

int text_putc(struct text* text, char c) {
    return text_append(text, &c, 1);
}

int text_printf(struct text* text, const char* format, ...) {
    va_list args;

    // measure first, then print straight into the buffer
    va_start(args, format);
    int len = vsnprintf(NULL, 0, format, args);
    va_end(args);

    if (len < 0 || text_reserve(text, len) == -1) {
        return -1;
    }

    va_start(args, format);
    vsnprintf(text->data + text->len, len + 1, format, args);
    va_end(args);

    text->len += len;
    return 0;
}

void text_free(struct text* text) {
    free(text->data);
    *text = (struct text){0};
}
