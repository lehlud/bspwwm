/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef BSPWWM_SERVER_H
#define BSPWWM_SERVER_H
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include "bsp.h"

struct server;
struct desktop {
	struct wl_list link;
	struct output *output;
	uint32_t id;
	char name[128];
	struct bsp_node *root, *focus;
	bool monocle;
};
struct output {
	struct wl_list link, desktops;
	struct server *server;
	struct wlr_output *wlr;
	struct desktop *current;
	uint32_t id;
	struct wl_listener frame, request_state, destroy;
};
enum view_state { VIEW_TILED, VIEW_FLOATING, VIEW_FULLSCREEN };
struct view {
	struct wl_list link;
	struct server *server;
	struct wlr_xdg_toplevel *xdg;
	struct wlr_scene_tree *tree, *surface_tree;
	struct wlr_scene_rect *border;
	struct desktop *desktop;
	struct bsp_node *node;
	struct bsp_box floating;
	enum view_state state, previous_state;
	enum bsp_direction presel;
	double presel_ratio;
	bool mapped;
	int configured_width, configured_height;
	uint32_t configured_edges;
	struct wl_listener map, unmap, commit, destroy;
	struct wl_listener request_fullscreen, request_maximize;
};
struct server {
	struct wl_display *display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_output_layout *layout;
	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wlr_seat *seat;
	struct wl_list outputs, views, keyboards, pointers, orphan_desktops;
	struct output *active_output;
	struct view *focused;
	uint32_t next_id;
	int gap, border_width;
	double split_ratio;
	bool focus_follows_pointer, shutting_down;
	struct wl_listener new_output, layout_change, new_input, new_toplevel, new_popup;
	struct wl_listener backend_destroy;
	struct wl_listener motion, motion_absolute, button, axis, cursor_frame;
	struct wl_listener request_cursor, pointer_focus, request_selection;
	int ipc_fd;
	char *socket_path;
	struct wl_event_source *ipc_source;
	struct wl_list ipc_clients;
};

void arrange(struct server *s);
void focus_view(struct server *s, struct view *v);
void focus_desktop(struct desktop *d);
struct desktop *desktop_create(struct output *o, const char *name);
bool move_view(struct view *v, struct desktop *d);
void set_view_state(struct view *v, enum view_state state);
void spawn(const char *command);
int ipc_init(struct server *s);
void ipc_finish(struct server *s);
char *ipc_execute(struct server *s, int argc, char **argv);
#endif
