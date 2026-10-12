/*
 * igame-led — control Colorful / iGame motherboard lighting on Linux
 *
 * Recovered from Colorful iGame Center (iGC.Lite) by static analysis.
 * See docs/ANALYSIS.md for the protocol documentation.
 *
 * The motherboard LED controller is a USB HID device:
 *   vendor 0x2F4C, product 0x1000, vendor usage page 0xFF01
 *   output report is 65 bytes: [0]=00 [1]=01 [2]=00 [3]=effect [4..6]=RGB
 *   effect 0 = off, effect 1 = static colour
 *
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <ctype.h>

#define IGEAME_VID          0x2F4C  /* Colorful HID LED controllers        */
#define HID_VENDOR_USAGE    0xFF01  /* the LED channel                     */
#define MB_REPORT_LEN       65

#define MB_EFFECT_OFF       0
#define MB_EFFECT_STATIC    1

static int opt_yes       = 0;
static int opt_dryrun    = 0;
static int opt_any_usage = 0;   /* send to every interface, any usage page */
static unsigned opt_pid  = 0;   /* filter by product id                    */

/* ------------------------------------------------------------------ */
/*  helpers                                                            */
/* ------------------------------------------------------------------ */

static int parse_color(const char *s, uint8_t *r, uint8_t *g, uint8_t *b)
{
    unsigned rr, gg, bb;

    if (strchr(s, ',') || strchr(s, ':')) {
        if (sscanf(s, "%u,%u,%u", &rr, &gg, &bb) != 3 &&
            sscanf(s, "%u:%u:%u", &rr, &gg, &bb) != 3)
            return -1;
    } else {
        char *end;
        unsigned long v = strtoul(s, &end, 16);
        if (*end != '\0' || strlen(s) > 6)
            return -1;
        rr = (v >> 16) & 0xFF;
        gg = (v >> 8) & 0xFF;
        bb = v & 0xFF;
    }
    if (rr > 255 || gg > 255 || bb > 255)
        return -1;
    *r = rr; *g = gg; *b = bb;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  HID transport                                                      */
/* ------------------------------------------------------------------ */

struct hiddev {
    char     path[300];
    unsigned vid;
    unsigned pid;
    unsigned usage_page;
    char     name[640];
};

/*
 * The LED channel lives on the vendor-defined usage page 0xFF01 (the original
 * software calls hid_open1(vid, pid, 0xFF01)).  A motherboard exposes several
 * HID collections, so pick the right one by scanning its report descriptor.
 */
static unsigned hid_usage_page(const char *hidraw_name)
{
    char p[512];
    uint8_t d[1024];
    ssize_t n;
    int fd;

    snprintf(p, sizeof(p), "/sys/class/hidraw/%s/device/report_descriptor",
             hidraw_name);
    fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    n = read(fd, d, sizeof(d));
    close(fd);
    if (n <= 0)
        return 0;

    /* walk HID descriptor items, return the first Usage Page */
    for (ssize_t i = 0; i + 1 < n; ) {
        uint8_t b = d[i];
        int size = b & 0x03;
        if (size == 3)
            size = 4;
        if (i + 1 + size > n)
            break;
        if (b == 0x05)
            return d[i + 1];
        if (b == 0x06 && size >= 2)
            return (unsigned)d[i + 1] | ((unsigned)d[i + 2] << 8);
        i += 1 + size;
    }
    return 0;
}

static void parse_uevent(const char *path, struct hiddev *d)
{
    FILE *f = fopen(path, "r");
    char line[640];

    d->vid = d->pid = 0;
    d->name[0] = '\0';
    if (!f)
        return;

    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "HID_ID=", 7)) {
            unsigned bus;
            sscanf(line + 7, "%x:%x:%x", &bus, &d->vid, &d->pid);
        } else if (!strncmp(line, "HID_NAME=", 9)) {
            snprintf(d->name, sizeof(d->name), "%s", line + 9);
            d->name[strcspn(d->name, "\r\n")] = '\0';
        }
    }
    fclose(f);
}

static int hid_collect(struct hiddev *out, int max, unsigned want_vid)
{
    DIR *dir = opendir("/sys/class/hidraw");
    struct dirent *e;
    int n = 0;

    if (!dir)
        return 0;

    while ((e = readdir(dir)) != NULL && n < max) {
        struct hiddev d;
        char p[512];

        if (strncmp(e->d_name, "hidraw", 6) != 0)
            continue;

        snprintf(p, sizeof(p), "/sys/class/hidraw/%s/device/uevent", e->d_name);
        parse_uevent(p, &d);

        if (want_vid && d.vid != want_vid)
            continue;

        d.usage_page = hid_usage_page(e->d_name);
        snprintf(d.path, sizeof(d.path), "/dev/%s", e->d_name);
        out[n++] = d;
    }
    closedir(dir);
    return n;
}

static void mb_build_report(uint8_t *p, unsigned effect, const uint8_t rgb[3])
{
    memset(p, 0, MB_REPORT_LEN);
    p[0] = 0x00;                /* HID report id                        */
    p[1] = 0x01;                /* constant, observed on every device   */
    p[2] = 0x00;
    p[3] = (uint8_t)effect;     /* 0 = off, 1 = static colour           */
    if (rgb) {
        p[4] = rgb[0];
        p[5] = rgb[1];
        p[6] = rgb[2];
    }
}

static int hid_write_report(const char *path, const uint8_t *p, int len)
{
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    uint8_t tmp[128];

    if (fd < 0)
        return -1;

    /* drain any stale input reports */
    while (read(fd, tmp, sizeof(tmp)) > 0)
        ;

    ssize_t r = write(fd, p, len);
    int saved = errno;
    close(fd);
    errno = saved;
    return (r == len) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/*  commands                                                           */
/* ------------------------------------------------------------------ */

static const char *USAGE =
"igame-led — control Colorful / iGame motherboard lighting on Linux\n"
"\n"
"Usage:\n"
"  igame-led mb list                 list Colorful HID LED devices\n"
"  igame-led mb off                  turn the motherboard lighting off\n"
"  igame-led mb set <RRGGBB|r,g,b>   set a static colour\n"
"  igame-led mb raw <hex bytes>      send a raw report (advanced)\n"
"  igame-led off                     alias for 'mb off'\n"
"\n"
"Options:\n"
"  --pid 0x1000     only talk to HID devices with this product id\n"
"  --any-usage      send to every matching interface, ignore usage page\n"
"  --dry-run        show what would be sent, do not write\n"
"  --yes            do not ask for confirmation\n";

static int cmd_mb_list(void)
{
    struct hiddev d[64];
    int n = hid_collect(d, 64, IGEAME_VID);

    if (n == 0) {
        printf("No Colorful (VID %04X) HID LED device found.\n", IGEAME_VID);
        printf("Listing every hidraw device instead:\n");
        n = hid_collect(d, 64, 0);
    }
    for (int i = 0; i < n; i++)
        printf("%-16s  %04X:%04X  usage_page=0x%04X  %s\n",
               d[i].path, d[i].vid, d[i].pid, d[i].usage_page, d[i].name);
    return n ? 0 : 1;
}

static int mb_targets(struct hiddev *d, int max)
{
    int n = hid_collect(d, max, IGEAME_VID);

    if (!opt_any_usage && n > 0) {
        int m = 0;
        for (int i = 0; i < n; i++)
            if (d[i].usage_page == HID_VENDOR_USAGE)
                d[m++] = d[i];
        if (m > 0)
            n = m;
    }
    return n;
}

static int cmd_mb_send(const uint8_t *report, const char *what)
{
    struct hiddev d[64];
    int n = mb_targets(d, 64);
    int ok = 0;

    if (n == 0) {
        fprintf(stderr, "igame-led: no Colorful HID LED device found\n");
        return 1;
    }

    if (opt_pid) {
        int m = 0;
        for (int i = 0; i < n; i++)
            if (d[i].pid == opt_pid) d[m++] = d[i];
        n = m;
        if (n == 0) {
            fprintf(stderr, "igame-led: no device with pid %04X\n", opt_pid);
            return 1;
        }
    }

    if (opt_dryrun) {
        printf("[dry-run] %s -> %d device(s), report:", what, n);
        for (int i = 0; i < MB_REPORT_LEN; i++)
            printf(" %02X", report[i]);
        printf("\n");
        return 0;
    }

    if (!opt_yes) {
        printf("About to send '%s' to:\n", what);
        for (int i = 0; i < n; i++)
            printf("  %s  %04X:%04X  %s\n", d[i].path, d[i].vid, d[i].pid, d[i].name);
        printf("Continue? [y/N] "); fflush(stdout);
        int c = getchar();
        if (c != 'y' && c != 'Y')
            return 1;
    }

    for (int i = 0; i < n; i++) {
        if (hid_write_report(d[i].path, report, MB_REPORT_LEN) == 0) {
            printf("  %s: ok\n", d[i].path);
            ok++;
        } else {
            printf("  %s: failed (%s)\n", d[i].path, strerror(errno));
        }
    }
    return ok ? 0 : 1;
}

static int cmd_mb_off(void)
{
    uint8_t p[MB_REPORT_LEN];
    mb_build_report(p, MB_EFFECT_OFF, NULL);
    return cmd_mb_send(p, "motherboard LED off");
}

static int cmd_mb_set(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t p[MB_REPORT_LEN];
    uint8_t rgb[3] = { r, g, b };
    mb_build_report(p, MB_EFFECT_STATIC, rgb);
    return cmd_mb_send(p, "motherboard static colour");
}

static int cmd_mb_raw(const char *hex)
{
    uint8_t p[MB_REPORT_LEN];
    int n = 0;
    const char *s = hex;

    memset(p, 0, sizeof(p));
    while (*s && n < MB_REPORT_LEN) {
        while (*s == ' ' || *s == ':' || *s == ',') s++;
        if (!isxdigit((unsigned char)s[0]) || !isxdigit((unsigned char)s[1]))
            break;
        char b[3] = { s[0], s[1], 0 };
        p[n++] = (uint8_t)strtoul(b, NULL, 16);
        s += 2;
    }
    if (n == 0) {
        fprintf(stderr, "igame-led: no hex bytes given\n");
        return 1;
    }
    return cmd_mb_send(p, "raw report");
}

int main(int argc, char **argv)
{
    const char *a[8];
    int n = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--yes"))            opt_yes = 1;
        else if (!strcmp(argv[i], "--dry-run"))   opt_dryrun = 1;
        else if (!strcmp(argv[i], "--any-usage")) opt_any_usage = 1;
        else if (!strcmp(argv[i], "--pid") && i + 1 < argc) opt_pid = strtoul(argv[++i], NULL, 0);
        else if (n < 8)                            a[n++] = argv[i];
        else { fprintf(stderr, "igame-led: too many arguments\n"); return 2; }
    }

    if (n == 0 || !strcmp(a[0], "help") || !strcmp(a[0], "-h") || !strcmp(a[0], "--help")) {
        fputs(USAGE, stdout);
        return n == 0 ? 2 : 0;
    }

    if (!strcmp(a[0], "mb")) {
        if (n < 2) { fputs(USAGE, stderr); return 2; }

        if (!strcmp(a[1], "list"))
            return cmd_mb_list();
        if (!strcmp(a[1], "off"))
            return cmd_mb_off();
        if (!strcmp(a[1], "set") && n >= 3) {
            uint8_t r, g, b;
            if (parse_color(a[2], &r, &g, &b) < 0) {
                fprintf(stderr, "igame-led: bad colour '%s'\n", a[2]);
                return 2;
            }
            return cmd_mb_set(r, g, b);
        }
        if (!strcmp(a[1], "raw") && n >= 3)
            return cmd_mb_raw(a[2]);
    }

    if (!strcmp(a[0], "off"))
        return cmd_mb_off();

    fputs(USAGE, stderr);
    return 2;
}
