/**
 * @file AVL.h
 * @author Joe Wingbermuehle
 * @date 2007-06-11
 *
 * @brief AVL-tree implementation.
 *
 */
 /* from	Joe Wingbermuehle <joewing@joewing.net>
 * to	Susmit Biswas <susmit@cs.ucsb.edu>
 * 2010-04-23Thu, Apr 15, 2010 at 6:50 AM
 * subject	Re: avl tree code
 *  	
 * Hi,
 * 
 * I haven't really picked a license for it, but you can treat it as
 * public domain.
 * 
 * Thanks!
 * Joe
 */

#ifndef AVL_H
#define AVL_H

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "Globals.h"

/** AVL tree data type. */
typedef void *AVLTree;


/*! 
 * @brief Comparator for AVL tree keys.
 * @param key1 The first key.
 * @param key2 The second key.
 * @return The result of the comparison.
 *    @li < 0 if key1 is less than key2.
 *    @li = 0 if key1 equals key2.
 *    @li > 0 if key1 is greater than key2.
 */
typedef int (*AVLComparator)(const void *key1, const void *key2);

/*! 
 * @brief Create an empty AVL tree.
 * @param comparator The comparator to use.
 * @return An empty AVL tree.
 */
AVLTree *CreateAVL(AVLComparator comparator);

/*! 
 * @brief Destroy an AVL tree.
 * @param tree The AVL tree to destroy.
 */
void DestroyAVL(AVLTree *tree);

/*!
 * @brief Insert an item to the AVL tree.
 * Note that if the key is already in the tree the value
 * will not be inserted.
 * @param tree The AVL tree.
 * @param key The key.
 * @param value The value.
 * @return The value currently in the tree, if any.
 */
void *InsertAVL(AVLTree *tree, const void *key, void *value);

/*! 
 * @brief Remove an item from the AVL tree.
 * @param tree The AVL tree.
 * @param key The key of the item to remove.
 * @return The removed item (NULL if not found).
 */
void *RemoveAVL(AVLTree *tree, const void *key);

/*! 
 * @brief Find an item in the AVL tree.
 * @param tree The AVL tree.
 * @param key The key.
 * @return The item (NULL if not found).
 */
void *FindAVL(const AVLTree *tree, const void *key);

/*! 
 * @brief Find if a value is in range of the AVL tree. 
 * @param tree The AVL tree.
 * @param key The key.
 * @return The item (NULL if not found).
 */
void *FindRangeAVL(const AVLTree *tree, const void *key);

/*! 
 * @brief Traverse each element of the tree.
 * @param tree The AVL tree.
 * @param func The traversal function.
 * @param data A value to be passed to the traversal function.
 */
void TraverseAVL(const AVLTree *tree,
   void (*func)(const void *key, const void *value, const void *data, void *isDirty));

/*! 
 * @brief Get the number of elements in the tree.
 * @param tree The AVL tree.
 * @return The number of elements in the tree.
 */
int GetAVLSize(const AVLTree *tree);

/*! 
 * @brief Get the height of the AVL tree.
 * @param tree The AVL tree.
 * @return The height of the tree.
 */
int GetAVLHeight(const AVLTree *tree);

/*! @brief Maximum depth of call stack stored */
#define MAX_STACK_DEPTH 20

/*! @brief Structure to represent an AVL tree node. */
typedef struct AVLTreeNode {

   const void *key; /**< used for comparison */
   void *value; /**< stored value */
   int height; /**< height of the avl tree */
   struct AVLTreeNode *left; /**< left child */
   struct AVLTreeNode *right; /**< right child */

   int dirty; /**< indicates if it is modified since last merge */
   uintptr_t creator;  /**< address of the code block that allocated this block */
   void *callStack[MAX_STACK_DEPTH]; /**< call stack when the malloc was called */

} AVLTreeNode;

/*! @brief Data for an AVL tree. */
typedef struct AVLTreeData {
   AVLTreeNode *root; /**< root of AVL tree */
   AVLComparator comparator; /**< The comparator function */
   int size; /**< size of the avl tree */
} AVLTreeData;


/*! @brief Returns the creator of the region 
 * @return address outside this library that created this region. 
 */
extern uintptr_t  	GetBacktrace();

/*! @brief Stores call stack in stack. depth is the maximum size supported 
 * @param stack an array to store the pointers 
 * @depth the length of the array stack 
 */
extern void         GetCallStack(void **stack, int depth);
#endif /* AVL_H */

