#ifndef TASK4_TEXT_H
#define TASK4_TEXT_H
#include <stddef.h>

/* 检查一个完整 Unicode 码点；不允许过长编码、代理项或超出 U+10FFFF。 */
static inline unsigned t4_character_width(unsigned char c)
{
    if (c >= 1 && c <= 0x7f) return 1;
    if (c >= 0xc2 && c <= 0xdf) return 2;
    if (c >= 0xe0 && c <= 0xef) return 3;
    if (c >= 0xf0 && c <= 0xf4) return 4;
    return 0;
}
static inline int t4_character_valid(const unsigned char *p, size_t n)
{
    if (n == 0 || n != t4_character_width(p[0])) return 0;
    for (size_t i = 1; i < n; ++i) if (p[i] < 0x80 || p[i] > 0xbf) return 0;
    if (n == 3 && ((p[0] == 0xe0 && p[1] < 0xa0) || (p[0] == 0xed && p[1] >= 0xa0))) return 0;
    if (n == 4 && ((p[0] == 0xf0 && p[1] < 0x90) || (p[0] == 0xf4 && p[1] >= 0x90))) return 0;
    return 1;
}
#endif
