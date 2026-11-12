/* Copyright (c) 2012, Bastien Dejean. All rights reserved.
 * Adapted from src/bspc.c for Wayland, without XCB. Distributed under LICENSE.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char **argv) {
	if (argc < 2) { fprintf(stderr, "Usage: bspc COMMAND [ARGUMENTS...]\n"); return 1; }
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	const char *path = getenv("BSPWM_SOCKET");
	int n;
	if (path) n = snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
	else {
		const char *runtime = getenv("XDG_RUNTIME_DIR"), *display = getenv("WAYLAND_DISPLAY");
		if (!runtime || !display || strchr(display, '/')) {
			fprintf(stderr, "Set BSPWM_SOCKET or XDG_RUNTIME_DIR and WAYLAND_DISPLAY.\n"); return 1;
		}
		n = snprintf(address.sun_path, sizeof(address.sun_path), "%s/bspwwm-%s.sock", runtime, display);
	}
	if (n < 0 || (size_t)n >= sizeof(address.sun_path)) { fprintf(stderr, "Socket path too long.\n"); return 1; }
	if (argc == 2 && !strcmp(argv[1], "--print-socket-path")) { puts(address.sun_path); return 0; }
	size_t size = 0;
	for (int i = 1; i < argc; i++) size += strlen(argv[i]) + 1;
	if (argc > 129 || size > 65536) { fprintf(stderr, "Command too large.\n"); return 1; }
	char *message = malloc(size);
	if (!message) return 1;
	size_t offset = 0;
	for (int i = 1; i < argc; i++) {
		size_t len = strlen(argv[i]) + 1;
		memcpy(message + offset, argv[i], len); offset += len;
	}
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		perror("bspc: connect"); free(message); if (fd >= 0) close(fd); return 1;
	}
	for (offset = 0; offset < size;) {
		ssize_t sent = send(fd, message + offset, size - offset, MSG_NOSIGNAL);
		if (sent < 0 && errno == EINTR) continue;
		if (sent <= 0) { perror("bspc: send"); free(message); close(fd); return 1; }
		offset += (size_t)sent;
	}
	free(message);
	/* The original NUL argv encoding is retained. EOF delimits the request,
	 * unlike the X11 server's single recv(), so stream fragmentation is safe. */
	shutdown(fd, SHUT_WR);
	char buffer[4096];
	int status = 0;
	/* First reply byte carries bspwm's failure marker. */
	int first = 1;
	for (;;) {
		ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
		if (count < 0 && errno == EINTR) continue;
		if (count < 0) { perror("bspc: recv"); status = 1; break; }
		if (!count) break;
		size_t start = 0;
		if (first && buffer[0] == '\x07') { status = 1; start = 1; }
		first = 0;
		FILE *stream = status ? stderr : stdout;
		if (fwrite(buffer + start, 1, (size_t)count - start, stream) != (size_t)count - start) { status = 1; break; }
	}
	close(fd);
	return status;
}
