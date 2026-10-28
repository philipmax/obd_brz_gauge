#pragma once
#include <stddef.h>
typedef struct { size_t content_len; } httpd_req_t;
#define HTTPD_SOCK_ERR_TIMEOUT -2
int httpd_req_recv(httpd_req_t *, char *, size_t);
