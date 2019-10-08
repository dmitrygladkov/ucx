/**
 * Copyright (C) Mellanox Technologies Ltd. 2019.  ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

/**
 * Copied from http://oopweb.com/Algorithms/Documents/Sman/VolumeFrames.html?/Algorithms/Documents/Sman/Volume/RedBlackTrees_files/s_rbt.htm
 *
 * Disclosure from the author's main page:
 * http://oopweb.com/Algorithms/Documents/Sman/VolumeFrames.html?/Algorithms/Documents/Sman/Volume/RedBlackTrees_files/s_rbt.htm
 *
 * Source code when part of a software project may be used freely
 * without reference to the author.
 *
 */

#ifndef UCS_RBTREE_H_
#define UCS_RBTREE_H_

typedef enum {
    RBT_STATUS_OK,
    RBT_STATUS_MEM_EXHAUSTED,
    RBT_STATUS_DUPLICATE_KEY,
    RBT_STATUS_KEY_NOT_FOUND
} RbtStatus;

typedef void *RbtIterator;
typedef void *RbtHandle;

RbtHandle rbtNew(int(*compare)(void *a, void *b));
// create red-black tree
// parameters:
//     compare  pointer to function that compares keys
//              return 0   if a == b
//              return < 0 if a < b
//              return > 0 if a > b
// returns:
//     handle   use handle in calls to rbt functions


void rbtDelete(RbtHandle h);
// destroy red-black tree

RbtStatus rbtInsert(RbtHandle h, void *key, void *value);
// insert key/value pair

RbtStatus rbtErase(RbtHandle h, RbtIterator i);
// delete node in tree associated with iterator
// this function does not free the key/value pointers

RbtIterator rbtNext(RbtHandle h, RbtIterator i);
// return ++i

RbtIterator rbtBegin(RbtHandle h);
// return pointer to first node

RbtIterator rbtEnd(RbtHandle h);
// return pointer to one past last node

void rbtKeyValue(RbtHandle h, RbtIterator i, void **key, void **value);
// returns key/value pair associated with iterator

RbtIterator rbtFindLeftmost(RbtHandle h, void *key,
                            int(*compare)(void *a, void *b));
// returns iterator associated with left-most match. This is useful when a new
//   key might invalidate the uniqueness property of the tree.

RbtIterator rbtFind(RbtHandle h, void *key);
// returns iterator associated with key

#endif
