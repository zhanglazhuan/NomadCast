/* ESP-IDF stub for pc_demo interfaces/i_http_client.h */
#ifndef I_HTTP_CLIENT_STUB_H
#define I_HTTP_CLIENT_STUB_H
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    bool (*get)(const char *url, char **body, int *len);
    bool (*post_json)(const char *url, const char *json, char **body, int *len);
} i_http_client_t;
extern i_http_client_t g_http; /* defined in backend_esp.c */
#ifdef __cplusplus
}
#endif
#endif
