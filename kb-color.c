/*
 * kb-color.c - Gigabyte Aorus 15P keyboard RGB control
 *
 * Build:  gcc -O2 -o kb-color kb-color.c
 * Usage:  kb-color --color red --brightness 100
 *         kb-color -c blue -b 50
 *         kb-color --brightness 30
 *         kb-color --list
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/hidraw.h>
#include <limits.h>
#include <sys/stat.h>

/* Device identification */
#define VENDOR_ID           0x1044
#define PRODUCT_ID          0x7a3b
#define USB_INPUT_IFACE     "input3"        /* RGB control interface on Aorus 15P */

/* sysfs / devfs paths */
#define HIDRAW_SYSFS_DIR    "/sys/class/hidraw"
#define HIDRAW_UEVENT_FMT   "%s/%s/device/uevent"
#define HIDRAW_DEV_FMT      "/dev/%s"
#define UEVENT_HID_ID_FMT   "HID_ID=%04X:%08X:%08X"
#define HID_BUS_USB         0x0003

/* Protocol constants */
#define PKT_SIZE            9
#define PKT_REPORT_ID       0x00
#define PKT_CMD             0x08
#define PKT_BYTE2           0x00
#define PKT_BYTE3           0x01
#define PKT_BYTE4           0x01
#define PKT_BYTE7           0x01
#define PKT_CHECKSUM_INIT   0xff
#define PKT_CHECKSUM_BYTES  8           /* bytes 0-7 are summed for checksum */
#define PKT_BRIGHTNESS_DIV  2           /* UI brightness / 2 = raw value     */

/* Color IDs */
#define COLOR_ID_OFF        0x05        /* firmware "off" state at 0% brightness */
#define COLOR_COUNT         7

/* Brightness range */
#define BRIGHTNESS_MIN      0
#define BRIGHTNESS_MAX      100

/* State file */
#define STATE_DIR_XDG       "XDG_CONFIG_HOME"   /* env var to check first      */
#define STATE_DIR_FALLBACK  ".config"            /* relative to HOME if not set */
#define STATE_APP_DIR       "kb-color"
#define STATE_FILE_NAME     "state"
#define STATE_HOME_FALLBACK "/root"

static const struct { const char *name; uint8_t id; } COLORS[COLOR_COUNT] = {
    { "red",    1 },
    { "green",  2 },
    { "yellow", 3 },
    { "blue",   4 },
    { "orange", 5 },
    { "purple", 6 },
    { "white",  7 },
};

typedef struct { uint8_t color_id; uint8_t brightness; } State;

static const char *state_path(void) {
    static char path[PATH_MAX];
    const char *xdg = getenv(STATE_DIR_XDG);
    if (xdg) {
        snprintf(path, sizeof(path), "%s/%s/%s", xdg, STATE_APP_DIR, STATE_FILE_NAME);
    } else {
        const char *home = getenv("HOME");
        if (!home) home = STATE_HOME_FALLBACK;
        snprintf(path, sizeof(path), "%s/%s/%s/%s", home, STATE_DIR_FALLBACK, STATE_APP_DIR, STATE_FILE_NAME);
    }
    return path;
}

static void ensure_state_dir(void) {
    const char *xdg = getenv(STATE_DIR_XDG);
    char dir[PATH_MAX];
    if (xdg) {
        snprintf(dir, sizeof(dir), "%s/%s", xdg, STATE_APP_DIR);
    } else {
        const char *home = getenv("HOME");
        if (!home) home = STATE_HOME_FALLBACK;
        snprintf(dir, sizeof(dir), "%s/%s/%s", home, STATE_DIR_FALLBACK, STATE_APP_DIR);
    }
    mkdir(dir, 0755);
}

static void save_state(uint8_t color_id, uint8_t brightness) {
    ensure_state_dir();
    FILE *f = fopen(state_path(), "wb");
    if (!f) return;
    State s = { color_id, brightness };
    fwrite(&s, sizeof(s), 1, f);
    fclose(f);
}

static State load_state(void) {
    State s = { COLORS[COLOR_COUNT - 1].id, BRIGHTNESS_MAX }; /* default: white 100% */
    FILE *f = fopen(state_path(), "rb");
    if (!f) return s;
    fread(&s, sizeof(s), 1, f);
    fclose(f);
    return s;
}

static uint8_t color_id_from_name(const char *name) {
    for (int i = 0; i < COLOR_COUNT; i++)
        if (strcmp(COLORS[i].name, name) == 0)
            return COLORS[i].id;
    return 0;
}

static int send_packet(uint8_t color_id, uint8_t brightness) {
    uint8_t pkt[PKT_SIZE];
    uint8_t sum = 0;

    pkt[0] = PKT_REPORT_ID;
    pkt[1] = PKT_CMD;
    pkt[2] = PKT_BYTE2;
    pkt[3] = PKT_BYTE3;
    pkt[4] = PKT_BYTE4;
    pkt[5] = brightness / PKT_BRIGHTNESS_DIV;
    pkt[6] = (brightness > BRIGHTNESS_MIN) ? color_id : COLOR_ID_OFF;
    pkt[7] = PKT_BYTE7;
    for (int i = 0; i < PKT_CHECKSUM_BYTES; i++) sum += pkt[i];
    pkt[8] = PKT_CHECKSUM_INIT - sum;

    DIR *d = opendir(HIDRAW_SYSFS_DIR);
    if (!d) { perror("opendir " HIDRAW_SYSFS_DIR); return -1; }

    struct dirent *e;
    int success = 0;
    while ((e = readdir(d)) && !success) {
        if (e->d_name[0] == '.') continue;

        char uevent[PATH_MAX];
        snprintf(uevent, sizeof(uevent), HIDRAW_UEVENT_FMT, HIDRAW_SYSFS_DIR, e->d_name);
        FILE *f = fopen(uevent, "r");
        if (!f) continue;

        char line[PATH_MAX];
        int match_vid = 0, match_iface = 0;
        while (fgets(line, sizeof(line), f)) {
            unsigned int bus, vid, pid;
            if (sscanf(line, UEVENT_HID_ID_FMT, &bus, &vid, &pid) == 3)
                if (bus == HID_BUS_USB && vid == VENDOR_ID && pid == PRODUCT_ID)
                    match_vid = 1;
            if (strstr(line, USB_INPUT_IFACE))
                match_iface = 1;
        }
        fclose(f);
        if (!match_vid || !match_iface) continue;

        char devpath[PATH_MAX];
        snprintf(devpath, sizeof(devpath), HIDRAW_DEV_FMT, e->d_name);
        int fd = open(devpath, O_RDWR);
        if (fd < 0) continue;

        int ret = ioctl(fd, HIDIOCSFEATURE(PKT_SIZE), pkt);
        close(fd);

        if (ret == PKT_SIZE)
            success = 1;
    }
    closedir(d);

    if (!success) {
        fprintf(stderr, "Failed. Try running as root or check udev rules.\n");
        return -1;
    }
    return 0;
}

static void usage(void) {
    fprintf(stderr,
        "Usage:\n"
        "  kb-color --color <color> [--brightness <0-100>]\n"
        "  kb-color --brightness <0-100>\n"
        "  kb-color --list\n"
        "\nShort forms: -c, -b\n"
        "Colors: red, green, yellow, blue, orange, purple, white\n"
    );
}

int main(int argc, char *argv[]) {
    if (argc < 2) { usage(); return 1; }

    if (strcmp(argv[1], "--list") == 0) {
        for (int i = 0; i < COLOR_COUNT; i++) printf("%s\n", COLORS[i].name);
        return 0;
    }

    State s = load_state();

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--brightness") == 0 || strcmp(argv[i], "-b") == 0) {
            if (++i >= argc) { fprintf(stderr, "Missing brightness value\n"); return 1; }
            int b = atoi(argv[i]);
            if (b < BRIGHTNESS_MIN || b > BRIGHTNESS_MAX) {
                fprintf(stderr, "Brightness must be %d-%d\n", BRIGHTNESS_MIN, BRIGHTNESS_MAX);
                return 1;
            }
            s.brightness = (uint8_t)b;
        } else if (strcmp(argv[i], "--color") == 0 || strcmp(argv[i], "-c") == 0) {
            if (++i >= argc) { fprintf(stderr, "Missing color value\n"); return 1; }
            uint8_t cid = color_id_from_name(argv[i]);
            if (!cid) { fprintf(stderr, "Unknown color '%s'\n", argv[i]); return 1; }
            s.color_id = cid;
        } else {
            fprintf(stderr, "Unknown option '%s'. Use --color and --brightness.\n", argv[i]);
            return 1;
        }
    }

    if (send_packet(s.color_id, s.brightness) < 0) return 1;
    save_state(s.color_id, s.brightness);

    const char *color_name = "?";
    for (int i = 0; i < COLOR_COUNT; i++)
        if (COLORS[i].id == s.color_id) { color_name = COLORS[i].name; break; }
    printf("OK: color=%s brightness=%d%%\n", color_name, s.brightness);
    return 0;
}