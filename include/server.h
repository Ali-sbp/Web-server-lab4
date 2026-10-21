#ifndef SERVER_H
#define SERVER_H

#include "config.h"
#include "logger.h"

struct files_context;

int server_listen(const struct server_config *config, struct logger *log);
void server_close(int listener);
int server_run(int listener, const struct server_config *config,
               struct files_context *files, struct logger *log);

#endif
