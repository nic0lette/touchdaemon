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
 *      Expanded for non-synaptic devices: David Sommer
 *      Update for non-synaptic devices: Nicole Borrelli
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/XInput.h>
#ifdef HAVE_XRECORD
#include <X11/Xproto.h>
#include <X11/extensions/record.h>
#endif				/* HAVE_XRECORD */

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <cstring>

static Bool pad_disabled
    /* internal flag, this does not correspond to device state */ ;
static int ignore_modifier_combos;
static int ignore_modifier_keys;
static int background;
static const char *pid_file;
static Display *display;
static XDevice *dev;
static Atom touchpad_off_prop;
static int previous_state;
static int disable_state = 0;

#define KEYMAP_SIZE 32
static unsigned char keyboard_mask[KEYMAP_SIZE];

static void usage(void)
{
	fprintf(stderr,
		"Usage: syndaemon [-i idle-time] [-m poll-delay] [-d] [-t] [-k] -c Command\n");
	fprintf(stderr,
		"  -i How many seconds to wait after the last key press before\n");
	fprintf(stderr, "     enabling the touchpad. (default is 2.0s)\n");
	fprintf(stderr,
		"  -m How many milli-seconds to wait until next poll.\n");
	fprintf(stderr, "     (default is 200ms)\n");
	fprintf(stderr, "  -d Start as a daemon, i.e. in the background.\n");
	fprintf(stderr, "  -p Create a pid file with the specified name.\n");
	fprintf(stderr,
		"  -k Ignore modifier keys when monitoring keyboard activity.\n");
	fprintf(stderr, "  -K Like -k but also ignore Modifier+Key combos.\n");
	fprintf(stderr, "  -D Specify a device ID to toggle.\n");
	fprintf(stderr,
		"     (By default will use first touchpad, mouse or trackball found).\n");
	exit(1);
}

static void store_current_touchpad_state(void)
{
	Atom real_type;
	int real_format;
	unsigned long nitems, bytes_after;
	unsigned char *data;

	if ((XGetDeviceProperty(display, dev, touchpad_off_prop, 0, 1, False,
				XA_INTEGER, &real_type, &real_format, &nitems,
				&bytes_after, &data) == Success)
	    && (real_type != None)) {
		previous_state = data[0];
	}
}

/**
 * Toggle touchpad enabled/disabled state, decided by value.
 */
static void toggle_touchpad(Bool enable)
{
	char data;
	if (pad_disabled && enable) {
		data = (char)previous_state;
		pad_disabled = False;
		if (!background)
			printf("Enable\n");
	} else if (!pad_disabled && !enable && previous_state != disable_state) {
		store_current_touchpad_state();
		pad_disabled = True;
		data = disable_state;
		if (!background)
			printf("Disable\n");
	} else
		return;

	XChangeDeviceProperty(display, dev, touchpad_off_prop, XA_INTEGER, 8,
			      PropModeReplace, (const unsigned char *)&data, 1);
	XFlush(display);
}

static void signal_handler(int signum)
{
	toggle_touchpad(True);

	if (pid_file)
		unlink(pid_file);
	kill(getpid(), signum);
}

static void install_signal_handler(void)
{
	static int signals[] = {
		SIGHUP, SIGINT, SIGQUIT, SIGILL, SIGTRAP, SIGABRT,
		SIGBUS, SIGFPE, SIGUSR1, SIGSEGV, SIGUSR2, SIGPIPE,
		SIGALRM, SIGTERM,
#ifdef SIGPWR
		SIGPWR
#endif
	};
	unsigned int i;
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

/**
 * Return non-zero if the keyboard state has changed since the last call.
 */
static int keyboard_activity(Display * display)
{
	static unsigned char old_key_state[KEYMAP_SIZE];
	unsigned char key_state[KEYMAP_SIZE];
	int i;
	int ret = 0;

	XQueryKeymap(display, (char *)key_state);

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

static double get_time(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1000000.0;
}

static void main_loop(Display * display, double idle_time, int poll_delay)
{
	double last_activity = 0.0;
	double current_time;

	keyboard_activity(display);

	for (;;) {
		current_time = get_time();
		if (keyboard_activity(display))
			last_activity = current_time;

		if (current_time > last_activity + idle_time) {	/* Enable touchpad */
			toggle_touchpad(True);
		} else {	/* Disable touchpad */
			toggle_touchpad(False);
		}

		usleep(poll_delay);
	}
}

static void clear_bit(unsigned char *ptr, int bit)
{
	int byte_num = bit / 8;
	int bit_num = bit % 8;
	ptr[byte_num] &= ~(1 << bit_num);
}

static void setup_keyboard_mask(Display * display, int ignore_modifier_keys)
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

static XDevice *dp_get_device(Display * dpy, unsigned int dev_id,
			      int have_dev_id)
{
	XDevice *dev = NULL;
	XDeviceInfo *info = NULL;
	int ndevices = 0;
	Atom touchpad_type = 0;
	Atom mouse_type = 0;
	Atom trackball_type = 0;
	Atom device_prop = 0;
	Atom *properties = NULL;
	int nprops = 0;
	int error = 0;

	/* Get the internal Atom ID for these devices */
	touchpad_type = XInternAtom(dpy, XI_TOUCHPAD, True);
	mouse_type = XInternAtom(dpy, XI_MOUSE, True);
	trackball_type = XInternAtom(dpy, XI_TRACKBALL, True);

	/* Get the internal Atom ID for the property */
	device_prop = XInternAtom(dpy, "Device Enabled", True);

	info = XListInputDevices(dpy, &ndevices);

	while (ndevices--) {
		/*
		 * Basically, this mess is the check to see to find the device
		 * to disable.  If no device id was specified, try to guess which
		 * device.  If a device id was specified, we should only accept
		 * that device.
		 */
		if ((!have_dev_id && (info[ndevices].type == touchpad_type
				      || info[ndevices].type == mouse_type
				      || info[ndevices].type == trackball_type))
		    || dev_id == info[ndevices].id) {

			dev = XOpenDevice(dpy, info[ndevices].id);
			if (!dev) {
				fprintf(stderr, "Failed to open device '%s'.\n",
					info[ndevices].name);
				error = 1;
				break;
			}

			properties = XListDeviceProperties(dpy, dev, &nprops);
			if (!properties || !nprops) {
				fprintf(stderr,
					"No properties on device '%s'.\n",
					info[ndevices].name);
				error = 1;
				break;
			}

			while (nprops--) {
				if (device_prop == properties[nprops]) {
					/* Save the Atom ID for later */
					touchpad_off_prop = properties[nprops];
					break;
				}
			}

			if (!nprops) {
				fprintf(stderr,
					"Could not identify enable/disable property on device '%s'.\n",
					info[ndevices].name);
				error = 1;
				break;
			}

			break;	/* Yay, device is suitable */
		}
	}

	XFree(properties);
	XFreeDeviceList(info);
	if (error) {
		if (!have_dev_id) {
			fprintf(stderr, "Unable to find a device.\n");
		} else {
			fprintf(stderr,
				"Unable to find the specified device (id=%d).\n",
				dev_id);
		}

		// Free allocated memory
		if (dev) {
			XCloseDevice(dpy, dev);
			dev = NULL;
		}
	}
	return dev;
}

int main(int argc, char *argv[])
{
	double idle_time = 1.0;	//one second
	int poll_delay = 200000;	/* 200 ms */
	unsigned int dev_id = 0;
        int have_dev_id = 0;
	int c;

	/* Parse command line parameters */
	while ((c = getopt(argc, argv, "i:m:dp:kKD:?")) != EOF) {
		switch (c) {
		case 'i':
			idle_time = atof(optarg);
			break;
		case 'm':
			poll_delay = atoi(optarg) * 1000;
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
		case 'D':
			dev_id = atoi(optarg);
			have_dev_id = 1;
			break;
		default:
			usage();
			break;
		}
	}

	if (idle_time <= 0.0)
		usage();

	/* Open a connection to the X server */
	display = XOpenDisplay(NULL);
	if (!display) {
		fprintf(stderr, "Can't open display.\n");
		exit(2);
	}

	if (!(dev = dp_get_device(display, dev_id, have_dev_id)))
		exit(2);

	/* Install a signal handler to restore synaptics parameters on exit */
	install_signal_handler();

	if (background) {
		pid_t pid;
		if ((pid = fork()) < 0) {
			perror("fork");
			exit(3);
		} else if (pid != 0)
			exit(0);

		/* Child (daemon) is running here */
		setsid();	/* Become session leader */
		chdir("/");	/* In case the file system gets unmounted */
		umask(0);	/* We don't want any surprises */
		if (pid_file) {
			FILE *fd = fopen(pid_file, "w");
			if (!fd) {
				perror("Can't create pid file");
				exit(2);
			}
			fprintf(fd, "%d\n", getpid());
			fclose(fd);
		}
	}

	pad_disabled = False;
	store_current_touchpad_state();

	setup_keyboard_mask(display, ignore_modifier_keys);

	/* Run the main loop */
	main_loop(display, idle_time, poll_delay);
	return 0;
}
