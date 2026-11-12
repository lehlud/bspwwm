/* SPDX-License-Identifier: BSD-2-Clause
 * bspwm-style commands over a private, nonblocking Unix socket.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <wlr/types/wlr_output.h>
#include "server.h"

#define EQ(a,b) (!strcmp((a),(b)))
static const char *state_name(enum view_state state) {
	return state == VIEW_FULLSCREEN ? "fullscreen" : state == VIEW_FLOATING ? "floating" : "tiled";
}
static bool number(const char *str, double *value) {
	char *end; errno = 0;
	*value = strtod(str, &end);
	return *str && !*end && !errno && isfinite(*value);
}
static enum bsp_direction direction(const char *str) {
	if (EQ(str, "north")) return BSP_NORTH;
	if (EQ(str, "east")) return BSP_EAST;
	if (EQ(str, "south")) return BSP_SOUTH;
	if (EQ(str, "west")) return BSP_WEST;
	return BSP_AUTO;
}
static struct output *select_output(struct server *s, const char *name) {
	if (!name || EQ(name, "focused")) return s->active_output;
	struct output *o;
	wl_list_for_each(o, &s->outputs, link) if (EQ(name, o->wlr->name)) return o;
	return NULL;
}
static struct desktop *select_desktop(struct server *s, const char *name) {
	struct output *active = s->active_output;
	if (!active) return NULL;
	struct desktop *current = active->current;
	if (!name || EQ(name, "focused")) return current;
	if (EQ(name, "next") || EQ(name, "prev")) {
		struct wl_list *p = EQ(name, "next") ? current->link.next : current->link.prev;
		if (p == &active->desktops) p = EQ(name, "next") ? p->next : p->prev;
		return wl_container_of(p, current, link);
	}
	struct output *o;
	/* Prefer matching names on the focused output. */
	struct desktop *d;
	wl_list_for_each(d, &active->desktops, link) if (EQ(name, d->name)) return d;
	wl_list_for_each(o, &s->outputs, link) wl_list_for_each(d, &o->desktops, link) {
		char id[32]; snprintf(id, sizeof(id), "0x%08X", d->id);
		if (EQ(name, d->name) || EQ(name, id)) return d;
	}
	return NULL;
}
static struct view *select_view(struct server *s, const char *name, struct view *relative) {
	if (!name || EQ(name, "focused")) return s->focused;
	enum bsp_direction dir = direction(name);
	if (dir != BSP_AUTO && relative && relative->desktop) {
		struct bsp_node *n = bsp_neighbor(relative->desktop->root, relative->node, dir);
		return n ? n->data : NULL;
	}
	if ((EQ(name, "next") || EQ(name, "prev")) && relative && relative->desktop) {
		struct bsp_node *first = bsp_first(relative->desktop->root), *last = first, *prev = NULL;
		for (struct bsp_node *n = first; n; n = bsp_next(n)) {
			if (n == relative->node && EQ(name, "next")) {
				struct bsp_node *next = bsp_next(n); return (next ? next : first)->data;
			}
			if (n == relative->node) prev = last;
			last = n;
		}
		return (relative->node == first ? last : prev)->data;
	}
	char *end; errno = 0;
	unsigned long id = strtoul(name, &end, 0);
	if (!*name || *end || errno || id > UINT32_MAX) return NULL;
	struct view *v;
	wl_list_for_each(v, &s->views, link) if (v->mapped && v->node && v->node->id == id) return v;
	return NULL;
}

static size_t utf8_length(const unsigned char *p) {
	size_t len = *p >= 0xc2 && *p <= 0xdf ? 2 :
		*p >= 0xe0 && *p <= 0xef ? 3 : *p >= 0xf0 && *p <= 0xf4 ? 4 : 0;
	for (size_t i = 1; i < len; i++) if (!p[i] || (p[i] & 0xc0) != 0x80) return 0;
	if (len == 3 && ((*p == 0xe0 && p[1] < 0xa0) || (*p == 0xed && p[1] >= 0xa0))) return 0;
	if (len == 4 && ((*p == 0xf0 && p[1] < 0x90) || (*p == 0xf4 && p[1] >= 0x90))) return 0;
	return len;
}
static void json_string(FILE *f, const char *str) {
	fputc('"', f);
	for (const unsigned char *p = (const unsigned char *)(str ? str : ""); *p; p++) {
		if (*p == '"' || *p == '\\') fprintf(f, "\\%c", *p);
		else if (*p >= 128) {
			size_t len = utf8_length(p);
			if (len) { fwrite(p, 1, len, f); p += len - 1; }
			else fprintf(f, "\\u%04x", *p);
		}
		else if (*p < 32) fprintf(f, "\\u%04x", *p);
		else fputc(*p, f);
	}
	fputc('"', f);
}
static void json_box(FILE *f, struct bsp_box b) {
	fprintf(f, "{\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d}", b.x, b.y, b.width, b.height);
}
static void json_node(FILE *f, struct bsp_node *n) {
	if (!n) { fputs("null", f); return; }
	fprintf(f, "{\"id\":%u,\"splitType\":\"%s\",\"splitRatio\":%.6f,\"vacant\":%s,\"rectangle\":",
		n->id, n->vertical ? "vertical" : "horizontal", n->ratio, n->vacant ? "true" : "false");
	json_box(f, n->box);
	struct view *v = n->data;
	if (v) {
		fputs(",\"client\":{\"appId\":", f); json_string(f, v->xdg->app_id);
		fputs(",\"title\":", f); json_string(f, v->xdg->title);
		fprintf(f, ",\"state\":\"%s\",\"floatingRectangle\":", state_name(v->state));
		json_box(f, v->floating); fputc('}', f);
	} else fputs(",\"client\":null", f);
	fputs(",\"firstChild\":", f); json_node(f, n->first);
	fputs(",\"secondChild\":", f); json_node(f, n->second); fputc('}', f);
}
static void json_desktop(FILE *f, struct desktop *d) {
	fprintf(f, "{\"id\":%u,\"name\":", d->id); json_string(f, d->name);
	fprintf(f, ",\"layout\":\"%s\",\"focusedNodeId\":%u,\"root\":",
		d->monocle ? "monocle" : "tiled", d->focus ? d->focus->id : 0);
	json_node(f, d->root); fputc('}', f);
}
static void json_output(FILE *f, struct output *o) {
	fprintf(f, "{\"id\":%u,\"name\":", o->id); json_string(f, o->wlr->name);
	fprintf(f, ",\"focusedDesktopId\":%u,\"desktops\":[", o->current->id);
	struct desktop *d; bool first = true;
	wl_list_for_each(d, &o->desktops, link) {
		if (!first) fputc(',', f);
		first = false; json_desktop(f, d);
	}
	fputs("]}", f);
}
static bool query(struct server *s, FILE *f, int argc, char **argv) {
	if (argc < 1) return false;
	char *kind = *argv++; argc--;
	struct output *o = NULL; struct desktop *d = NULL; struct view *v = NULL;
	bool names = false;
	while (argc) {
		if (EQ(argv[0], "--names")) { names = true; argc--; argv++; continue; }
		if (argc < 2 || o || d || v) return false;
		if (EQ(argv[0], "-m")) { o = select_output(s, argv[1]); if (!o) return false; }
		else if (EQ(argv[0], "-d")) { d = select_desktop(s, argv[1]); if (!d) return false; }
		else if (EQ(argv[0], "-n")) { v = select_view(s, argv[1], s->focused); if (!v) return false; }
		else return false;
		argc -= 2; argv += 2;
	}
	bool is_tree = EQ(kind, "-T") || EQ(kind, "--tree");
	bool is_nodes = EQ(kind, "-N") || EQ(kind, "--nodes");
	bool is_desktops = EQ(kind, "-D") || EQ(kind, "--desktops");
	bool is_outputs = EQ(kind, "-M") || EQ(kind, "--monitors");
	if ((names && !is_desktops && !is_outputs) || (v && !is_tree && !is_nodes) ||
		(d && !is_tree && !is_nodes && !is_desktops)) return false;
	if (EQ(kind, "-T") || EQ(kind, "--tree")) {
		if (v) json_node(f, v->node);
		else if (d) json_desktop(f, d);
		else if (o) json_output(f, o);
		else {
			fprintf(f, "{\"focusedMonitorId\":%u,\"monitors\":[", s->active_output ? s->active_output->id : 0);
			bool first = true; struct output *it;
			wl_list_for_each(it, &s->outputs, link) { if (!first) fputc(',', f); first = false; json_output(f, it); }
			fputs("]}", f);
		}
		fputc('\n', f); return true;
	}
	if (EQ(kind, "-N") || EQ(kind, "--nodes")) {
		struct view *it;
		wl_list_for_each(it, &s->views, link) if (it->mapped && it->node &&
			(!d || it->desktop == d) && (!o || (it->desktop && it->desktop->output == o)) && (!v || v == it))
			fprintf(f, "0x%08X\n", it->node->id);
		return true;
	}
	if (EQ(kind, "-D") || EQ(kind, "--desktops")) {
		struct output *it; struct desktop *dt;
		wl_list_for_each(it, &s->outputs, link) if (!o || it == o)
			wl_list_for_each(dt, &it->desktops, link) if (!d || d == dt) {
				if (names) fprintf(f, "%s\n", dt->name); else fprintf(f, "0x%08X\n", dt->id);
			}
		return true;
	}
	if (EQ(kind, "-M") || EQ(kind, "--monitors")) {
		struct output *it;
		wl_list_for_each(it, &s->outputs, link) if (!o || it == o) {
			if (names) fprintf(f, "%s\n", it->wlr->name); else fprintf(f, "0x%08X\n", it->id);
		}
		return true;
	}
	return false;
}
static bool config(struct server *s, FILE *f, int argc, char **argv) {
	if (argc < 1 || argc > 2) return false;
	int *integer = NULL;
	if (EQ(argv[0], "window_gap")) integer = &s->gap;
	else if (EQ(argv[0], "border_width")) integer = &s->border_width;
	if (integer) {
		if (argc == 1) fprintf(f, "%d\n", *integer);
		else {
			double value;
			if (!number(argv[1], &value) || value < 0 || value > 512 || floor(value) != value) return false;
			*integer = (int)value;
		}
	} else if (EQ(argv[0], "split_ratio")) {
		if (argc == 1) fprintf(f, "%.6f\n", s->split_ratio);
		else { double value; if (!number(argv[1], &value) || value <= 0 || value >= 1) return false; s->split_ratio = value; }
	} else if (EQ(argv[0], "focus_follows_pointer")) {
		if (argc == 1) fprintf(f, "%s\n", s->focus_follows_pointer ? "true" : "false");
		else if (EQ(argv[1], "true") || EQ(argv[1], "on")) s->focus_follows_pointer = true;
		else if (EQ(argv[1], "false") || EQ(argv[1], "off")) s->focus_follows_pointer = false;
		else return false;
	} else return false;
	arrange(s); return true;
}
static bool node_command(struct server *s, int argc, char **argv) {
	struct view *v = s->focused;
	if (argc && argv[0][0] != '-') { v = select_view(s, argv[0], v); argc--; argv++; }
	if (!v || argc < 1 || argc > 2) return false;
	const char *op = argv[0], *arg = argc == 2 ? argv[1] : NULL;
	if (EQ(op, "-f") || EQ(op, "--focus")) {
		if (arg) v = select_view(s, arg, v);
		if (!v) return false;
		focus_view(s, v);
	} else if ((EQ(op, "-c") || EQ(op, "--close")) && !arg) wlr_xdg_toplevel_send_close(v->xdg);
	else if ((EQ(op, "-t") || EQ(op, "--state")) && arg) {
		bool toggle = *arg == '~'; if (toggle) arg++;
		enum view_state state;
		if (EQ(arg, "tiled")) state = VIEW_TILED;
		else if (EQ(arg, "floating")) state = VIEW_FLOATING;
		else if (EQ(arg, "fullscreen")) state = VIEW_FULLSCREEN;
		else return false;
		if (toggle && state == v->state) state = state == VIEW_FULLSCREEN ? v->previous_state : VIEW_TILED;
		set_view_state(v, state); if (state == VIEW_FULLSCREEN) focus_view(s, v);
	} else if ((EQ(op, "-d") || EQ(op, "--to-desktop")) && arg) {
		struct desktop *d = select_desktop(s, arg); if (!d || !move_view(v, d)) return false;
	} else if ((EQ(op, "-p") || EQ(op, "--presel-dir")) && arg) {
		enum bsp_direction dir = direction(arg); if (dir == BSP_AUTO && !EQ(arg, "cancel")) return false;
		v->presel = dir;
	} else if ((EQ(op, "-r") || EQ(op, "--ratio")) && arg) {
		double value; if (!v->node->parent || !number(arg, &value) || value <= 0 || value >= 1) return false;
		v->node->parent->ratio = value;
	} else if ((EQ(op, "-o") || EQ(op, "--presel-ratio")) && arg) {
		double value; if (!number(arg, &value) || value <= 0 || value >= 1) return false;
		v->presel_ratio = value;
	} else if ((EQ(op, "-s") || EQ(op, "--swap")) && arg) {
		struct view *other = select_view(s, arg, v);
		if (!other || other == v) return false;
		struct bsp_node *a = v->node, *b = other->node;
		struct desktop *ad = v->desktop, *bd = other->desktop;
		uint32_t id = a->id; a->id = b->id; b->id = id;
		a->data = other; b->data = v; a->vacant = other->state != VIEW_TILED; b->vacant = v->state != VIEW_TILED;
		v->node = b; other->node = a; v->desktop = bd; other->desktop = ad;
		if (s->focused == v) focus_view(s, v); else if (s->focused == other) focus_view(s, other);
	} else return false;
	arrange(s); return true;
}
static bool desktop_command(struct server *s, int argc, char **argv) {
	struct desktop *d = select_desktop(s, NULL);
	if (argc && argv[0][0] != '-') { d = select_desktop(s, argv[0]); argc--; argv++; }
	if (!d || argc < 1 || argc > 2) return false;
	const char *op = argv[0], *arg = argc == 2 ? argv[1] : NULL;
	if (EQ(op, "-f") || EQ(op, "--focus")) {
		if (arg) d = select_desktop(s, arg);
		if (!d) return false;
		focus_desktop(d);
	} else if ((EQ(op, "-l") || EQ(op, "--layout")) && arg) {
		if (EQ(arg, "monocle")) d->monocle = true;
		else if (EQ(arg, "tiled")) d->monocle = false;
		else return false;
	} else if ((EQ(op, "-R") || EQ(op, "--rotate")) && arg) {
		if (!EQ(arg, "90") && !EQ(arg, "180") && !EQ(arg, "270")) return false;
		bsp_rotate(d->root, (unsigned)atoi(arg));
	} else if ((EQ(op, "-F") || EQ(op, "--flip")) && arg) {
		if (!EQ(arg, "horizontal") && !EQ(arg, "vertical")) return false;
		bsp_flip(d->root, EQ(arg, "horizontal"));
	} else if ((EQ(op, "-B") || EQ(op, "--balance")) && !arg) bsp_balance(d->root);
	else if ((EQ(op, "-n") || EQ(op, "--rename")) && arg && *arg && strlen(arg) < sizeof(d->name)) {
		struct desktop *it;
		wl_list_for_each(it, &d->output->desktops, link) if (it != d && EQ(it->name, arg)) return false;
		snprintf(d->name, sizeof(d->name), "%s", arg);
	} else return false;
	arrange(s); return true;
}
static bool monitor_command(struct server *s, int argc, char **argv) {
	struct output *o = s->active_output;
	if (argc && argv[0][0] != '-') { o = select_output(s, argv[0]); argc--; argv++; }
	if (!o || argc < 1) return false;
	if (EQ(argv[0], "-f") && argc <= 2) {
		if (argc == 2) o = select_output(s, argv[1]);
		if (!o) return false;
		focus_desktop(o->current); return true;
	}
	bool reset = EQ(argv[0], "-d") || EQ(argv[0], "--reset-desktops");
	if ((!reset && !EQ(argv[0], "-a") && !EQ(argv[0], "--add-desktops")) || argc < 2) return false;
	argc--; argv++;
	for (int i = 0; i < argc; i++) {
		if (!*argv[i] || strlen(argv[i]) >= 128) return false;
		for (int j = 0; j < i; j++) if (EQ(argv[i], argv[j])) return false;
	}
	struct desktop *d, *tmp; int i = 0;
	if (reset) {
		wl_list_for_each(d, &o->desktops, link) if (i++ >= argc && d->root) return false;
	} else wl_list_for_each(d, &o->desktops, link) for (i = 0; i < argc; i++) if (EQ(d->name, argv[i])) return false;
	i = 0;
	if (reset) wl_list_for_each_safe(d, tmp, &o->desktops, link) {
		if (i < argc) snprintf(d->name, sizeof(d->name), "%s", argv[i++]);
		else {
			if (o->current == d) o->current = NULL;
			wl_list_remove(&d->link); free(d);
		}
	}
	for (; i < argc; i++) desktop_create(o, argv[i]);
	if (!o->current) o->current = wl_container_of(o->desktops.next, o->current, link);
	arrange(s); return true;
}

char *ipc_execute(struct server *s, int argc, char **argv) {
	char *reply = NULL; size_t size = 0;
	FILE *f = open_memstream(&reply, &size);
	if (!f) return NULL;
	bool ok = false;
	if (argc > 0) {
		if (EQ(argv[0], "query")) ok = query(s, f, argc - 1, argv + 1);
		else if (EQ(argv[0], "config")) ok = config(s, f, argc - 1, argv + 1);
		else if (EQ(argv[0], "node")) ok = node_command(s, argc - 1, argv + 1);
		else if (EQ(argv[0], "desktop")) ok = desktop_command(s, argc - 1, argv + 1);
		else if (EQ(argv[0], "monitor")) ok = monitor_command(s, argc - 1, argv + 1);
		else if (EQ(argv[0], "quit") && argc == 1) { ok = true; wl_display_terminate(s->display); }
	}
	fclose(f);
	if (!ok) {
		free(reply);
		reply = strdup("\x07" "Unsupported command, invalid argument, or target not found (see doc/WAYLAND.md).\n");
	}
	return reply;
}

struct ipc_client {
	struct wl_list link;
	struct server *server;
	int fd;
	struct wl_event_source *source, *timer;
	char input[65537];
	size_t used, sent, reply_size;
	char *reply;
};
static void client_destroy(struct ipc_client *c) {
	wl_list_remove(&c->link);
	wl_event_source_remove(c->source); wl_event_source_remove(c->timer);
	close(c->fd); free(c->reply); free(c);
}
static int client_timeout(void *data) { client_destroy(data); return 0; }
static int client_io(int fd, uint32_t mask, void *data) {
	struct ipc_client *c = data;
	if (mask & WL_EVENT_ERROR) { client_destroy(c); return 0; }
	if (!c->reply) {
		for (;;) {
			ssize_t count = recv(fd, c->input + c->used, sizeof(c->input) - c->used, 0);
			if (count < 0 && errno == EINTR) continue;
			if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
			if (count < 0) { client_destroy(c); return 0; }
			if (!count) break;
			c->used += (size_t)count;
			if (c->used > 65536) { client_destroy(c); return 0; }
		}
		char *argv[128]; int argc = 0; size_t offset = 0;
		while (offset < c->used && argc < 128) {
			char *end = memchr(c->input + offset, 0, c->used - offset);
			if (!end || end == c->input + offset) break;
			argv[argc++] = c->input + offset; offset = (size_t)(end - c->input) + 1;
		}
		if (offset != c->used || !argc) c->reply = strdup("\x07" "Malformed request.\n");
		else c->reply = ipc_execute(c->server, argc, argv);
		if (!c->reply) { client_destroy(c); return 0; }
		c->reply_size = strlen(c->reply);
		wl_event_source_fd_update(c->source, WL_EVENT_WRITABLE);
	}
	while (c->sent < c->reply_size) {
		ssize_t count = send(fd, c->reply + c->sent, c->reply_size - c->sent, MSG_NOSIGNAL);
		if (count < 0 && errno == EINTR) continue;
		if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
		if (count <= 0) { client_destroy(c); return 0; }
		c->sent += (size_t)count;
	}
	client_destroy(c); return 0;
}
static int accept_client(int fd, uint32_t mask, void *data) {
	(void)mask;
	struct server *s = data;
	for (;;) {
		int client = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (client < 0 && errno == EINTR) continue;
		if (client < 0) return 0;
		if (wl_list_length(&s->ipc_clients) >= 64) { close(client); continue; }
		struct ipc_client *c = calloc(1, sizeof(*c));
		if (!c) { close(client); continue; }
		c->fd = client; c->server = s;
		struct wl_event_loop *loop = wl_display_get_event_loop(s->display);
		c->source = wl_event_loop_add_fd(loop, client, WL_EVENT_READABLE, client_io, c);
		c->timer = wl_event_loop_add_timer(loop, client_timeout, c);
		if (!c->source || !c->timer) {
			if (c->source) wl_event_source_remove(c->source);
			if (c->timer) wl_event_source_remove(c->timer);
			close(client); free(c); continue;
		}
		wl_event_source_timer_update(c->timer, 5000);
		wl_list_insert(&s->ipc_clients, &c->link);
	}
}
int ipc_init(struct server *s) {
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	const char *path = getenv("BSPWM_SOCKET"), *runtime = getenv("XDG_RUNTIME_DIR"), *display = getenv("WAYLAND_DISPLAY");
	int len = path ? snprintf(address.sun_path, sizeof(address.sun_path), "%s", path) :
		snprintf(address.sun_path, sizeof(address.sun_path), "%s/bspwwm-%s.sock", runtime ? runtime : "", display ? display : "");
	if (len <= 0 || (size_t)len >= sizeof(address.sun_path)) { errno = ENAMETOOLONG; goto fail; }
	s->ipc_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (s->ipc_fd < 0) goto fail;
	/* Never unlink an existing endpoint: that could belong to another session. */
	mode_t old = umask(0077);
	int ret = bind(s->ipc_fd, (struct sockaddr *)&address, sizeof(address));
	umask(old);
	if (ret < 0) goto fail;
	s->socket_path = strdup(address.sun_path);
	if (!s->socket_path || listen(s->ipc_fd, 16) < 0) goto fail;
	s->ipc_source = wl_event_loop_add_fd(wl_display_get_event_loop(s->display), s->ipc_fd, WL_EVENT_READABLE, accept_client, s);
	if (!s->ipc_source) goto fail;
	setenv("BSPWM_SOCKET", s->socket_path, 1);
	return 0;
fail:
	perror("bspwwm: IPC"); ipc_finish(s); return -1;
}
void ipc_finish(struct server *s) {
	struct ipc_client *c, *tmp;
	wl_list_for_each_safe(c, tmp, &s->ipc_clients, link) client_destroy(c);
	if (s->ipc_source) { wl_event_source_remove(s->ipc_source); s->ipc_source = NULL; }
	if (s->ipc_fd >= 0) { close(s->ipc_fd); s->ipc_fd = -1; }
	if (s->socket_path) { unlink(s->socket_path); free(s->socket_path); s->socket_path = NULL; }
}
