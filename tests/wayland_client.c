/* SPDX-License-Identifier: BSD-2-Clause
 * A real SHM xdg-shell client for deterministic, GPU-free integration tests.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

struct buffer { struct wl_buffer *wl; void *pixels; size_t size; struct buffer *next; };
struct frame { struct wl_callback *wl; struct frame *next; };
static struct buffer *buffers;
static struct frame *callbacks;
static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
static struct wl_surface *surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *toplevel;
static struct wl_surface *popup_surface;
static struct xdg_surface *popup_xdg_surface;
static struct xdg_popup *popup;
static int width = 320, height = 240;
static bool running = true, mapped = true, full;
static unsigned frames;

static void released(void *data, struct wl_buffer *wl) {
	struct buffer *b = data;
	struct buffer **link = &buffers;
	while (*link != b) link = &(*link)->next;
	*link = b->next;
	wl_buffer_destroy(wl); munmap(b->pixels, b->size); free(b);
}
static const struct wl_buffer_listener buffer_listener = {.release = released};
static void frame_done(void *data, struct wl_callback *callback, uint32_t time) {
	(void)time;
	struct frame *frame = data;
	struct frame **link = &callbacks;
	while (*link != frame) link = &(*link)->next;
	*link = frame->next; free(frame);
	wl_callback_destroy(callback); printf("frame %u\n", ++frames);
}
static const struct wl_callback_listener frame_listener = {.done = frame_done};
static void draw_surface(struct wl_surface *target, int width, int height) {
	assert(width > 0 && height > 0 && width <= 16384 && height <= 16384);
	struct buffer *b = calloc(1, sizeof(*b)); assert(b);
	b->next = buffers; buffers = b;
	b->size = (size_t)width * height * 4;
	int fd = memfd_create("bspwwm-test-client", MFD_CLOEXEC); assert(fd >= 0);
	assert(ftruncate(fd, (off_t)b->size) == 0);
	b->pixels = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	assert(b->pixels != MAP_FAILED);
	for (size_t i = 0; i < b->size / 4; i++) ((uint32_t *)b->pixels)[i] = 0xff346578;
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int)b->size);
	b->wl = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_XRGB8888);
	wl_buffer_add_listener(b->wl, &buffer_listener, b);
	wl_shm_pool_destroy(pool); close(fd);
	wl_surface_attach(target, b->wl, 0, 0);
	wl_surface_damage_buffer(target, 0, 0, width, height);
	struct frame *frame = calloc(1, sizeof(*frame)); assert(frame);
	frame->next = callbacks; callbacks = frame;
	frame->wl = wl_surface_frame(target);
	wl_callback_add_listener(frame->wl, &frame_listener, frame);
	wl_surface_commit(target);
}
static void configured(void *data, struct xdg_surface *xdg, uint32_t serial) {
	(void)data;
	xdg_surface_ack_configure(xdg, serial);
	printf("configure %d %d\n", width, height);
	if (mapped) draw_surface(surface, width, height);
}
static const struct xdg_surface_listener surface_listener = {.configure = configured};
static void popup_configured(void *data, struct xdg_surface *xdg, uint32_t serial) {
	(void)data;
	xdg_surface_ack_configure(xdg, serial);
	draw_surface(popup_surface, 100, 50); puts("popup configured");
}
static const struct xdg_surface_listener popup_surface_listener = {.configure = popup_configured};
static void popup_geometry(void *data, struct xdg_popup *p, int32_t x, int32_t y, int32_t w, int32_t h) {
	(void)data; (void)p; (void)x; (void)y; (void)w; (void)h;
}
static void destroy_popup(void) {
	if (!popup) return;
	xdg_popup_destroy(popup); xdg_surface_destroy(popup_xdg_surface); wl_surface_destroy(popup_surface);
	popup = NULL; popup_xdg_surface = NULL; popup_surface = NULL;
}
static void popup_done(void *data, struct xdg_popup *p) { (void)data; (void)p; destroy_popup(); }
static const struct xdg_popup_listener popup_listener = {.configure = popup_geometry, .popup_done = popup_done};
static void create_popup(void) {
	if (popup) return;
	struct xdg_positioner *positioner = xdg_wm_base_create_positioner(wm);
	xdg_positioner_set_size(positioner, 100, 50);
	xdg_positioner_set_anchor_rect(positioner, 0, 0, 10, 10);
	xdg_positioner_set_anchor(positioner, XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT);
	xdg_positioner_set_gravity(positioner, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
	popup_surface = wl_compositor_create_surface(compositor);
	popup_xdg_surface = xdg_wm_base_get_xdg_surface(wm, popup_surface);
	xdg_surface_add_listener(popup_xdg_surface, &popup_surface_listener, NULL);
	popup = xdg_surface_get_popup(popup_xdg_surface, xdg_surface, positioner);
	xdg_popup_add_listener(popup, &popup_listener, NULL);
	xdg_positioner_destroy(positioner); wl_surface_commit(popup_surface);
}
static void size_configure(void *data, struct xdg_toplevel *top, int32_t w, int32_t h, struct wl_array *states) {
	(void)data; (void)top; (void)states;
	if (w > 0) width = w;
	if (h > 0) height = h;
}
static void closed(void *data, struct xdg_toplevel *top) {
	(void)data; (void)top; puts("close"); running = false;
}
static const struct xdg_toplevel_listener top_listener = {.configure = size_configure, .close = closed};
static void ping(void *data, struct xdg_wm_base *base, uint32_t serial) { (void)data; xdg_wm_base_pong(base, serial); }
static const struct xdg_wm_base_listener wm_listener = {.ping = ping};
static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
	(void)data;
	if (!strcmp(interface, wl_compositor_interface.name)) compositor = wl_registry_bind(registry, name, &wl_compositor_interface, version < 4 ? version : 4);
	if (!strcmp(interface, wl_shm_interface.name)) shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	if (!strcmp(interface, xdg_wm_base_interface.name)) wm = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
}
static void global_remove(void *data, struct wl_registry *registry, uint32_t name) { (void)data; (void)registry; (void)name; }
static const struct wl_registry_listener registry_listener = {.global = global, .global_remove = global_remove};

int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IOLBF, 0);
	display = wl_display_connect(NULL); if (!display) { perror("connect"); return 1; }
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	assert(wl_display_roundtrip(display) >= 0 && compositor && wm && shm);
	xdg_wm_base_add_listener(wm, &wm_listener, NULL);
	surface = wl_compositor_create_surface(compositor);
	xdg_surface = xdg_wm_base_get_xdg_surface(wm, surface);
	xdg_surface_add_listener(xdg_surface, &surface_listener, NULL);
	toplevel = xdg_surface_get_toplevel(xdg_surface);
	xdg_toplevel_add_listener(toplevel, &top_listener, NULL);
	xdg_toplevel_set_app_id(toplevel, argc > 1 ? argv[1] : "bspwwm-test");
	xdg_toplevel_set_title(toplevel, "Test \"window\" \\ title\nGrüße");
	wl_surface_commit(surface);
	char command[128]; size_t used = 0;
	while (running) {
		while (wl_display_prepare_read(display) != 0) if (wl_display_dispatch_pending(display) < 0) return 1;
		if (wl_display_flush(display) < 0 && errno != EAGAIN) { wl_display_cancel_read(display); return 1; }
		struct pollfd fds[] = {{wl_display_get_fd(display), POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
		int ret = poll(fds, 2, 10000);
		if (ret < 0 && errno == EINTR) { wl_display_cancel_read(display); continue; }
		if (ret <= 0) { wl_display_cancel_read(display); continue; }
		if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
			if (wl_display_read_events(display) < 0) return 1;
			if (wl_display_dispatch_pending(display) < 0) return 1;
		} else wl_display_cancel_read(display);
		if (fds[1].revents & (POLLIN | POLLHUP)) {
			char ch; ssize_t n = read(STDIN_FILENO, &ch, 1);
			if (n <= 0) { running = false; continue; }
			if (ch != '\n') { if (used < sizeof(command) - 1) command[used++] = ch; continue; }
			command[used] = 0; used = 0;
			if (!strcmp(command, "popup")) create_popup();
			else if (!strcmp(command, "unpopup")) destroy_popup();
			else if (!strcmp(command, "fullscreen")) {
				full = !full;
				if (full) xdg_toplevel_set_fullscreen(toplevel, NULL); else xdg_toplevel_unset_fullscreen(toplevel);
			} else if (!strcmp(command, "unmap")) {
				mapped = false; wl_surface_attach(surface, NULL, 0, 0); wl_surface_commit(surface);
			} else if (!strcmp(command, "remap")) {
				/* xdg-shell resets these properties on a NULL-buffer unmap. */
				xdg_toplevel_set_app_id(toplevel, argc > 1 ? argv[1] : "bspwwm-test");
				xdg_toplevel_set_title(toplevel, "Test \"window\" \\ title\nGrüße");
				mapped = true; full = false; wl_surface_commit(surface);
			} else if (!strcmp(command, "exit")) running = false;
		}
	}
	destroy_popup();
	xdg_toplevel_destroy(toplevel); xdg_surface_destroy(xdg_surface); wl_surface_destroy(surface);
	while (buffers) released(buffers, buffers->wl);
	while (callbacks) {
		struct frame *next = callbacks->next;
		wl_callback_destroy(callbacks->wl); free(callbacks); callbacks = next;
	}
	xdg_wm_base_destroy(wm); wl_shm_destroy(shm); wl_compositor_destroy(compositor); wl_registry_destroy(registry);
	wl_display_flush(display); wl_display_disconnect(display);
	return 0;
}
