/*
 * M0 toolchain check: proves a qcc-built binary runs on the QNX target.
 * Uses only standard C (C11) and POSIX uname(); no QNX-specific API.
 *
 * uname(), <sys/utsname.h>:
 *   https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/u/uname.html
 */
#include <stdio.h>
#include <sys/utsname.h>

int main(void)
{
    struct utsname u;

    if (uname(&u) != 0) {
        perror("uname");
        return 1;
    }
    printf("hello from Arbiter on %s %s %s (%s)\n",
           u.sysname, u.release, u.version, u.machine);
    return 0;
}
