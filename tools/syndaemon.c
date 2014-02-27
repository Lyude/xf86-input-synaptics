/*
 * Copyright © 2003-2004 Peter Osterlund
 * Copyright © 2014 Stephen Chandler Paul
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all copies
 * and that both that copyright notice and this permission notice
 * appear in supporting documentation, and that the name of Red Hat
 * not be used in advertising or publicity pertaining to distribution
 * of the software without specific, written prior permission.  Red
 * Hat makes no representations about the suitability of this software
 * for any purpose.  It is provided "as is" without express or implied
 * warranty.
 *
 * THE AUTHORS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN
 * NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Authors:
 *      Peter Osterlund (petero2@telia.com)
 *      Stephen Chandler Paul (thatslyude@gmail.com)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/XInput.h>
#ifdef HAVE_X11_EXTENSIONS_RECORD_H
#include <X11/Xproto.h>
#include <X11/extensions/record.h>
#endif                          /* HAVE_X11_EXTENSIONS_RECORD_H */

#include <X11/extensions/sync.h>
#define DEFAULT_POINTING_STICK "TPPS/2 IBM TrackPoint"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <poll.h>
#include <time.h>

#include "synaptics-properties.h"

enum TouchpadState {
    TouchpadOn = 0,
    TouchpadOff = 1,
    TappingOff = 2,
    ClickOnly = 3,
    InvalidState = -1
};

enum TouchpadIdleMode {
    KeyboardIdle = 0,
    PStickIdle = 1,
};

typedef struct {
    int x;
    int y;
} CursorPosition;

static Bool tp_state[2];
static int ignore_modifier_combos;
static int ignore_modifier_keys;
static int background;
static const char *pid_file;
static Display *display;
static XDevice *dev;
static Atom touchpad_off_prop;
static enum TouchpadState previous_state;
static enum TouchpadState idle_state[2];
static double idle_time;
static Bool use_xrecord;
static Bool monitor_keyboard;
static int verbose;

Bool monitor_pstick = False;
static int sync_event;
static int pstick_id; /* trackpoint device */
static const char * pstick_name;
static XSyncAlarm pstick_leave_idle_alarm;
static XSyncAlarm pstick_enter_idle_alarm;

#define KEYMAP_SIZE 32
static unsigned char keyboard_mask[KEYMAP_SIZE];

static void
usage(void)
{
    fprintf(stderr,
            "Usage: syndaemon [-i idle-time] [-m poll-delay] [-d] [-t [off|tapping|click-only]] [-T [off|tapping|click-only]] [-k] [-K] [-M] [-R] [-P [pointing-stick]]\n");
    fprintf(stderr,
            "  -i How many seconds to wait after the last key press before\n");
    fprintf(stderr, "     enabling the touchpad. (default is 2.0s)\n");
    fprintf(stderr, "  -m How many milli-seconds to wait until next poll.\n");
    fprintf(stderr, "     (default is 200ms)\n");
    fprintf(stderr, "  -d Start as a daemon, i.e. in the background.\n");
    fprintf(stderr, "  -p Create a pid file with the specified name.\n");
    fprintf(stderr,
            "  -t Disable state.\n"
            "     'off' for disabling the touchpad entirely, \n"
            "     'tapping' for disabling tapping and scrolling only,\n"
            "     'click-only' for disabling everything but physical clicks.\n");
    fprintf(stderr,
            "  -T Disable state.\n"
            "     Same as -t, applies to pointing stick monitoring.\n");
    fprintf(stderr,
            "  -M Enables monitoring of the keyboard (default).\n");
    fprintf(stderr,
            "  -P Pointing Stick.\n"
            "     Enables monitoring of the pointing stick (disables keyboard \n"
            "     monitoring unless -R or -M is also specified)\n");
    fprintf(stderr,
            "  -k Ignore modifier keys when monitoring keyboard activity.\n");
    fprintf(stderr, "  -K Like -k but also ignore Modifier+Key combos.\n");
    fprintf(stderr, "  -R Use the XRecord extension.\n");
    fprintf(stderr, "  -v Print diagnostic messages.\n");
    fprintf(stderr, "  -? Show this help message.\n");
    exit(1);
}

static void
store_current_touchpad_state(void)
{
    Atom real_type;
    int real_format;
    unsigned long nitems, bytes_after;
    unsigned char *data;

    if ((XGetDeviceProperty(display, dev, touchpad_off_prop, 0, 1, False,
                            XA_INTEGER, &real_type, &real_format, &nitems,
                            &bytes_after, &data) == Success) &&
        (real_type != None)) {
        previous_state = data[0];
    }
}

static void
set_touchpad_state(enum TouchpadState new_state) {
    unsigned char data = new_state;

    XChangeDeviceProperty(display, dev, touchpad_off_prop, XA_INTEGER, 8,
                          PropModeReplace, &data, 1);
    XFlush(display);
}

static void
update_touchpad_state() {
    static Bool last_tp_state[2];
    enum TouchpadState new_state;

    if (memcmp(&last_tp_state, &tp_state, sizeof(tp_state)) == 0)
        return;

    /* Decide on the idle state to use */
    if (tp_state[PStickIdle]) {
        if (last_tp_state[PStickIdle])
            goto nothing_to_update;

        if (!last_tp_state[KeyboardIdle])
            store_current_touchpad_state();

        if (verbose)
            printf("Switching to pointing stick mode.\n");

        new_state = idle_state[PStickIdle];
    }
    else if (tp_state[KeyboardIdle]) {
        if (!last_tp_state[PStickIdle])
            store_current_touchpad_state();

        if (verbose)
            printf("Switching to keyboard mode.\n");

        new_state = idle_state[KeyboardIdle];
    }
    else {
        if (verbose)
            printf("Enabling touchpad.\n");

        new_state = previous_state;
    }

    set_touchpad_state(new_state);
nothing_to_update:
    memcpy(&last_tp_state, &tp_state, sizeof(tp_state));
}

static void
signal_handler(int signum)
{
    set_touchpad_state(previous_state);

    if (pid_file)
        unlink(pid_file);
    kill(getpid(), signum);
}

static void
install_signal_handler(void)
{
    static int signals[] = {
        SIGHUP, SIGINT, SIGQUIT, SIGILL, SIGTRAP, SIGABRT,
        SIGBUS, SIGFPE, SIGUSR1, SIGSEGV, SIGUSR2, SIGPIPE,
        SIGALRM, SIGTERM,
#ifdef SIGPWR
        SIGPWR
#endif
    };
    int i;
    struct sigaction act;
    sigset_t set;

    sigemptyset(&set);
    act.sa_handler = signal_handler;
    act.sa_mask = set;
#ifdef SA_ONESHOT
    act.sa_flags = SA_ONESHOT;
#else
    act.sa_flags = 0;
#endif

    for (i = 0; i < sizeof(signals) / sizeof(int); i++) {
        if (sigaction(signals[i], &act, NULL) == -1) {
            perror("sigaction");
            exit(2);
        }
    }
}


static int
setup_xsync(Display * display)
{
    int sync_error;
    int sync_major, sync_minor;

    if (!XSyncQueryExtension(display, &sync_event, &sync_error))
        return 1;

    if (!XSyncInitialize(display, &sync_major, &sync_minor))
        return 1;
    if (verbose)
        printf("X Sync extension version %d.%d\n", sync_major, sync_minor);

    return 0;
}

/**
 * Return non-zero if the keyboard state has changed since the last call.
 */
static int
keyboard_activity(Display * display)
{
    static unsigned char old_key_state[KEYMAP_SIZE];
    unsigned char key_state[KEYMAP_SIZE];
    int i;
    int ret = 0;

    XQueryKeymap(display, (char *) key_state);

    for (i = 0; i < KEYMAP_SIZE; i++) {
        if ((key_state[i] & ~old_key_state[i]) & keyboard_mask[i]) {
            ret = 1;
            break;
        }
    }
    if (ignore_modifier_combos) {
        for (i = 0; i < KEYMAP_SIZE; i++) {
            if (key_state[i] & ~keyboard_mask[i]) {
                ret = 0;
                break;
            }
        }
    }
    for (i = 0; i < KEYMAP_SIZE; i++)
        old_key_state[i] = key_state[i];
    return ret;
}

/**
 * Handles XSync events from the pointing stick
 */
static void
handle_pstick_events(Display * display) {
    while (XPending(display)) {
        XEvent event;
        XSyncAlarmNotifyEvent * alarm_event;

        XNextEvent(display, &event);
        alarm_event = (XSyncAlarmNotifyEvent*)&event;

        if (alarm_event->alarm == pstick_leave_idle_alarm)
            tp_state[PStickIdle] = True;
        else if (alarm_event->alarm == pstick_enter_idle_alarm)
            tp_state[PStickIdle] = False;
        else
            fprintf(stderr, "Got event from unknown alarm %ld\n",
                    alarm_event->alarm);
    }
}

static double
get_time(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1000000.0;
}

static void
main_loop(Display * display, int poll_delay)
{
    double last_activity_time = 0.0;
    double current_time;
    struct pollfd pstick_pollfd;
    int poll_res = 0;

    if (monitor_keyboard)
        keyboard_activity(display);

    if (monitor_pstick) {
        pstick_pollfd.fd = XConnectionNumber(display);
        pstick_pollfd.events = POLLIN;
    }

    for (;;) {
        if (monitor_keyboard) {
            current_time = get_time();

            if (keyboard_activity(display)) {
                tp_state[KeyboardIdle] = True;
                last_activity_time = current_time;
            }
            else if (tp_state[KeyboardIdle] == True) {
                /* If system time goes backwards, touchpad can get locked. Make
                 * sure out last activity wasn't in the future and reset it if
                 * it was. */
                if (last_activity_time > current_time)
                    last_activity_time = 0.0;

                if (current_time > last_activity_time + idle_time)
                    tp_state[KeyboardIdle] = False;
            }
        }

        if (monitor_pstick) {
            if (poll_res != 0)
                handle_pstick_events(display);

            /* Only have a timeout if we're doing plain monitoring for the
             * keyboard too */
            poll_res = poll(&pstick_pollfd, 1,
                            monitor_keyboard ? poll_delay : -1);
        }
        else
            sleep(poll_delay);

        update_touchpad_state();
    }
}

static void
clear_bit(unsigned char *ptr, int bit)
{
    int byte_num = bit / 8;
    int bit_num = bit % 8;

    ptr[byte_num] &= ~(1 << bit_num);
}

static void
setup_keyboard_mask(Display * display, int ignore_modifier_keys)
{
    XModifierKeymap *modifiers;
    int i;

    for (i = 0; i < KEYMAP_SIZE; i++)
        keyboard_mask[i] = 0xff;

    if (ignore_modifier_keys) {
        modifiers = XGetModifierMapping(display);
        for (i = 0; i < 8 * modifiers->max_keypermod; i++) {
            KeyCode kc = modifiers->modifiermap[i];

            if (kc != 0)
                clear_bit(keyboard_mask, kc);
        }
        XFreeModifiermap(modifiers);
    }
}

static XSyncAlarm
setup_device_alarm(Display * display, int device_id, XSyncTestType test_type,
                   int wait_time) {
    XSyncAlarm alarm;
    int alarm_flags;
    XSyncAlarmAttributes attr;
    XSyncValue delta, interval;

    int ncounters;
    char idle_counter_name[20];
    XSyncCounter counter = 0;
    XSyncSystemCounter * counters;

    snprintf(&idle_counter_name[0], sizeof(idle_counter_name),
             "DEVICEIDLETIME %d", device_id);
    counters = XSyncListSystemCounters(display, &ncounters);

    /* Find the idle counter for the trackpoint */
    for (int i = 0; i < ncounters; i++) {
        if (strcmp(counters[i].name, &idle_counter_name[0]) == 0) {
            counter = counters[i].counter;
            break;
        }
    }

    if (counter == 0)
        return 0;

    XSyncFreeSystemCounterList(counters);

    XSyncIntToValue(&delta, 0);
    XSyncIntToValue(&interval, wait_time);

    attr.trigger.counter = counter;
    attr.trigger.test_type = test_type;
    attr.trigger.value_type = XSyncAbsolute;
    attr.trigger.wait_value = interval;
    attr.delta = delta;
    attr.events = True;

    alarm_flags = XSyncCACounter | XSyncCAValueType | XSyncCATestType |
                  XSyncCAValue | XSyncCADelta;

    alarm = XSyncCreateAlarm(display, alarm_flags, &attr);
    return alarm;
}

static int
get_device_id(Display * display, const char * dev_name,
              char * dev_type) {
    XDeviceInfo * info = NULL;
    int ndevices = 0;
    int dev_id = 0;
    Atom dev_type_atom;

    dev_type_atom =
        (dev_type != NULL) ? XInternAtom(display, dev_type, True) : 0;
    info = XListInputDevices(display, &ndevices);

    while (ndevices--) {
        if (info[ndevices].type == dev_type_atom &&
            strcmp(info[ndevices].name, dev_name) == 0) {
            dev_id = info[ndevices].id;
            break;
        }
    }

    XFreeDeviceList(info);
    return dev_id;
}

/* ---- the following code is for using the xrecord extension ----- */
#ifdef HAVE_X11_EXTENSIONS_RECORD_H

#define MAX_MODIFIERS 16

/* used for exchanging information with the callback function */
struct xrecord_callback_results {
    XModifierKeymap *modifiers;
    Bool key_event;
    Bool non_modifier_event;
    KeyCode pressed_modifiers[MAX_MODIFIERS];
};

/* test if the xrecord extension is found */
Bool
check_xrecord(Display * display)
{

    Bool found;
    Status status;
    int major_opcode, minor_opcode, first_error;
    int version[2];

    found = XQueryExtension(display,
                            "RECORD",
                            &major_opcode, &minor_opcode, &first_error);

    status = XRecordQueryVersion(display, version, version + 1);
    if (verbose && status) {
        printf("X RECORD extension version %d.%d\n", version[0], version[1]);
    }
    return found;
}

/* called by XRecordProcessReplies() */
void
xrecord_callback(XPointer closure, XRecordInterceptData * recorded_data)
{

    struct xrecord_callback_results *cbres;
    xEvent *xev;
    int nxev;

    cbres = (struct xrecord_callback_results *) closure;

    if (recorded_data->category != XRecordFromServer) {
        XRecordFreeData(recorded_data);
        return;
    }

    nxev = recorded_data->data_len / 8;
    xev = (xEvent *) recorded_data->data;
    while (nxev--) {

        if ((xev->u.u.type == KeyPress) || (xev->u.u.type == KeyRelease)) {
            int i;
            int is_modifier = 0;

            cbres->key_event = 1;       /* remember, a key was pressed or released. */

            /* test if it was a modifier */
            for (i = 0; i < 8 * cbres->modifiers->max_keypermod; i++) {
                KeyCode kc = cbres->modifiers->modifiermap[i];

                if (kc == xev->u.u.detail) {
                    is_modifier = 1;    /* yes, it is a modifier. */
                    break;
                }
            }

            if (is_modifier) {
                if (xev->u.u.type == KeyPress) {
                    for (i = 0; i < MAX_MODIFIERS; ++i)
                        if (!cbres->pressed_modifiers[i]) {
                            cbres->pressed_modifiers[i] = xev->u.u.detail;
                            break;
                        }
                }
                else {          /* KeyRelease */
                    for (i = 0; i < MAX_MODIFIERS; ++i)
                        if (cbres->pressed_modifiers[i] == xev->u.u.detail)
                            cbres->pressed_modifiers[i] = 0;
                }

            }
            else {
                /* remember, a non-modifier was pressed. */
                cbres->non_modifier_event = 1;
            }
        }

        xev++;
    }

    XRecordFreeData(recorded_data);     /* cleanup */
}

static int
is_modifier_pressed(const struct xrecord_callback_results *cbres)
{
    int i;

    for (i = 0; i < MAX_MODIFIERS; ++i)
        if (cbres->pressed_modifiers[i])
            return 1;

    return 0;
}

void
record_main_loop(Display * display)
{

    struct xrecord_callback_results cbres;
    XRecordContext context;
    XRecordClientSpec cspec = XRecordAllClients;
    Display *dpy_data;
    XRecordRange *range;
    int keyboard_fd = 0;
    int pstick_fd = 0;
    int max_fd;
    struct timeval timeout;
    struct timespec kbd_activity_end;
    fd_set read_fds;

    /* we need an additional data connection. */
    dpy_data = XOpenDisplay(NULL);
    range = XRecordAllocRange();

    range->device_events.first = KeyPress;
    range->device_events.last = KeyRelease;

    context = XRecordCreateContext(dpy_data, 0, &cspec, 1, &range, 1);

    XRecordEnableContextAsync(dpy_data, context, xrecord_callback,
                              (XPointer) & cbres);

    cbres.modifiers = XGetModifierMapping(display);
    /* clear list of modifiers */
    for (int i = 0; i < MAX_MODIFIERS; ++i)
        cbres.pressed_modifiers[i] = 0;

    keyboard_fd = ConnectionNumber(dpy_data);

    if (monitor_pstick)
        pstick_fd = ConnectionNumber(display);

    if (keyboard_fd > pstick_fd)
        max_fd = keyboard_fd;
    else
        max_fd = pstick_fd;

    while (1) {
        int ret;
        int keyboard_event = 0;

        FD_ZERO(&read_fds);

        FD_SET(keyboard_fd, &read_fds);

        if (monitor_pstick)
            FD_SET(pstick_fd, &read_fds);

        ret = select(max_fd + 1, &read_fds, NULL, NULL,
                     tp_state[KeyboardIdle] ? &timeout : NULL
                     /* timeout only required for enabling keyboard */);

        if (FD_ISSET(pstick_fd, &read_fds)) {
            handle_pstick_events(display);

            if (tp_state[KeyboardIdle]) {
                struct timespec current_time;
                clock_gettime(CLOCK_MONOTONIC, &current_time);

                /* Check to make sure the keyboard activity didn't stop while we
                 * were worrying about pointing stick events */
                if (current_time.tv_sec >= kbd_activity_end.tv_sec &&
                    current_time.tv_nsec >= kbd_activity_end.tv_nsec) {
                    tp_state[KeyboardIdle] = False;
                }
                /* Update the keyboard timeout so we still get notified when
                 * keyboard activity ceases */
                else {
                    timeout.tv_sec =
                        kbd_activity_end.tv_sec - current_time.tv_sec;
                    timeout.tv_usec =
                        (kbd_activity_end.tv_nsec - current_time.tv_nsec) /
                        1000;
                }
            }
        }

        if (FD_ISSET(keyboard_fd, &read_fds)) {
            cbres.key_event = 0;
            cbres.non_modifier_event = 0;

            XRecordProcessReplies(dpy_data);

            /* If there are any events left over, they are in error. Drain them
             * from the connection queue so we don't get stuck. */
            while (XEventsQueued(dpy_data, QueuedAlready) > 0) {
                XEvent event;

                XNextEvent(dpy_data, &event);
                fprintf(stderr, "bad event received, major opcode %d\n",
                        event.type);
            }

            if (!ignore_modifier_keys && cbres.key_event) {
                keyboard_event = 1;
            }

            if (cbres.non_modifier_event &&
                !(ignore_modifier_combos && is_modifier_pressed(&cbres))) {
                keyboard_event = 1;
            }
        }

        if (keyboard_event) {
            /* adjust the enable_time */
            timeout.tv_sec = (int) idle_time;
            timeout.tv_usec =
                (idle_time - (double) timeout.tv_sec) * 1.e6;

            /* Knowing when the end of the activity period for the keyboard
             * only matters if we could get interrupted before a select()
             * timeout by pstick events
             */
            if (monitor_pstick) {
                clock_gettime(CLOCK_MONOTONIC, &kbd_activity_end);

                kbd_activity_end.tv_sec += timeout.tv_sec;
                kbd_activity_end.tv_nsec += timeout.tv_usec * 1000;

                // Normalize the tv_usec value
                if (kbd_activity_end.tv_nsec >= 1.e9) {
                    kbd_activity_end.tv_sec++;
                    kbd_activity_end.tv_nsec -= 1.e9;
                }
            }

            tp_state[KeyboardIdle] = True;
        }

        if (ret == 0) /* timeout => enable event */
            tp_state[KeyboardIdle] = False;

        update_touchpad_state();
    }

    XFreeModifiermap(cbres.modifiers);
}
#endif                          /* HAVE_X11_EXTENSIONS_RECORD_H */

static XDevice *
dp_get_device(Display * dpy)
{
    XDevice *dev = NULL;
    XDeviceInfo *info = NULL;
    int ndevices = 0;
    Atom touchpad_type = 0;
    Atom *properties = NULL;
    int nprops = 0;
    int error = 0;

    touchpad_type = XInternAtom(dpy, XI_TOUCHPAD, True);
    touchpad_off_prop = XInternAtom(dpy, SYNAPTICS_PROP_OFF, True);
    info = XListInputDevices(dpy, &ndevices);

    while (ndevices--) {
        if (info[ndevices].type == touchpad_type) {
            dev = XOpenDevice(dpy, info[ndevices].id);
            if (!dev) {
                fprintf(stderr, "Failed to open device '%s'.\n",
                        info[ndevices].name);
                error = 1;
                goto unwind;
            }

            properties = XListDeviceProperties(dpy, dev, &nprops);
            if (!properties || !nprops) {
                fprintf(stderr, "No properties on device '%s'.\n",
                        info[ndevices].name);
                error = 1;
                goto unwind;
            }

            while (nprops--) {
                if (properties[nprops] == touchpad_off_prop)
                    break;
            }
            if (nprops < 0) {
                fprintf(stderr, "No synaptics properties on device '%s'.\n",
                        info[ndevices].name);
                error = 1;
                goto unwind;
            }

            break;              /* Yay, device is suitable */
        }
    }

 unwind:
    XFree(properties);
    XFreeDeviceList(info);
    if (!dev)
        fprintf(stderr, "Unable to find a synaptics device.\n");
    else if (error && dev) {
        XCloseDevice(dpy, dev);
        dev = NULL;
    }
    return dev;
}

static enum TouchpadState
parse_idle_mode(const char * state) {
    if (state[0] == '-')
        return 0;
    else if (strcmp(state, "off") == 0)
        return TouchpadOff;
    else if (strcmp(state, "tapping") == 0)
        return TappingOff;
    else if (strcmp(state, "click-only") == 0)
        return ClickOnly;
    else
        return InvalidState;
}

int
main(int argc, char *argv[])
{
    int poll_delay = 200;
    int c;

    idle_time = 2.0;

    idle_state[KeyboardIdle] = TouchpadOff;
    idle_state[PStickIdle] = ClickOnly;

    /* Parse command line parameters */
    while ((c = getopt(argc, argv, ":i:m:dp:MkKR?v")) != EOF) {
        switch (c) {
        case 'i':
            idle_time = atof(optarg);
            break;
        case 'm':
            poll_delay = atoi(optarg);
            break;
        case 'd':
            background = 1;
            break;
        case 'p':
            pid_file = optarg;
            break;
        case 'k':
            ignore_modifier_keys = 1;
            break;
        case 'K':
            ignore_modifier_combos = 1;
            ignore_modifier_keys = 1;
            break;
        case 'R':
            use_xrecord = True;
            break;
        case 'M':
            monitor_keyboard = True;
            break;
        case 'v':
            verbose = 1;
            break;
        case '?':
            if (optopt == 'P') {
                monitor_pstick = True;
                if (optind < argc) {
                    if (argv[optind][0] == '-')
                        pstick_name = DEFAULT_POINTING_STICK;
                    else
                        pstick_name = &argv[optind][0];
                }
                else
                    pstick_name = DEFAULT_POINTING_STICK;
            }
            else if (optopt == 'T') {
                idle_state[PStickIdle] = parse_idle_mode(argv[optind]);
                if (idle_state[PStickIdle] == InvalidState)
                    usage();
            }
            else if (optopt == 't') {
                idle_state[KeyboardIdle] = parse_idle_mode(argv[optind]);
                if (idle_state[KeyboardIdle] == InvalidState)
                    usage();
            }
            else {
                usage();
                break;
            }
            break;
        default:
            usage();
            break;
        }
    }
    if (idle_time <= 0.0)
        usage();

    /* If the keyboard was not selected and no alternate devices to monitor were
     * selected, default to the keyboard */
    if (!monitor_pstick)
        monitor_keyboard = True;

    /* Open a connection to the X server */
    display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "Can't open display.\n");
        exit(2);
    }

    if (!(dev = dp_get_device(display)))
        exit(2);

    /* Install a signal handler to restore synaptics parameters on exit */
    install_signal_handler();

    if (background) {
        pid_t pid;

        if ((pid = fork()) < 0) {
            perror("fork");
            exit(3);
        }
        else if (pid != 0)
            exit(0);

        /* Child (daemon) is running here */
        setsid();               /* Become session leader */
        chdir("/");             /* In case the file system gets unmounted */
        umask(0);               /* We don't want any surprises */
        if (pid_file) {
            FILE *fd = fopen(pid_file, "w");

            if (!fd) {
                perror("Can't create pid file");
	    }
        }
    }
    if (idle_time <= 0.0)
        usage();

    /* If the keyboard was not selected and no alternate devices to monitor were
     * selected, default to the keyboard */
    if (!monitor_pstick)
        monitor_keyboard = True;

    /* Open a connection to the X server */
    display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "Can't open display.\n");
        exit(2);
    }

    if (!(dev = dp_get_device(display)))
        exit(2);

    /* Install a signal handler to restore synaptics parameters on exit */
    install_signal_handler();

    if (background) {
        pid_t pid;

        if ((pid = fork()) < 0) {
            perror("fork");
            exit(3);
        }
        else if (pid != 0)
            exit(0);

        /* Child (daemon) is running here */
        setsid();               /* Become session leader */
        chdir("/");             /* In case the file system gets unmounted */
        umask(0);               /* We don't want any surprises */
        if (pid_file) {
            FILE *fd = fopen(pid_file, "w");

            if (!fd) {
                perror("Can't create pid file");
                exit(3);
            }
            fprintf(fd, "%d\n", getpid());
            fclose(fd);
        }
    }

    store_current_touchpad_state();

    memset(&tp_state, 0, sizeof(tp_state));

    if (monitor_pstick) {
        pstick_id = get_device_id(display, pstick_name, XI_MOUSE);
        if (pstick_id == 0) {
            fprintf(stderr, "Couldn't find pointing stick %s\n", pstick_name);
            exit(6);
        }

        pstick_leave_idle_alarm =
            setup_device_alarm(display, pstick_id, XSyncNegativeTransition,
                               idle_time * 1000);
        pstick_enter_idle_alarm =
            setup_device_alarm(display, pstick_id, XSyncPositiveTransition,
                               idle_time * 1000);

        if (setup_xsync(display) != 0) {
            fprintf(stderr, "Monitoring of pointing stick requested, but "
                    "XSync could not be initialized.\n");
            exit(5);
        }
    }

#ifdef HAVE_X11_EXTENSIONS_RECORD_H
    if (use_xrecord) {
        if (!check_xrecord(display)) {
            fprintf(stderr, "Use of XRecord requested, but failed to "
                    " initialize.\n");
            exit(4);
        }
        record_main_loop(display);
    }
    else
#endif                          /* HAVE_X11_EXTENSIONS_RECORD_H */
    {
        if (monitor_keyboard)
            setup_keyboard_mask(display, ignore_modifier_keys);

        /* Run the main loop */
        main_loop(display, poll_delay);
    }
    return 0;
}

/* vim: set noexpandtab tabstop=8 shiftwidth=4: */
