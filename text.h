#ifndef TEXT_H
#define TEXT_H

#include <stddef.h>

// a growable string for building response bodies in memory. start it as
// {0}, append to it, then check failed once at the end. data is always
// NUL terminated once anything has been added
struct text {
    char* data;
    size_t len;
    size_t cap;
    int failed; // set if any append ran out of memory
};

int text_append(struct text* text, const char* data, size_t len);
int text_puts(struct text* text, const char* str);
int text_putc(struct text* text, char c);
int text_printf(struct text* text, const char* format, ...);
void text_free(struct text* text);

#endif
