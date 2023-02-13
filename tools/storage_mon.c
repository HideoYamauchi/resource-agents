#include <stdio.h>
#include <getopt.h>
#include <stdlib.h>
#include <stdint.h>
#include <syslog.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#ifdef __FreeBSD__
#include <sys/disk.h>
#endif
#include <config.h>
#include <glib.h>
#include <libgen.h>

#include <qb/qbdefs.h>
#include <qb/qblog.h>
#include <qb/qbloop.h>
#include <qb/qbutil.h>
#include <qb/qbipcs.h>
#include <qb/qbipcc.h>

#define MAX_DEVICES 25
#define DEFAULT_TIMEOUT 10
#define DEFAULT_INTERVAL 30
#define DEFAULT_PIDFILE HA_VARRUNDIR "storage_mon.pid"
#define DEFAULT_ATTRNAME "#health-storage_mon"
#define CLIENT_COMMAND "get_check_value"
#define BUFF_1MEG 1048576
#define MAX_IPCSNAME 256
#define MAX_MSGSIZE 128

#define PRINT_STORAGE_MON_ERR(fmt, ...) if (!daemonize) { \
					fprintf(stderr, fmt"\n", __VA_ARGS__); \
				} else { \
					syslog(LOG_ERR, fmt, __VA_ARGS__); \
				}
#define PRINT_STORAGE_MON_ERR_NOARGS(str) if (!daemonize) { \
					fprintf(stderr, str"\n"); \
				} else { \
					syslog(LOG_ERR, str); \
				}

#define PRINT_STORAGE_MON_INFO(fmt, ...) if (!daemonize) { \
					printf(fmt"\n", __VA_ARGS__); \
				} else { \
					syslog(LOG_INFO, fmt, __VA_ARGS__); \
				}

struct storage_mon_timer_data {
	int interval;
};

struct storage_mon_check_value_req {
	struct qb_ipc_request_header hdr;
	char message[MAX_MSGSIZE];
};


struct storage_mon_check_value_res {
        struct qb_ipc_response_header hdr;
        char message[MAX_MSGSIZE];
};


char *devices[MAX_DEVICES];
int scores[MAX_DEVICES];
size_t device_count = 0;
int timeout = DEFAULT_TIMEOUT;
int verbose = 0;
int inject_error_percent = 0;
const char *attrname = DEFAULT_ATTRNAME;
gboolean daemonize = FALSE;
int shutting_down = FALSE;
static qb_ipcs_service_t *ipcs;
const char *check_value = "";

static qb_loop_t *storage_mon_poll_handle;
static qb_loop_timer_handle timer_handle;
static qb_loop_timer_handle shutdown_timer_handle;
static struct storage_mon_timer_data timer_d;

static int test_device_main(gpointer data);
static void wrap_test_device_main(void *data);

static void usage(char *name, FILE *f)
{
	fprintf(f, "usage: %s [-hv] [-d <device>]... [-s <score>]... [-t <secs>]\n", name);
	fprintf(f, "      --device <dev>  device to test, up to %d instances\n", MAX_DEVICES);
	fprintf(f, "      --score  <n>    score if device fails the test. Must match --device count\n");
	fprintf(f, "      --timeout <n>   max time to wait for a device test to come back. in seconds (default %d)\n", DEFAULT_TIMEOUT);
	fprintf(f, "      --inject-errors-percent <n> Generate EIO errors <n>%% of the time (for testing only)\n");
	fprintf(f, "      --daemonize      test run in daemons.\n");      
	fprintf(f, "      --client      client connection to daemon. requires the attrname option.\n");
	fprintf(f, "      --interval <n>       interval to test. in seconds (default %d)(for daemonize only)\n", DEFAULT_INTERVAL);
	fprintf(f, "      --pidfile <path>     file path to record pid (default %s)(for daemonize only)\n", DEFAULT_PIDFILE);
	fprintf(f, "      --attrname <attr>    attribute name to update test result (default %s)(for daemonize/client only)\n", DEFAULT_ATTRNAME);
	fprintf(f, "      --verbose        emit extra output to stdout\n");
	fprintf(f, "      --help           print this message\n");
}

/* Check one device */
static void *test_device(const char *device, int verbose, int inject_error_percent)
{
	uint64_t devsize;
	int flags = O_RDONLY | O_DIRECT;
	int device_fd;
	int res;
	off_t seek_spot;

	if (verbose) {
		printf("Testing device %s\n", device);
	}

	device_fd = open(device, flags);
	if (device_fd < 0) {
		if (errno != EINVAL) {
			PRINT_STORAGE_MON_ERR("Failed to open %s: %s", device, strerror(errno));
			exit(-1);
		}
		flags &= ~O_DIRECT;
		device_fd = open(device, flags);
		if (device_fd < 0) {
			PRINT_STORAGE_MON_ERR("Failed to open %s: %s", device, strerror(errno));
			exit(-1);
		}
	}
#ifdef __FreeBSD__
	res = ioctl(device_fd, DIOCGMEDIASIZE, &devsize);
#else
	res = ioctl(device_fd, BLKGETSIZE64, &devsize);
#endif
	if (res < 0) {
		PRINT_STORAGE_MON_ERR("Failed to get device size for %s: %s", device, strerror(errno));
		goto error;
	}
	if (verbose) {
		PRINT_STORAGE_MON_INFO("%s: opened %s O_DIRECT, size=%zu", device, (flags & O_DIRECT)?"with":"without", devsize);
	}

	/* Don't fret about real randomness */
	srand(time(NULL) + getpid());
	/* Pick a random place on the device - sector aligned */
	seek_spot = (rand() % (devsize-1024)) & 0xFFFFFFFFFFFFFE00;
	res = lseek(device_fd, seek_spot, SEEK_SET);
	if (res < 0) {
		PRINT_STORAGE_MON_ERR("Failed to seek %s: %s", device, strerror(errno));
		goto error;
	}
	if (verbose) {
		PRINT_STORAGE_MON_INFO("%s: reading from pos %ld", device, seek_spot);
	}

	if (flags & O_DIRECT) {
		int sec_size = 0;
		void *buffer;

#ifdef __FreeBSD__
		res = ioctl(device_fd, DIOCGSECTORSIZE, &sec_size);
#else
		res = ioctl(device_fd, BLKSSZGET, &sec_size);
#endif
		if (res < 0) {
			PRINT_STORAGE_MON_ERR("Failed to get block device sector size for %s: %s", device, strerror(errno));
			goto error;
		}

		if (posix_memalign(&buffer, sysconf(_SC_PAGESIZE), sec_size) != 0) {
			PRINT_STORAGE_MON_ERR("Failed to allocate aligned memory: %s", strerror(errno));
			goto error;
		}
		res = read(device_fd, buffer, sec_size);
		free(buffer);
		if (res < 0) {
			PRINT_STORAGE_MON_ERR("Failed to read %s: %s", device, strerror(errno));
			goto error;
		}
		if (res < sec_size) {
			PRINT_STORAGE_MON_ERR("Failed to read %d bytes from %s, got %d", sec_size, device, res);
			goto error;
		}
	} else {
		char buffer[512];

		res = read(device_fd, buffer, sizeof(buffer));
		if (res < 0) {
			PRINT_STORAGE_MON_ERR("Failed to read %s: %s", device, strerror(errno));
			goto error;
		}
		if (res < (int)sizeof(buffer)) {
			PRINT_STORAGE_MON_ERR("Failed to read %ld bytes from %s, got %d", sizeof(buffer), device, res);
			goto error;
		}
	}

	/* Fake an error */
	if (inject_error_percent && ((rand() % 100) < inject_error_percent)) {
		PRINT_STORAGE_MON_ERR_NOARGS("People, please fasten your seatbelts, injecting errors!");
		goto error;
	}
	res = close(device_fd);
	if (res != 0) {
		PRINT_STORAGE_MON_ERR("Failed to close %s: %s", device, strerror(errno));
		exit(-1);
	}

	if (verbose) {
		PRINT_STORAGE_MON_INFO("%s: done", device);
	}
	exit(0);

error:
	close(device_fd);
	exit(-1);
}

static int32_t sig_exit_handler (int num, void *data)
{
	shutting_down = TRUE;

	qb_loop_timer_del(storage_mon_poll_handle, timer_handle);

	qb_loop_timer_add(storage_mon_poll_handle,
			QB_LOOP_MED,
			0,
			NULL,
			wrap_test_device_main,
			&shutdown_timer_handle);
	return 0;
}

static void child_shutdown(int nsig)
{
	exit(1);
}

static int write_pid_file(const char *pidfile)
{
	char *pid;
	char *dir, *str = NULL;
	int fd = -1;
	int rc = -1;
	int i, len;

	if (asprintf(&pid, "%jd", (intmax_t)getpid()) < 0) {
		syslog(LOG_ERR, "Failed to allocate memory to store PID");
		pid = NULL;
		goto done;
	}

	str = strdup(pidfile);
	if (str == NULL) {
		syslog(LOG_ERR, "Failed to duplicate string ['%s']", pidfile);
		goto done;
	}
	dir = dirname(str);
	for (i = 1, len = strlen(dir); i < len; i++) {
		if (dir[i] == '/') {
			dir[i] = 0;
			if ((mkdir(dir, 0640) < 0) && (errno != EEXIST)) {
				syslog(LOG_ERR, "Failed to create directory %s: %s", dir, strerror(errno));
				goto done;
			}
			dir[i] = '/';
		}
	}
	if ((mkdir(dir, 0640) < 0) && (errno != EEXIST)) {
		syslog(LOG_ERR, "Failed to create directory %s: %s", dir, strerror(errno));
		goto done;
	}

	fd = open(pidfile, O_CREAT | O_WRONLY, 0640);
	if (fd < 0) {
		syslog(LOG_ERR, "Failed to open %s: %s", pidfile, strerror(errno));
		goto done;
	}

	if (write(fd, pid, strlen(pid)) != strlen(pid)) {
		syslog(LOG_ERR, "Failed to write '%s' to %s: %s", pid, pidfile, strerror(errno));
		goto done;
	}
	close(fd);
	rc = 0;
done:
	if (pid != NULL) {
		free(pid);
	}
	if (str != NULL) {
		free(str);
	}
	return rc;
}

static void wrap_test_device_main(void *data)
{
	struct storage_mon_timer_data *timer_data = (struct storage_mon_timer_data*)data;
	test_device_main((timer_data != NULL) ? &timer_data->interval : NULL);

}
static int test_device_main(gpointer data)
{
	pid_t test_forks[MAX_DEVICES];
	size_t i;
	struct timespec ts;
	time_t start_time;
	size_t finished_count = 0;
	int final_score = 0;


	if (daemonize && shutting_down == TRUE) {
		goto done;
	}

	memset(test_forks, 0, sizeof(test_forks));
	for (i=0; i<device_count; i++) {
		test_forks[i] = fork();
		if (test_forks[i] < 0) {
			fprintf(stderr, "Error spawning fork for %s: %s\n", devices[i], strerror(errno));
			syslog(LOG_ERR, "Error spawning fork for %s: %s\n", devices[i], strerror(errno));
			/* Just test the devices we have */
			break;
		}
		/* child */
		if (test_forks[i] == 0) {
			if (daemonize) {
				signal(SIGTERM, &child_shutdown);
			}
			test_device(devices[i], verbose, inject_error_percent);
		}
	}

	/* See if they have finished */
	clock_gettime(CLOCK_REALTIME, &ts);
	start_time = ts.tv_sec;

	while ((finished_count < device_count) && ((start_time + timeout) > ts.tv_sec)) {
		for (i=0; i<device_count; i++) {
			int wstatus;
			pid_t w;

			if (test_forks[i] > 0) {
				w = waitpid(test_forks[i], &wstatus, WUNTRACED | WNOHANG | WCONTINUED);
				if (w < 0) {
					PRINT_STORAGE_MON_ERR("waitpid on %s failed: %s", devices[i], strerror(errno));
					return -1;
				}

				if (w == test_forks[i]) {
					if (WIFEXITED(wstatus)) {
						if (WEXITSTATUS(wstatus) != 0) {
							syslog(LOG_ERR, "Error reading from device %s", devices[i]);
							final_score += scores[i];
						}

						finished_count++;
						test_forks[i] = 0;
					}
				}
			}
		}

		usleep(100000);

		clock_gettime(CLOCK_REALTIME, &ts);
	}

	/* See which threads have not finished */
	for (i=0; i<device_count; i++) {
		if (test_forks[i] != 0) {
			syslog(LOG_ERR, "Reading from device %s did not complete in %d seconds timeout", devices[i], timeout);
			fprintf(stderr, "Thread for device %s did not complete in time\n", devices[i]);
			final_score += scores[i];
		}
	}
	if (!daemonize) {
		if (verbose) {
			printf("Final score is %d\n", final_score);
		}
		return final_score;
	} else {
		int wstatus;
		/* Update node health attribute */
		check_value = (final_score > 0) ? "red" : "green";

		/* See if threads have finished */
		while (finished_count < device_count) {
			for (i=0; i<device_count; i++) {
				if (test_forks[i] > 0
				    && waitpid(test_forks[i], &wstatus, WUNTRACED | WNOHANG | WCONTINUED) == test_forks[i]
				    && WIFEXITED(wstatus)) {
					finished_count++;
					test_forks[i] = 0;
				}
			}
			usleep(100000);
		}

		if (shutting_down == TRUE) {
			goto done;
		}
		if (data != NULL) {
			qb_loop_timer_del(storage_mon_poll_handle, timer_handle);
			qb_loop_timer_add(storage_mon_poll_handle,
				QB_LOOP_MED,
				*(int *)data * QB_TIME_NS_IN_SEC,
				&timer_d,
				wrap_test_device_main,
				&timer_handle);
		}
		return TRUE;
	}


done:
	qb_loop_stop(storage_mon_poll_handle);
	return FALSE;
}

static int32_t
storage_mon_job_add(enum qb_loop_priority p, void *data, qb_loop_job_dispatch_fn fn)
{
	return qb_loop_job_add(storage_mon_poll_handle, p, data, fn);
}

static int32_t
storage_mon_dispatch_add(enum qb_loop_priority p, int32_t fd, int32_t evts,
		void *data, qb_ipcs_dispatch_fn_t fn)
{
	return qb_loop_poll_add(storage_mon_poll_handle, p, fd, evts, data, fn);
}

static int32_t
storage_mon_dispatch_mod(enum qb_loop_priority p, int32_t fd, int32_t evts,
		void *data, qb_ipcs_dispatch_fn_t fn)
{
	return qb_loop_poll_mod(storage_mon_poll_handle, p, fd, evts, data, fn);
}

static int32_t
storage_mon_dispatch_del(int32_t fd)
{
	return qb_loop_poll_del(storage_mon_poll_handle, fd);
}

static int32_t
storage_mon_ipcs_connection_accept_fn(qb_ipcs_connection_t * c, uid_t uid, gid_t gid)
{
	return 0;
}

static void
storage_mon_ipcs_connection_created_fn(qb_ipcs_connection_t *c)
{
	struct qb_ipcs_stats srv_stats;

	qb_ipcs_stats_get(ipcs, &srv_stats, QB_FALSE);
	syslog(LOG_DEBUG, "Connection created (active:%d, closed:%d)",
		srv_stats.active_connections, srv_stats.closed_connections);
}

static void
storage_mon_ipcs_connection_destroyed_fn(qb_ipcs_connection_t *c)
{
	syslog(LOG_DEBUG, "Connection about to be freed");
}

static int32_t
storage_mon_ipcs_connection_closed_fn(qb_ipcs_connection_t *c)
{       
	struct qb_ipcs_connection_stats stats;
        struct qb_ipcs_stats srv_stats;

	qb_ipcs_stats_get(ipcs, &srv_stats, QB_FALSE);
        qb_ipcs_connection_stats_get(c, &stats, QB_FALSE);

	syslog(LOG_DEBUG,
		"Connection to pid:%d destroyed (active:%d, closed:%d)",
		stats.client_pid, srv_stats.active_connections,
		srv_stats.closed_connections);

	return 0;
}

static int32_t
storage_mon_ipcs_msg_process_fn(qb_ipcs_connection_t *c, void *data, size_t size)
{
	struct qb_ipc_request_header *hdr;
	struct storage_mon_check_value_req *request;
	struct qb_ipc_response_header resps;
	ssize_t res;
	struct iovec iov[2];
	char resp[100];
	int32_t rc;

	hdr = (struct qb_ipc_request_header *)data;
	if (hdr->id == (QB_IPC_MSG_USER_START + 1)) {
		return 0;
	}

	request = (struct storage_mon_check_value_req *)data;
	syslog(LOG_DEBUG, "msg received (id:%d, size:%d, data:%s)",
		request->hdr.id, request->hdr.size, request->message);

	resps.size = sizeof(struct qb_ipc_response_header);
	resps.id = 13;
	resps.error = 0;

	rc = snprintf(resp, 100, "%s", check_value) + 1;
	iov[0].iov_len = sizeof(resps);
	iov[0].iov_base = &resps;
	iov[1].iov_len = rc;
	iov[1].iov_base = resp;
	resps.size += rc;

	res = qb_ipcs_response_sendv(c, iov, 2);
	if (res < 0) {
		errno = -res;
		syslog(LOG_ERR, "qb_ipcs_response_send : errno = %d", errno);
	}
	return 0;
}

static int32_t
storage_mon_client(void)
{
	struct storage_mon_check_value_req request;
	struct storage_mon_check_value_res response;
	qb_ipcc_connection_t *conn;
	char ipcs_name[MAX_IPCSNAME];
	int32_t rc;


	snprintf(ipcs_name, MAX_IPCSNAME, "storage_mon_%s", attrname);
	conn = qb_ipcc_connect(ipcs_name, 0);
	if (conn == NULL) {
		fprintf(stderr, "qb_ipcc_connect error");
		return(-1);
	}

	snprintf(request.message, MAX_MSGSIZE, "%s", CLIENT_COMMAND);
	request.hdr.id = QB_IPC_MSG_USER_START + 3;
	request.hdr.size = sizeof(struct storage_mon_check_value_req);
	rc = qb_ipcc_send(conn, &request, request.hdr.size);
	if (rc < 0) {
		fprintf(stderr, "qb_ipcc_send error : %d", rc);
		return(-1);
	}
	if (rc > 0) {
		rc = qb_ipcc_recv(conn, &response, sizeof(response), -1);
		if (rc < 0) {
			fprintf(stderr, "qb_ipcc_recv error : %d", rc);
			return(-1);
		}
	}

	qb_ipcc_disconnect(conn);

	if (strcmp(response.message, "green") == 0) {
		rc = 0; /* green */
	} else {
		rc = 1;	/* red */
	}

	syslog(LOG_DEBUG, "daemon response[%d]: %s \n", response.hdr.id, response.message);
	return(rc);
}

static int32_t
storage_mon_daemon(int interval, const char *pidfile)
{
	int32_t rc;
	char ipcs_name[MAX_IPCSNAME];

	struct qb_ipcs_service_handlers sh = {
		.connection_accept = storage_mon_ipcs_connection_accept_fn,
		.connection_created = storage_mon_ipcs_connection_created_fn,
		.msg_process = storage_mon_ipcs_msg_process_fn,
		.connection_destroyed = storage_mon_ipcs_connection_destroyed_fn,
		.connection_closed = storage_mon_ipcs_connection_closed_fn,
	};

	struct qb_ipcs_poll_handlers ph = {
		.job_add = storage_mon_job_add,
		.dispatch_add = storage_mon_dispatch_add,
		.dispatch_mod = storage_mon_dispatch_mod,
		.dispatch_del = storage_mon_dispatch_del,
	};

	if (daemon(0, 0) < 0) {
		syslog(LOG_ERR, "Failed to daemonize: %s", strerror(errno));
		return -1;
	}

	umask(S_IWGRP | S_IWOTH | S_IROTH);

	if (write_pid_file(pidfile) < 0) {
		return -1;
	}

	snprintf(ipcs_name, MAX_IPCSNAME, "storage_mon_%s", attrname);
	ipcs = qb_ipcs_create(ipcs_name, 0, QB_IPC_NATIVE, &sh);
	if (ipcs == 0) {
		syslog(LOG_ERR, "qb_ipcs_create");
		return -1;
	}

	qb_ipcs_enforce_buffer_size(ipcs, BUFF_1MEG);

	storage_mon_poll_handle = qb_loop_create();

	qb_ipcs_poll_handlers_set(ipcs, &ph);
	rc = qb_ipcs_run(ipcs);
	if (rc != 0) {
		errno = -rc;
		syslog(LOG_ERR, "qb_ipcs_run");
		return -1;
	}


	qb_loop_signal_add(storage_mon_poll_handle, QB_LOOP_HIGH,
		SIGTERM, NULL, sig_exit_handler, NULL);

	timer_d.interval = interval;
	qb_loop_timer_add(storage_mon_poll_handle,
		QB_LOOP_MED,
		0,
		&timer_d,
		wrap_test_device_main,
		&timer_handle);

	qb_loop_run(storage_mon_poll_handle);
	qb_loop_destroy(storage_mon_poll_handle);

	unlink(pidfile);

	return 0;
}

int main(int argc, char *argv[])
{
	size_t score_count = 0;
	int final_score = 0;
	int opt, option_index;
	int interval = DEFAULT_INTERVAL;
	const char *pidfile = DEFAULT_PIDFILE;
	gboolean client = FALSE;
	struct option long_options[] = {
		{"timeout", required_argument, 0, 't' },
		{"device",  required_argument, 0, 'd' },
		{"score",   required_argument, 0, 's' },
		{"inject-errors-percent",   required_argument, 0, 0 },
		{"daemonize", no_argument, 0, 0 },
		{"client", no_argument, 0, 0 },
		{"interval", required_argument, 0, 'i' },
		{"pidfile", required_argument, 0, 'p' },
		{"attrname", required_argument, 0, 'a' },
		{"verbose", no_argument, 0, 'v' },
		{"help",    no_argument, 0,       'h' },
		{0,         0,           0,        0  }
	};

	while ( (opt = getopt_long(argc, argv, "hvt:d:s:i:p:a:",
				   long_options, &option_index)) != -1 ) {
		switch (opt) {
			case 0: /* Long-only options */
				if (strcmp(long_options[option_index].name, "inject-errors-percent") == 0) {
					inject_error_percent = atoi(optarg);
					if (inject_error_percent < 1 || inject_error_percent > 100) {
						fprintf(stderr, "inject_error_percent should be between 1 and 100\n");
						return -1;
					}
				}
				if (strcmp(long_options[option_index].name, "daemonize") == 0) {
					daemonize = TRUE;
				}
				if (strcmp(long_options[option_index].name, "client") == 0) {
					client = TRUE;
				}
				if (daemonize && client) {
					fprintf(stderr,"The daemonize option and client option cannot be specified at the same time.");	
					return -1;
				}
				break;
			case 'd':
				if (device_count < MAX_DEVICES) {
					devices[device_count++] = strdup(optarg);
				} else {
					fprintf(stderr, "too many devices, max is %d\n", MAX_DEVICES);
					return -1;
				}
				break;
			case 's':
				if (score_count < MAX_DEVICES) {
					int score = atoi(optarg);
					if (score < 1 || score > 10) {
						fprintf(stderr, "Score must be between 1 and 10 inclusive\n");
						return -1;
					}
					scores[score_count++] = score;
				} else {
					fprintf(stderr, "too many scores, max is %d\n", MAX_DEVICES);
					return -1;
				}
				break;
			case 'v':
				verbose++;
				break;
			case 't':
				timeout = atoi(optarg);
				if (timeout < 1) {
					fprintf(stderr, "invalid timeout %d. Min 1, recommended %d (default)\n", timeout, DEFAULT_TIMEOUT);
					return -1;
				}
				break;
			case 'h':
				usage(argv[0], stdout);
				return 0;
				break;
			case 'i':
				interval = atoi(optarg);
				if (interval < 1) {
					fprintf(stderr, "invalid interval %d. Min 1, default is %d\n", interval, DEFAULT_INTERVAL);
					return -1;
				}
				break;
			case 'p':
				pidfile = strdup(optarg);
				if (pidfile == NULL) {
					fprintf(stderr, "Failed to duplicate string ['%s']\n", optarg);
					return -1;
				}
				break;
			case 'a':
				attrname = strdup(optarg);
				if (attrname == NULL) {
					fprintf(stderr, "Failed to duplicate string ['%s']\n", optarg);
					return -1;
				}
				break;
			default:
				usage(argv[0], stderr);
				return -1;
				break;
		}

	}

	if (client) {
		return(storage_mon_client());
	}

	if (device_count == 0) {
		fprintf(stderr, "No devices to test, use the -d  or --device argument\n");
		return -1;
	}

	if (device_count != score_count) {
		fprintf(stderr, "There must be the same number of devices and scores\n");
		return -1;
	}

	openlog("storage_mon", 0, LOG_DAEMON);

	if (!daemonize) {
		final_score = test_device_main(NULL);
	} else {
		return(storage_mon_daemon(interval, pidfile));
	}
	return final_score;
}
