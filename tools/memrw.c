// memrw: read or write one 32-bit physical register through /dev/mem (busybox has no devmem).
//   memrw ADDR            print it
//   memrw ADDR VALUE      write it, then read it back
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: memrw ADDR [VALUE]\n"); return 2; }
    unsigned long a = strtoul(argv[1], NULL, 0), page = a & ~0xfffUL;
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("/dev/mem"); return 1; }
    volatile uint8_t *m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, page);
    if (m == MAP_FAILED) { perror("mmap"); return 1; }
    volatile uint32_t *r = (volatile uint32_t *)(m + (a - page));
    printf("%#010lx = %#010x\n", a, *r);
    if (argc < 3) return 0;
    *r = (uint32_t)strtoul(argv[2], NULL, 0);
    printf("%#010lx <- %s, reads %#010x\n", a, argv[2], *r);
    return 0;
}
