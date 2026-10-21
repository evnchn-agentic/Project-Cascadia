// pmu-gpio: read or set one D1815 PMU GPIO config register (0x50 + n, n = 0..9).
// Fenced to that block only: it refuses every other PMU register.
//   pmu-gpio N               print 0x50+N
//   pmu-gpio N VALUE         write it, read it back
// Layout from AppleD1815PMU::_setGPIOFunction (12H847): bit 1 = output level for an output.
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define PMU 0x74

static int xfer(int fd, unsigned char reg, unsigned char *val, int write) {
    unsigned char w[2] = {reg, val ? *val : 0};
    struct i2c_msg m[2] = {
        {.addr = PMU, .flags = 0, .len = write ? 2 : 1, .buf = w},
        {.addr = PMU, .flags = I2C_M_RD, .len = 1, .buf = val},
    };
    struct i2c_rdwr_ioctl_data x = {.msgs = m, .nmsgs = write ? 1 : 2};
    return ioctl(fd, I2C_RDWR, &x) < 0 ? -1 : 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: pmu-gpio N [VALUE]\n"); return 2; }
    int n = atoi(argv[1]);
    if (n < 0 || n > 9) { fprintf(stderr, "gpio %d out of range 0..9\n", n); return 2; }
    unsigned char reg = 0x50 + n, v;
    int fd = open("/dev/i2c-0", O_RDWR);
    if (fd < 0 || xfer(fd, reg, &v, 0)) { perror("read"); return 1; }
    printf("gpio%d reg 0x%02x = 0x%02x\n", n, reg, v);
    if (argc < 3) return 0;
    unsigned char nv = (unsigned char)strtol(argv[2], NULL, 0);
    if (xfer(fd, reg, &nv, 1)) { perror("write"); return 1; }
    if (xfer(fd, reg, &v, 0)) { perror("readback"); return 1; }
    printf("gpio%d reg 0x%02x <- 0x%02x, reads 0x%02x\n", n, reg, nv, v);
    return v == nv ? 0 : 1;
}
