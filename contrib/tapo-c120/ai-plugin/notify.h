#ifndef C120_NOTIFY_H
#define C120_NOTIFY_H
#include <json-c/json.h>

struct json_object *notify_defaults(void);
int notify_valid(struct json_object *config);
void notify_configure(struct json_object *config);
void notify_observe(const char *label);
void notify_prime(const char *label);
void notify_event(struct json_object *event);
void notify_poll(void);
struct json_object *notify_state(void);
void notify_stop(void);
#endif
