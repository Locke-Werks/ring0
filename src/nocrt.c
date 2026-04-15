#include <stddef.h>
#include <string.h>

/* MSVC emits a reference to _fltused from any object that uses float math, to
   force the CRT's floating-point initializer to link. Without a CRT we just
   provide the symbol ourselves. */
int _fltused = 0;

#pragma function(memcpy)
void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

#pragma function(memset)
void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

#pragma function(memcmp)
int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    while (n--) {
        if (*x != *y) return *x - *y;
        ++x; ++y;
    }
    return 0;
}
