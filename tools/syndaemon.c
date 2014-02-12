/*
 * Copyright © 2003-2004 Peter Osterlund
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
 *      Stephen Chandler "Lyude" Paul (thatslyude@gmail.com)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/XInput.h>
#include <X11/extensions/sync.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/stat.h>

#include "synaptics-properties.h"

enum TouchpadState {
    TouchpadOn = 0,
    TouchpadOff = 1,
    TappingOff = 2,
    TrackpointMode = 3
};

static Bool pad_disabled;
static Bool trackpoint_mode;
    /* internal flags, these do not correspond to device state */ ;
static int ignore_modifier_combos;
static int ignore_modifier_keys;
static Bool monitor_trackpoint;
static Bool monitor_keyboard;
static int background;
static const char *pid_file;
static const char *trackpoint_name;
static Display *display;
static XDevice *dev;
static int trackpoint_id; /* trackpoint device */
static Atom touchpad_off_prop;
static enum TouchpadState previous_state;
static enum TouchpadState disable_state = TouchpadOff;
static int verbose;
static int sync_event;

#define KEYMAP_SIZE 32
static unsigned char keyboard_mask[KEYMAP_SIZE];

#define TP_DEAD_ZONE 3

static void
usage(void)
{
    fprintf(stderr,
            "Usage: syndaemon [-i idle-time] [-b] [-T trackpoint-name]\n");
    fprintf(stderr,
            "                 [-I trackpoint-idle-time] [-m poll-delay]\n");
    fprintf(stderr,
            "                 [-d] [-t] [-k]\n");
    fprintf(stderr,
            "  -i How many seconds to wait after the last key press before\n");
    fprintf(stderr, "     enabling the touchpad. (default is 2.0s)\n");
    fprintf(stderr, "  -m How many milli-seconds to wait until next poll.\n");
    fprintf(stderr, "     (default is 200ms)\n");
    fprintf(stderr, "  -d Start as a daemon, i.e. in the background.\n");
    fprintf(stderr, "  -p Create a pid file with the specified name.\n");
    fprintf(stderr,
            "  -t Only disable tapping and scrolling, not mouse movements.\n");
    fprintf(stderr,
            "  -k Ignore modifier keys when monitoring keyboard activity.\n");
    fprintf(stderr, "  -K Like -k but also ignore Modifier+Key combos.\n");
    fprintf(stderr, "  -B Monitor keyboard activity (default)\n");
    fprintf(stderr,
        "  -T Monitor activity of trackpoint with the specified name.\n");
    fprintf(stderr, "  -I How many seconds to wait after the last\n");
    fprintf(stderr, "     trackpoint movement before deactivating trackpoint \n");
    fprintf(stderr, "     mode.\n");
    fprintf(stderr, "  -R Use the XRecord extension for monitoring the "
                    "     keyboard.\n");
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

/**
 * Toggle touchpad enabled/disabled state, decided by value.
 */
static void
toggle_touchpad(Bool enable)
{
    unsigned char data;

    if (pad_disabled && enable) {
        data = previous_state;
        pad_disabled = False;
        trackpoint_mode = False;
        if (verbose)
            printf("Touchpad enabled.\n");
    }
    else if (!enable &&
             previous_state != disable_state && previous_state != TouchpadOff) {
        /* If we're switching from TrackPoint mode to our disable state, don't
         * get rid of the last state */
        if (!pad_disabled)
            store_current_touchpad_state();
        /* If we're not switching from TrackPoint mode and the touchpad was
         * already enabled, then the user probably manually changed it, ignore
         * this event */
        else if (!trackpoint_mode)
            return;

        pad_disabled = True;
        data = disable_state;
        if (verbose)
            printf("Touchpad disabled.\n");
    }
    else
        return;

    /* This potentially overwrites a different client's setting, but ... */
    XChangeDeviceProperty(display, dev, touchpad_off_prop, XA_INTEGER, 8,
                          PropModeReplace, &data, 1);
    XFlush(display);
}

/**
 * Toggle TrackPoint mode on the touchpad
 */
static void
toggle_trackpoint()
{
    unsigned char data;

    /* Don't do anything if the user manually disabled the touchpad or manually
     * put the touchpad into trackpoint mode */
    if ((!pad_disabled && previous_state == TouchpadOff) ||
        (!trackpoint_mode && previous_state == TrackpointMode))
        return;

    if (!pad_disabled)
        store_current_touchpad_state();
    pad_disabled = True;
    trackpoint_mode = True;
    data = TrackpointMode;
    if (verbose)
        printf("Switching to TrackPoint mode\n");

    XChangeDeviceProperty(display, dev, touchpad_off_prop, XA_INTEGER, 8,
                          PropModeReplace, &data, 1);
    XFlush(display);
}

static void
signal_handler(int signum)
{
    toggle_touchpad(True);

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


typedef struct {
    int x;
    int y;
} CursorPosition;

static int
setup_xsync(Display * display)
{
    int sync_error;
    int sync_major, sync_minor;

    if (!XSyncQueryExtension(display, &sync_event, &sync_error))
        return 1;

    XSyncInitialize(display, &sync_major, &sync_minor);
    return 0;
}

/**
 * Return non-zero if the trackpoint state has changed since the last call.
 */
static int
trackpoint_activity(Display * display)
{
    static CursorPosition old_tppos = {-1, -1};
    CursorPosition *tppos;

    int i;
    int ret = 0;
    int diff_x = 0, diff_y = 0;

    XDeviceState   *state;
    XInputClass    *data;
    XValuatorState *val_state;

    /*state = XQueryDeviceState(display, tp_dev);*/ //TODO: UNCOMMENT THIS

    /* Get current Trackpoint coordinates  */
    if (state) {
        data = state->data;
        for(i = 0; i < state->num_classes; ++i) {
            if (data->class == ValuatorClass) {
                val_state = (XValuatorState *) data;
                tppos = (CursorPosition *) val_state->valuators;
            }
            data = (XInputClass *) ((char *) data + data->length);    
        }
        XFreeDeviceState(state);
    }

    /* Compare coordinates */
    if (old_tppos.x >= 0) {
        diff_x = ~(old_tppos.x - tppos->x)+1;
        diff_y = ~(old_tppos.y - tppos->y)+1;
        if (diff_x > TP_DEAD_ZONE || diff_y > TP_DEAD_ZONE) {
            ret = 1;
        }
    }

    /* Copy over old values */
    old_tppos.x = tppos->x;
    old_tppos.y = tppos->y;

    return ret;
}

/**
 * Return non-zero if the keyboard state has changed since the last call.
 * Only used when xsync or xrecord are not being used
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

static double
get_time(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1000000.0;
}

static void
main_loop(Display * display, double keyboard_idle_time,
          double trackpoint_idle_time, int poll_delay)
{
    double current_time;
    double idle_time;
    double last_activity_time;

    if (monitor_keyboard)
        keyboard_activity(display);

    for (;;) {
        current_time = get_time();

        if (monitor_trackpoint && trackpoint_activity(display)) {
            toggle_trackpoint();
            last_activity_time = current_time;
            idle_time = trackpoint_idle_time;
        }
        else if (monitor_keyboard && keyboard_activity(display)) {
            toggle_touchpad(False);
            last_activity_time = current_time;
            idle_time = keyboard_idle_time;
        }
        else if (pad_disabled) {
            /* If system time goes backwards, touchpad can get locked. Make sure
             * out last activity wasn't in the future and reset it if it was. */
            if (last_activity_time > current_time)
                last_activity_time = 0.0;

            if (current_time > last_activity_time + idle_time) {
                toggle_touchpad(True);
                continue;
            }
        }
        
        usleep(poll_delay);
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

static int
get_device_id_by_name(Display * display, const char * dev_name,
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

static XDevice *
dp_get_device(Display * display)
{
    XDevice *dev = NULL;
    XDeviceInfo *info = NULL;
    int ndevices = 0;
    Atom touchpad_type = 0;
    Atom *properties = NULL;
    int nprops = 0;
    int error = 0;

    touchpad_type = XInternAtom(display, XI_TOUCHPAD, True);
    touchpad_off_prop = XInternAtom(display, SYNAPTICS_PROP_OFF, True);
    info = XListInputDevices(display, &ndevices);

    while (ndevices--) {
        if (info[ndevices].type == touchpad_type) {
            dev = XOpenDevice(display, info[ndevices].id);
            if (!dev) {
                fprintf(stderr, "Failed to open device '%s'.\n",
                        info[ndevices].name);
                error = 1;
                goto unwind;
            }

            properties = XListDeviceProperties(display, dev, &nprops);
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
        XCloseDevice(display, dev);
        dev = NULL;
    }
    return dev;
}

static XSyncAlarm
setup_device_alarm(Display * display, int device_id, int wait_time) {
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
    attr.trigger.test_type = XSyncNegativeTransition;
    attr.trigger.value_type = XSyncAbsolute;
    attr.trigger.wait_value = interval;
    attr.delta = delta;
    attr.events = True;

    alarm_flags = XSyncCACounter | XSyncCAValueType | XSyncCATestType |
                  XSyncCAValue | XSyncCADelta;

    alarm = XSyncCreateAlarm(display, alarm_flags, &attr);
    return alarm;
}

static Status change_alarm_trigger(Display * display, XSyncAlarm alarm,
                                   XSyncTestType test_type, int wait_time)
{
    XSyncAlarmAttributes attr;
    XSyncValue interval;
    Bool status;

    XSyncQueryAlarm(display, alarm, &attr);

    XSyncIntToValue(&interval, wait_time);
    
    attr.trigger.test_type = test_type;
    attr.trigger.wait_value = interval;

    status = XSyncChangeAlarm(display, alarm, XSyncCACounter | XSyncCAValueType |
                              XSyncCATestType | XSyncCAValue | XSyncCADelta,
                              &attr);
    XFlush(display);
    return status;
}

// TODO: Set this up to open the device the ID passed to it corresponds to.
static XDevice *
trackpoint_get_device(Display * display)
{
    XDevice *dev = NULL;
    int error = 0;
    
    dev = XOpenDevice(display, trackpoint_id);

    if (!dev)
        fprintf(stderr, "Unable to find a trackpoint device.\n");
    else if (error && dev) {
        XCloseDevice(display, dev);
        dev = NULL;
    }
    return dev;
}

int
xsync_loop(Display * display, double keyboard_idle_time,
           double trackpoint_idle_time)
{
    XSyncAlarm keyboard_alarm;
    XSyncAlarm trackpoint_alarm;

    if (monitor_trackpoint)
        trackpoint_alarm = setup_device_alarm(display, trackpoint_id,
                                              trackpoint_idle_time);
    if (monitor_keyboard) {
        int dev_id = get_device_id_by_name(display, "Virtual core keyboard", 0);
        keyboard_alarm = setup_device_alarm(display, dev_id, keyboard_idle_time);
    }

    for (;;) {
        XEvent ev;
        XSyncAlarmNotifyEvent * alarm_event;

        XNextEvent(display, &ev);

        if (ev.type != sync_event + XSyncAlarmNotify)
            exit(0);

        alarm_event = (XSyncAlarmNotifyEvent *)&ev;

        if (alarm_event->alarm == trackpoint_alarm) {
            if (pad_disabled && trackpoint_mode) {
                toggle_touchpad(True);
                change_alarm_trigger(display, trackpoint_alarm,
                                     XSyncNegativeTransition,
                                     trackpoint_idle_time);
            }
            else {
                /* Change the alarm so it goes off when the trackpoint is idle
                 * again */
                toggle_trackpoint();
                change_alarm_trigger(display, trackpoint_alarm,
                                     XSyncPositiveTransition,
                                     trackpoint_idle_time);

                /* Reset the keyboard alarm if we're monitoring that too */
                if (monitor_keyboard)
                    change_alarm_trigger(display, keyboard_alarm,
                                         XSyncNegativeTransition,
                                         keyboard_idle_time);
            }
        }
        else if (alarm_event->alarm == keyboard_alarm) {
            if (pad_disabled && !trackpoint_mode) {
                toggle_touchpad(True);
                change_alarm_trigger(display, keyboard_alarm,
                                     XSyncNegativeTransition,
                                     keyboard_idle_time);
            }
            else {
                if (ignore_modifier_keys) {
                    unsigned char key_state[KEYMAP_SIZE];

                    XQueryKeymap(display, (char *) key_state);

                    for (int i = 0; i < KEYMAP_SIZE; i++) {
                        if (key_state[i] & ~keyboard_mask[i])
                            continue;
                    }
                }

                toggle_touchpad(False);
                /* Change the alarm so it goes off when the trackpoint is idle
                 * again */
                change_alarm_trigger(display, keyboard_alarm,
                                     XSyncPositiveTransition,
                                     keyboard_idle_time);

                /* Reset the TrackPoint alarm if we're monitoring that too */
                if (monitor_trackpoint)
                    change_alarm_trigger(display, trackpoint_alarm,
                                         XSyncNegativeTransition,
                                         trackpoint_idle_time);
            }
        }
    }
}

int
main(int argc, char *argv[])
{
    double keyboard_idle_time = 2000.0;
    double trackpoint_idle_time = 2000.0;
    int poll_delay = 200000;    /* 200 ms */
    int c;

    /* Parse command line parameters */
    while ((c = getopt(argc, argv, "i:m:dtp:kKBRT:I:?v")) != EOF) {
        switch (c) {
        case 'i':
            keyboard_idle_time = atof(optarg);
            break;
        case 'm':
            poll_delay = atoi(optarg) * 1000;
            break;
        case 'd':
            background = 1;
            break;
        case 't':
            disable_state = TappingOff;
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
        case 'B':
            monitor_keyboard = True;
            break;
        case 'T':
            monitor_trackpoint = True;
            trackpoint_name = optarg;
            break;
        case 'I':
            trackpoint_idle_time = atof(optarg);
            break;
        case 'R':
            /* Does nothing, kept for historical reasons */
            break;
        case 'v':
            verbose = 1;
            break;
        case '?':
        default:
            usage();
            break;
        }
    }
    if (keyboard_idle_time <= 0.0 ||
        trackpoint_idle_time <= 0.0)
        usage();

    /* If neither keyboard tracking or trackpoint tracking were explicitly
     * enabled, default to just keyboard tracking
     */
    if (!monitor_trackpoint)
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

    pad_disabled = False;
    trackpoint_mode = False;
    store_current_touchpad_state();

    if (monitor_keyboard && ignore_modifier_keys)
        setup_keyboard_mask(display, ignore_modifier_keys);

    /* Setup the trackpoint if we're supposed to monitor it */
    if (monitor_trackpoint) {
        trackpoint_id = get_device_id_by_name(display, trackpoint_name,
                                              XI_MOUSE);
        if (trackpoint_id == 0) {
            fprintf(stderr,
                    "Can't a TrackPoint to monitor. The TrackPoint name you "
                    "gave might be incorrect, or may not be a TrackPoint.\n");
            exit(2);
        }
    }
                    

    /* Try to use xsync */
    if (setup_xsync(display) == 0)
        xsync_loop(display, keyboard_idle_time, trackpoint_idle_time);
    else {
        fprintf(stderr,
                "Warning: Could not setup XSync, falling back to normal "
                "polling.\n");
        main_loop(display, keyboard_idle_time, trackpoint_idle_time,
                  poll_delay);
    }

    return 0;
}

/* vim: set expandtab tabstop=8 shiftwidth=4: */
