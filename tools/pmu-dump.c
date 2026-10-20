// pmu-dump: read every register of an I2C PMU, read-only (combined write-address/read).
//   pmu-dump [bus=0] [addr=0x3a] [width=1|2]     width = register-address bytes
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int bus = argc > 1 ? atoi(argv[1]) : 0;
    int addr = argc > 2 ? (int)strtol(argv[2], NULL, 0) : 0x3a;
    int width = argc > 3 ? atoi(argv[3]) : 1;
    char dev[32];
    snprintf(dev, sizeof dev, "/dev/i2c-%d", bus);
    int fd = open(dev, O_RDWR);
    if (fd < 0) { perror(dev); return 1; }
    int ok = 0;
    for (int reg = 0; reg < (width == 2 ? 0x400 : 0x100); reg++) {
        unsigned char a[2] = {width == 2 ? reg >> 8 : reg, reg & 0xff}, v = 0;
        struct i2c_msg m[2] = {
            {.addr = addr, .flags = 0, .len = width, .buf = width == 2 ? a : a + 1},
            {.addr = addr, .flags = I2C_M_RD, .len = 1, .buf = &v},
        };
        struct i2c_rdwr_ioctl_data x = {.msgs = m, .nmsgs = 2};
        if (ioctl(fd, I2C_RDWR, &x) < 0) { printf("%03x: --\n", reg); continue; }
        printf("%03x: %02x\n", reg, v);
        ok++;
    }
    fprintf(stderr, "%d registers read\n", ok);
    return ok ? 0 : 1;
}
