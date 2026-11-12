/* Copyright (c) 2012, Bastien Dejean
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* Pure BSP policy adapted from src/tree.c: no display-server dependencies. */
#include "bsp.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>

static bool is_leaf(const struct bsp_node *node)
{
	return node != NULL && node->first == NULL && node->second == NULL;
}

static bool valid_ratio(double ratio)
{
	return isfinite(ratio) && ratio > 0.0 && ratio < 1.0;
}

static double safe_ratio(double ratio)
{
	return valid_ratio(ratio) ? ratio : 0.5;
}

static double reversed_ratio(double ratio)
{
	double reversed = 1.0 - safe_ratio(ratio);
	/* A valid subnormal ratio can round to 1 when complemented. */
	return reversed < 1.0 ? reversed : nextafter(1.0, 0.0);
}

static bool contains(const struct bsp_node *root, const struct bsp_node *node)
{
	return root != NULL && (root == node || contains(root->first, node) ||
		contains(root->second, node));
}

static bool refresh_vacancy(struct bsp_node *node)
{
	if (node == NULL) {
		return true;
	}
	if (!is_leaf(node)) {
		bool first = refresh_vacancy(node->first);
		bool second = refresh_vacancy(node->second);
		node->vacant = first && second;
	}
	return node->vacant;
}

struct bsp_node *bsp_new(uint32_t id, void *data)
{
	struct bsp_node *node = calloc(1, sizeof(*node));
	if (node != NULL) {
		node->id = id;
		node->data = data;
		node->ratio = 0.5;
		node->vertical = true;
	}
	return node;
}

bool bsp_insert(struct bsp_node **root, struct bsp_node *anchor,
	struct bsp_node *leaf, enum bsp_direction direction, double ratio)
{
	if (root == NULL || !is_leaf(leaf) || leaf->id == 0 ||
		leaf->parent != NULL || !valid_ratio(ratio) ||
		direction < BSP_AUTO || direction > BSP_WEST) {
		return false;
	}
	if (*root == NULL) {
		if (anchor != NULL) {
			return false;
		}
		*root = leaf;
		return true;
	}
	if (contains(*root, leaf) || bsp_find(*root, leaf->id) != NULL) {
		return false;
	}
	/* As upstream, a NULL anchor selects the whole tree, not an arbitrary leaf. */
	if (anchor == NULL) {
		anchor = *root;
	}
	if (!contains(*root, anchor)) {
		return false;
	}
	struct bsp_node *branch = bsp_new(0, NULL);
	if (branch == NULL) {
		return false;
	}
	struct bsp_node *parent = anchor->parent;
	branch->parent = parent;
	branch->box = anchor->box;
	branch->ratio = ratio; /* Fraction assigned to the first child in every direction. */
	branch->vertical = direction == BSP_EAST || direction == BSP_WEST ||
		(direction == BSP_AUTO && anchor->box.width > anchor->box.height);
	bool new_first = direction == BSP_NORTH || direction == BSP_WEST;
	branch->first = new_first ? leaf : anchor;
	branch->second = new_first ? anchor : leaf;
	if (parent == NULL) {
		*root = branch;
	} else if (parent->first == anchor) {
		parent->first = branch;
	} else {
		parent->second = branch;
	}
	anchor->parent = leaf->parent = branch;
	refresh_vacancy(*root);
	return true;
}

struct bsp_node *bsp_remove(struct bsp_node **root, struct bsp_node *leaf)
{
	if (root == NULL || !is_leaf(leaf) || !contains(*root, leaf)) {
		return NULL;
	}
	struct bsp_node *branch = leaf->parent;
	if (branch == NULL) {
		*root = NULL;
		return NULL;
	}
	struct bsp_node *sibling = branch->first == leaf ? branch->second : branch->first;
	struct bsp_node *parent = branch->parent;
	sibling->parent = parent;
	if (parent == NULL) {
		*root = sibling;
	} else if (parent->first == branch) {
		parent->first = sibling;
	} else {
		parent->second = sibling;
	}
	leaf->parent = NULL;
	free(branch);
	refresh_vacancy(*root);
	return bsp_first(sibling);
}

void bsp_destroy(struct bsp_node *root)
{
	if (root == NULL) {
		return;
	}
	bsp_destroy(root->first);
	bsp_destroy(root->second);
	free(root);
}

struct bsp_node *bsp_first(struct bsp_node *root)
{
	while (root != NULL && root->first != NULL) {
		root = root->first;
	}
	return root;
}

struct bsp_node *bsp_next(struct bsp_node *leaf)
{
	if (leaf == NULL) {
		return NULL;
	}
	while (leaf->parent != NULL) {
		struct bsp_node *parent = leaf->parent;
		if (parent->first == leaf) {
			return bsp_first(parent->second);
		}
		leaf = parent;
	}
	return NULL;
}

struct bsp_node *bsp_find(struct bsp_node *root, uint32_t id)
{
	if (root == NULL || root->id == id) {
		return root;
	}
	struct bsp_node *found = bsp_find(root->first, id);
	return found != NULL ? found : bsp_find(root->second, id);
}

static int saturated_add(int coordinate, int offset)
{
	int64_t result = (int64_t)coordinate + offset;
	return result > INT_MAX ? INT_MAX : result < INT_MIN ? INT_MIN : (int)result;
}

static void apply_layout(struct bsp_node *node, struct bsp_box box, bool monocle)
{
	node->box = box;
	if (is_leaf(node)) {
		return;
	}
	node->ratio = safe_ratio(node->ratio);
	struct bsp_box first = box, second = box;
	if (!monocle && !node->first->vacant && !node->second->vacant) {
		int extent = node->vertical ? box.width : box.height;
		/* At one pixel the two children necessarily overlap. Otherwise conserve
		 * the parent's extent, with at least one pixel assigned to each child. */
		if (extent > 1) {
			int fence = (int)(extent * node->ratio);
			if (fence < 1) {
				fence = 1;
			} else if (fence >= extent) {
				fence = extent - 1;
			}
			if (node->vertical) {
				first.width = fence;
				second.x = saturated_add(box.x, fence);
				second.width = extent - fence;
			} else {
				first.height = fence;
				second.y = saturated_add(box.y, fence);
				second.height = extent - fence;
			}
		}
	}
	apply_layout(node->first, first, monocle);
	apply_layout(node->second, second, monocle);
}

void bsp_layout(struct bsp_node *root, struct bsp_box box, bool monocle)
{
	if (root == NULL) {
		return;
	}
	if (box.width < 1) {
		box.width = 1;
	}
	if (box.height < 1) {
		box.height = 1;
	}
	/* Leaves are compositor-owned state; internal vacancy is always derived. */
	refresh_vacancy(root);
	apply_layout(root, box, monocle);
}

static void swap_children(struct bsp_node *node)
{
	struct bsp_node *tmp = node->first;
	node->first = node->second;
	node->second = tmp;
	node->ratio = reversed_ratio(node->ratio);
}

void bsp_rotate(struct bsp_node *root, unsigned degrees)
{
	degrees %= 360;
	if (root == NULL || is_leaf(root) || degrees == 0 || degrees % 90 != 0) {
		return;
	}
	if ((degrees == 90 && !root->vertical) ||
		(degrees == 270 && root->vertical) || degrees == 180) {
		swap_children(root);
	}
	if (degrees != 180) {
		root->vertical = !root->vertical;
	}
	bsp_rotate(root->first, degrees);
	bsp_rotate(root->second, degrees);
}

void bsp_flip(struct bsp_node *root, bool horizontal)
{
	if (root == NULL || is_leaf(root)) {
		return;
	}
	/* Preserve bspwm's axis convention: horizontal mirrors top/bottom,
	 * vertical mirrors left/right. Geometry is refreshed by bsp_layout. */
	if (horizontal != root->vertical) {
		swap_children(root);
	}
	bsp_flip(root->first, horizontal);
	bsp_flip(root->second, horizontal);
}

static size_t balance(struct bsp_node *node)
{
	if (node == NULL || node->vacant) {
		return 0;
	}
	if (is_leaf(node)) {
		return 1;
	}
	size_t first = balance(node->first), second = balance(node->second);
	if (first > 0 && second > 0) {
		node->ratio = (double)first / (double)(first + second);
	}
	return first + second;
}

void bsp_balance(struct bsp_node *root)
{
	refresh_vacancy(root);
	(void)balance(root);
}

struct bsp_node *bsp_neighbor(struct bsp_node *root, struct bsp_node *from,
	enum bsp_direction direction)
{
	if (!is_leaf(from) || from->vacant || !contains(root, from) ||
		direction < BSP_NORTH || direction > BSP_WEST) {
		return NULL;
	}
	struct bsp_node *best = NULL;
	bool best_aligned = false;
	long double best_distance = 0, best_center = 0;
	bool vertical = direction == BSP_NORTH || direction == BSP_SOUTH;
	int64_t start = vertical ? from->box.y : from->box.x;
	int64_t end = start + (vertical ? from->box.height : from->box.width);
	int64_t side = vertical ? from->box.x : from->box.y;
	int64_t side_end = side + (vertical ? from->box.width : from->box.height);
	for (struct bsp_node *node = bsp_first(root); node != NULL; node = bsp_next(node)) {
		if (node == from || node->vacant) {
			continue;
		}
		int64_t near = vertical ? node->box.y : node->box.x;
		int64_t far = near + (vertical ? node->box.height : node->box.width);
		int64_t other = vertical ? node->box.x : node->box.y;
		int64_t other_end = other + (vertical ? node->box.width : node->box.height);
		bool forward = direction == BSP_EAST || direction == BSP_SOUTH;
		if (forward ? near < end : far > start) {
			continue;
		}
		bool aligned = other < side_end && other_end > side;
		long double gap = forward ? near - end : start - far;
		long double side_gap = other > side_end ? other - side_end :
			other_end < side ? side - other_end : 0;
		long double distance = gap * gap + side_gap * side_gap;
		long double dx = ((long double)near + far - start - end) / 2;
		long double dy = ((long double)other + other_end - side - side_end) / 2;
		long double center = dx * dx + dy * dy;
		/* Prefer the directional beam, then nearest rectangle edges and centers.
		 * IDs make exact ties deterministic, independently of tree rotations. */
		if (best == NULL || (aligned && !best_aligned) ||
			(aligned == best_aligned && (distance < best_distance ||
			(distance == best_distance && (center < best_center ||
			(center == best_center && node->id < best->id)))))) {
			best = node;
			best_aligned = aligned;
			best_distance = distance;
			best_center = center;
		}
	}
	return best;
}
