/*
 * main.c
 *
 * Copyright (C) 2009-2021 Nikias Bassen <nikias@gmx.li>
 * Copyright (C) 2013-2014 Martin Szulecki <m.szulecki@libimobiledevice.org>
 * Copyright (C) 2009 Hector Martin <hector@marcansoft.com>
 * Copyright (C) 2009 Paul Sladen <libiphone@paul.sladen.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 or version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <getopt.h>
#include <pwd.h>
#include <grp.h>

#include "log.h"
#include "usb.h"
#include "device.h"
#include "client.h"
#include "conf.h"

//#include <android/log.h>

#define DEFAULT_LOCKFILE "/var/run/usbmuxd.pid"

//#define logAnd(...) __android_log_print(ANDROID_LOG_INFO, "libusb-usbmuxd", __VA_ARGS__)


#ifdef __ANDROID__
//static const char *socket_path = "/data/local/tmp/usbmuxd";
static const char *socket_path = "/data/data/com.mtn.move.to.ios.watransfer/files/usbmuxd";
#else
static const char *socket_path = "/var/run/usbmuxd";
#endif

#ifdef __ANDROID__
//static const char *lockfile = "/data/local/tmp/usbmuxd.pid";
static const char *lockfile = "/data/data/com.mtn.move.to.ios.watransfer/files/usbmuxd.pid";
#else
static const char *lockfile = DEFAULT_LOCKFILE;
#endif


// Global state used in other files
int should_exit;
int should_discover;
int use_logfile = 0;
int no_preflight = 0;

// Global state for main.c
static int verbose = 0;
static int foreground = 0;
static int fileDescriptor = 0;
static int drop_privileges = 0;
static const char *drop_user = NULL;
static int opt_disable_hotplug = 0;
static int opt_enable_exit = 0;
static int opt_exit = 0;
static int exit_signal = 0;
static int daemon_pipe;
static const char *listen_addr = NULL;

static int report_to_parent = 0;

static int create_socket(void)
{
	int listenfd;
	const char* socket_addr = socket_path;
	const char* tcp_port;
	char listen_addr_str[256];

	if (listen_addr) {
		socket_addr = listen_addr;
	}
	tcp_port = strrchr(socket_addr, ':');
	if (tcp_port) {
		tcp_port++;
		size_t nlen = tcp_port - socket_addr;
		char* hostname = malloc(nlen);
		struct addrinfo hints;
		struct addrinfo *result, *rp;
		int yes = 1;
		int res;

		strncpy(hostname, socket_addr, nlen-1);
		hostname[nlen-1] = '\0';

		memset(&hints, '\0', sizeof(struct addrinfo));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
		hints.ai_protocol = IPPROTO_TCP;

		res = getaddrinfo(hostname, tcp_port, &hints, &result);
		free(hostname);
		if (res != 0) {
			usbmuxd_log(LL_FATAL, "%s: getaddrinfo() failed: %s\n", __func__, gai_strerror(res));
			
			//logAnd(LL_FATAL, "%s: getaddrinfo() failed: %s\n", __func__, gai_strerror(res));
			
			return -1;
		}

		for (rp = result; rp != NULL; rp = rp->ai_next) {
			listenfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
			if (listenfd == -1) {
				listenfd = -1;
				continue;
			}

			if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (void*)&yes, sizeof(int)) == -1) {
				usbmuxd_log(LL_ERROR, "%s: setsockopt(): %s", __func__, strerror(errno));
				
				//logAnd(LL_ERROR, "%s: setsockopt(): %s", __func__, strerror(errno));
				
				close(listenfd);
				listenfd = -1;
				continue;
			}

#ifdef SO_NOSIGPIPE
			if (setsockopt(listenfd, SOL_SOCKET, SO_NOSIGPIPE, (void*)&yes, sizeof(int)) == -1) {
				usbmuxd_log(LL_ERROR, "%s: setsockopt(): %s", __func__, strerror(errno));
				
				//logAnd(LL_ERROR, "%s: setsockopt(): %s", __func__, strerror(errno));
				
				close(listenfd);
				listenfd = -1;
				continue;
			}
#endif

#if defined(AF_INET6) && defined(IPV6_V6ONLY)
			if (rp->ai_family == AF_INET6) {
				if (setsockopt(listenfd, IPPROTO_IPV6, IPV6_V6ONLY, (void*)&yes, sizeof(int)) == -1) {
					usbmuxd_log(LL_ERROR, "%s: setsockopt() IPV6_V6ONLY: %s", __func__, strerror(errno));
					//logAnd(LL_ERROR, "%s: setsockopt() IPV6_V6ONLY: %s", __func__, strerror(errno));
				}
			}
#endif

			if (bind(listenfd, rp->ai_addr, rp->ai_addrlen) < 0) {
				usbmuxd_log(LL_FATAL, "%s: bind() failed: %s", __func__, strerror(errno));
				//logAnd(LL_FATAL, "%s: bind() failed: %s", __func__, strerror(errno));
				close(listenfd);
				listenfd = -1;
				continue;
			}

			const void *addrdata = NULL;
			if (rp->ai_family == AF_INET) {
				addrdata = &((struct sockaddr_in*)rp->ai_addr)->sin_addr;
			}
#ifdef AF_INET6
			else if (rp->ai_family == AF_INET6) {
				addrdata = &((struct sockaddr_in6*)rp->ai_addr)->sin6_addr;
			}
#endif
			if (addrdata) {
				char* endp = NULL;
				uint16_t listen_port = 0;
				if (rp->ai_family == AF_INET) {
					listen_port = ntohs(((struct sockaddr_in*)rp->ai_addr)->sin_port);
					if (inet_ntop(AF_INET, addrdata, listen_addr_str, sizeof(listen_addr_str)-6)) {
						endp = &listen_addr_str[0] + strlen(listen_addr_str);
					}
				}
#ifdef AF_INET6
				else if (rp->ai_family == AF_INET6) {
					listen_port = ntohs(((struct sockaddr_in6*)rp->ai_addr)->sin6_port);
					listen_addr_str[0] = '[';
					if (inet_ntop(AF_INET6, addrdata, listen_addr_str+1, sizeof(listen_addr_str)-8)) {
						endp = &listen_addr_str[0] + strlen(listen_addr_str);
					}
					if (endp) {
						*endp = ']';
						endp++;
					}
				}
#endif
				if (endp) {
					sprintf(endp, ":%u", listen_port);
				}
			}
			break;
		}
		freeaddrinfo(result);
		if (listenfd == -1) {
			usbmuxd_log(LL_FATAL, "%s: Failed to create listening socket", __func__);
			//logAnd(LL_FATAL, "%s: Failed to create listening socket", __func__);
			return -1;
		}
	} else {
		#ifdef __ANDROID__
			//struct sockaddr_in bind_addr;
		#else
			//struct sockaddr_un bind_addr;
		#endif
		
		struct sockaddr_un bind_addr;

		if (strcmp(socket_addr, socket_path) != 0) {
			struct stat fst;
			if (stat(socket_addr, &fst) == 0) {
				if (!S_ISSOCK(fst.st_mode)) {
					usbmuxd_log(LL_FATAL, "FATAL: File '%s' already exists and is not a socket file. Refusing to continue.", socket_addr);
					//logAnd(LL_FATAL, "FATAL: File '%s' already exists and is not a socket file. Refusing to continue.", socket_addr);
					return -1;
				}
			}
		}

		if (unlink(socket_addr) == -1 && errno != ENOENT) {
			usbmuxd_log(LL_FATAL, "%s: unlink(%s) failed: %s", __func__, socket_addr, strerror(errno));
			//logAnd(LL_FATAL, "%s: unlink(%s) failed: %s", __func__, socket_addr, strerror(errno));
			return -1;
		}

		
		
		#ifdef __ANDROID__
			//listenfd = socket(AF_INET, SOCK_STREAM, 0);
		#else
			//listenfd = socket(AF_UNIX, SOCK_STREAM, 0);
		#endif
		
		listenfd = socket(AF_UNIX, SOCK_STREAM, 0);
		
		if (listenfd == -1) {
			usbmuxd_log(LL_FATAL, "socket() failed: %s", strerror(errno));
			//logAnd(LL_FATAL, "socket() failed: %s", strerror(errno));
			return -1;
		}

		bzero(&bind_addr, sizeof(bind_addr));
		
		
		#ifdef __ANDROID__
			//bind_addr.sin_family = AF_INET;
			//bind_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
			//bind_addr.sin_port = htons(USBMUXD_SOCKET_PORT);
		#else
			//bind_addr.sun_family = AF_UNIX;
			//strncpy(bind_addr.sun_path, socket_addr, sizeof(bind_addr.sun_path));
		#endif
		
		
		bind_addr.sun_family = AF_UNIX;
		strncpy(bind_addr.sun_path, socket_addr, sizeof(bind_addr.sun_path));
		
		#ifdef __ANDROID__
			//bind_addr.sin_path[sizeof(bind_addr.sin_path) - 1] = '\0';
		#else
			//bind_addr.sun_path[sizeof(bind_addr.sun_path) - 1] = '\0';
		#endif
		
		bind_addr.sun_path[sizeof(bind_addr.sun_path) - 1] = '\0';

		if (bind(listenfd, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) != 0) {
			usbmuxd_log(LL_FATAL, "bind() failed: %s", strerror(errno));
			//logAnd(LL_FATAL, "bind() failed: %s", strerror(errno));
			return -1;
		}
		chmod(socket_addr, 0666);

		snprintf(listen_addr_str, sizeof(listen_addr_str), "%s", socket_addr);
	}

	int flags = fcntl(listenfd, F_GETFL, 0);
	if (flags < 0) {
		usbmuxd_log(LL_FATAL, "ERROR: Could not get flags for socket");
		//logAnd(LL_FATAL, "ERROR: Could not get flags for socket");
	} else {
		if (fcntl(listenfd, F_SETFL, flags | O_NONBLOCK) < 0) {
			usbmuxd_log(LL_FATAL, "ERROR: Could not set socket to non-blocking");
			//logAnd(LL_FATAL, "ERROR: Could not set socket to non-blocking");
		}
	}

	// Start listening
	if (listen(listenfd, 256) != 0) {
		usbmuxd_log(LL_FATAL, "listen() failed: %s", strerror(errno));
		//logAnd(LL_FATAL, "listen() failed: %s", strerror(errno));
		return -1;
	}

	usbmuxd_log(LL_INFO, "Listening on %s", listen_addr_str);
	//logAnd(LL_INFO, "Listening on %s", listen_addr_str);

	return listenfd;
}

static void handle_signal(int sig)
{
	if (sig != SIGUSR1 && sig != SIGUSR2) {
		usbmuxd_log(LL_NOTICE,"Caught signal %d, exiting", sig);
		//logAnd(LL_NOTICE,"Caught signal %d, exiting", sig);
		should_exit = 1;
	} else {
		if(opt_enable_exit) {
			if (sig == SIGUSR1) {
				usbmuxd_log(LL_INFO, "Caught SIGUSR1, checking if we can terminate (no more devices attached)...");
				//logAnd(LL_INFO, "Caught SIGUSR1, checking if we can terminate (no more devices attached)...");
				if (device_get_count(1) > 0) {
					// we can't quit, there are still devices attached.
					usbmuxd_log(LL_NOTICE, "Refusing to terminate, there are still devices attached. Kill me with signal 15 (TERM) to force quit.");
					//logAnd(LL_NOTICE, "Refusing to terminate, there are still devices attached. Kill me with signal 15 (TERM) to force quit.");
				} else {
					// it's safe to quit
					should_exit = 1;
				}
			} else if (sig == SIGUSR2) {
				usbmuxd_log(LL_INFO, "Caught SIGUSR2, scheduling device discovery");
				//logAnd(LL_INFO, "Caught SIGUSR2, scheduling device discovery");
				should_discover = 1;
			}
		} else {
			usbmuxd_log(LL_INFO, "Caught SIGUSR1/2 but this instance was not started with \"--enable-exit\", ignoring.");
			//logAnd(LL_INFO, "Caught SIGUSR1/2 but this instance was not started with \"--enable-exit\", ignoring.");
		}
	}
}

static void set_signal_handlers(void)
{
	struct sigaction sa;
	sigset_t set;

	// Mask all signals we handle. They will be unmasked by ppoll().
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGQUIT);
	sigaddset(&set, SIGTERM);
	sigaddset(&set, SIGUSR1);
	sigaddset(&set, SIGUSR2);
	sigprocmask(SIG_SETMASK, &set, NULL);

	memset(&sa, 0, sizeof(struct sigaction));
	sa.sa_handler = handle_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGUSR2, &sa, NULL);
}

#ifndef HAVE_PPOLL
static int ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *timeout, const sigset_t *sigmask)
{
	usbmuxd_log(LL_FLOOD, "ppoll 1111");
	int ready;
	sigset_t origmask;
	int to = timeout->tv_sec*1000 + timeout->tv_nsec/1000000;
usbmuxd_log(LL_FLOOD, "ppoll 2222");
	sigprocmask(LL_FLOOD, sigmask, &origmask);
	usbmuxd_log(LL_FLOOD, "ppoll 3333");
	ready = poll(fds, nfds, to);
	usbmuxd_log(LL_FLOOD, "ppoll 4444");
	sigprocmask(SIG_SETMASK, &origmask, NULL);
	usbmuxd_log(LL_FLOOD, "ppoll 5555");

	return ready;
}
#endif

static int main_loop(int listenfd)
{
	int to, cnt, i, dto;
	struct fdlist pollfds;
	struct timespec tspec;

	sigset_t empty_sigset;
	sigemptyset(&empty_sigset); // unmask all signals

	fdlist_create(&pollfds);
	while(!should_exit) {
		usbmuxd_log(LL_FLOOD, "main_loop iteration");
		//logAnd(LL_FLOOD, "main_loop iteration");
		to = usb_get_timeout();
		usbmuxd_log(LL_FLOOD, "USB timeout is %d ms", to);
		//logAnd(LL_FLOOD, "USB timeout is %d ms", to);
		dto = device_get_timeout();
		usbmuxd_log(LL_FLOOD, "Device timeout is %d ms", dto);
		//logAnd(LL_FLOOD, "Device timeout is %d ms", dto);
		if(dto < to)
			to = dto;

		fdlist_reset(&pollfds);
		fdlist_add(&pollfds, FD_LISTEN, listenfd, POLLIN);
		usb_get_fds(&pollfds);
		client_get_fds(&pollfds);
		usbmuxd_log(LL_FLOOD, "fd count is %d", pollfds.count);
		//logAnd(LL_FLOOD, "fd count is %d", pollfds.count);

		tspec.tv_sec = to / 1000;
		usbmuxd_log(LL_FLOOD, "main_loop iteration aaaa");
		tspec.tv_nsec = (to % 1000) * 1000000;
		usbmuxd_log(LL_FLOOD, "main_loop iteration bbbb");
		cnt = ppoll(pollfds.fds, pollfds.count, &tspec, &empty_sigset);
		usbmuxd_log(LL_FLOOD, "poll() returned %d", cnt);
		//logAnd(LL_FLOOD, "poll() returned %d", cnt);
		if(cnt == -1) {
			usbmuxd_log(LL_FLOOD, "cnt 1111");
			if(errno == EINTR) {
				usbmuxd_log(LL_FLOOD, "cnt 1111 aaaaa");
				if(should_exit) {
					usbmuxd_log(LL_FLOOD, "Event processing interrupted");
					//logAnd(LL_INFO, "Event processing interrupted");
					break;
				}
				if(should_discover) {
					should_discover = 0;
					usbmuxd_log(LL_FLOOD, "Device discovery triggered");
					//logAnd(LL_INFO, "Device discovery triggered");
					usb_discover();
				}
			}
		} else if(cnt == 0) {
			usbmuxd_log(LL_FLOOD, "cnt 00000");
			if(usb_process() < 0) {
				usbmuxd_log(LL_FLOOD, "usb_process() failed");
				//logAnd(LL_FATAL, "usb_process() failed");
				fdlist_free(&pollfds);
				return -1;
			}
			usbmuxd_log(LL_FLOOD, "cnt 00000 aaaaa");
			device_check_timeouts();
		} else {
			usbmuxd_log(LL_FLOOD, "cnt else");
			int done_usb = 0;
			for(i=0; i<pollfds.count; i++) {
				usbmuxd_log(LL_FLOOD, "cnt else i %d",i);
				if(pollfds.fds[i].revents) {
					usbmuxd_log(LL_FLOOD, "cnt else 1111");
					if(!done_usb && pollfds.owners[i] == FD_USB) {
						usbmuxd_log(LL_FLOOD, "cnt else 22222");
						if(usb_process() < 0) {
							usbmuxd_log(LL_FLOOD, "usb_process() failed");
							//logAnd(LL_FATAL, "usb_process() failed");
							fdlist_free(&pollfds);
							return -1;
						}
						done_usb = 1;
					}
					if(pollfds.owners[i] == FD_LISTEN) {
						usbmuxd_log(LL_FLOOD, "cnt else 3333");
						if(client_accept(listenfd) < 0) {
							usbmuxd_log(LL_FLOOD, "client_accept() failed");
							//logAnd(LL_FATAL, "client_accept() failed");
							fdlist_free(&pollfds);
							return -1;
						}
					}
					if(pollfds.owners[i] == FD_CLIENT) {
						usbmuxd_log(LL_FLOOD, "cnt else 444444");
						client_process(pollfds.fds[i].fd, pollfds.fds[i].revents);
					}
				}
			}
		}
	}
	usbmuxd_log(LL_FLOOD, "cnt 55555");
	fdlist_free(&pollfds);
	return 0;
}

/**
 * make this program run detached from the current console
 */
static int daemonize(void)
{
	pid_t pid;
	pid_t sid;
	int pfd[2];
	int res;

	// already a daemon
	if (getppid() == 1)
		return 0;

	if((res = pipe(pfd)) < 0) {
		usbmuxd_log(LL_FATAL, "pipe() failed.");
		//logAnd(LL_FATAL, "pipe() failed.");
		return res;
	}

	pid = fork();
	if (pid < 0) {
		usbmuxd_log(LL_FATAL, "fork() failed.");
		//logAnd(LL_FATAL, "fork() failed.");
		return pid;
	}

	if (pid > 0) {
		// exit parent process
		int status;
		close(pfd[1]);

		if((res = read(pfd[0],&status,sizeof(int))) != sizeof(int)) {
			fprintf(stderr, "usbmuxd: ERROR: Failed to get init status from child, check syslog for messages.\n");
			exit(1);
		}
		if(status != 0)
			fprintf(stderr, "usbmuxd: ERROR: Child process exited with error %d, check syslog for messages.\n", status);
		exit(status);
	}
	// At this point we are executing as the child process
	// but we need to do one more fork

	daemon_pipe = pfd[1];
	close(pfd[0]);
	report_to_parent = 1;

	// Create a new SID for the child process
	sid = setsid();
	if (sid < 0) {
		usbmuxd_log(LL_FATAL, "setsid() failed.");
		//logAnd(LL_FATAL, "setsid() failed.");
		return -1;
	}

	pid = fork();
	if (pid < 0) {
		usbmuxd_log(LL_FATAL, "fork() failed (second).");
		//logAnd(LL_FATAL, "fork() failed (second).");
		return pid;
	}

	if (pid > 0) {
		// exit parent process
		close(daemon_pipe);
		exit(0);
	}

	// Change the current working directory.
	if ((chdir("/")) < 0) {
		usbmuxd_log(LL_FATAL, "chdir() failed");
		//logAnd(LL_FATAL, "chdir() failed");
		return -2;
	}
	// Redirect standard files to /dev/null
	if (!freopen("/dev/null", "r", stdin)) {
		usbmuxd_log(LL_FATAL, "Redirection of stdin failed.");
		//logAnd(LL_FATAL, "Redirection of stdin failed.");
		return -3;
	}
	if (!freopen("/dev/null", "w", stdout)) {
		usbmuxd_log(LL_FATAL, "Redirection of stdout failed.");
		//logAnd(LL_FATAL, "Redirection of stdout failed.");
		return -3;
	}

	return 0;
}

static int notify_parent(int status)
{
	int res;

	report_to_parent = 0;
	if ((res = write(daemon_pipe, &status, sizeof(int))) != sizeof(int)) {
		usbmuxd_log(LL_FATAL, "Could not notify parent!");
		//logAnd(LL_FATAL, "Could not notify parent!");
		if(res >= 0)
			return -2;
		else
			return res;
	}
	close(daemon_pipe);
	if (!freopen("/dev/null", "w", stderr)) {
		usbmuxd_log(LL_FATAL, "Redirection of stderr failed.");
		//logAnd(LL_FATAL, "Redirection of stderr failed.");
		return -1;
	}
	return 0;
}

static void usage()
{
	
	usbmuxd_log(LL_NOTICE, "usage %s", "11111");
	
	printf("Usage: %s [OPTIONS]\n", PACKAGE_NAME);
	printf("\n");
	printf("Expose a socket to multiplex connections from and to iOS devices.\n");
	printf("\n");
	printf("OPTIONS:\n");
	printf("  -h, --help\t\tPrint this message.\n");
	printf("  -v, --verbose\t\tBe verbose (use twice or more to increase).\n");
	printf("  -f, --foreground\tDo not daemonize (implies one -v).\n");
	printf("  -U, --user USER\tChange to this user after startup (needs USB privileges).\n");
	printf("  -n, --disable-hotplug\tDisables automatic discovery of devices on hotplug.\n");
	printf("                       \tStarting another instance will trigger discovery instead.\n");
	printf("  -z, --enable-exit\tEnable \"--exit\" request from other instances and exit\n");
	printf("                   \tautomatically if no device is attached.\n");
	printf("  -p, --no-preflight\tDisable lockdownd preflight on new device.\n");
#ifdef HAVE_UDEV
	printf("  -u, --udev\t\tRun in udev operation mode (implies -n and -z).\n");
#endif
#ifdef HAVE_SYSTEMD
	printf("  -s, --systemd\t\tRun in systemd operation mode (implies -z and -f).\n");
#endif
	printf("  -S, --socket ADDR:PORT | PATH   Specify source ADDR and PORT or a UNIX\n");
	printf("            \t\tsocket PATH to use for the listening socket.\n");
	printf("            \t\tDefault: %s\n", socket_path);
	printf("  -P, --pidfile PATH\tSpecify a different location for the pid file, or pass\n");
	printf("            \t\tNONE to disable. Default: %s\n", DEFAULT_LOCKFILE);
	printf("  -x, --exit\t\tNotify a running instance to exit if there are no devices\n");
	printf("            \t\tconnected (sends SIGUSR1 to running instance) and exit.\n");
	printf("  -X, --force-exit\tNotify a running instance to exit even if there are still\n");
	printf("                  \tdevices connected (always works) and exit.\n");
	printf("  -l, --logfile=LOGFILE\tLog (append) to LOGFILE instead of stderr or syslog.\n");
	printf("  -V, --version\t\tPrint version information and exit.\n");
	printf("\n");
	printf("Homepage:    <" PACKAGE_URL ">\n");
	printf("Bug Reports: <" PACKAGE_BUGREPORT ">\n");
}

static void parse_opts(int argc, char **argv)
{
	
	usbmuxd_log(LL_INFO, "parse_opts %s", "11111");
	
	static struct option longopts[] = {
		{"help", no_argument, NULL, 'h'},
		{"foreground", no_argument, NULL, 'f'},
		{"fileDescriptor", required_argument, NULL, 'c'},
		{"verbose", no_argument, NULL, 'v'},
		{"user", required_argument, NULL, 'U'},
		{"disable-hotplug", no_argument, NULL, 'n'},
		{"enable-exit", no_argument, NULL, 'z'},
		{"no-preflight", no_argument, NULL, 'p'},
#ifdef HAVE_UDEV
		{"udev", no_argument, NULL, 'u'},
#endif
#ifdef HAVE_SYSTEMD
		{"systemd", no_argument, NULL, 's'},
#endif
		{"socket", required_argument, NULL, 'S'},
		{"pidfile", required_argument, NULL, 'P'},
		{"exit", no_argument, NULL, 'x'},
		{"force-exit", no_argument, NULL, 'X'},
		{"logfile", required_argument, NULL, 'l'},
		{"version", no_argument, NULL, 'V'},
		{NULL, 0, NULL, 0}
	};
	int c;

#ifdef HAVE_SYSTEMD
	const char* opts_spec = "hfvVuU:xXsnzl:pS:P:";
#elif HAVE_UDEV
	const char* opts_spec = "hfvVuU:xXnzl:pS:P:";
#else
	const char* opts_spec = "hfvVU:xXnzl:pS:P:";
#endif

	while (1) {
		c = getopt_long(argc, argv, opts_spec, longopts, (int *) 0);
		if (c == -1) {
			break;
		}
		
		//usbmuxd_log(LL_INFO, "parse_opts optarg %s", "geldi");
		//usbmuxd_log(LL_INFO, "parse_opts optarg %d", atoi(optarg));

		switch (c) {
		case 'h':
			usage();
			exit(0);
		case 'f':
			foreground = 1;
			break;
		case 'c':
			if (!*optarg || *optarg == '-') {
				usbmuxd_log(LL_FATAL, "ERROR: fileDescriptor requires an argument");
				exit(2);
			}
				printf("Usage: %s \n", optarg);
				printf("Usage: %d \n", atoi(optarg));
			usbmuxd_log(LL_INFO, "parse_opts fileDescriptor str %s", optarg);	
			usbmuxd_log(LL_INFO, "parse_opts fileDescriptor %d", atoi(optarg));	
			fileDescriptor = atoi(optarg);
			break;		
		case 'v':
			++verbose;
			break;
		case 'V':
			printf("%s\n", PACKAGE_STRING);
			exit(0);
		case 'U':
			drop_privileges = 1;
			drop_user = optarg;
			break;
		case 'p':
			no_preflight = 1;
			break;
#ifdef HAVE_UDEV
		case 'u':
			opt_disable_hotplug = 1;
			opt_enable_exit = 1;
			break;
#endif
#ifdef HAVE_SYSTEMD
		case 's':
			opt_enable_exit = 1;
			foreground = 1;
			break;
#endif
		case 'n':
			opt_disable_hotplug = 1;
			break;
		case 'z':
			opt_enable_exit = 1;
			break;
		case 'S':
			if (!*optarg || *optarg == '-') {
				usbmuxd_log(LL_FATAL, "ERROR: --socket requires an argument");
				//logAnd(LL_FATAL, "ERROR: --socket requires an argument");
				usage();
				exit(2);
			}
			listen_addr = optarg;
			break;
		case 'P':
			if (!*optarg || *optarg == '-') {
				usbmuxd_log(LL_FATAL, "ERROR: --pidfile requires an argument");
				//logAnd(LL_FATAL, "ERROR: --pidfile requires an argument");
				usage();
				exit(2);
			}
			if (!strcmp(optarg, "NONE")) {
				lockfile = NULL;
			} else {
				lockfile = optarg;
			}
			break;
		case 'x':
			opt_exit = 1;
			exit_signal = SIGUSR1;
			break;
		case 'X':
			opt_exit = 1;
			exit_signal = SIGTERM;
			break;
		case 'l':
			if (!*optarg) {
				usbmuxd_log(LL_FATAL, "ERROR: --logfile requires a non-empty filename");
				//logAnd(LL_FATAL, "ERROR: --logfile requires a non-empty filename");
				usage();
				exit(2);
			}
			if (use_logfile) {
				usbmuxd_log(LL_FATAL, "ERROR: --logfile cannot be used multiple times");
				//logAnd(LL_FATAL, "ERROR: --logfile cannot be used multiple times");
				exit(2);
			}
			if (!freopen(optarg, "a", stderr)) {
				usbmuxd_log(LL_FATAL, "ERROR: fdreopen: %s", strerror(errno));
				//logAnd(LL_FATAL, "ERROR: fdreopen");
			} else {
				use_logfile = 1;
			}
			break;
		default:
			usage();
			exit(2);
		}
	}
}

int main(int argc, char *argv[])
{
	
	int listenfd;
	int res = 0;
	int lfd;
	struct flock lock;
	char pids[10];

	parse_opts(argc, argv);

	argc -= optind;
	argv += optind;

	if (!foreground && !use_logfile) {
		verbose += LL_WARNING;
		log_enable_syslog();
	} else {
		verbose += LL_NOTICE;
	}

	/* set log level to specified verbosity */
	log_level = verbose;

	//usbmuxd_log(LL_NOTICE, "usbmuxd v%s starting up", PACKAGE_VERSION);
	//logAnd(LL_NOTICE, "usbmuxd starting up");
	should_exit = 0;
	should_discover = 0;

	set_signal_handlers();
	signal(SIGPIPE, SIG_IGN);

	if (lockfile) {
		res = lfd = open(lockfile, O_WRONLY|O_CREAT, 0644);
		if(res == -1) {
			usbmuxd_log(LL_FATAL, "Could not open lockfile");
			//logAnd(LL_FATAL, "Could not open lockfile");
			goto terminate;
		}
		lock.l_type = F_WRLCK;
		lock.l_whence = SEEK_SET;
		lock.l_start = 0;
		lock.l_len = 0;
		lock.l_pid = 0;
		fcntl(lfd, F_GETLK, &lock);
		close(lfd);
	}
	if (lockfile && lock.l_type != F_UNLCK) {
		if (opt_exit) {
			if (lock.l_pid && !kill(lock.l_pid, 0)) {
				usbmuxd_log(LL_INFO, "Sending signal %d to instance with pid %d", exit_signal, lock.l_pid);
				//logAnd(LL_NOTICE, "Sending signal %d to instance with pid %d", exit_signal, lock.l_pid);
				res = 0;
				if (kill(lock.l_pid, exit_signal) < 0) {
					usbmuxd_log(LL_FATAL, "Could not deliver signal %d to pid %d", exit_signal, lock.l_pid);
					//logAnd(LL_FATAL, "Could not deliver signal %d to pid %d", exit_signal, lock.l_pid);
					res = -1;
				}
				goto terminate;
			} else {
				usbmuxd_log(LL_ERROR, "Could not determine pid of the other running instance!");
				//logAnd(LL_ERROR, "Could not determine pid of the other running instance!");
				res = -1;
				goto terminate;
			}
		} else {
			if (!opt_disable_hotplug) {
				usbmuxd_log(LL_ERROR, "Another instance is already running (pid %d). exiting.", lock.l_pid);
				//logAnd(LL_ERROR, "Another instance is already running (pid %d). exiting.", lock.l_pid);
				res = -1;
			} else {
				usbmuxd_log(LL_INFO, "Another instance is already running (pid %d). Telling it to check for devices.", lock.l_pid);
				//logAnd(LL_NOTICE, "Another instance is already running (pid %d). Telling it to check for devices.", lock.l_pid);
				if (lock.l_pid && !kill(lock.l_pid, 0)) {
					usbmuxd_log(LL_NOTICE, "Sending signal SIGUSR2 to instance with pid %d", lock.l_pid);
					//logAnd(LL_NOTICE, "Sending signal SIGUSR2 to instance with pid %d", lock.l_pid);
					res = 0;
					if (kill(lock.l_pid, SIGUSR2) < 0) {
						usbmuxd_log(LL_FATAL, "Could not deliver SIGUSR2 to pid %d", lock.l_pid);
						//logAnd(LL_FATAL, "Could not deliver SIGUSR2 to pid %d", lock.l_pid);
						res = -1;
					}
				} else {
					usbmuxd_log(LL_ERROR, "Could not determine pid of the other running instance!");
					//logAnd(LL_ERROR, "Could not determine pid of the other running instance!");
					res = -1;
				}
			}
			goto terminate;
		}
	}
	if (lockfile) {
		unlink(lockfile);
	}

	if (opt_exit) {
		usbmuxd_log(LL_INFO, "No running instance found, none killed. Exiting.");
		//logAnd(LL_NOTICE, "No running instance found, none killed. Exiting.");
		goto terminate;
	}

	if (!foreground) {
		if ((res = daemonize()) < 0) {
			fprintf(stderr, "usbmuxd: FATAL: Could not daemonize!\n");
			usbmuxd_log(LL_FATAL, "Could not daemonize!");
			//logAnd(LL_FATAL, "Could not daemonize!");
			goto terminate;
		}
	}

	if (lockfile) {
		// now open the lockfile and place the lock
		res = lfd = open(lockfile, O_WRONLY|O_CREAT|O_TRUNC|O_EXCL, 0644);
		if(res < 0) {
			usbmuxd_log(LL_FATAL, "Could not open pidfile '%s'", lockfile);
			//logAnd(LL_FATAL, "Could not open pidfile '%s'", lockfile);
			goto terminate;
		}
		lock.l_type = F_WRLCK;
		lock.l_whence = SEEK_SET;
		lock.l_start = 0;
		lock.l_len = 0;
		if ((res = fcntl(lfd, F_SETLK, &lock)) < 0) {
			usbmuxd_log(LL_FATAL, "Locking pidfile '%s' failed!", lockfile);
			//logAnd(LL_FATAL, "Locking pidfile '%s' failed!", lockfile);
			goto terminate;
		}
		sprintf(pids, "%d", getpid());
		if ((size_t)(res = write(lfd, pids, strlen(pids))) != strlen(pids)) {
			usbmuxd_log(LL_FATAL, "Could not write pidfile!");
			//logAnd(LL_FATAL, "Could not write pidfile!");
			if(res >= 0)
				res = -2;
			goto terminate;
		}
	}

	// set number of file descriptors to higher value
	struct rlimit rlim;
	getrlimit(RLIMIT_NOFILE, &rlim);
	rlim.rlim_max = 65536;
	setrlimit(RLIMIT_NOFILE, (const struct rlimit*)&rlim);

	usbmuxd_log(LL_INFO, "Creating socket");
	//logAnd(LL_INFO, "Creating socket");
	res = listenfd = create_socket();
	if(listenfd < 0)
		goto terminate;

#ifdef HAVE_LIBIMOBILEDEVICE
	const char* userprefdir = config_get_config_dir();
	struct stat fst;
	memset(&fst, '\0', sizeof(struct stat));
	
	usbmuxd_log(LL_INFO, "userprefdir %s", userprefdir);
	
	if (stat(userprefdir, &fst) < 0) {
		if (mkdir(userprefdir, 0775) < 0) {
			usbmuxd_log(LL_FATAL, "Failed to create required directory '%s': %s", userprefdir, strerror(errno));
			//logAnd(LL_FATAL, "Failed to create required directory '%s': %s", userprefdir, strerror(errno));
			res = -1;
			goto terminate;
		}
		if (stat(userprefdir, &fst) < 0) {
			usbmuxd_log(LL_FATAL, "stat() failed after creating directory '%s': %s", userprefdir, strerror(errno));
			//logAnd(LL_FATAL, "stat() failed after creating directory '%s': %s", userprefdir, strerror(errno));
			res = -1;
			goto terminate;
		}
	}

	// make sure permission bits are set correctly
	if (fst.st_mode != 02775) {
		if (chmod(userprefdir, 02775) < 0) {
			usbmuxd_log(LL_WARNING, "chmod(%s, 02775) failed: %s", userprefdir, strerror(errno));
			//logAnd(LL_WARNING, "chmod(%s, 02775) failed: %s", userprefdir, strerror(errno));
		}
	}
#endif

	// drop elevated privileges
	if (drop_privileges && (getuid() == 0 || geteuid() == 0)) {
		struct passwd *pw;
		if (!drop_user) {
			usbmuxd_log(LL_FATAL, "No user to drop privileges to?");
			//logAnd(LL_FATAL, "No user to drop privileges to?");
			res = -1;
			goto terminate;
		}
		pw = getpwnam(drop_user);
		if (!pw) {
			usbmuxd_log(LL_FATAL, "Dropping privileges failed, check if user '%s' exists!", drop_user);
			//logAnd(LL_FATAL, "Dropping privileges failed, check if user '%s' exists!", drop_user);
			res = -1;
			goto terminate;
		}
		if (pw->pw_uid == 0) {
			usbmuxd_log(LL_INFO, "Not dropping privileges to root");
			//logAnd(LL_INFO, "Not dropping privileges to root");
		} else {
#ifdef HAVE_LIBIMOBILEDEVICE
			/* make sure the non-privileged user has proper access to the config directory */
			if ((fst.st_uid != pw->pw_uid) || (fst.st_gid != pw->pw_gid)) {
				if (chown(userprefdir, pw->pw_uid, pw->pw_gid) < 0) {
					usbmuxd_log(LL_WARNING, "chown(%s, %d, %d) failed: %s", userprefdir, pw->pw_uid, pw->pw_gid, strerror(errno));
					//logAnd(LL_WARNING, "chown(%s, %d, %d) failed: %s", userprefdir, pw->pw_uid, pw->pw_gid, strerror(errno));
				}
			}
#endif

			if ((res = initgroups(drop_user, pw->pw_gid)) < 0) {
				usbmuxd_log(LL_FATAL, "Failed to drop privileges (cannot set supplementary groups)");
				//logAnd(LL_FATAL, "Failed to drop privileges (cannot set supplementary groups)");
				goto terminate;
			}
			if ((res = setgid(pw->pw_gid)) < 0) {
				usbmuxd_log(LL_FATAL, "Failed to drop privileges (cannot set group ID to %d)", pw->pw_gid);
				//logAnd(LL_FATAL, "Failed to drop privileges (cannot set group ID to %d)", pw->pw_gid);
				goto terminate;
			}
			if ((res = setuid(pw->pw_uid)) < 0) {
				usbmuxd_log(LL_FATAL, "Failed to drop privileges (cannot set user ID to %d)", pw->pw_uid);
				//logAnd(LL_FATAL, "Failed to drop privileges (cannot set user ID to %d)", pw->pw_uid);
				goto terminate;
			}

			// security check
			if (setuid(0) != -1) {
				usbmuxd_log(LL_FATAL, "Failed to drop privileges properly!");
				//logAnd(LL_FATAL, "Failed to drop privileges properly!");
				res = -1;
				goto terminate;
			}
			if (getuid() != pw->pw_uid || getgid() != pw->pw_gid) {
				usbmuxd_log(LL_FATAL, "Failed to drop privileges properly!");
				//logAnd(LL_FATAL, "Failed to drop privileges properly!");
				res = -1;
				goto terminate;
			}
			usbmuxd_log(LL_INFO, "Successfully dropped privileges to '%s'", drop_user);
			//logAnd(LL_NOTICE, "Successfully dropped privileges to '%s'", drop_user);
		}
	}

	client_init();
	device_init();
	usbmuxd_log(LL_INFO, "Initializing USB %d",fileDescriptor);
	printf("libusbPrint Initializing USB %d \n", fileDescriptor);
	//return res;
	
	//usbmuxd_log(LL_INFO, "Initializing USB 111 %s",argv[2]);
	//usbmuxd_log(LL_INFO, "Initializing USB 2222 %s",argv[3]);
	
	//logAnd(LL_INFO, "Initializing USB");
	//if((res = usb_init_android(fileDescriptor)) < 0)
	//	goto terminate;
	
	if((res = usb_init()) < 0){
		printf("libusbPrint Initializing USB usb_init terminate %d",res);
		usbmuxd_log(LL_INFO, "Initializing USB usb_init terminate %d",res);
		goto terminate;
	}

	usbmuxd_log(LL_INFO, "%d device%s detected", res, (res==1)?"":"s");
	printf("libusbPrint %d device%s detected", res, (res==1)?"":"s");
	//logAnd(LL_INFO, "%d device%s detected", res, (res==1)?"":"s");

	usbmuxd_log(LL_INFO, "Initialization complete");
	printf("libusbPrint Initialization complete");
	//logAnd(LL_NOTICE, "Initialization complete");

	if (report_to_parent)
		if((res = notify_parent(0)) < 0)
			goto terminate;

	if(opt_disable_hotplug) {
		printf("libusbPrint Automatic device discovery on hotplug disabled.");
		usbmuxd_log(LL_INFO, "Automatic device discovery on hotplug disabled.");
		//logAnd(LL_NOTICE, "Automatic device discovery on hotplug disabled.");
		usb_autodiscover(0); // discovery to be triggered by new instance
	}
	if (opt_enable_exit) {
		printf("libusbPrint Enabled exit on SIGUSR1 if no devices are attached. Start a new instance with \"--exit\" to trigger.");
		usbmuxd_log(LL_INFO, "Enabled exit on SIGUSR1 if no devices are attached. Start a new instance with \"--exit\" to trigger.");
		//logAnd(LL_NOTICE, "Enabled exit on SIGUSR1 if no devices are attached. Start a new instance with \"--exit\" to trigger.");
	}

	res = main_loop(listenfd);
	if(res < 0){
		printf("libusbPrint main_loop failed");
		usbmuxd_log(LL_FATAL, "main_loop failed");
	}
		
	printf("libusbPrint usbmuxd shutting down");
	usbmuxd_log(LL_INFO, "usbmuxd shutting down");
	//logAnd(LL_NOTICE, "usbmuxd shutting down");
	device_kill_connections();
	usb_shutdown();
	device_shutdown();
	client_shutdown();
	usbmuxd_log(LL_INFO, "Shutdown complete");
	printf("libusbPrint Shutdown complete");
	//logAnd(LL_NOTICE, "Shutdown complete");

terminate:
	log_disable_syslog();

	if (res < 0)
		res = -res;
	else
		res = 0;
	if (report_to_parent)
		notify_parent(res);

	return res;
}
