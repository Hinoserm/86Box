/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          A keyboard that reads from a named pipe.
 *
 *          The emulator normally takes its keystrokes from whatever the
 *          host's window system hands it, which is fine for a person and
 *          useless for anything automatic: a machine running headless
 *          has no window to focus and nothing to type into it. This is
 *          the other way in. It opens a FIFO and turns what is written
 *          there into key presses, so a script can drive the guest by
 *          echoing into a file.
 *
 *          Text is sent as itself. Anything that is not a printable
 *          character is named between braces -- {F1}, {ENTER}, {ESC},
 *          {UP} -- and a brace is escaped by doubling it. A line is not
 *          required; bytes are taken as they arrive.
 *
 * Authors: Michael Pratte, <mpratte@makefox.group>
 *
 *          Copyright 2026 Michael Pratte.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#ifndef _WIN32
#    include <fcntl.h>
#    include <sys/stat.h>
#    include <unistd.h>
#    include <errno.h>
#endif
#define HAVE_STDARG_H
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/timer.h>
#include <86box/plat.h>
#include <86box/keyboard.h>
#include <86box/plat_unused.h>
#include <ctype.h>

#ifdef ENABLE_KBD_PIPE_LOG
int kbd_pipe_do_log = ENABLE_KBD_PIPE_LOG;

static void
kbd_pipe_log(const char *fmt, ...)
{
    va_list ap;

    if (kbd_pipe_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define kbd_pipe_log(fmt, ...)
#endif

/* How long a key is held down, and how long between keys. Fast enough not
   to be tedious, slow enough that firmware polling a status port at its
   own pace does not miss one. */
#define KBD_PIPE_HOLD  30000.0
#define KBD_PIPE_GAP   30000.0

#define KBD_PIPE_QUEUE 256

typedef struct kbd_pipe_t {
    pc_timer_t timer;
    int        fd;
    char       path[1024];

    /* Scan codes waiting to go in, each with the state to set. */
    uint16_t queue[KBD_PIPE_QUEUE];
    uint8_t  down[KBD_PIPE_QUEUE];
    uint8_t  head;
    uint8_t  tail;

    char    pending[64]; /* a brace name being collected */
    uint8_t pending_len;
    uint8_t in_brace;
} kbd_pipe_t;

/* The printable characters, as scan code and whether shift is needed. The
   table is the US layout because that is what the firmware of the period
   assumes it is talking to. */
static const uint16_t kbd_pipe_ascii[128] = {
    // clang-format off
    [0x08] = 0x0e, [0x09] = 0x0f, [0x0a] = 0x1c, [0x0d] = 0x1c, [0x1b] = 0x01,
    [' ']  = 0x39, ['-']  = 0x0c, ['=']  = 0x0d, ['[']  = 0x1a, [']']  = 0x1b,
    ['\\'] = 0x2b, [';']  = 0x27, ['\''] = 0x28, ['`']  = 0x29, [',']  = 0x33,
    ['.']  = 0x34, ['/']  = 0x35,
    ['1']  = 0x02, ['2']  = 0x03, ['3']  = 0x04, ['4']  = 0x05, ['5']  = 0x06,
    ['6']  = 0x07, ['7']  = 0x08, ['8']  = 0x09, ['9']  = 0x0a, ['0']  = 0x0b,
    ['a']  = 0x1e, ['b']  = 0x30, ['c']  = 0x2e, ['d']  = 0x20, ['e']  = 0x12,
    ['f']  = 0x21, ['g']  = 0x22, ['h']  = 0x23, ['i']  = 0x17, ['j']  = 0x24,
    ['k']  = 0x25, ['l']  = 0x26, ['m']  = 0x32, ['n']  = 0x31, ['o']  = 0x18,
    ['p']  = 0x19, ['q']  = 0x10, ['r']  = 0x13, ['s']  = 0x1f, ['t']  = 0x14,
    ['u']  = 0x16, ['v']  = 0x2f, ['w']  = 0x11, ['x']  = 0x2d, ['y']  = 0x15,
    ['z']  = 0x2c,
    // clang-format on
};

/* What a shifted character is, so that a capital or a symbol arrives as
   shift plus the unshifted key rather than as a scan code of its own. */
static char
kbd_pipe_unshift(char c)
{
    static const char *shifted = "~!@#$%^&*()_+{}|:\"<>?";
    static const char *plain   = "`1234567890-=[]\\;',./";
    const char        *p       = strchr(shifted, c);

    if ((c >= 'A') && (c <= 'Z'))
        return (char) (c - 'A' + 'a');
    if (p != NULL)
        return plain[p - shifted];
    return 0;
}

static const struct {
    const char *name;
    uint16_t    scan;
} kbd_pipe_named[] = {
    // clang-format off
    { "ENTER",  0x1c   }, { "RETURN", 0x1c   }, { "ESC",    0x01   },
    { "TAB",    0x0f   }, { "BKSP",   0x0e   }, { "SPACE",  0x39   },
    { "F1",     0x3b   }, { "F2",     0x3c   }, { "F3",     0x3d   },
    { "F4",     0x3e   }, { "F5",     0x3f   }, { "F6",     0x40   },
    { "F7",     0x41   }, { "F8",     0x42   }, { "F9",     0x43   },
    { "F10",    0x44   }, { "F11",    0x57   }, { "F12",    0x58   },
    { "UP",     0xe048 }, { "DOWN",   0xe050 }, { "LEFT",   0xe04b },
    { "RIGHT",  0xe04d }, { "HOME",   0xe047 }, { "END",    0xe04f },
    { "PGUP",   0xe049 }, { "PGDN",   0xe051 }, { "INS",    0xe052 },
    { "DEL",    0xe053 }, { "CTRL",   0x1d   }, { "ALT",    0x38   },
    { "SHIFT",  0x2a   },
    /* The keypad, which setup screens of the period often want instead of
       the arrows proper. */
    { "KP0",    0x52   }, { "KP1",    0x4f   }, { "KP2",    0x50   },
    { "KP3",    0x51   }, { "KP4",    0x4b   }, { "KP5",    0x4c   },
    { "KP6",    0x4d   }, { "KP7",    0x47   }, { "KP8",    0x48   },
    { "KP9",    0x49   }, { "KPPLUS", 0x4e   }, { "KPMINUS", 0x4a  },
    { NULL,     0      }
    // clang-format on
};

static void
kbd_pipe_push(kbd_pipe_t *dev, uint16_t scan, int down)
{
    uint8_t next = (uint8_t) ((dev->head + 1) % KBD_PIPE_QUEUE);

    if (next == dev->tail) /* full; dropping is better than blocking */
        return;

    dev->queue[dev->head] = scan;
    dev->down[dev->head]  = (uint8_t) down;
    dev->head             = next;
}

/* One key, pressed and released, with whatever modifier it needs. */
static void
kbd_pipe_key(kbd_pipe_t *dev, uint16_t scan, int shift)
{
    if (shift)
        kbd_pipe_push(dev, 0x2a, 1);
    kbd_pipe_push(dev, scan, 1);
    kbd_pipe_push(dev, scan, 0);
    if (shift)
        kbd_pipe_push(dev, 0x2a, 0);
}

static void
kbd_pipe_named_key(kbd_pipe_t *dev, const char *name)
{
    char   upper[64];
    int    hold = 0;
    int    rel  = 0;
    size_t i;

    /* A name may be prefixed to hold the key down or let it up, so that
       combinations can be written: {+CTRL}{DEL}{-CTRL}. */
    if ((name[0] == '+') || (name[0] == '-')) {
        hold = (name[0] == '+');
        rel  = (name[0] == '-');
        name++;
    }

    for (i = 0; (i < (sizeof(upper) - 1)) && name[i]; i++)
        upper[i] = (char) toupper((unsigned char) name[i]);
    upper[i] = '\0';

    for (i = 0; kbd_pipe_named[i].name != NULL; i++) {
        if (!strcmp(upper, kbd_pipe_named[i].name)) {
            uint16_t scan = kbd_pipe_named[i].scan;

            if (hold)
                kbd_pipe_push(dev, scan, 1);
            else if (rel)
                kbd_pipe_push(dev, scan, 0);
            else
                kbd_pipe_key(dev, scan, 0);
            return;
        }
    }

    kbd_pipe_log("kbd_pipe: no key called %s\n", upper);
}

static void
kbd_pipe_char(kbd_pipe_t *dev, char c)
{
    char plain;

    if (dev->in_brace) {
        if (c == '}') {
            dev->pending[dev->pending_len] = '\0';
            kbd_pipe_named_key(dev, dev->pending);
            dev->in_brace    = 0;
            dev->pending_len = 0;
        } else if (dev->pending_len < (sizeof(dev->pending) - 1))
            dev->pending[dev->pending_len++] = c;
        return;
    }

    if (c == '{') {
        dev->in_brace    = 1;
        dev->pending_len = 0;
        return;
    }

    plain = kbd_pipe_unshift(c);
    if (plain != 0) {
        kbd_pipe_key(dev, kbd_pipe_ascii[(uint8_t) plain & 0x7f], 1);
        return;
    }

    if (((uint8_t) c < 128) && (kbd_pipe_ascii[(uint8_t) c] != 0))
        kbd_pipe_key(dev, kbd_pipe_ascii[(uint8_t) c], 0);
}

static void
kbd_pipe_callback(void *priv)
{
    kbd_pipe_t *dev = (kbd_pipe_t *) priv;

    timer_on_auto(&dev->timer, KBD_PIPE_GAP);

#ifndef _WIN32
    /* Take whatever has been written since last time. The pipe is opened
       without blocking, so an empty one costs nothing. */
    if (dev->fd >= 0) {
        char    buf[256];
        ssize_t got;

        while ((got = read(dev->fd, buf, sizeof(buf))) > 0) {
            for (ssize_t i = 0; i < got; i++)
                kbd_pipe_char(dev, buf[i]);
        }
    }
#endif

    /* One state change per tick: a key held for a moment, then let up. */
    if (dev->tail != dev->head) {
        uint16_t scan = dev->queue[dev->tail];
        uint8_t  down = dev->down[dev->tail];

        dev->tail = (uint8_t) ((dev->tail + 1) % KBD_PIPE_QUEUE);
        keyboard_input(down, scan);
        kbd_pipe_log("kbd_pipe: %s %04x\n", down ? "down" : "up", scan);
    }
}

static void *
kbd_pipe_init(UNUSED(const device_t *info))
{
    kbd_pipe_t *dev = (kbd_pipe_t *) calloc(1, sizeof(kbd_pipe_t));

    dev->fd = -1;

#ifndef _WIN32
    /* The pipe lives beside the machine it types into. */
    snprintf(dev->path, sizeof(dev->path), "%skeyboard.pipe", usr_path);

    if (mkfifo(dev->path, 0666) && (errno != EEXIST)) {
        pclog("Keyboard pipe: cannot make %s: %s\n", dev->path,
              strerror(errno));
    }

    /* Read and write, so that the read end never sees end of file when no
       writer happens to be attached. */
    dev->fd = open(dev->path, O_RDWR | O_NONBLOCK);
    if (dev->fd < 0) {
        pclog("Keyboard pipe: cannot open %s: %s\n", dev->path,
              strerror(errno));
    } else
        pclog("Keyboard pipe: %s\n", dev->path);
#endif

    timer_add(&dev->timer, kbd_pipe_callback, dev, 0);
    timer_on_auto(&dev->timer, KBD_PIPE_GAP);

    return dev;
}

static void
kbd_pipe_close(void *priv)
{
    kbd_pipe_t *dev = (kbd_pipe_t *) priv;

#ifndef _WIN32
    if (dev->fd >= 0)
        close(dev->fd);
    if (dev->path[0])
        unlink(dev->path);
#endif

    free(dev);
}

const device_t kbd_pipe_device = {
    .name          = "Keyboard pipe",
    .internal_name = "kbd_pipe",
    .flags         = 0,
    .local         = 0,
    .init          = kbd_pipe_init,
    .close         = kbd_pipe_close,
    .reset         = NULL,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = NULL
};
