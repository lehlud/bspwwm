/* BSP policy ported from bspwm. See LICENSE and doc/WAYLAND.md. */
#ifndef BSPWWM_BSP_H
#define BSPWWM_BSP_H
#include <stdbool.h>
#include <stdint.h>

struct bsp_box { int x, y, width, height; };
enum bsp_direction { BSP_AUTO, BSP_NORTH, BSP_EAST, BSP_SOUTH, BSP_WEST };
struct bsp_node {
	uint32_t id;
	void *data;
	struct bsp_node *parent, *first, *second;
	struct bsp_box box;
	double ratio;
	bool vertical, vacant;
};

struct bsp_node *bsp_new(uint32_t id, void *data);
/* Insert allocates a branch, never a leaf; false leaves the tree unchanged. */
bool bsp_insert(struct bsp_node **root, struct bsp_node *anchor,
	struct bsp_node *leaf, enum bsp_direction direction, double ratio);
/* Detach leaf, free its old branch, return a remaining leaf (or NULL). */
struct bsp_node *bsp_remove(struct bsp_node **root, struct bsp_node *leaf);
void bsp_destroy(struct bsp_node *root); /* frees nodes, not data */
struct bsp_node *bsp_first(struct bsp_node *root);
struct bsp_node *bsp_next(struct bsp_node *leaf);
struct bsp_node *bsp_find(struct bsp_node *root, uint32_t id);
struct bsp_node *bsp_neighbor(struct bsp_node *root, struct bsp_node *from,
	enum bsp_direction direction);
/* Preserve bspwm's split geometry and vacant-branch behavior. */
void bsp_layout(struct bsp_node *root, struct bsp_box box, bool monocle);
void bsp_rotate(struct bsp_node *root, unsigned degrees);
void bsp_flip(struct bsp_node *root, bool horizontal);
void bsp_balance(struct bsp_node *root);
#endif
