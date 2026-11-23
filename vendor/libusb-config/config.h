/* libusb's config.h, written by hand instead of autotools (./configure) for the three OSes Patchisserie ships on.
 * Mirrors what configure.ac would define for each backend: macOS (IOKit), Linux (usbfs + netlink hotplug, no udev) and
 * Windows (MinGW-w64; WinUSB and UsbDk, no hotplug). Logging is on, debug logging off. */

/* Hidden symbols: the plugin is built with -fvisibility=hidden so Rack's own libusb, if any, never binds to ours. */
#define DEFAULT_VISIBILITY
#define ENABLE_LOGGING 1
#define PRINTF_FORMAT(a, b) __attribute__ ((__format__ (__printf__, a, b)))

#ifdef _WIN32

#define PLATFORM_WINDOWS 1
#define _WIN32_WINNT 0x0600 /* _WIN32_WINNT_VISTA, as configure.ac */

#else /* POSIX */

#define PLATFORM_POSIX 1
#define HAVE_NFDS_T 1
#define HAVE_SYS_TIME_H 1
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#ifdef __APPLE__

#define HAVE_PTHREAD_THREADID_NP 1

#elif defined(__linux__)

#define HAVE_ASM_TYPES_H 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_PIPE2 1
#define HAVE_EVENTFD 1
#define HAVE_TIMERFD 1
#define HAVE_PTHREAD_CONDATTR_SETCLOCK 1

#else
#error "libusb-config: unsupported OS"
#endif

#endif
