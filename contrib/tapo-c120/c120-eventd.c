#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define EVENT_CONF "/etc/c120-eventd.conf"
#define LIGHT_CONF "/etc/c120-light-pins.conf"
#define EVENT_PID "/run/c120-eventd.pid"
#define BUTTON_PID "/run/c120-button-apd.pid"
#define LOG_PATH "/tmp/c120-eventd.log"
#define AP_STATE "/run/c120-setup-ap.active"
#define LAMP_STATE "/tmp/c120-lamps.state"

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

	if (!cfg->log_enabled)
		return;

	fp = fopen(LOG_PATH, "a");
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

static int parse_bool_int(const char *value, int fallback)
{
	if (!value || !*value)
		return fallback;
	if (!strcmp(value, "true") || !strcmp(value, "yes") || !strcmp(value, "on"))
		return 1;
	if (!strcmp(value, "false") || !strcmp(value, "no") || !strcmp(value, "off"))
		return 0;
	return atoi(value) ? 1 : 0;
}

static int parse_ms(const char *value, int fallback_ms)
{
	double seconds;
	char *end = NULL;

	if (!value || !*value)
		return fallback_ms;

	seconds = strtod(value, &end);
	if (end == value || seconds <= 0.0)
		return fallback_ms;

	if (seconds > 60.0)
		return 60000;

	return (int)(seconds * 1000.0 + 0.5);
}

static void parse_pins(struct config *cfg, char *value)
{
	char *tok;
	int count = 0;

	for (char *p = value; *p; p++) {
		if (*p == ',' || *p == ';')
			*p = ' ';
	}

	for (tok = strtok(value, " \t"); tok && count < MAX_PINS; tok = strtok(NULL, " \t")) {
		char *end = NULL;
		long pin = strtol(tok, &end, 10);
		if (end == tok || *end != '\0' || pin < 0 || pin > 255)
			continue;
		cfg->pins[count++] = (int)pin;
	}

	if (count > 0)
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
			cfg->reset_gpio = atoi(value);
		} else if (!strcmp(key, "C120_RESET_ACTIVE_VALUE")) {
			cfg->reset_active_value = atoi(value) ? 1 : 0;
		} else if (!strcmp(key, "C120_RESET_HOLD_TICKS")) {
			int ticks = atoi(value);
			if (ticks > 0 && ticks < 1000)
				cfg->reset_hold_ticks = ticks;
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
	snprintf(buf, len, "/sys/class/gpio/gpio%d/%s", gpio, leaf);
}

static int gpio_export(int gpio)
{
	char dir[64];
	char text[16];

	snprintf(dir, sizeof(dir), "/sys/class/gpio/gpio%d", gpio);
	if (file_exists(dir))
		return 0;

	snprintf(text, sizeof(text), "%d", gpio);
	write_text("/sys/class/gpio/export", text);
	return file_exists(dir) ? 0 : -1;
}

static int gpio_direction(int gpio, const char *direction)
{
	char path[96];

	if (gpio_export(gpio) < 0)
		return -1;
	gpio_path(path, sizeof(path), gpio, "direction");
	return write_text(path, direction);
}

static int gpio_read_value(int gpio, int fallback)
{
	char path[96];
	char value = '\0';
	int fd;

	if (gpio_export(gpio) < 0)
		return fallback;

	gpio_path(path, sizeof(path), gpio, "value");
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return fallback;

	if (read(fd, &value, 1) != 1)
		value = fallback ? '1' : '0';
	close(fd);

	return value == '1' ? 1 : 0;
}

static int gpio_write_value(int gpio, int value)
{
	char path[96];

	if (gpio_direction(gpio, "out") < 0)
		return -1;

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
	value = gpio_read_value(leader, 0);

	for (int i = 1; i < cfg->pin_count; i++) {
		if (cfg->pins[i] == leader)
			continue;
		gpio_write_value(cfg->pins[i], value);
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
	unlink(EVENT_PID);
	unlink(BUTTON_PID);
}

static int read_pid(const char *path)
{
	char buf[32];
	char *end = NULL;
	long pid;

	if (read_first_line(path, buf, sizeof(buf)) < 0)
		return -1;
	pid = strtol(buf, &end, 10);
	if (end == buf || pid <= 0)
		return -1;
	return (int)pid;
}

static int signal_daemon(int sig)
{
	int pid = read_pid(EVENT_PID);
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
	    "/usr/bin/c120-setup-ap stop >/dev/null 2>&1" :
	    "/usr/bin/c120-setup-ap start >/dev/null 2>&1";

	log_msg(cfg, "%s setup AP", active ? "stopping" : "starting");
	(void)system(cmd);
}

static void daemon_loop(void)
{
	struct config cfg;
	int pressed = 0;
	int held = 0;
	int light_elapsed = 0;

	load_config(&cfg);
	signal(SIGHUP, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

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
			load_config(&cfg);
			gpio_direction(cfg.reset_gpio, "in");
			reload_requested = 0;
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
					usleep((useconds_t)cfg.reset_poll_ms * 1000U);
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

		usleep((useconds_t)cfg.reset_poll_ms * 1000U);
	}
}

static int status_cmd(void)
{
	struct config cfg;
	char mode[32] = "";
	int leader;

	load_config(&cfg);
	leader = cfg.pin_count > 0 ? cfg.pins[0] : -1;
	read_first_line(LAMP_STATE, mode, sizeof(mode));

	printf("eventd=%s\n", read_pid(EVENT_PID) > 0 ? "running" : "stopped");
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
		daemon_loop();
		return 0;
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
