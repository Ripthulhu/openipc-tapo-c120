#ifndef C120_MODELS_H
#define C120_MODELS_H
#include <json-c/json.h>
#include "bird.h"
struct detector_model {
    char id[49], name[81], sha256[65];
    unsigned bytes;
    int bird, fd; /* 0=stock; otherwise enum bird_decoder. */
    double confidence, nms;
};
int model_id_valid(const char *id);
int model_profile(struct json_object *json, struct detector_model *model);
int model_open(const char *id, struct detector_model *model, char error[160]);
int model_available(const char *id);
struct json_object *models_state(const char *selected, const char *active);
struct json_object *models_request(struct json_object *request, const char *selected,
                                  const char *active, const char *previous);
#endif
