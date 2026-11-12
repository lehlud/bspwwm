/* SPDX-License-Identifier: BSD-2-Clause
 * Wayland platform layer for bspwwm. BSP policy lives in bsp.c.
 * wlroots event/scene integration follows tinywl (MIT, see LICENSE.wlroots).
 */
#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#include "server.h"

#define LISTEN(signal, listener, handler) do { \
	(listener).notify = (handler); wl_signal_add((signal), &(listener)); \
} while (0)
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define MIN(a,b) ((a) < (b) ? (a) : (b))

static void *alloc(size_t size) {
	void *p = calloc(1, size);
	if (!p) { perror("calloc"); exit(EXIT_FAILURE); }
	return p;
}

void spawn(const char *command) {
	pid_t pid = fork();
	if (pid < 0) { perror("fork"); return; }
	if (pid == 0) {
		setsid();
		execl("/bin/sh", "sh", "-c", command, (char *)NULL);
		_exit(127);
	}
}

struct desktop *desktop_create(struct output *o, const char *name) {
	struct desktop *d = alloc(sizeof(*d));
	d->output = o;
	d->id = ++o->server->next_id;
	snprintf(d->name, sizeof(d->name), "%s", name);
	wl_list_insert(o->desktops.prev, &d->link);
	if (!o->current) o->current = d;
	return d;
}

static struct bsp_box output_box(struct output *o) {
	struct wlr_box box;
	wlr_output_layout_get_box(o->server->layout, o->wlr, &box);
	return (struct bsp_box){box.x, box.y, MAX(1, box.width), MAX(1, box.height)};
}

void arrange(struct server *s) {
	struct output *o;
	wl_list_for_each(o, &s->outputs, link) {
		struct bsp_box box = output_box(o);
		int gap = s->gap;
		/* Like bspwm: reserve one gap here and one at each leaf's far edge. */
		box.x += gap; box.y += gap;
		box.width = MAX(1, box.width - gap);
		box.height = MAX(1, box.height - gap);
		struct desktop *d;
		wl_list_for_each(d, &o->desktops, link) bsp_layout(d->root, box, d->monocle);
	}
	struct view *v;
	wl_list_for_each(v, &s->views, link) {
		bool visible = v->mapped && v->desktop && v->desktop->output &&
			v->desktop->output->current == v->desktop;
		if (!visible && v->tree->node.enabled) wlr_seat_pointer_notify_clear_focus(s->seat);
		wlr_scene_node_set_enabled(&v->tree->node, visible);
		if (!visible) continue;
		struct bsp_box box = v->node->box;
		int bw = s->border_width;
		if (v->state == VIEW_FLOATING) {
			/* Keep floating windows reachable after transfer or output hotplug. */
			struct bsp_box bounds = output_box(v->desktop->output);
			box = v->floating;
			box.width = MIN(box.width, bounds.width); box.height = MIN(box.height, bounds.height);
			box.x = MAX(bounds.x, MIN(box.x, bounds.x + bounds.width - box.width));
			box.y = MAX(bounds.y, MIN(box.y, bounds.y + bounds.height - box.height));
			v->floating = box;
		}
		else if (v->state == VIEW_FULLSCREEN) {
			box = output_box(v->desktop->output); bw = 0;
		} else {
			box.width = MAX(1, box.width - s->gap);
			box.height = MAX(1, box.height - s->gap);
		}
		/* An arbitrarily small output must never produce a zero-size configure. */
		int width = MAX(1, box.width - 2 * bw), height = MAX(1, box.height - 2 * bw);
		wlr_scene_node_set_position(&v->tree->node, box.x, box.y);
		wlr_scene_node_set_position(&v->surface_tree->node, bw, bw);
		wlr_scene_rect_set_size(v->border, width + 2 * bw, height + 2 * bw);
		const float active[] = {0.37f, 0.56f, 0.80f, 1.0f};
		const float normal[] = {0.20f, 0.22f, 0.25f, 1.0f};
		wlr_scene_rect_set_color(v->border, v == s->focused ? active : normal);
		if (v->configured_width != width || v->configured_height != height) {
			wlr_xdg_toplevel_set_size(v->xdg, width, height);
			v->configured_width = width; v->configured_height = height;
		}
		uint32_t edges = v->state == VIEW_TILED ?
			WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT : 0;
		if (v->configured_edges != edges) {
			wlr_xdg_toplevel_set_tiled(v->xdg, edges); v->configured_edges = edges;
		}
	}
}

void focus_view(struct server *s, struct view *v) {
	if (v && (!v->mapped || !v->desktop || !v->desktop->output)) return;
	if (s->focused && s->focused != v) wlr_xdg_toplevel_set_activated(s->focused->xdg, false);
	s->focused = v;
	if (!v) {
		wlr_seat_keyboard_notify_clear_focus(s->seat);
		arrange(s); return;
	}
	struct desktop *d = v->desktop;
	s->active_output = d->output;
	if (d->output->current != d) wlr_seat_pointer_notify_clear_focus(s->seat);
	d->output->current = d;
	d->focus = v->node;
	/* Do not leave an unrelated fullscreen surface covering the new focus. */
	struct view *other;
	wl_list_for_each(other, &s->views, link) {
		if (other != v && other->desktop == d && other->state == VIEW_FULLSCREEN)
			set_view_state(other, other->previous_state);
	}
	wlr_scene_node_raise_to_top(&v->tree->node);
	wlr_xdg_toplevel_set_activated(v->xdg, true);
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(s->seat);
	if (keyboard) wlr_seat_keyboard_notify_enter(s->seat, v->xdg->base->surface,
		keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	arrange(s);
}

void focus_desktop(struct desktop *d) {
	if (!d || !d->output) return;
	struct server *s = d->output->server;
	d->output->current = d;
	s->active_output = d->output;
	wlr_seat_pointer_notify_clear_focus(s->seat);
	struct bsp_node *n = d->focus ? d->focus : bsp_first(d->root);
	focus_view(s, n ? n->data : NULL);
}

bool move_view(struct view *v, struct desktop *d) {
	if (!v->mapped || v->desktop == d) return true;
	/* Allocate destination branch first so an OOM cannot lose the window. */
	struct bsp_node *leaf = bsp_new(v->node->id, v);
	if (!leaf) return false;
	leaf->vacant = v->state != VIEW_TILED;
	if (!bsp_insert(&d->root, d->focus, leaf, BSP_AUTO, v->server->split_ratio)) {
		free(leaf); return false;
	}
	struct desktop *old = v->desktop;
	if (old) {
		struct bsp_node *fallback = bsp_remove(&old->root, v->node);
		if (old->focus == v->node) old->focus = fallback;
	}
	free(v->node); v->node = leaf; v->desktop = d; d->focus = leaf;
	if (v->server->focused == v) focus_desktop(old && old->output ? old : d);
	arrange(v->server);
	return true;
}

void set_view_state(struct view *v, enum view_state state) {
	if (state == VIEW_FULLSCREEN && v->state != VIEW_FULLSCREEN) v->previous_state = v->state;
	v->state = state;
	if (v->node) v->node->vacant = state != VIEW_TILED;
	if (v->xdg->base->initialized) wlr_xdg_toplevel_set_fullscreen(v->xdg, state == VIEW_FULLSCREEN);
	arrange(v->server);
}

static void output_frame(struct wl_listener *listener, void *data) {
	(void)data;
	struct output *o = wl_container_of(listener, o, frame);
	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(o->server->scene, o->wlr);
	if (!wlr_scene_output_commit(scene_output, NULL)) return;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_request(struct wl_listener *listener, void *data) {
	struct output *o = wl_container_of(listener, o, request_state);
	struct wlr_output_event_request_state *event = data;
	if (wlr_output_commit_state(o->wlr, event->state)) arrange(o->server);
}

static void output_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct output *o = wl_container_of(listener, o, destroy);
	struct server *s = o->server;
	bool was_active = s->active_output == o;
	struct desktop *old_current = o->current;
	wl_list_remove(&o->link);
	struct output *replacement = wl_list_empty(&s->outputs) ? NULL :
		wl_container_of(s->outputs.next, replacement, link);
	struct desktop *d, *tmp;
	wl_list_for_each_safe(d, tmp, &o->desktops, link) {
		wl_list_remove(&d->link); d->output = replacement;
		wl_list_insert(replacement ? replacement->desktops.prev : s->orphan_desktops.prev, &d->link);
	}
	if (s->active_output == o) s->active_output = replacement;
	wl_list_remove(&o->frame.link); wl_list_remove(&o->request_state.link);
	wl_list_remove(&o->destroy.link);
	free(o);
	if (!s->shutting_down) {
		if (!was_active) arrange(s);
		else if (replacement) focus_desktop(old_current->root ? old_current : replacement->current);
		else focus_view(s, NULL);
	}
}

static void new_output(struct wl_listener *listener, void *data) {
	struct server *s = wl_container_of(listener, s, new_output);
	struct wlr_output *wlr = data;
	if (!wlr_output_init_render(wlr, s->allocator, s->renderer)) return;
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr);
	if (mode) wlr_output_state_set_mode(&state, mode);
	bool ok = wlr_output_commit_state(wlr, &state);
	wlr_output_state_finish(&state);
	if (!ok) { wlr_log(WLR_ERROR, "Cannot enable output %s", wlr->name); return; }
	struct output *o = alloc(sizeof(*o));
	o->server = s; o->wlr = wlr; o->id = ++s->next_id;
	wl_list_init(&o->desktops);
	wl_list_insert(s->outputs.prev, &o->link);
	LISTEN(&wlr->events.frame, o->frame, output_frame);
	LISTEN(&wlr->events.request_state, o->request_state, output_request);
	LISTEN(&wlr->events.destroy, o->destroy, output_destroy);
	struct wlr_output_layout_output *lo = wlr_output_layout_add_auto(s->layout, wlr);
	struct wlr_scene_output *so = wlr_scene_output_create(s->scene, wlr);
	wlr_scene_output_layout_add_output(s->scene_layout, lo, so);
	struct desktop *d, *tmp;
	wl_list_for_each_safe(d, tmp, &s->orphan_desktops, link) {
		wl_list_remove(&d->link); d->output = o;
		wl_list_insert(o->desktops.prev, &d->link);
		if (!o->current) o->current = d;
	}
	if (!o->current) desktop_create(o, "1");
	if (!s->active_output) s->active_output = o;
	struct view *v;
	wl_list_for_each(v, &s->views, link) if (v->mapped && !v->desktop) move_view(v, o->current);
	if (!s->focused && o->current->focus) focus_desktop(o->current);
	else arrange(s);
}

static void layout_change(struct wl_listener *listener, void *data) {
	(void)data;
	struct server *s = wl_container_of(listener, s, layout_change);
	arrange(s);
}

static void view_map(struct wl_listener *listener, void *data) {
	(void)data;
	struct view *v = wl_container_of(listener, v, map);
	struct server *s = v->server;
	v->mapped = true;
	v->node = bsp_new(++s->next_id, v);
	if (!v->node) { wl_client_post_no_memory(v->xdg->base->client->client); return; }
	struct output *o = s->active_output;
	struct bsp_box box = o ? output_box(o) : (struct bsp_box){0, 0, 800, 600};
	v->floating = (struct bsp_box){box.x + box.width / 4, box.y + box.height / 4,
		MAX(1, box.width / 2), MAX(1, box.height / 2)};
	if (v->xdg->parent) v->state = VIEW_FLOATING;
	if (o) {
		v->desktop = o->current;
		struct view *anchor = v->desktop->focus ? v->desktop->focus->data : NULL;
		enum bsp_direction dir = anchor ? anchor->presel : BSP_AUTO;
		double ratio = anchor && anchor->presel_ratio > 0 ? anchor->presel_ratio : s->split_ratio;
		if (!bsp_insert(&v->desktop->root, v->desktop->focus, v->node, dir, ratio)) {
			free(v->node); v->node = NULL; v->desktop = NULL; v->mapped = false;
			wl_client_post_no_memory(v->xdg->base->client->client); return;
		}
		if (anchor) { anchor->presel = BSP_AUTO; anchor->presel_ratio = 0; }
	}
	set_view_state(v, v->xdg->requested.fullscreen ? VIEW_FULLSCREEN : v->state);
	focus_view(s, v);
}

static void view_unmap(struct wl_listener *listener, void *data) {
	(void)data;
	struct view *v = wl_container_of(listener, v, unmap);
	struct server *s = v->server;
	v->mapped = false;
	struct desktop *d = v->desktop;
	if (d && v->node) {
		struct bsp_node *fallback = bsp_remove(&d->root, v->node);
		if (d->focus == v->node) d->focus = fallback;
	}
	free(v->node); v->node = NULL; v->desktop = NULL;
	v->configured_width = v->configured_height = 0; v->configured_edges = 0;
	v->state = v->previous_state = VIEW_TILED; v->presel = BSP_AUTO; v->presel_ratio = 0;
	if (s->focused == v) {
		s->focused = NULL;
		focus_view(s, d && d->focus ? d->focus->data : NULL);
	}
	arrange(s);
}

static void view_commit(struct wl_listener *listener, void *data) {
	(void)data;
	struct view *v = wl_container_of(listener, v, commit);
	if (v->xdg->base->initial_commit) {
		wlr_xdg_toplevel_set_size(v->xdg, 0, 0);
		wlr_xdg_toplevel_set_fullscreen(v->xdg, v->xdg->requested.fullscreen);
	}
}

static void view_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct view *v = wl_container_of(listener, v, destroy);
	if (v->mapped) view_unmap(&v->unmap, NULL);
	wl_list_remove(&v->map.link); wl_list_remove(&v->unmap.link);
	wl_list_remove(&v->commit.link); wl_list_remove(&v->destroy.link);
	wl_list_remove(&v->request_fullscreen.link); wl_list_remove(&v->request_maximize.link);
	wl_list_remove(&v->link);
	/* wlroots destroys the xdg scene subtree; we own its surrounding border tree. */
	wlr_scene_node_destroy(&v->tree->node);
	free(v);
}

static void request_fullscreen(struct wl_listener *listener, void *data) {
	(void)data;
	struct view *v = wl_container_of(listener, v, request_fullscreen);
	if (!v->xdg->base->initialized) return;
	set_view_state(v, v->xdg->requested.fullscreen ? VIEW_FULLSCREEN : v->previous_state);
	if (v->mapped) focus_view(v->server, v);
}

static void request_maximize(struct wl_listener *listener, void *data) {
	(void)data;
	struct view *v = wl_container_of(listener, v, request_maximize);
	/* Tiling, not client maximization, owns geometry. Always acknowledge. */
	if (v->xdg->base->initialized) wlr_xdg_surface_schedule_configure(v->xdg->base);
}

static void new_toplevel(struct wl_listener *listener, void *data) {
	struct server *s = wl_container_of(listener, s, new_toplevel);
	struct view *v = alloc(sizeof(*v));
	v->server = s; v->xdg = data;
	v->tree = wlr_scene_tree_create(&s->scene->tree);
	v->tree->node.data = v;
	const float color[] = {0.2f, 0.22f, 0.25f, 1.0f};
	v->border = wlr_scene_rect_create(v->tree, 1, 1, color);
	v->surface_tree = wlr_scene_xdg_surface_create(v->tree, v->xdg->base);
	v->xdg->base->data = v->surface_tree;
	wlr_scene_node_set_enabled(&v->tree->node, false);
	wl_list_insert(s->views.prev, &v->link);
	LISTEN(&v->xdg->base->surface->events.map, v->map, view_map);
	LISTEN(&v->xdg->base->surface->events.unmap, v->unmap, view_unmap);
	LISTEN(&v->xdg->base->surface->events.commit, v->commit, view_commit);
	LISTEN(&v->xdg->events.destroy, v->destroy, view_destroy);
	LISTEN(&v->xdg->events.request_fullscreen, v->request_fullscreen, request_fullscreen);
	LISTEN(&v->xdg->events.request_maximize, v->request_maximize, request_maximize);
}

struct popup {
	struct wlr_xdg_popup *xdg;
	struct wl_listener commit, destroy;
};
static void popup_commit(struct wl_listener *listener, void *data) {
	(void)data;
	struct popup *p = wl_container_of(listener, p, commit);
	if (p->xdg->base->initial_commit) wlr_xdg_surface_schedule_configure(p->xdg->base);
}
static void popup_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct popup *p = wl_container_of(listener, p, destroy);
	wl_list_remove(&p->commit.link); wl_list_remove(&p->destroy.link); free(p);
}
static void new_popup(struct wl_listener *listener, void *data) {
	(void)listener;
	struct wlr_xdg_popup *xdg = data;
	struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xdg->parent);
	if (!parent || !parent->data) { wlr_xdg_popup_destroy(xdg); return; }
	xdg->base->data = wlr_scene_xdg_surface_create(parent->data, xdg->base);
	struct popup *p = alloc(sizeof(*p)); p->xdg = xdg;
	LISTEN(&xdg->base->surface->events.commit, p->commit, popup_commit);
	LISTEN(&xdg->events.destroy, p->destroy, popup_destroy);
}

struct keyboard {
	struct wl_list link;
	struct server *server;
	struct wlr_keyboard *wlr;
	bool consumed[KEY_MAX + 1];
	struct wl_listener key, modifiers, destroy;
};
struct pointer {
	struct wl_list link;
	struct server *server;
	struct wl_listener destroy;
};
static void capabilities(struct server *s) {
	uint32_t caps = wl_list_empty(&s->pointers) ? 0 : WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&s->keyboards)) caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	wlr_seat_set_capabilities(s->seat, caps);
}
static bool keybinding(struct server *s, xkb_keysym_t sym, uint32_t mods) {
	if (!(mods & WLR_MODIFIER_LOGO)) return false;
	struct view *v = s->focused;
	struct desktop *d = s->active_output ? s->active_output->current : NULL;
	bool shift = mods & WLR_MODIFIER_SHIFT;
	if (sym == XKB_KEY_Return) { spawn(getenv("BSPWWM_TERMINAL") ? getenv("BSPWWM_TERMINAL") : "foot"); return true; }
	if (sym == XKB_KEY_Escape && shift) { wl_display_terminate(s->display); return true; }
	if (sym == XKB_KEY_q || sym == XKB_KEY_Q) { if (v) wlr_xdg_toplevel_send_close(v->xdg); return true; }
	if (sym == XKB_KEY_f && v) { set_view_state(v, v->state == VIEW_FULLSCREEN ? v->previous_state : VIEW_FULLSCREEN); return true; }
	if (sym == XKB_KEY_space && d) { d->monocle = !d->monocle; arrange(s); return true; }
	if (sym >= XKB_KEY_1 && sym <= XKB_KEY_9 && d) {
		char name[] = {(char)('1' + sym - XKB_KEY_1), 0};
		struct desktop *target = NULL, *it;
		wl_list_for_each(it, &d->output->desktops, link) if (!strcmp(it->name, name)) target = it;
		if (!target) target = desktop_create(d->output, name);
		if (shift && v) move_view(v, target); else focus_desktop(target);
		return true;
	}
	enum bsp_direction dir = BSP_AUTO;
	if (sym == XKB_KEY_h || sym == XKB_KEY_Left) dir = BSP_WEST;
	if (sym == XKB_KEY_j || sym == XKB_KEY_Down) dir = BSP_SOUTH;
	if (sym == XKB_KEY_k || sym == XKB_KEY_Up) dir = BSP_NORTH;
	if (sym == XKB_KEY_l || sym == XKB_KEY_Right) dir = BSP_EAST;
	if (dir != BSP_AUTO && v) {
		struct bsp_node *n = bsp_neighbor(d->root, v->node, dir);
		if (n) focus_view(s, n->data);
		return true;
	}
	return false;
}
static void keyboard_key(struct wl_listener *listener, void *data) {
	struct keyboard *k = wl_container_of(listener, k, key);
	struct wlr_keyboard_key_event *event = data;
	bool handled = false;
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		const xkb_keysym_t *syms;
		/* Bind the unshifted symbol so Super+Shift+1 is still desktop 1,
		 * not the exclamation-mark symbol on e.g. a US keymap. */
		xkb_layout_index_t layout = xkb_state_key_get_layout(k->wlr->xkb_state, event->keycode + 8);
		int count = xkb_keymap_key_get_syms_by_level(k->wlr->keymap, event->keycode + 8, layout, 0, &syms);
		for (int i = 0; i < count && !handled; i++) handled = keybinding(k->server, syms[i], wlr_keyboard_get_modifiers(k->wlr));
		if (event->keycode <= KEY_MAX) k->consumed[event->keycode] = handled;
	} else if (event->keycode <= KEY_MAX) {
		handled = k->consumed[event->keycode]; k->consumed[event->keycode] = false;
	}
	if (!handled) {
		wlr_seat_set_keyboard(k->server->seat, k->wlr);
		wlr_seat_keyboard_notify_key(k->server->seat, event->time_msec, event->keycode, event->state);
	}
}
static void keyboard_modifiers(struct wl_listener *listener, void *data) {
	(void)data;
	struct keyboard *k = wl_container_of(listener, k, modifiers);
	wlr_seat_set_keyboard(k->server->seat, k->wlr);
	wlr_seat_keyboard_notify_modifiers(k->server->seat, &k->wlr->modifiers);
}
static void keyboard_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct keyboard *k = wl_container_of(listener, k, destroy);
	wl_list_remove(&k->key.link); wl_list_remove(&k->modifiers.link);
	wl_list_remove(&k->destroy.link); wl_list_remove(&k->link);
	capabilities(k->server); free(k);
}
static void pointer_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct pointer *p = wl_container_of(listener, p, destroy);
	wl_list_remove(&p->destroy.link); wl_list_remove(&p->link);
	capabilities(p->server); free(p);
}
static void new_input(struct wl_listener *listener, void *data) {
	struct server *s = wl_container_of(listener, s, new_input);
	struct wlr_input_device *device = data;
	if (device->type == WLR_INPUT_DEVICE_KEYBOARD) {
		struct keyboard *k = alloc(sizeof(*k)); k->server = s;
		k->wlr = wlr_keyboard_from_input_device(device);
		struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
		struct xkb_keymap *map = ctx ? xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
		if (!map || !wlr_keyboard_set_keymap(k->wlr, map)) {
			xkb_keymap_unref(map); xkb_context_unref(ctx); free(k); return;
		}
		xkb_keymap_unref(map); xkb_context_unref(ctx);
		wlr_keyboard_set_repeat_info(k->wlr, 25, 600);
		LISTEN(&k->wlr->events.key, k->key, keyboard_key);
		LISTEN(&k->wlr->events.modifiers, k->modifiers, keyboard_modifiers);
		LISTEN(&device->events.destroy, k->destroy, keyboard_destroy);
		wl_list_insert(&s->keyboards, &k->link);
		wlr_seat_set_keyboard(s->seat, k->wlr);
		if (s->focused) focus_view(s, s->focused);
	} else if (device->type == WLR_INPUT_DEVICE_POINTER) {
		struct pointer *p = alloc(sizeof(*p)); p->server = s;
		LISTEN(&device->events.destroy, p->destroy, pointer_destroy);
		wl_list_insert(&s->pointers, &p->link);
		wlr_cursor_attach_input_device(s->cursor, device);
	}
	capabilities(s);
}

static struct view *view_at(struct server *s, struct wlr_surface **surface, double *sx, double *sy) {
	struct wlr_scene_node *node = wlr_scene_node_at(&s->scene->tree.node, s->cursor->x, s->cursor->y, sx, sy);
	if (!node) return NULL;
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
		if (ss) *surface = ss->surface;
	}
	for (; node; node = node->parent ? &node->parent->node : NULL) if (node->data) return node->data;
	return NULL;
}
static void cursor_process(struct server *s, uint32_t time) {
	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct view *v = view_at(s, &surface, &sx, &sy);
	if (v && s->focus_follows_pointer && v != s->focused) focus_view(s, v);
	if (surface) {
		wlr_seat_pointer_notify_enter(s->seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(s->seat, time, sx, sy);
	} else {
		wlr_seat_pointer_notify_clear_focus(s->seat);
		wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, "default");
	}
}
static void cursor_motion(struct wl_listener *listener, void *data) {
	struct server *s = wl_container_of(listener, s, motion);
	struct wlr_pointer_motion_event *e = data;
	wlr_cursor_move(s->cursor, &e->pointer->base, e->delta_x, e->delta_y);
	cursor_process(s, e->time_msec);
}
static void cursor_absolute(struct wl_listener *listener, void *data) {
	struct server *s = wl_container_of(listener, s, motion_absolute);
	struct wlr_pointer_motion_absolute_event *e = data;
	wlr_cursor_warp_absolute(s->cursor, &e->pointer->base, e->x, e->y);
	cursor_process(s, e->time_msec);
}
static void cursor_button(struct wl_listener *listener, void *data) {
	struct server *s = wl_container_of(listener, s, button);
	struct wlr_pointer_button_event *e = data;
	if (e->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		double sx, sy; struct wlr_surface *surface = NULL;
		struct view *v = view_at(s, &surface, &sx, &sy);
		if (v) focus_view(s, v);
	}
	wlr_seat_pointer_notify_button(s->seat, e->time_msec, e->button, e->state);
}
static void cursor_axis(struct wl_listener *listener, void *data) {
	struct server *s = wl_container_of(listener, s, axis);
	struct wlr_pointer_axis_event *e = data;
	wlr_seat_pointer_notify_axis(s->seat, e->time_msec, e->orientation, e->delta,
		e->delta_discrete, e->source, e->relative_direction);
}
static void cursor_frame(struct wl_listener *listener, void *data) {
	(void)data; struct server *s = wl_container_of(listener, s, cursor_frame);
	wlr_seat_pointer_notify_frame(s->seat);
}
static void request_cursor(struct wl_listener *listener, void *data) {
	struct server *s = wl_container_of(listener, s, request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *e = data;
	if (s->seat->pointer_state.focused_client == e->seat_client)
		wlr_cursor_set_surface(s->cursor, e->surface, e->hotspot_x, e->hotspot_y);
}
static void pointer_focus(struct wl_listener *listener, void *data) {
	struct server *s = wl_container_of(listener, s, pointer_focus);
	struct wlr_seat_pointer_focus_change_event *e = data;
	if (!e->new_surface) wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, "default");
}
static void request_selection(struct wl_listener *listener, void *data) {
	struct server *s = wl_container_of(listener, s, request_selection);
	struct wlr_seat_request_set_selection_event *e = data;
	wlr_seat_set_selection(s->seat, e->source, e->serial);
}
static int terminate_signal(int signal_number, void *data) {
	(void)signal_number;
	struct server *s = data; wl_display_terminate(s->display); return 0;
}
static int reap(int signal_number, void *data) {
	(void)signal_number; (void)data;
	while (waitpid(-1, NULL, WNOHANG) > 0) {}
	return 0;
}
static void unlisten(struct wl_listener *listener) {
	if (listener->notify) { wl_list_remove(&listener->link); listener->notify = NULL; }
}
static void backend_destroyed(struct wl_listener *listener, void *data) {
	(void)data;
	struct server *s = wl_container_of(listener, s, backend_destroy);
	unlisten(&s->new_input); unlisten(&s->new_output); unlisten(&s->backend_destroy);
	s->backend = NULL;
	wl_display_terminate(s->display);
}

int main(int argc, char **argv) {
	char *config = NULL, *startup = NULL;
	bool no_config = false, debug = false;
	int c;
	while ((c = getopt(argc, argv, "c:s:ndhv")) != -1) switch (c) {
	case 'c': config = optarg; break;
	case 's': startup = optarg; break;
	case 'n': no_config = true; break;
	case 'd': debug = true; break;
	case 'v': puts("bspwwm 0.1.0 (wlroots 0.20)"); return 0;
	case 'h': puts("Usage: bspwwm [-d] [-n | -c config] [-s command] [-v]"); return 0;
	default: return 1;
	}
	if (optind != argc) { fprintf(stderr, "Unexpected argument\n"); return 1; }
	wlr_log_init(debug ? WLR_DEBUG : WLR_INFO, NULL);
	struct server s = {.gap = 8, .border_width = 2, .split_ratio = 0.5, .ipc_fd = -1};
	int status = 1;
	struct wl_event_source *sigterm = NULL, *sigint = NULL, *sigchld = NULL;
	wl_list_init(&s.outputs); wl_list_init(&s.views); wl_list_init(&s.keyboards);
	wl_list_init(&s.pointers); wl_list_init(&s.orphan_desktops); wl_list_init(&s.ipc_clients);
	s.display = wl_display_create();
	if (!s.display) return 1;
	struct wl_event_loop *loop = wl_display_get_event_loop(s.display);
	s.backend = wlr_backend_autocreate(loop, NULL);
	if (!s.backend) goto fail;
	s.renderer = wlr_renderer_autocreate(s.backend);
	if (!s.renderer || !wlr_renderer_init_wl_display(s.renderer, s.display)) goto fail;
	s.allocator = wlr_allocator_autocreate(s.backend, s.renderer);
	if (!s.allocator) goto fail;
	wlr_compositor_create(s.display, 6, s.renderer);
	wlr_subcompositor_create(s.display);
	wlr_data_device_manager_create(s.display);
	s.layout = wlr_output_layout_create(s.display);
	s.scene = wlr_scene_create();
	s.scene_layout = wlr_scene_attach_output_layout(s.scene, s.layout);
	s.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(s.cursor, s.layout);
	s.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
	s.seat = wlr_seat_create(s.display, "seat0");
	LISTEN(&s.backend->events.new_output, s.new_output, new_output);
	LISTEN(&s.backend->events.destroy, s.backend_destroy, backend_destroyed);
	LISTEN(&s.layout->events.change, s.layout_change, layout_change);
	LISTEN(&s.backend->events.new_input, s.new_input, new_input);
	struct wlr_xdg_shell *shell = wlr_xdg_shell_create(s.display, 3);
	LISTEN(&shell->events.new_toplevel, s.new_toplevel, new_toplevel);
	LISTEN(&shell->events.new_popup, s.new_popup, new_popup);
	LISTEN(&s.cursor->events.motion, s.motion, cursor_motion);
	LISTEN(&s.cursor->events.motion_absolute, s.motion_absolute, cursor_absolute);
	LISTEN(&s.cursor->events.button, s.button, cursor_button);
	LISTEN(&s.cursor->events.axis, s.axis, cursor_axis);
	LISTEN(&s.cursor->events.frame, s.cursor_frame, cursor_frame);
	LISTEN(&s.seat->events.request_set_cursor, s.request_cursor, request_cursor);
	LISTEN(&s.seat->pointer_state.events.focus_change, s.pointer_focus, pointer_focus);
	LISTEN(&s.seat->events.request_set_selection, s.request_selection, request_selection);
	const char *socket = wl_display_add_socket_auto(s.display);
	if (!socket) goto fail;
	/* Keep the parent's WAYLAND_DISPLAY until the nested backend has started. */
	if (!wlr_backend_start(s.backend)) goto fail;
	setenv("WAYLAND_DISPLAY", socket, 1);
	if (ipc_init(&s) < 0) goto fail;
	signal(SIGPIPE, SIG_IGN);
	sigterm = wl_event_loop_add_signal(loop, SIGTERM, terminate_signal, &s);
	sigint = wl_event_loop_add_signal(loop, SIGINT, terminate_signal, &s);
	sigchld = wl_event_loop_add_signal(loop, SIGCHLD, reap, &s);
	wlr_log(WLR_INFO, "WAYLAND_DISPLAY=%s BSPWM_SOCKET=%s", socket, s.socket_path);
	char path[4096];
	if (!no_config) {
		if (!config) {
			const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
			if (xdg) snprintf(path, sizeof(path), "%s/bspwwm/bspwwmrc", xdg);
			else snprintf(path, sizeof(path), "%s/.config/bspwwm/bspwwmrc", home ? home : "");
			config = path;
		}
		if (access(config, X_OK) == 0) {
			pid_t pid = fork();
			if (pid == 0) { setsid(); execl(config, config, (char *)NULL); _exit(127); }
		} else if (config != path) wlr_log(WLR_ERROR, "Config not executable: %s", config);
	}
	if (startup) spawn(startup);
	wl_display_run(s.display);
	status = 0;
	goto cleanup;
fail:
	wlr_log(WLR_ERROR, "%s", "Compositor initialization failed");
cleanup:
	if (sigterm) wl_event_source_remove(sigterm);
	if (sigint) wl_event_source_remove(sigint);
	if (sigchld) wl_event_source_remove(sigchld);
	s.shutting_down = true;
	ipc_finish(&s);
	wl_display_destroy_clients(s.display);
	unlisten(&s.new_output); unlisten(&s.new_input); unlisten(&s.layout_change); unlisten(&s.backend_destroy);
	unlisten(&s.new_toplevel); unlisten(&s.new_popup);
	unlisten(&s.motion); unlisten(&s.motion_absolute); unlisten(&s.button);
	unlisten(&s.axis); unlisten(&s.cursor_frame);
	unlisten(&s.request_cursor); unlisten(&s.pointer_focus); unlisten(&s.request_selection);
	if (s.backend) wlr_backend_destroy(s.backend);
	if (s.scene) wlr_scene_node_destroy(&s.scene->tree.node);
	if (s.cursor_mgr) wlr_xcursor_manager_destroy(s.cursor_mgr);
	if (s.cursor) wlr_cursor_destroy(s.cursor);
	if (s.allocator) wlr_allocator_destroy(s.allocator);
	if (s.renderer) wlr_renderer_destroy(s.renderer);
	struct desktop *d, *tmp;
	wl_list_for_each_safe(d, tmp, &s.orphan_desktops, link) {
		wl_list_remove(&d->link); bsp_destroy(d->root); free(d);
	}
	wl_display_destroy(s.display);
	return status;
}
