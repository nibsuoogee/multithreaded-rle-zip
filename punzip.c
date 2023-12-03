#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <pthread.h>
#include <sys/mman.h>
#include <ctype.h>

#define ARRSIZE 20

#define handle_error(msg) \
    do { perror(msg); exit(EXIT_FAILURE); } while (0)

int main(int argc, char** argv, char *envp[])
{
    int fd;
    char *addr;
    off_t offset, pa_offset, current = 0;
    size_t length;
    ssize_t s;
    struct stat sb;
    char c;
    u_int32_t count_c = 0;


    if (argc < 2 || argc > 4) {
        printf("usage: punzip <input> > <output>\n");
        return(1);
    }
    
    fd = open(argv[1], O_RDONLY);
    if (fd == -1)
        handle_error("open");

    if (fstat(fd, &sb) == -1)           /* To obtain file size */
        handle_error("fstat");

    offset = 0; //atoi(argv[2]);
    pa_offset = offset & ~(sysconf(_SC_PAGE_SIZE) - 1);
        /* offset for mmap() must be page aligned */

    if (offset >= sb.st_size) {
        fprintf(stderr, "offset is past end of file\n");
        exit(EXIT_FAILURE);
    }

    if (argc == 4) {
        length = atoi(argv[3]);
        if (offset + length > sb.st_size)
            length = sb.st_size - offset;
                /* Can't display bytes past end of file */

    } else {    /* No length arg ==> display to end of file */
        length = sb.st_size - offset;
    }

    addr = mmap(NULL, length + offset - pa_offset, PROT_READ,
        MAP_PRIVATE, fd, pa_offset);
    if (addr == MAP_FAILED)
        handle_error("mmap");

    // read 5 byte chunks
    while(current < length) {
        memcpy(&count_c, addr + offset - pa_offset + current, sizeof(count_c));
        current += sizeof(count_c); // jump to next ascii character

        memcpy(&c, addr + offset - pa_offset + current, sizeof(c));
        current += sizeof(c); // jump to beginning of next u_int32_t
        for (size_t i = 0; i < count_c; i++) {
            fprintf(stdout, "%c", c);
        }
    }

    munmap(addr, length + offset - pa_offset);
    close(fd);

    exit(EXIT_SUCCESS);
}