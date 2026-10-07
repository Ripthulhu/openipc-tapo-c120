#ifndef C120_CATALOGUE_H
#define C120_CATALOGUE_H
#include <json-c/json.h>
#include <stdint.h>

struct json_object *catalogue_defaults(void);
int catalogue_valid(struct json_object *config);
void catalogue_configure(struct json_object *config);
void catalogue_storage(struct json_object *records);
void catalogue_recording_enabled(int enabled);
/* Completed media only. Detections and snapshot are optional; imports never notify. */
int catalogue_complete(const char *path, const char *source, double start,
    double duration, struct json_object *detections, const char *snapshot,
    double captured, int notify);
struct json_object *catalogue_query(const char *query, int *status);
void catalogue_work(void);
void catalogue_poll(void);
void catalogue_stop(void);
int catalogue_native(const char *path, const char *reason, const char *seconds);
#endif
