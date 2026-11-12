/* Run: cc -std=c11 -Wall -Wextra -pedantic -Iwayland
 *      tests/bsp_test.c wayland/bsp.c -lm -o /tmp/bsp_test && /tmp/bsp_test */
#include "bsp.h"

#include <assert.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static struct bsp_node *new_leaf(uint32_t id)
{
	struct bsp_node *node = bsp_new(id, NULL);
	assert(node != NULL);
	return node;
}

static void box_is(struct bsp_box box, int x, int y, int width, int height)
{
	assert(box.x == x && box.y == y);
	assert(box.width == width && box.height == height);
}

static void same_box(struct bsp_box a, struct bsp_box b)
{
	box_is(a, b.x, b.y, b.width, b.height);
}

static void close_ratio(double actual, double expected)
{
	assert(fabs(actual - expected) < 1e-12);
}

static size_t check_node(struct bsp_node *node, struct bsp_node *parent,
	struct bsp_node **seen, size_t *used, size_t limit)
{
	assert(node != NULL && *used < limit);
	for (size_t i = 0; i < *used; ++i) {
		assert(seen[i] != node); /* Cycles and aliased children are both invalid. */
	}
	seen[(*used)++] = node;
	assert(node->parent == parent);
	assert(isfinite(node->ratio) && node->ratio > 0 && node->ratio < 1);
	assert((node->first == NULL) == (node->second == NULL));
	if (node->first == NULL) {
		assert(node->id > 0);
		return 1;
	}
	assert(node->id == 0); /* Branches deliberately have no window ID. */
	assert(node->first != node->second);
	size_t count = check_node(node->first, node, seen, used, limit);
	count += check_node(node->second, node, seen, used, limit);
	assert(node->vacant == (node->first->vacant && node->second->vacant));
	return count;
}

static void check_tree(struct bsp_node *root, size_t expected)
{
	struct bsp_node *seen[256];
	size_t used = 0, count = 0;
	assert(expected <= 128);
	if (expected == 0) {
		assert(root == NULL);
	} else {
		assert(check_node(root, NULL, seen, &used, 2 * expected - 1) == expected);
		assert(used == 2 * expected - 1);
	}
	for (struct bsp_node *node = bsp_first(root); node != NULL; node = bsp_next(node)) {
		assert(++count <= expected);
		assert(node->first == NULL && node->second == NULL);
		assert(bsp_find(root, node->id) == node); /* Also checks uniqueness. */
	}
	assert(count == expected);
	assert(bsp_find(root, UINT32_MAX) == NULL);
}

static void check_geometry(struct bsp_node *node, bool monocle)
{
	if (node == NULL) {
		return;
	}
	assert(node->box.width >= 1 && node->box.height >= 1);
	if (node->first == NULL) {
		return;
	}
	struct bsp_box box = node->box, a = node->first->box, b = node->second->box;
	if (monocle || node->first->vacant || node->second->vacant ||
		(node->vertical ? box.width : box.height) == 1) {
		same_box(a, box);
		same_box(b, box);
	} else if (node->vertical) {
		assert(a.width + b.width == box.width);
		box_is(a, box.x, box.y, a.width, box.height);
		box_is(b, box.x + a.width, box.y, b.width, box.height);
	} else {
		assert(a.height + b.height == box.height);
		box_is(a, box.x, box.y, box.width, a.height);
		box_is(b, box.x, box.y + a.height, box.width, b.height);
	}
	check_geometry(node->first, monocle);
	check_geometry(node->second, monocle);
}

static void test_empty_and_invalid(void)
{
	struct bsp_node *root = NULL, *a = new_leaf(1), *b = new_leaf(2);
	struct bsp_node *zero = new_leaf(0), *duplicate = new_leaf(1);
	double invalid[] = {0, 1, -0.1, 1.1, NAN, INFINITY, -INFINITY};
	assert(bsp_first(NULL) == NULL && bsp_next(NULL) == NULL);
	assert(bsp_find(NULL, 1) == NULL);
	assert(!bsp_insert(NULL, NULL, a, BSP_AUTO, 0.5));
	assert(!bsp_insert(&root, NULL, NULL, BSP_AUTO, 0.5));
	assert(!bsp_insert(&root, NULL, zero, BSP_AUTO, 0.5));
	assert(!bsp_insert(&root, b, a, BSP_AUTO, 0.5));
	for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
		assert(!bsp_insert(&root, NULL, a, BSP_AUTO, invalid[i]));
		assert(root == NULL && a->parent == NULL);
	}
	assert(bsp_insert(&root, NULL, a, BSP_AUTO, 0.5));
	assert(!bsp_insert(&root, a, b, (enum bsp_direction)-1, 0.5));
	assert(!bsp_insert(&root, a, b, (enum bsp_direction)99, 0.5));
	assert(!bsp_insert(&root, b, b, BSP_EAST, 0.5));
	assert(!bsp_insert(&root, a, a, BSP_EAST, 0.5));
	assert(!bsp_insert(&root, a, duplicate, BSP_EAST, 0.5));
	for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
		assert(!bsp_insert(&root, a, b, BSP_EAST, invalid[i]));
		assert(root == a && a->parent == NULL && b->parent == NULL);
	}
	assert(bsp_remove(NULL, a) == NULL);
	assert(bsp_remove(&root, NULL) == NULL);
	assert(bsp_remove(&root, b) == NULL && root == a);
	assert(bsp_neighbor(root, NULL, BSP_EAST) == NULL);
	assert(bsp_neighbor(root, b, BSP_EAST) == NULL);
	assert(bsp_remove(&root, a) == NULL && root == NULL && a->parent == NULL);
	check_tree(root, 0);
	bsp_destroy(a);
	bsp_destroy(b);
	bsp_destroy(zero);
	bsp_destroy(duplicate);
	bsp_destroy(NULL);
	bsp_layout(NULL, (struct bsp_box){0, 0, 0, 0}, false);
	bsp_rotate(NULL, 90);
	bsp_flip(NULL, true);
	bsp_balance(NULL);
}

static void test_insertion_and_removal(void)
{
	struct bsp_node *root = NULL, *a = new_leaf(1), *b = new_leaf(2);
	struct bsp_node *c = new_leaf(3), *d = new_leaf(4);
	int owned_by_caller = 42;
	a->data = &owned_by_caller;
	assert(bsp_insert(&root, NULL, a, BSP_AUTO, 0.5));
	bsp_layout(root, (struct bsp_box){10, 20, 120, 80}, false);
	assert(bsp_insert(&root, a, b, BSP_EAST, 0.25));
	struct bsp_node *old_root = root;
	assert(root->id == 0 && root->first == a && root->second == b);
	assert(bsp_find(root, 0) == root);
	assert(!bsp_insert(&root, a, b, BSP_EAST, 0.5)); /* Already attached. */
	assert(!bsp_insert(&root, a, root, BSP_EAST, 0.5)); /* Not a leaf. */
	assert(bsp_remove(&root, root) == NULL && root == old_root);
	assert(bsp_insert(&root, b, c, BSP_NORTH, 0.75));
	assert(bsp_insert(&root, a, d, BSP_WEST, 0.5));
	check_tree(root, 4);
	assert(bsp_first(root) == d && bsp_next(d) == a);
	assert(bsp_next(a) == c && bsp_next(c) == b && bsp_next(b) == NULL);
	assert(bsp_remove(&root, d) == a && d->parent == NULL);
	assert(root->first == a && a->parent == root);
	assert(bsp_remove(&root, b) == c && b->parent == NULL);
	assert(root->second == c && c->parent == root);
	check_tree(root, 2);
	assert(bsp_insert(&root, NULL, b, BSP_AUTO, 0.5)); /* Wrap an internal root. */
	assert(root->first == old_root && root->second == b);
	check_tree(root, 3);
	assert(bsp_remove(&root, b) == a && root == old_root);
	assert(bsp_remove(&root, a) == c && root == c && c->parent == NULL);
	assert(a->parent == NULL && a->data == &owned_by_caller);
	assert(bsp_insert(&root, c, a, BSP_SOUTH, 0.5)); /* Detached leaves are reusable. */
	check_tree(root, 2);
	bsp_destroy(root); /* Does not free a->data. */
	assert(owned_by_caller == 42);
	bsp_destroy(b);
	bsp_destroy(d);
}

static void test_directions_and_geometry(void)
{
	for (enum bsp_direction dir = BSP_AUTO; dir <= BSP_WEST; ++dir) {
		struct bsp_node *root = NULL, *a = new_leaf(1), *b = new_leaf(2);
		assert(bsp_insert(&root, NULL, a, BSP_AUTO, 0.5));
		bsp_layout(root, (struct bsp_box){-10, 20, 120, 80}, false);
		assert(bsp_insert(&root, a, b, dir, 0.25));
		bool new_first = dir == BSP_NORTH || dir == BSP_WEST;
		assert(root->first == (new_first ? b : a));
		assert(root->second == (new_first ? a : b));
		assert(root->vertical == (dir == BSP_AUTO || dir == BSP_EAST || dir == BSP_WEST));
		bsp_layout(root, (struct bsp_box){-10, 20, 120, 80}, false);
		if (root->vertical) {
			box_is(root->first->box, -10, 20, 30, 80);
			box_is(root->second->box, 20, 20, 90, 80);
		} else {
			box_is(root->first->box, -10, 20, 120, 20);
			box_is(root->second->box, -10, 40, 120, 60);
		}
		check_tree(root, 2);
		bsp_destroy(root);
	}
	for (int square = 0; square < 2; ++square) {
		struct bsp_node *root = new_leaf(1), *b = new_leaf(2);
		bsp_layout(root, (struct bsp_box){0, 0, 80, square ? 80 : 120}, false);
		struct bsp_node *a = root;
		assert(bsp_insert(&root, a, b, BSP_AUTO, 0.5));
		assert(!root->vertical); /* Longest side: square ties follow upstream. */
		bsp_destroy(root);
	}
	struct bsp_node *a = new_leaf(1), *b = new_leaf(2), *root = a;
	assert(bsp_insert(&root, a, b, BSP_EAST, DBL_MIN));
	bsp_layout(root, (struct bsp_box){5, 6, 3, 2}, false);
	box_is(a->box, 5, 6, 1, 2);
	box_is(b->box, 6, 6, 2, 2);
	root->ratio = nextafter(1.0, 0.0);
	bsp_layout(root, (struct bsp_box){5, 6, 3, 2}, false);
	box_is(a->box, 5, 6, 2, 2);
	box_is(b->box, 7, 6, 1, 2);
	bsp_layout(root, (struct bsp_box){5, 6, 0, INT_MIN}, false);
	box_is(root->box, 5, 6, 1, 1);
	same_box(a->box, root->box);
	same_box(b->box, root->box);
	root->ratio = NAN; /* Defensive handling of public-struct edits. */
	bsp_layout(root, (struct bsp_box){0, 0, 101, 51}, false);
	close_ratio(root->ratio, 0.5);
	box_is(a->box, 0, 0, 50, 51);
	box_is(b->box, 50, 0, 51, 51);
	bsp_layout(root, (struct bsp_box){INT_MAX, INT_MIN, INT_MAX, INT_MAX}, false);
	assert(b->box.x == INT_MAX); /* No signed-coordinate overflow. */
	root->vertical = false;
	bsp_layout(root, (struct bsp_box){INT_MIN, INT_MAX, INT_MAX, INT_MAX}, false);
	assert(b->box.y == INT_MAX);
	root->ratio = nextafter(0.0, 1.0);
	bsp_rotate(root, 180);
	assert(root->ratio > 0 && root->ratio < 1 && isfinite(root->ratio));
	check_tree(root, 2);
	bsp_destroy(root);
}

static struct bsp_node *grid(struct bsp_node **leaf)
{
	for (size_t i = 0; i < 4; ++i) {
		leaf[i] = new_leaf((uint32_t)i + 1);
	}
	struct bsp_node *root = leaf[0];
	assert(bsp_insert(&root, leaf[0], leaf[1], BSP_EAST, 0.25));
	assert(bsp_insert(&root, leaf[0], leaf[2], BSP_SOUTH, 0.25));
	assert(bsp_insert(&root, leaf[1], leaf[3], BSP_SOUTH, 0.75));
	bsp_layout(root, (struct bsp_box){0, 0, 400, 400}, false);
	return root;
}

static void test_vacant_and_monocle(void)
{
	struct bsp_node *leaf[4], *root = grid(leaf);
	for (unsigned mask = 0; mask < 16; ++mask) {
		for (size_t i = 0; i < 4; ++i) {
			leaf[i]->vacant = (mask & (1u << i)) != 0;
		}
		root->vacant = !root->vacant; /* Force stale internal state. */
		for (unsigned monocle = 0; monocle < 2; ++monocle) {
			bsp_layout(root, (struct bsp_box){-5, -10, 400, 300}, monocle != 0);
			check_tree(root, 4);
			check_geometry(root, monocle != 0);
			assert(root->vacant == (mask == 15));
			for (size_t i = 0; i < 4; ++i) {
				assert(leaf[i]->vacant == ((mask & (1u << i)) != 0));
				if (monocle) {
					same_box(leaf[i]->box, root->box);
				}
			}
		}
	}
	leaf[0]->vacant = leaf[2]->vacant = true;
	leaf[1]->vacant = leaf[3]->vacant = false;
	bsp_layout(root, (struct bsp_box){0, 0, 400, 400}, false);
	assert(root->first->vacant && !root->vacant);
	same_box(root->second->box, root->box); /* Entire vacant branch collapses. */
	box_is(leaf[1]->box, 0, 0, 400, 300);
	box_is(leaf[3]->box, 0, 300, 400, 100);
	bsp_destroy(root);
}

static void test_rotate_and_flip(void)
{
	struct bsp_node *leaf[4], *root = grid(leaf);
	struct bsp_box original[4];
	for (size_t i = 0; i < 4; ++i) {
		original[i] = leaf[i]->box;
	}
	bsp_rotate(root, 450); /* Clockwise, normalized modulo 360. */
	bsp_layout(root, (struct bsp_box){0, 0, 400, 400}, false);
	check_tree(root, 4);
	for (size_t i = 0; i < 4; ++i) {
		struct bsp_box old = original[i];
		box_is(leaf[i]->box, 400 - old.y - old.height, old.x, old.height, old.width);
	}
	bsp_rotate(root, 270);
	bsp_layout(root, (struct bsp_box){0, 0, 400, 400}, false);
	for (size_t i = 0; i < 4; ++i) {
		same_box(leaf[i]->box, original[i]);
	}
	bsp_rotate(root, 45); /* Non-quarter-turns are a no-op. */
	bsp_rotate(root, 360);
	bsp_rotate(root, 180);
	bsp_layout(root, (struct bsp_box){0, 0, 400, 400}, false);
	for (size_t i = 0; i < 4; ++i) {
		struct bsp_box old = original[i];
		box_is(leaf[i]->box, 400 - old.x - old.width, 400 - old.y - old.height,
			old.width, old.height);
	}
	bsp_rotate(root, 180);
	for (unsigned horizontal = 0; horizontal < 2; ++horizontal) {
		bsp_flip(root, horizontal != 0);
		bsp_layout(root, (struct bsp_box){0, 0, 400, 400}, false);
		for (size_t i = 0; i < 4; ++i) {
			struct bsp_box old = original[i];
			box_is(leaf[i]->box, horizontal ? old.x : 400 - old.x - old.width,
				horizontal ? 400 - old.y - old.height : old.y, old.width, old.height);
		}
		check_tree(root, 4);
		bsp_flip(root, horizontal != 0);
	}
	bsp_layout(root, (struct bsp_box){0, 0, 400, 400}, false);
	for (size_t i = 0; i < 4; ++i) {
		same_box(leaf[i]->box, original[i]);
	}
	bsp_destroy(root);
}

static void test_balance(void)
{
	struct bsp_node *a = new_leaf(1), *b = new_leaf(2), *c = new_leaf(3);
	struct bsp_node *d = new_leaf(4), *root = a;
	assert(bsp_insert(&root, a, b, BSP_EAST, 0.8));
	assert(bsp_insert(&root, b, c, BSP_SOUTH, 0.8));
	assert(bsp_insert(&root, c, d, BSP_EAST, 0.8));
	bsp_balance(root);
	close_ratio(root->ratio, 0.25);
	close_ratio(root->second->ratio, 1.0 / 3.0);
	close_ratio(c->parent->ratio, 0.5);
	b->vacant = true;
	root->second->vacant = true; /* Balance must rederive branch state, too. */
	bsp_balance(root);
	close_ratio(root->ratio, 1.0 / 3.0);
	close_ratio(root->second->ratio, 1.0 / 3.0); /* One vacant child: retain ratio. */
	assert(!root->second->vacant);
	c->vacant = d->vacant = true;
	bsp_balance(root);
	assert(root->second->vacant && !root->vacant);
	close_ratio(root->ratio, 1.0 / 3.0);
	a->vacant = true;
	bsp_balance(root);
	assert(root->vacant);
	check_tree(root, 4);
	bsp_destroy(root);
}

static void test_neighbors(void)
{
	struct bsp_node *leaf[4], *root = grid(leaf);
	/* Equal quadrants make cardinal-neighbor expectations unambiguous. */
	root->ratio = root->first->ratio = root->second->ratio = 0.5;
	bsp_layout(root, (struct bsp_box){0, 0, 200, 200}, false);
	assert(bsp_neighbor(root, leaf[0], BSP_EAST) == leaf[1]);
	assert(bsp_neighbor(root, leaf[0], BSP_SOUTH) == leaf[2]);
	assert(bsp_neighbor(root, leaf[0], BSP_NORTH) == NULL);
	assert(bsp_neighbor(root, leaf[0], BSP_WEST) == NULL);
	assert(bsp_neighbor(root, leaf[3], BSP_WEST) == leaf[2]);
	assert(bsp_neighbor(root, leaf[3], BSP_NORTH) == leaf[1]);
	assert(bsp_neighbor(root, leaf[0], BSP_AUTO) == NULL);
	assert(bsp_neighbor(root, root, BSP_EAST) == NULL);
	leaf[1]->vacant = true;
	bsp_layout(root, (struct bsp_box){0, 0, 200, 200}, false);
	assert(bsp_neighbor(root, leaf[0], BSP_EAST) == leaf[3]);
	assert(bsp_neighbor(root, leaf[1], BSP_WEST) == NULL);
	leaf[1]->vacant = false;
	leaf[0]->box = (struct bsp_box){0, 0, 10, 10};
	leaf[1]->box = (struct bsp_box){11, 11, 10, 10};
	leaf[2]->box = (struct bsp_box){100, 0, 10, 10};
	leaf[3]->vacant = true;
	assert(bsp_neighbor(root, leaf[0], BSP_EAST) == leaf[2]); /* Beam beats diagonal. */
	leaf[2]->vacant = true;
	assert(bsp_neighbor(root, leaf[0], BSP_EAST) == leaf[1]); /* Diagonal fallback. */
	leaf[2]->vacant = false;
	leaf[2]->box = leaf[1]->box;
	assert(bsp_neighbor(root, leaf[0], BSP_EAST) == leaf[1]); /* Stable ID tie-break. */
	leaf[1]->box = leaf[2]->box = leaf[0]->box;
	assert(bsp_neighbor(root, leaf[0], BSP_EAST) == NULL); /* Monocle overlap. */
	leaf[0]->box = (struct bsp_box){INT_MIN, 0, 1, 1};
	leaf[1]->box = (struct bsp_box){INT_MAX, 0, 1, 1};
	leaf[2]->vacant = true;
	assert(bsp_neighbor(root, leaf[0], BSP_EAST) == leaf[1]);
	assert(bsp_neighbor(root, leaf[1], BSP_WEST) == leaf[0]);
	bsp_destroy(root);
}

static uint32_t random_u32(uint32_t *state)
{
	*state ^= *state << 13;
	*state ^= *state >> 17;
	*state ^= *state << 5;
	return *state;
}

static void test_random_trees(void)
{
	enum { CAPACITY = 64, SEEDS = 16, STEPS = 2000 };
	for (uint32_t seed = 1; seed <= SEEDS; ++seed) {
		uint32_t state = seed * 0x9e3779b9u;
		struct bsp_node *root = NULL, *leaves[CAPACITY];
		size_t count = 0;
		for (uint32_t step = 0; step < STEPS; ++step) {
			bool insert = count == 0 || (count < CAPACITY && random_u32(&state) % 100 < 53);
			if (insert) {
				struct bsp_node *node = new_leaf(step + 1);
				struct bsp_node *anchor = count ? leaves[random_u32(&state) % count] : NULL;
				if (step % 11 == 0) {
					anchor = root; /* Exercise insertion around arbitrary subtrees. */
				}
				enum bsp_direction dir = (enum bsp_direction)(random_u32(&state) % 5);
				double ratio = (1.0 + random_u32(&state) % 999) / 1000.0;
				assert(bsp_insert(&root, anchor, node, dir, ratio));
				leaves[count++] = node;
			} else {
				size_t index = random_u32(&state) % count;
				struct bsp_node *node = leaves[index];
				struct bsp_node *remaining = bsp_remove(&root, node);
				assert(node->parent == NULL && node->first == NULL && node->second == NULL);
				leaves[index] = leaves[--count];
				assert((remaining == NULL) == (count == 0));
				if (remaining != NULL) {
					assert(bsp_find(root, remaining->id) == remaining && remaining->first == NULL);
				}
				free(node);
			}
			if (count && step % 5 == 0) {
				struct bsp_node *node = leaves[random_u32(&state) % count];
				node->vacant = !node->vacant;
			}
			if (step % 7 == 0) {
				bsp_balance(root);
			}
			if (step % 11 == 0) {
				bsp_rotate(root, 90 * (random_u32(&state) % 4));
			}
			if (step % 13 == 0) {
				bsp_flip(root, (random_u32(&state) & 1u) != 0);
			}
			bool monocle = step % 19 == 0;
			struct bsp_box box = {-31, -17, (int)(random_u32(&state) % 260) - 2,
				(int)(random_u32(&state) % 260) - 2};
			bsp_layout(root, box, monocle);
			check_tree(root, count);
			check_geometry(root, monocle);
			for (size_t i = 0; i < count; ++i) {
				assert(bsp_find(root, leaves[i]->id) == leaves[i]);
			}
		}
		while (count) {
			struct bsp_node *node = leaves[--count];
			(void)bsp_remove(&root, node);
			free(node);
			check_tree(root, count);
		}
	}
}

int main(void)
{
	test_empty_and_invalid();
	test_insertion_and_removal();
	test_directions_and_geometry();
	test_vacant_and_monocle();
	test_rotate_and_flip();
	test_balance();
	test_neighbors();
	test_random_trees();
	puts("bsp_test: all tests passed (16 seeds, 32000 randomized operations)");
	return 0;
}
