#ifndef DAEMON_H
#define DAEMON_H

/* Return in the daemon only; the launching process exits with its status. */
int daemon_detach(void);

#endif
