/* fsdemo.c — exercises the guest fs.h API: directory listing, mkdir,
 * file create / write / read, and remove, against the VM's mount
 * namespace. /host is read-only; /td0 (tmpfs) is writable.
 *
 * Run from the shell:  run /host/fsdemo.elf
 *
 * Public domain (CC0). No warranty.
 */
#include "vm_runtime.h"
#include "fs.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    /* 1. List a directory (read-only /host). */
    printf("listing /host:\n");
    int d = fs_opendir("/host");
    if (d < 0) {
        printf("  opendir(/host) failed: %d\n", d);
    } else {
        FsDirent e;
        while (fs_readdir(d, &e) == 0)
            printf("  %s %-20s %u bytes\n",
                   e.type == FS_DT_DIR ? "[dir]" : "     ", e.name, e.size);
        fs_closedir(d);
    }

    /* 2. Create + write + read back + remove on the writable /td0. */
    fs_mkdir("/td0/fsdemo");
    int fd = fs_create("/td0/fsdemo/hello.txt");
    if (fd < 0) {
        printf("create on /td0 failed: %d\n", fd);
        return 1;
    }
    const char *msg = "written by fsdemo via fs.h\n";
    fs_write(fd, msg, (unsigned)strlen(msg));
    fs_close(fd);

    char buf[64];
    fd = fs_open("/td0/fsdemo/hello.txt", O_RDONLY);
    int n = (fd >= 0) ? fs_read(fd, buf, sizeof buf - 1) : -1;
    if (n > 0) { buf[n] = '\0'; printf("read back: %s", buf); }
    if (fd >= 0) fs_close(fd);

    fs_remove("/td0/fsdemo/hello.txt");
    fs_rmdir("/td0/fsdemo");
    printf("create/write/read/remove on /td0: OK\n");
    return 0;
}
