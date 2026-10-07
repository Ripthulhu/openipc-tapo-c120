#ifndef C120_RECORD_H
#define C120_RECORD_H
#include <json-c/json.h>
struct json_object *record_defaults(void);
int record_valid(struct json_object *config);
void record_configure(struct json_object *config);
void record_storage(struct json_object *records);
void record_observe(const char *label);
void record_poll(void);
void record_stop(void);
struct json_object *record_state(void);
#endif
