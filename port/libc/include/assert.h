#ifndef VERSITILE_LIBC_ASSERT_H
#define VERSITILE_LIBC_ASSERT_H

#ifdef NDEBUG
#define assert(expression) ((void)0)
#else
void __assert_fail(
    const char *expression,
    const char *file,
    unsigned line,
    const char *function
) __attribute__((noreturn));
#define assert(expression) \
    ((expression) ? (void)0 : \
     __assert_fail(#expression, __FILE__, __LINE__, __func__))
#endif

#endif
