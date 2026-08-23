#define _GNU_SOURCE

#include <stddef.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/utsname.h>

_Static_assert(sizeof(((struct utsname *)0)->sysname) == 65U,
               "Linux sysname field must be 65 bytes");
_Static_assert(sizeof(((struct utsname *)0)->nodename) == 65U,
               "Linux nodename field must be 65 bytes");
_Static_assert(sizeof(((struct utsname *)0)->release) == 65U,
               "Linux release field must be 65 bytes");
_Static_assert(sizeof(((struct utsname *)0)->version) == 65U,
               "Linux version field must be 65 bytes");
_Static_assert(sizeof(((struct utsname *)0)->machine) == 65U,
               "Linux machine field must be 65 bytes");
_Static_assert(sizeof(((struct utsname *)0)->domainname) == 65U,
               "Linux domainname field must be 65 bytes");
_Static_assert(sizeof(struct utsname) == 390U,
               "Linux struct utsname must be 390 bytes");
_Static_assert(_Alignof(struct utsname) == 1U,
               "Linux struct utsname must have byte alignment");
_Static_assert(SYS_uname == 63, "Linux x86-64 uname must be syscall 63");

int main(void)
{
    printf("syscall uname: %ld\n", (long)SYS_uname);
    printf("struct utsname bytes: %zu\n", sizeof(struct utsname));
    printf("struct utsname alignment: %zu\n", _Alignof(struct utsname));
    printf("sysname: offset %zu bytes %zu\n",
           offsetof(struct utsname, sysname),
           sizeof(((struct utsname *)0)->sysname));
    printf("nodename: offset %zu bytes %zu\n",
           offsetof(struct utsname, nodename),
           sizeof(((struct utsname *)0)->nodename));
    printf("release: offset %zu bytes %zu\n",
           offsetof(struct utsname, release),
           sizeof(((struct utsname *)0)->release));
    printf("version: offset %zu bytes %zu\n",
           offsetof(struct utsname, version),
           sizeof(((struct utsname *)0)->version));
    printf("machine: offset %zu bytes %zu\n",
           offsetof(struct utsname, machine),
           sizeof(((struct utsname *)0)->machine));
    printf("domainname: offset %zu bytes %zu\n",
           offsetof(struct utsname, domainname),
           sizeof(((struct utsname *)0)->domainname));
    return 0;
}
