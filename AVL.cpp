/**
 * @file AVL.cpp
 * @author Joe Wingbermuehle
 * @date 2007-06-11
 *
 * @brief AVL-tree implementation. In order to change what you want to store in
 * the node, please add more elements in AVLTreeNode and pass it through the
 * function templates.
 *
 */

#include "AVL.h"

static void Destroy(AVLTreeNode *node);
static void *Insert(AVLTreeData *data, AVLTreeNode **node,
   const void *key, void *value);
static void *Remove(AVLTreeData *data, AVLTreeNode **node, const void *key);
static AVLTreeNode *RemoveLeftMost(AVLTreeNode **node);
static AVLTreeNode *RemoveRightMost(AVLTreeNode **node);
static void Traverse(const AVLTreeNode *node,
   void (*func)(const void *key, const void *value, const void *data, void *isDirty));
static void Balance(AVLTreeNode **node);
static int GetBalance(const AVLTreeNode *node);
static int GetHeight(const AVLTreeNode *node);
static void RotateSingleRight(AVLTreeNode **node);
static void RotateSingleLeft(AVLTreeNode **node);
static void RotateDoubleRight(AVLTreeNode **node);
static void RotateDoubleLeft(AVLTreeNode **node);

#define NDEBUG

/* Create an empty AVL tree. */
AVLTree *CreateAVL(AVLComparator comparator) {

   AVLTreeData *tree;

#ifndef NDEBUG
   printf("AVLTree created\n");
#endif
   tree = (AVLTreeData*)ptmalloc(sizeof(AVLTreeData));
   tree->root = NULL;
   tree->comparator = comparator;
   tree->size = 0;

   return (AVLTree*)tree;

}

/* Destroy an AVL tree. */
void DestroyAVL(AVLTree *tree) {

   AVLTreeData *data = (AVLTreeData*)tree;

   Destroy(data->root);
   ptfree(data);
#ifndef NDEBUG
   printf("AVLTree destroyed\n");
#endif
}

/* Helper method for destroying an AVL tree. */
void Destroy(AVLTreeNode *node) {

   if(node) {

      Destroy(node->left);
      Destroy(node->right);

      ptfree(node);

   }

}

/* Insert an item to the AVL tree. */
void *InsertAVL(AVLTree *tree, const void *key, void *value) {

   void *result;
   AVLTreeData *data = (AVLTreeData*)tree;
   result = Insert(data, &data->root, key, value);
   Balance(&data->root);

   return result;

}

/* Helper method for inserting to the AVL tree. */
void *Insert(AVLTreeData *data, AVLTreeNode **node,
   const void *key, void *value) {

   AVLTreeNode *np;
   void *result;
   int rc;

   /* If this is an empty tree, just set the root and return. */
   if(!*node) {
      *node = (AVLTreeNode*)ptmalloc(sizeof(AVLTreeNode));
      (*node)->key = key;
      (*node)->value = value;
      (*node)->left = NULL;
      (*node)->right = NULL;
      (*node)->height = 1;
	  (*node)->creator = GetBacktrace();
	  GetCallStack((*node)->callStack, MAX_STACK_DEPTH);
      (*node)->dirty = 0;

      ++data->size;
      return NULL;
   }

   rc = (data->comparator)(key, (*node)->key);
   if(rc < 0) {
      if((*node)->left) {
         result = Insert(data, &(*node)->left, key, value);
         Balance(node);
         return result;
      } else {
         (*node)->left = (AVLTreeNode*)ptmalloc(sizeof(AVLTreeNode));
         np = (*node)->left;
      }
   } else if(rc > 0) {
      if((*node)->right) {
         result = Insert(data, &(*node)->right, key, value);
         Balance(node);
         return result;
      } else {
         (*node)->right = (AVLTreeNode*)ptmalloc(sizeof(AVLTreeNode));
         np = (*node)->right;
      }
   } else {

      /* The key is already in the tree. */
      return (*node)->value;

   }

   np->key = key;
   np->value = value;
   np->left = NULL;
   np->right = NULL;
   np->height = 1;
   np->creator = GetBacktrace();
   GetCallStack(np->callStack, MAX_STACK_DEPTH);
   ++data->size;

   if(   ((*node)->right && !(*node)->left)
      || ((*node)->left  && !(*node)->right)) {
      ++(*node)->height;
   }

   Balance(node);

   return NULL;

}

/* Remove an item from the AVL tree. */
void *RemoveAVL(AVLTree *tree, const void *key) {

   AVLTreeData *data = (AVLTreeData*)tree;
   return Remove(data, &data->root, key);
}

/* Helper method for removing an item from the AVL tree. */
void *Remove(AVLTreeData *data, AVLTreeNode **node, const void *key) {

   AVLTreeNode *np;
   void *result;
   int rc;

   if(*node == NULL) {
#ifndef NDEBUG
	   printf("NOT FOUND\n");
	   fflush(stdout);
#endif
	   /* No matching node was found for this key. */
	   return NULL;

   }

#ifndef NDEBUG
   printf("comparing with %p --> ", (unsigned)((*node)->key));
   fflush(stdout);
#endif

   rc = (data->comparator)(key, (*node)->key);
   if(rc < 0) {

      /* The left subtree contains the node. */
      result = Remove(data, &(*node)->left, key);
      Balance(node);
      return result;

   } else if(rc > 0) {

      /* The right subtree contains the node. */
      result = Remove(data, &(*node)->right, key);
      Balance(node);
      return result;

   } else {

	   /* This is the node. */
#ifndef NDEBUG
	   printf("FOUND\n");
	   fflush(stdout);
#endif
	   result = (*node)->value;
	   if((*node)->right) {

		   /* This node contains a right subtree.
			* Replace this node with the right's left-most subtree. */
		   np = RemoveLeftMost(&(*node)->right);

		   /* Move the data in np to this node and remove it. */
		   (*node)->key = np->key;
		   (*node)->value = np->value;
		   ptfree(np);

		   Balance(node);

	   } else if((*node)->left) {

		   /* This node contains a left subtree.
			* Replace this node with the left's right-most subtree. */
		   np = RemoveRightMost(&(*node)->left);

		   /* Move the data in np to this node and remove it. */
		   (*node)->key = np->key;
		   (*node)->value = np->value;
		   ptfree(np);

		   Balance(node);

	   } else {

		   /* This node contains no children.
			* Just remove the node. */

		   ptfree(*node);
		   *node = NULL;

	   }

	   --data->size;
	   return result;

   }

}

/* Remove the left-most node and return it. */
AVLTreeNode *RemoveLeftMost(AVLTreeNode **node) {

   AVLTreeNode *left;
   int lh, rh;

   if((*node)->left) {

      left = RemoveLeftMost(&(*node)->left);

      lh = 0;
      if((*node)->left) {
         lh = (*node)->left->height + 1;
      }
      rh = 0;
      if((*node)->right) {
         rh = (*node)->right->height + 1;
      }
      (*node)->height = rh > lh ? rh : lh;

      Balance(node);

   } else {

      left = *node;
      *node = left->right;
      left->right = NULL;

   }

   return left;

}

/* Remove the right-most node and return it. */
AVLTreeNode *RemoveRightMost(AVLTreeNode **node) {

   AVLTreeNode *right;
   int lh, rh;

   if((*node)->right) {

      right = RemoveRightMost(&(*node)->right);

      lh = 0;
      if((*node)->left) {
         lh = (*node)->left->height + 1;
      }
      rh = 0;
      if((*node)->right) {
         rh = (*node)->right->height + 1;
      }
      (*node)->height = rh > lh ? rh : lh;

      Balance(node);

   } else {

      right = *node;
      *node = right->left;
      right->left = NULL;

   }

   return right;

}

/* Find a value in the AVL tree. */
void *FindAVL(const AVLTree *tree, const void *key) {

   const AVLTreeData *data = (const AVLTreeData*)tree;
   const AVLTreeNode *np;
   int rc;

   np = data->root;
   while(np) {
      rc = (data->comparator)(key, np->key);
      if(rc < 0) {
         np = np->left;
      } else if (rc > 0) {
         np = np->right;
      } else {
         return np->value;
      }
   }

   return NULL;

}

/* Find if a value is in range of the AVL tree. */
void *FindRangeAVL(const AVLTree *tree, const void *key) {

	const AVLTreeData *data = (const AVLTreeData*)tree;
	AVLTreeNode *np;
	int rc;

	np = data->root;
	while(np) {
		rc = (data->comparator)(key, np->key);
		if(rc < 0) {
			np = np->left;
		} else if (rc > 0) { /* queriy > node, might be in this node */
			if(((unsigned long)np->key + (unsigned long)np->value) > (unsigned long)key)
				return np;
			np = np->right;
		} else {
			return np;
		}
	}
	return NULL;
}

/* Traverse the tree. */
void TraverseAVL(const AVLTree *tree,
   void (*func)(const void *key, const void *value, const void *data, void *isDirty)
   ){

   const AVLTreeData *ad = (const AVLTreeData*)tree;
   Traverse(ad->root, func);

}

/* Helper method for traversing the tree. */
void Traverse(const AVLTreeNode *node,
   void (*func)(const void *key, const void *value, const void *data, void *isDirty)
   ){

   if(node) {
      Traverse(node->left, func);
	  // Susmit: previously was passing only the creator address. Now passing the entire callstack.
	  (func)(node->key, node->value, (void*)(node->callStack), (void *)&(node->dirty));
	  //      (func)(node->key, node->value, (void*)(node->creator), (void *)&(node->dirty));
      Traverse(node->right, func);
   }
}

/* Rebalance the tree starting at node. */
void Balance(AVLTreeNode **node) {

   int balance;
   int left, right;

   balance = GetBalance(*node);
   if(balance < -1) {

      balance = GetBalance((*node)->left);
      if(balance > 0) {
         RotateDoubleRight(node);
      } else {
         RotateSingleRight(node);
      }

   } else if(balance > 1) {

      balance = GetBalance((*node)->right);
      if(balance < 0) {
         RotateDoubleLeft(node);
      } else {
         RotateSingleLeft(node);
      }

   }

   left = GetHeight((*node)->left);
   right = GetHeight((*node)->right);
   (*node)->height = (left > right ? left : right) + 1;

}

/* Get the balance of the specified node. */
int GetBalance(const AVLTreeNode *node) {

   int balance = 0;

   if(node) {
      if(node->left) {
         balance -= node->left->height;
      }
      if(node->right) {
         balance += node->right->height;
      }
   }

   return balance;

}

/* Get the size of the AVL tree. */
int GetAVLSize(const AVLTree *tree) {

   return ((AVLTreeData*)tree)->size;

}

/* Get the height of the AVL tree. */
int GetAVLHeight(const AVLTree *tree) {

   const AVLTreeData *data = (const AVLTreeData*)tree;
   return GetHeight(data->root);

}

/* Helper method for getting the height. */
int GetHeight(const AVLTreeNode *node) {
   if(node) {
      return node->height;
   } else {
      return 0;
   }
}

/* Perform a single right rotation. */
void RotateSingleRight(AVLTreeNode **node) {

   AVLTreeNode *a, *b, *c, *d, *e;
   int left, right;

   a = (*node)->left->left;
   b = (*node)->left->right;
   c = (*node)->right;
   d = (*node)->left;
   e = (*node);

   (*node) = d;
   (*node)->right = e;
   (*node)->left = a;
   (*node)->right->left = b;
   (*node)->right->right = c;

   left = GetHeight(b);
   right = GetHeight(b);
   e->height = (left > right ? left : right) + 1;
   d->height = e->height + 1;

}

/* Perform a single left rotation. */
void RotateSingleLeft(AVLTreeNode **node) {

   AVLTreeNode *a, *b, *c, *d, *e;
   int left, right;

   a = (*node)->left;
   b = (*node)->right->left;
   c = (*node)->right->right;
   d = (*node);
   e = (*node)->right;

   *node = e;
   (*node)->left = d;
   (*node)->left->left = a;
   (*node)->left->right = b;
   (*node)->right = c;

   left = GetHeight(a);
   right = GetHeight(b);
   d->height = (left > right ? left : right) + 1;
   e->height = d->height + 1;

}

/* Perform a double right rotation. */
void RotateDoubleRight(AVLTreeNode **node) {

   RotateSingleLeft(&(*node)->left);
   RotateSingleRight(node);

}

/* Perform a double left rotation. */
void RotateDoubleLeft(AVLTreeNode **node) {

   RotateSingleRight(&(*node)->right);
   RotateSingleLeft(node);

}

