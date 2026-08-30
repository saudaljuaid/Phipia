/* SPDX-License-Identifier: GPL-3.0-only */
#include <locale.h>
#include <signal.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

char *setlocale(int category, const char *locale)
{
    static char c_locale[] = "C";
    if (category < LC_ALL || category > LC_TIME) { errno = EINVAL; return NULL; }
    if (locale == NULL || strcmp(locale, "C") == 0 || strcmp(locale, "") == 0) return c_locale;
    errno = ENOTSUP;
    return NULL;
}
struct lconv *localeconv(void)
{
    static char decimal[] = ".";
    static struct lconv result = {decimal};
    return &result;
}
sighandler_t signal(int signal_number, sighandler_t handler)
{
    (void)signal_number; (void)handler; errno = ENOTSUP; return SIG_DFL;
}
int raise(int signal_number)
{
    if (signal_number == SIGABRT) abort();
    errno = ENOTSUP;
    return -1;
}
