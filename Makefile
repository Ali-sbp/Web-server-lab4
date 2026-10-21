CC = cc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -Iinclude
# macOS also builds the lab; Debian uses the POSIX declarations above.
ifeq ($(shell uname -s),Darwin)
CPPFLAGS += -D_DARWIN_C_SOURCE
endif
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O2
LDFLAGS =
LDLIBS =

SOURCES = src/main.c src/config.c src/server.c src/http.c src/files.c \
          src/daemon.c src/logger.c
OBJECTS = $(SOURCES:src/%.c=build/%.o)
DEPS = $(OBJECTS:.o=.d)

.PHONY: all clean
all: webserver

webserver: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

clean:
	$(RM) $(OBJECTS) $(DEPS) webserver
