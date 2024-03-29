#include "db.h"
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

/* Forward declaration */
node_t *search(char *, node_t *, node_t **);

node_t head = { "", "", 0, 0, PTHREAD_MUTEX_INITIALIZER, 0, PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER };

// Method to read-lock a node
void read_lock(node_t *node)
{
    pthread_mutex_lock(&node->request_mutex);
    // If you get here, you are the only request being processed at this time
    pthread_mutex_lock(&node->num_readers_mutex);
    node->num_readers++;
    if (node->num_readers == 1) // If you are the first reader in the node
        pthread_mutex_lock(&node->node_mutex); // Lock access to the node
    pthread_mutex_unlock(&node->num_readers_mutex);
    pthread_mutex_unlock(&node->request_mutex);
}

// Method to read-unlock a node
void read_unlock(node_t *node)
{
    pthread_mutex_lock(&node->num_readers_mutex);
    node->num_readers--;
    if (node->num_readers == 0) // If you were the last reader in the node
        pthread_mutex_unlock(&node->node_mutex);
    pthread_mutex_unlock(&node->num_readers_mutex);
}

// Method to write-lock a node
void write_lock(node_t *node)
{
    pthread_mutex_lock(&node->request_mutex);
    pthread_mutex_lock(&node->node_mutex);
    pthread_mutex_unlock(&node->request_mutex);
}

// Method to write-unlock a node
void write_unlock(node_t *node)
{
    pthread_mutex_unlock(&node->node_mutex);
}
/*
 * Allocate a new node with the given key, value and children.
 */
node_t *node_create(char *arg_name, char *arg_value, node_t * arg_left,
	node_t * arg_right) {
    node_t *new_node;

    new_node = (node_t *) malloc(sizeof(node_t));
    if (!new_node) return NULL;

    if (!(new_node->name = (char *)malloc(strlen(arg_name) + 1))) {
	free(new_node);
	return NULL;
    }

    if (!(new_node->value = (char *)malloc(strlen(arg_value) + 1))) {
	free(new_node->name);
	free(new_node);
	return NULL;
    }

    strcpy(new_node->name, arg_name);
    strcpy(new_node->value, arg_value);
    new_node->lchild = arg_left;
    new_node->rchild = arg_right;

    // Initialize mutexes/variables for read-write lock
    pthread_mutex_init(&new_node->request_mutex, NULL);
    new_node->num_readers = 0;
    pthread_mutex_init(&new_node->num_readers_mutex, NULL);
    pthread_mutex_init(&new_node->node_mutex, NULL);
    
    return new_node;
}

/* Free the data structures in node and the node itself. */
void node_destroy(node_t * node) {
    /* Clearing name and value after they are freed is defensive programming in
     * case the node_destroy is called again. */
    if (node->name) {free(node->name); node->name = NULL; }
    if (node->value) { free(node->value); node->value = NULL; }

    // Destroy mutexes created for read-write lock
    pthread_mutex_destroy(&node->request_mutex);
    pthread_mutex_destroy(&node->num_readers_mutex);
    pthread_mutex_destroy(&node->node_mutex);

    free(node);
}

/* Find the node with key name and return a result or error string in result.
 * Result must have space for len characters. */
void query(char *name, char *result, int len) {
    node_t *target;

    // Acquire read-lock for head node before executing search().
    read_lock(&head);

    target = search(name, &head, NULL);

    // When you get here, target is either target node (read-locked) or NULL

    if (!target) { // Target was not found
	strncpy(result, "not found", len - 1);
	return;
    } else { // Target was found, node is read-locked
	strncpy(result, target->value, len - 1);
        read_unlock(target);
	return;
    }
}

/* Insert a node with name and value into the proper place in the DB rooted at
 * head. */
int add(char *name, char *value) {
	node_t *parent;	    /* The new node will be the child of this node */
	node_t *target;	    /* The existing node with key name if any */
	node_t *newnode;    /* The new node to add */

        // Acquire write-lock for head before search
        write_lock(&head);

	if ((target = search(name, &head, &parent))) {
	    /* There is already a node with this key in the tree */
            write_unlock(target);
            write_unlock(parent);
	    return 0;
	}

        // If you get here, target didn't exist
        //   Only parent node is write-locked

	/* No idea how this could happen, but... */
	if (!parent) return 0;

	/* make the new node and attach it to parent */
	newnode = node_create(name, value, 0, 0);

	if (strcmp(name, parent->name) < 0) parent->lchild = newnode;
	else parent->rchild = newnode;

        write_unlock(parent);

	return 1;
}

/*
 * When deleting a node with 2 children, we swap the contents leftmost child of
 * its right subtree with the node to be deleted.  This is used to swap those
 * content pointers without copying the data, which is unsafe if the
 * allocations are different sizes (copying "alamorgodo" into "ny" for
 * example).
 */
static inline void swap_pointers(char **a, char **b) {
    char *tmp = *b;
    *b = *a;
    *a = tmp;
}

/* Remove the node with key name from the tree if it is there.  See inline
 * comments for algorithmic details.  Return true if something was deleted. */
int xremove(char *name) {
	node_t *parent;	    /* Parent of the node to delete */
	node_t *dnode;	    /* Node to delete */
	node_t *next;	    /* used to find leftmost child of right subtree */
	node_t **pnext;	    /* A pointer in the tree that points to next so we
			       can change that nodes children (see below). */

        // Acquire write-lock for head before search
        write_lock(&head);

	/* first, find the node to be removed */
	if (!(dnode = search(name, &head, &parent))) {
	    /* it's not there */
            write_unlock(parent);
	    return 0;
	}

	/* we found it.  Now check out the easy cases.  If the node has no
	 * right child, then we can merely replace its parent's pointer to
	 * it with the node's left child. */
	if (dnode->rchild == 0) {
	    if (strcmp(dnode->name, parent->name) < 0)
		parent->lchild = dnode->lchild;
	    else
		parent->rchild = dnode->lchild;

	    /* done with dnode */
	    node_destroy(dnode);
            write_unlock(parent);
	} else if (dnode->lchild == 0) {
	    /* ditto if the node had no left child */
	    if (strcmp(dnode->name, parent->name) < 0)
		parent->lchild = dnode->rchild;
	    else
		parent->rchild = dnode->rchild;

	    /* done with dnode */
	    node_destroy(dnode);
            write_unlock(parent);
	} else {
	    /* So much for the easy cases ...
	     * We know that all nodes in a node's right subtree have
	     * lexicographically greater names than the node does, and all
	     * nodes in a node's left subtree have lexicographically smaller
	     * names than the node does. So, we find the lexicographically
	     * smallest node in the right subtree and replace the node to be
	     * deleted with that node. This new node thus is lexicographically
	     * smaller than all nodes in its right subtree, and greater than
	     * all nodes in its left subtree. Thus the modified tree is well
	     * formed. */

            // If you get here, dnode has two children
            //   Don't need to change dnode's parent node in this case, so let it go
            write_unlock(parent);

	    /* pnext is the address of the pointer which points to next (either
	     * parent's lchild or rchild) */
            write_lock(dnode->rchild); // Know rchild exists because dnode has two children here
	    pnext = &dnode->rchild;
	    next = *pnext;

	    while (next->lchild != 0) {
                    // If you get here, you know next->lchild exists (is a node, not NULL)

		    /* work our way down the lchild chain, finding the smallest
		     * node in the subtree. */
                    write_lock(next->lchild);
                    write_unlock(next); // As long as dnode is locked, no one should be able to
                                        //   access nodes between dnode and next->lchild
		    pnext = &next->lchild;
		    next = *pnext;
	    }
	    swap_pointers(&dnode->name, &next->name);
	    swap_pointers(&dnode->value, &next->value);
	    *pnext = next->rchild;

	    node_destroy(next);
            write_unlock(dnode);
    }
    return 1;
}

/* Search the tree, starting at parent, for a node containing name (the "target
 * node").  Return a pointer to the node, if found, otherwise return 0.  If
 * parentpp is not 0, then it points to a location at which the address of the
 * parent of the target node is stored.  If the target node is not found, the
 * location pointed to by parentpp is set to what would be the the address of
 * the parent of the target node, if it were there.
 *
 * Assumptions:
 * parent is not null and it does not contain name */
node_t *search(char *name, node_t * parent, node_t ** parentpp) {

    node_t *next;
    node_t *result;

    if (strcmp(name, parent->name) < 0) next = parent->lchild;
    else next = parent->rchild;

    if (next == NULL) { // You're reached the bottom of the tree
	result = NULL; // Target was not in tree    
        if (parentpp == NULL) read_unlock(parent); // If reading, let the parent go here
    }
    else {
        // If you get here, you know the next node was not NULL (i.e. it exists)
        //   Acquire the appropriate lock (based on read/write command type)
        if (parentpp == NULL) { // If reading
            read_lock(next);
            read_unlock(parent);
        }
        else { // If writing
            write_lock(next);
        }

	if (strcmp(name, next->name) == 0) { // If next is the target node
	    /* Note that this falls through to the if (parentpp .. ) statement
	     * below. */
	    result = next; // You found it!
	}
        else { // Have to keep searching
	    /* "We have to go deeper!" This recurses and returns from here
	     * after the recursion has returned result and set parentpp */
            if (parentpp != 0) { // If writing
                write_unlock(parent); // If you're recursing, you know current parent isn't
                                      //   the parent of the target node, so you can let it go
            }
	    result = search(name, next, parentpp);
	    return result;
	}
    }

    /* record a parent if we are looking for one */
    if (parentpp != 0) *parentpp = parent;

    return (result); // If reading, only target node (if found) will be locked upon return
                     // If writing, parent will be locked. If target node exists, it will also
                     //   be locked.
                     // Need to handle unlocking appropriate nodes in methods calling search().
}

/*
 * Parse the command in command, execute it on the DB rooted at head and return
 * a string describing the results.  Response must be a writable string that
 * can hold len characters.  The response is stored in response.
 */
void interpret_command(char *command, char *response, int len)
{
    char value[256];
    char ibuf[256];
    char name[256];

    if (strlen(command) <= 1) {
	strncpy(response, "ill-formed command", len - 1);
	return;
    }

    switch (command[0]) {
    case 'q':
	/* Query */
	sscanf(&command[1], "%255s", name);
	if (strlen(name) == 0) {
	    strncpy(response, "ill-formed command", len - 1);
	    return;
	}

	query(name, response, len);
	if (strlen(response) == 0) {
	    strncpy(response, "not found", len - 1);
	}

	return;

    case 'a':
	/* Add to the database */
	sscanf(&command[1], "%255s %255s", name, value);
	if ((strlen(name) == 0) || (strlen(value) == 0)) {
	    strncpy(response, "ill-formed command", len - 1);
	    return;
	}

	if (add(name, value)) {
	    strncpy(response, "added", len - 1);
	} else {
	    strncpy(response, "already in database", len - 1);
	}

	return;

    case 'd':
	/* Delete from the database */
	sscanf(&command[1], "%255s", name);
	if (strlen(name) == 0) {
	    strncpy(response, "ill-formed command", len - 1);
	    return;
	}

	if (xremove(name)) {
	    strncpy(response, "removed", len - 1);
	} else {
	    strncpy(response, "not in database", len - 1);
	}

	    return;

    case 'f':
	/* process the commands in a file (silently) */
	sscanf(&command[1], "%255s", name);
	if (name[0] == '\0') {
	    strncpy(response, "ill-formed command", len - 1);
	    return;
	}

	{
	    FILE *finput = fopen(name, "r");
	    if (!finput) {
		strncpy(response, "bad file name", len - 1);
		return;
	    }
	    while (fgets(ibuf, sizeof(ibuf), finput) != 0) {
		interpret_command(ibuf, response, len);
	    }
	    fclose(finput);
	}
	strncpy(response, "file processed", len - 1);
	return;

    default:
	strncpy(response, "ill-formed command", len - 1);
	return;
    }
}
