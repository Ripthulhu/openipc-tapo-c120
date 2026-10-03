#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef C120_ROOT
#define C120_ROOT ""
#endif
#define EVENT_CONF C120_ROOT "/etc/c120-eventd.conf"
#define LIGHT_CONF C120_ROOT "/etc/c120-light-pins.conf"
#define EVENT_PID C120_ROOT "/run/c120-eventd.pid"
#define BUTTON_PID C120_ROOT "/run/c120-button-apd.pid"
#define EVENT_LOCK C120_ROOT "/run/c120-eventd.lock"
#define LOG_PATH C120_ROOT "/tmp/c120-eventd.log"
#define AP_STATE C120_ROOT "/run/c120-setup-ap.active"
#define LAMP_STATE C120_ROOT "/tmp/c120-lamps.state"
#define GPIO_ROOT C120_ROOT "/sys/class/gpio"

#define MAX_PINS 16

struct config {
	int reset_gpio;
	int reset_active_value;
	int reset_hold_ticks;
	int reset_poll_ms;
	int light_poll_ms;
	int respect_exclusive;
	int log_enabled;
	int pins[MAX_PINS];
	int pin_count;
};

static volatile sig_atomic_t keep_running = 1;
static volatile sig_atomic_t reload_requested = 0;
static int read_first_line(const char *path, char *buf, size_t len);
static int read_pid(const char *path);

static void on_signal(int sig)
{
	if (sig == SIGHUP)
		reload_requested = 1;
	else
		keep_running = 0;
}

static int file_exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

static void log_msg(const struct config *cfg, const char *fmt, ...)
{
	FILE *fp;
	va_list ap;
	time_t now;
	struct tm tm_now;
	char ts[32];
	struct stat st;

	if (!cfg->log_enabled)
		return;

	fp = fopen(LOG_PATH, stat(LOG_PATH, &st) == 0 && st.st_size > 65536 ? "w" : "a");
	if (!fp)
		return;

	now = time(NULL);
	if (localtime_r(&now, &tm_now))
		strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now);
	else
		strcpy(ts, "0000-00-00 00:00:00");

	fprintf(fp, "%s ", ts);
	va_start(ap, fmt);
	vfprintf(fp, fmt, ap);
	va_end(ap);
	fputc('\n', fp);
	fclose(fp);
}

static char *trim(char *s)
{
	char *end;

	while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
		s++;

	end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
	    end[-1] == '\r' || end[-1] == '\n'))
		*--end = '\0';

	return s;
}

static void unquote(char *s)
{
	size_t len = strlen(s);

	if (len >= 2 && ((s[0] == '"' && s[len - 1] == '"') ||
	    (s[0] == '\'' && s[len - 1] == '\''))) {
		memmove(s, s + 1, len - 2);
		s[len - 2] = '\0';
	}
}

static int parse_int(const char *value, int min, int max, int fallback)
{
	char *end;
	long result;
	errno = 0;
	result = strtol(value, &end, 10);
	if (errno || end == value || *end || result < min || result > max)
		return fallback;
	return (int)result;
}

static int parse_bool_int(const char *value, int fallback)
{
	if (!value || !*value)
		return fallback;
	if (!strcmp(value, "true") || !strcmp(value, "yes") || !strcmp(value, "on"))
		return 1;
	if (!strcmp(value, "false") || !strcmp(value, "no") || !strcmp(value, "off"))
		return 0;
	return parse_int(value, 0, 1, fallback);
}

static int parse_ms(const char *value, int fallback_ms)
{
	double seconds;
	char *end = NULL;

	if (!value || !*value)
		return fallback_ms;

	seconds = strtod(value, &end);
	if (end == value || *end || !isfinite(seconds) || seconds <= 0.0)
		return fallback_ms;

	if (seconds > 60.0)
		return 60000;

	return (int)(seconds * 1000.0 + 0.5);
}

static void parse_pins(struct config *cfg, char *value)
{
	char *tok;
	int count = 0;
	int pins[MAX_PINS];

	for (char *p = value; *p; p++) {
		if (*p == ',' || *p == ';')
			*p = ' ';
	}

	for (tok = strtok(value, " \t"); tok; tok = strtok(NULL, " \t")) {
		int pin = parse_int(tok, 0, 255, -1);
		int duplicate = 0;
		if (pin < 0)
			return;
		for (int i = 0; i < count; i++)
			duplicate |= pins[i] == pin;
		if (duplicate)
			continue;
		if (count == MAX_PINS)
			return;
		pins[count++] = pin;
	}

	memcpy(cfg->pins, pins, (size_t)count * sizeof(*pins));
	cfg->pin_count = count;
}

static void defaults(struct config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->reset_gpio = 9;
	cfg->reset_active_value = 0;
	cfg->reset_hold_ticks = 5;
	cfg->reset_poll_ms = 200;
	cfg->light_poll_ms = 500;
	cfg->respect_exclusive = 1;
	cfg->log_enabled = 0;
	cfg->pins[0] = 12;
	cfg->pins[1] = 13;
	cfg->pin_count = 2;
}

static void parse_config_file(struct config *cfg, const char *path)
{
	FILE *fp;
	char line[512];

	fp = fopen(path, "r");
	if (!fp)
		return;

	while (fgets(line, sizeof(line), fp)) {
		char *key;
		char *value;
		char *eq;

		key = trim(line);
		if (*key == '\0' || *key == '#')
			continue;

		eq = strchr(key, '=');
		if (!eq)
			continue;

		*eq = '\0';
		value = trim(eq + 1);
		key = trim(key);
		unquote(value);

		if (!strcmp(key, "C120_CAMERA_LIGHT_PINS")) {
			parse_pins(cfg, value);
		} else if (!strcmp(key, "C120_LIGHT_PINS_POLL")) {
			cfg->light_poll_ms = parse_ms(value, cfg->light_poll_ms);
		} else if (!strcmp(key, "C120_LIGHT_PINS_RESPECT_EXCLUSIVE")) {
			cfg->respect_exclusive = parse_bool_int(value, cfg->respect_exclusive);
		} else if (!strcmp(key, "C120_LIGHT_PINS_LOG") ||
		    !strcmp(key, "C120_EVENTD_LOG") ||
		    !strcmp(key, "C120_RESET_LOG")) {
			cfg->log_enabled = parse_bool_int(value, cfg->log_enabled);
		} else if (!strcmp(key, "C120_RESET_GPIO")) {
			cfg->reset_gpio = parse_int(value, 0, 255, cfg->reset_gpio);
		} else if (!strcmp(key, "C120_RESET_ACTIVE_VALUE")) {
			cfg->reset_active_value = parse_bool_int(value, cfg->reset_active_value);
		} else if (!strcmp(key, "C120_RESET_HOLD_TICKS")) {
			cfg->reset_hold_ticks = parse_int(value, 1, 999, cfg->reset_hold_ticks);
		} else if (!strcmp(key, "C120_RESET_POLL_DELAY")) {
			cfg->reset_poll_ms = parse_ms(value, cfg->reset_poll_ms);
		}
	}

	fclose(fp);
}

static void load_config(struct config *cfg)
{
	defaults(cfg);
	parse_config_file(cfg, EVENT_CONF);
	parse_config_file(cfg, LIGHT_CONF);

	if (cfg->reset_poll_ms < 50)
		cfg->reset_poll_ms = 50;
	if (cfg->light_poll_ms < 50)
		cfg->light_poll_ms = 50;
	if (cfg->pin_count > MAX_PINS)
		cfg->pin_count = MAX_PINS;
}

static int write_text(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	ssize_t want = (ssize_t)strlen(text);
	ssize_t got;

	if (fd < 0)
		return -1;
	got = write(fd, text, (size_t)want);
	close(fd);
	return got == want ? 0 : -1;
}

static void gpio_path(char *buf, size_t len, int gpio, const char *leaf)
{
	snprintf(buf, len, GPIO_ROOT "/gpio%d/%s", gpio, leaf);
}

static int gpio_export(int gpio)
{
	char dir[sizeof(GPIO_ROOT) + 32];
	char text[16];

	snprintf(dir, sizeof(dir), GPIO_ROOT "/gpio%d", gpio);
	if (file_exists(dir))
		return 0;

	snprintf(text, sizeof(text), "%d", gpio);
	write_text(GPIO_ROOT "/export", text);
	return file_exists(dir) ? 0 : -1;
}

static int gpio_direction(int gpio, const char *direction)
{
	char path[sizeof(GPIO_ROOT) + 48];
	char current[16];

	if (gpio_export(gpio) < 0)
		return -1;
	gpio_path(path, sizeof(path), gpio, "direction");
	if (read_first_line(path, current, sizeof(current)) == 0 && !strcmp(current, direction))
		return 0;
	return write_text(path, direction);
}

static int gpio_read_value(int gpio, int fallback)
{
	char path[sizeof(GPIO_ROOT) + 48];
	char value = '\0';
	int fd;

	if (gpio_export(gpio) < 0)
		return fallback;

	gpio_path(path, sizeof(path), gpio, "value");
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return fallback;

	if (read(fd, &value, 1) != 1) {
		close(fd);
		return fallback;
	}
	close(fd);

	return value == '1' ? 1 : value == '0' ? 0 : fallback;
}

static int gpio_write_value(int gpio, int value)
{
	char path[sizeof(GPIO_ROOT) + 48];
	char direction[16];

	if (gpio_export(gpio) < 0)
		return -1;
	gpio_path(path, sizeof(path), gpio, "direction");
	if (read_first_line(path, direction, sizeof(direction)) < 0)
		return -1;
	/* Set the initial output level atomically; writing "out" first drives low. */
	if (strcmp(direction, "out"))
		return write_text(path, value ? "high" : "low");
	if (gpio_read_value(gpio, -1) == value)
		return 0;

	gpio_path(path, sizeof(path), gpio, "value");
	return write_text(path, value ? "1" : "0");
}

static int read_first_line(const char *path, char *buf, size_t len)
{
	FILE *fp = fopen(path, "r");

	if (!fp)
		return -1;
	if (!fgets(buf, (int)len, fp)) {
		fclose(fp);
		return -1;
	}
	fclose(fp);
	buf[strcspn(buf, "\r\n")] = '\0';
	return 0;
}

static int exclusive_lamp_mode(const struct config *cfg)
{
	char mode[32] = "";

	if (!cfg->respect_exclusive)
		return 0;
	if (read_first_line(LAMP_STATE, mode, sizeof(mode)) < 0)
		return 0;
	return !strcmp(mode, "850") || !strcmp(mode, "940");
}

static int sync_light(const struct config *cfg)
{
	int leader;
	int value;

	if (cfg->pin_count < 2)
		return 0;
	if (file_exists(AP_STATE))
		return 0;
	if (exclusive_lamp_mode(cfg))
		return 0;

	leader = cfg->pins[0];
	value = gpio_read_value(leader, -1);
	if (value < 0)
		return 1;

	for (int i = 1; i < cfg->pin_count; i++) {
		if (cfg->pins[i] == leader)
			continue;
		if (cfg->pins[i] == cfg->reset_gpio || gpio_write_value(cfg->pins[i], value) < 0)
			return 1;
	}

	return 0;
}

static void write_pid_file(const char *path)
{
	FILE *fp = fopen(path, "w");
	if (!fp)
		return;
	fprintf(fp, "%ld\n", (long)getpid());
	fclose(fp);
}

static void remove_pid_files(void)
{
	if (read_pid(EVENT_PID) == (int)getpid())
		unlink(EVENT_PID);
	if (read_pid(BUTTON_PID) == (int)getpid())
		unlink(BUTTON_PID);
}

static int read_pid(const char *path)
{
	char buf[32];

	if (read_first_line(path, buf, sizeof(buf)) < 0)
		return -1;
	return parse_int(buf, 2, INT_MAX, -1);
}

static int daemon_pid(void)
{
	int pid = read_pid(EVENT_PID);
	char path[64], name[32];
	if (pid < 0 || kill(pid, 0) < 0)
		return -1;
	snprintf(path, sizeof(path), "/proc/%d/comm", pid);
	if (read_first_line(path, name, sizeof(name)) < 0 || strcmp(name, "c120-eventd"))
		return -1;
	return pid;
}

static int signal_daemon(int sig)
{
	int pid = daemon_pid();
	if (pid < 0) {
		fprintf(stderr, "c120-eventd is not running\n");
		return 1;
	}
	if (kill(pid, sig) < 0) {
		perror("kill");
		return 1;
	}
	return 0;
}

static void control_ap(const struct config *cfg)
{
	int active = file_exists(AP_STATE);
	const char *cmd = active ?
	    C120_ROOT "/usr/bin/c120-setup-ap stop >/dev/null 2>&1" :
	    C120_ROOT "/usr/bin/c120-setup-ap start >/dev/null 2>&1";

	log_msg(cfg, "%s setup AP", active ? "stopping" : "starting");
	if (system(cmd) != 0)
		log_msg(cfg, "setup AP transition failed");
}

static void sleep_ms(int ms)
{
	struct timespec delay = { ms / 1000, (long)(ms % 1000) * 1000000L };
	while (nanosleep(&delay, &delay) < 0 && errno == EINTR && keep_running && !reload_requested)
		;
}

static int daemon_loop(void)
{
	struct config cfg;
	int pressed = 0;
	int held = 0;
	int light_elapsed = 0;
	int lock_fd;
	struct sigaction action = {0};

	load_config(&cfg);
	lock_fd = open(EVENT_LOCK, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
		fprintf(stderr, "c120-eventd: cannot acquire daemon lock\n");
		if (lock_fd >= 0)
			close(lock_fd);
		return 1;
	}
	action.sa_handler = on_signal;
	sigemptyset(&action.sa_mask);
	sigaction(SIGHUP, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGINT, &action, NULL);

	write_pid_file(EVENT_PID);
	write_pid_file(BUTTON_PID);
	atexit(remove_pid_files);

	gpio_direction(cfg.reset_gpio, "in");
	sync_light(&cfg);
	log_msg(&cfg, "started gpio=%d active=%d hold=%d reset_poll_ms=%d light_poll_ms=%d",
	    cfg.reset_gpio, cfg.reset_active_value, cfg.reset_hold_ticks,
	    cfg.reset_poll_ms, cfg.light_poll_ms);

	while (keep_running) {
		int value;

		if (reload_requested) {
			reload_requested = 0;
			load_config(&cfg);
			gpio_direction(cfg.reset_gpio, "in");
			pressed = held = light_elapsed = 0;
			log_msg(&cfg, "reloaded config");
		}

		value = gpio_read_value(cfg.reset_gpio, cfg.reset_active_value ? 0 : 1);
		if (value == cfg.reset_active_value) {
			if (!pressed) {
				pressed = 1;
				held = 0;
				log_msg(&cfg, "reset button down");
			}
			held++;
			if (held >= cfg.reset_hold_ticks) {
				control_ap(&cfg);
				while (keep_running &&
				    gpio_read_value(cfg.reset_gpio, cfg.reset_active_value ? 0 : 1) ==
				    cfg.reset_active_value) {
					/* Reload after release, without turning the held button into a second press. */
					sleep_ms(cfg.reset_poll_ms);
				}
				pressed = 0;
				held = 0;
				log_msg(&cfg, "reset button released");
			}
		} else {
			pressed = 0;
			held = 0;
		}

		light_elapsed += cfg.reset_poll_ms;
		if (light_elapsed >= cfg.light_poll_ms) {
			sync_light(&cfg);
			light_elapsed = 0;
		}

		sleep_ms(cfg.reset_poll_ms);
	}
	remove_pid_files();
	close(lock_fd);
	return 0;
}

static int status_cmd(void)
{
	struct config cfg;
	char mode[32] = "";
	int leader;

	load_config(&cfg);
	leader = cfg.pin_count > 0 ? cfg.pins[0] : -1;
	read_first_line(LAMP_STATE, mode, sizeof(mode));

	printf("eventd=%s\n", daemon_pid() > 0 ? "running" : "stopped");
	printf("pins=\"");
	for (int i = 0; i < cfg.pin_count; i++)
		printf("%s%d", i ? " " : "", cfg.pins[i]);
	printf("\"\n");
	printf("leader=%d value=%d\n", leader, leader >= 0 ? gpio_read_value(leader, 0) : -1);
	printf("extras=\"");
	for (int i = 1; i < cfg.pin_count; i++)
		printf("%s%d", i > 1 ? " " : "", cfg.pins[i]);
	printf("\"\n");
	for (int i = 1; i < cfg.pin_count; i++)
		printf("extra_%d=%d\n", cfg.pins[i], gpio_read_value(cfg.pins[i], 0));
	printf("lamp_state=%s\n", mode);
	if (file_exists(AP_STATE))
		printf("mirror=paused-setup-ap\n");
	else if (exclusive_lamp_mode(&cfg))
		printf("mirror=paused-exclusive\n");
	else
		printf("mirror=active\n");
	printf("reset_gpio=%d value=%d active=%d hold_ticks=%d\n",
	    cfg.reset_gpio,
	    gpio_read_value(cfg.reset_gpio, cfg.reset_active_value ? 0 : 1),
	    cfg.reset_active_value,
	    cfg.reset_hold_ticks);
	printf("setup_ap=%s\n", file_exists(AP_STATE) ? "active" : "inactive");
	return 0;
}

static void usage(const char *argv0)
{
	fprintf(stderr, "usage: %s [daemon|status|sync|reload|stop]\n", argv0);
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "status";

	if (!strcmp(cmd, "daemon")) {
		return daemon_loop();
	}
	if (!strcmp(cmd, "status"))
		return status_cmd();
	if (!strcmp(cmd, "sync")) {
		struct config cfg;
		load_config(&cfg);
		return sync_light(&cfg);
	}
	if (!strcmp(cmd, "reload"))
		return signal_daemon(SIGHUP);
	if (!strcmp(cmd, "stop"))
		return signal_daemon(SIGTERM);

	usage(argv[0]);
	return 2;
}
