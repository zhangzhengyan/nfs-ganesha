/*
 * vim:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) Panasas Inc., 2011
 * Author: Jim Lieb jlieb@panasas.com
 *
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 * -------------
 */

/* handle.c
 */

#include "config.h"

#include "fsal.h"
#include "fsal_convert.h"
#include "FSAL/fsal_commonlib.h"
#include "mem_int.h"
#include "city.h"
#include "nfs_file_handle.h"
#include "display.h"
#ifdef USE_LTTNG
#include "gsh_lttng/fsal_mem.h"
#endif

static void mem_release(struct fsal_obj_handle *obj_hdl);

/* Atomic uint64_t that is used to generate inode numbers in the mem FS */
uint64_t mem_inode_number = 1;

/* helpers
 */

static inline int
mem_n_cmpf(const struct avltree_node *lhs,
		const struct avltree_node *rhs)
{
	struct mem_fsal_obj_handle *lk, *rk;

	lk = avltree_container_of(lhs, struct mem_fsal_obj_handle, avl_n);
	rk = avltree_container_of(rhs, struct mem_fsal_obj_handle, avl_n);

	return strcmp(lk->m_name, rk->m_name);
}

static inline int
mem_i_cmpf(const struct avltree_node *lhs,
		const struct avltree_node *rhs)
{
	struct mem_fsal_obj_handle *lk, *rk;

	lk = avltree_container_of(lhs, struct mem_fsal_obj_handle, avl_i);
	rk = avltree_container_of(rhs, struct mem_fsal_obj_handle, avl_i);

	if (lk->index < rk->index)
		return -1;

	if (lk->index == rk->index)
		return 0;

	return 1;
}

/**
 * @brief Construct the fs opaque part of a mem nfsv4 handle
 *
 * Given the components of a mem nfsv4 handle, the nfsv4 handle is
 * created by concatenating the components. This is the fs opaque piece
 * of struct file_handle_v4 and what is sent over the wire.
 *
 * @param[in] pathbuf Full patch of the mem node
 * @param[in] hashkey a 64 bit hash of the mempath parameter
 *
 * @return The nfsv4 mem file handle as a char *
 */
static void package_mem_handle(char *buff,
				  struct display_buffer *pathbuf)
{
	ushort len = display_buffer_len(pathbuf);
	int opaque_bytes_used = 0, pathlen = 0;
	uint64_t hashkey = CityHash64(pathbuf->b_start,
				      display_buffer_len(pathbuf));

	memcpy(buff, &hashkey, sizeof(hashkey));
	opaque_bytes_used += sizeof(hashkey);

	/* include length of the path in the handle.
	 * MAXPATHLEN=4096 ... max path length can be contained in a short int.
	 */
	memcpy(buff + opaque_bytes_used, &len, sizeof(len));
	opaque_bytes_used += sizeof(len);

	/* Either the nfsv4 fh opaque size or the length of the mempath.
	 * Ideally we can include entire mem pathname for guaranteed
	 * uniqueness of mem handles.
	 */
	pathlen = MIN(V4_FH_OPAQUE_SIZE - opaque_bytes_used, len);
	memcpy(buff + opaque_bytes_used, pathbuf->b_start, pathlen);
	opaque_bytes_used += pathlen;

	/* If there is more space in the opaque handle due to a short mem
	 * path ... zero it.
	 */
	if (opaque_bytes_used < V4_FH_OPAQUE_SIZE) {
		memset(buff + opaque_bytes_used, 0,
		       V4_FH_OPAQUE_SIZE - opaque_bytes_used);
	}
}

/**
 * @brief Concatenate a number of mem tokens into a string
 *
 * When reading mem paths from export entries, we divide the
 * path into tokens. This function will recombine a specific number
 * of those tokens into a string.
 *
 * @param[in/out] pathbuf Must be not NULL. Tokens are copied to here.
 * @param[in] node for which a full mempath needs to be formed.
 * @param[in] maxlen maximum number of chars to copy to pathbuf
 *
 * @return void
 */
static int fullpath(struct display_buffer *pathbuf,
		    struct mem_fsal_obj_handle *this_node)
{
	int b_left;

	if (this_node->parent != NULL)
		b_left = fullpath(pathbuf, this_node->parent);
	else
		b_left = display_start(pathbuf);

	/* Add slash for all but root node */
	if (b_left > 0 && this_node->parent != NULL)
		b_left = display_cat(pathbuf, "/");

	/* Append the node's name.
	 * Note that a mem FS root's name is it's full path.
	 */
	if (b_left > 0)
		b_left = display_cat(pathbuf, this_node->m_name);

	return b_left;
}

/**
 * @brief Insert an obj into it's parent's tree
 *
 * @param[in] parent	Parent directory
 * @param[in] child	Child to insert.
 */
static void mem_insert_obj(struct mem_fsal_obj_handle *parent,
			   struct mem_fsal_obj_handle *child)
{
	uint32_t numlinks;

	PTHREAD_RWLOCK_wrlock(&parent->obj_handle.obj_lock);

	assert(!child->inavl);
	avltree_insert(&child->avl_n, &parent->mh_dir.avl_name);
	child->index = (parent->next_i)++;
	avltree_insert(&child->avl_i, &parent->mh_dir.avl_index);
	child->inavl = true;
	numlinks = atomic_inc_uint32_t(&parent->mh_dir.numlinks);
	LogFullDebug(COMPONENT_FSAL, "%s numlinks %"PRIu32, parent->m_name,
		     numlinks);

	PTHREAD_RWLOCK_unlock(&parent->obj_handle.obj_lock);
}

/**
 * @brief Remove an obj from it's parent's tree
 *
 * @note Caller must hold the obj_lock on the parent
 *
 * @param[in] parent	Parent directory
 * @param[in] child	Child to remove
 * @param[in] release	If true, release @a child
 */
static void mem_remove_obj_locked(struct mem_fsal_obj_handle *parent,
				  struct mem_fsal_obj_handle *child,
				  bool release)
{
	uint32_t numlinks;

	if (!child->inavl)
		return;

	avltree_remove(&child->avl_n, &parent->mh_dir.avl_name);
	avltree_remove(&child->avl_i, &parent->mh_dir.avl_index);
	child->inavl = false;
	numlinks = atomic_dec_uint32_t(&parent->mh_dir.numlinks);
	LogFullDebug(COMPONENT_FSAL, "%s numlinks %"PRIu32, parent->m_name,
		     numlinks);

	if (release)
		mem_release(&child->obj_handle);
}

/**
 * @brief Remove an obj from it's parent's tree
 *
 * @param[in] parent	Parent directory
 * @param[in] child	Child to remove
 */
static void mem_remove_obj(struct mem_fsal_obj_handle *parent,
				  struct mem_fsal_obj_handle *child)
{
	PTHREAD_RWLOCK_wrlock(&parent->obj_handle.obj_lock);
	mem_remove_obj_locked(parent, child, false);
	PTHREAD_RWLOCK_unlock(&parent->obj_handle.obj_lock);
}

/**
 * @brief Remove all children from a directory's tree
 *
 * @param[in] parent	Directroy to clean
 */
void mem_clean_dir_tree(struct mem_fsal_obj_handle *parent)
{
	struct avltree_node *node;
	struct mem_fsal_obj_handle *child = NULL;

	PTHREAD_RWLOCK_wrlock(&parent->obj_handle.obj_lock);

	while ((node = avltree_first(&parent->mh_dir.avl_name))) {
		child = avltree_container_of(node, struct mem_fsal_obj_handle,
					     avl_n);
		mem_remove_obj_locked(parent, child, true);
	}

	PTHREAD_RWLOCK_unlock(&parent->obj_handle.obj_lock);
}

static void mem_copy_attrs_mask(struct attrlist *attrs_in,
				struct attrlist *attrs_out)
{
	/* Use full timer resolution */
	now(&attrs_out->ctime);

	if ((attrs_in->valid_mask & ATTR_SIZE) != 0)
		attrs_out->filesize = attrs_in->filesize;
	if ((attrs_in->valid_mask & ATTR_MODE) != 0)
		attrs_out->mode = attrs_in->mode & (~S_IFMT & 0xFFFF) &
			~op_ctx->fsal_export->exp_ops.fs_umask(
						op_ctx->fsal_export);
	if ((attrs_in->valid_mask & ATTR_OWNER) != 0)
		attrs_out->owner = attrs_in->owner;

	if ((attrs_in->valid_mask & ATTR_GROUP) != 0)
		attrs_out->group = attrs_in->group;

	if ((attrs_in->valid_mask & ATTR_ATIME) != 0)
		attrs_out->atime = attrs_in->atime;
	else
		attrs_out->atime = attrs_out->ctime;

	if ((attrs_in->valid_mask & ATTR_CREATION) != 0)
		attrs_out->creation = attrs_in->creation;

	if ((attrs_in->valid_mask & ATTR_MTIME) != 0)
		attrs_out->mtime = attrs_in->mtime;
	else
		attrs_out->mtime = attrs_out->ctime;

	if ((attrs_in->valid_mask & ATTR_SPACEUSED) != 0)
		attrs_out->spaceused = attrs_in->spaceused;

	attrs_out->chgtime = attrs_out->ctime;
	attrs_out->change = timespec_to_nsecs(&attrs_out->chgtime);
}

/**
 * @brief Close a FD
 *
 * @param[in] my_fd	FD to close
 * @return FSAL status
 */
static fsal_status_t mem_close_my_fd(struct mem_fd *my_fd)
{
	my_fd->openflags = FSAL_O_CLOSED;

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

#define mem_alloc_handle(p, n, t, e, a) \
	_mem_alloc_handle(p, n, t, e, a, __func__, __LINE__)
/**
 * @brief Allocate a MEM handle
 *
 * @param[in] parent	Parent directory handle
 * @param[in] name	Name of handle to allocate
 * @param[in] type	Type of handle to allocate
 * @param[in] mfe	MEM Export owning new handle
 * @param[in] attrs	Attributes of new handle
 * @return Handle on success, NULL on failure
 */
static struct mem_fsal_obj_handle *
_mem_alloc_handle(struct mem_fsal_obj_handle *parent,
		  const char *name,
		  object_file_type_t type,
		  struct mem_fsal_export *mfe,
		  struct attrlist *attrs,
		  const char *func, int line)
{
	struct mem_fsal_obj_handle *hdl;
	char path[MAXPATHLEN];
	struct display_buffer pathbuf = {sizeof(path), path, path};
	int rc;
	size_t isize;

	isize = sizeof(struct mem_fsal_obj_handle);
	if (type == REGULAR_FILE) {
		/* Regular files need space to read/write */
		isize += MEM.inode_size;
	}

	hdl = gsh_calloc(1, isize);

	/* Establish tree details for this directory */
	hdl->m_name = gsh_strdup(name);
	hdl->parent = parent;
	hdl->datasize = MEM.inode_size;

	/* Create the full path */
	rc = fullpath(&pathbuf, hdl);

	if (rc < 0) {
		LogDebug(COMPONENT_FSAL,
			 "Could not create handle");
		goto spcerr;
	}

	glist_add_tail(&mfe->mfe_objs, &hdl->mfo_exp_entry);
	package_mem_handle(hdl->handle, &pathbuf);

	/* Fills the output struct */
	hdl->obj_handle.type = type;
	hdl->attrs.type = hdl->obj_handle.type;

	/* Need an FSID */
	hdl->obj_handle.fsid.major = op_ctx->ctx_export->export_id;
	hdl->obj_handle.fsid.minor = 0;
	hdl->attrs.fsid.major = hdl->obj_handle.fsid.major;
	hdl->attrs.fsid.minor = hdl->obj_handle.fsid.minor;

	hdl->obj_handle.fileid = atomic_postinc_uint64_t(&mem_inode_number);
	hdl->attrs.fileid = hdl->obj_handle.fileid;

	if ((attrs && attrs->valid_mask & ATTR_MODE) != 0)
		hdl->attrs.mode = attrs->mode & (~S_IFMT & 0xFFFF) &
			~op_ctx->fsal_export->exp_ops.fs_umask(
						op_ctx->fsal_export);
	else
		hdl->attrs.mode = 0600;

	if ((attrs && attrs->valid_mask & ATTR_OWNER) != 0)
		hdl->attrs.owner = attrs->owner;
	else
		hdl->attrs.owner = op_ctx->creds->caller_uid;

	if ((attrs && attrs->valid_mask & ATTR_GROUP) != 0)
		hdl->attrs.group = attrs->group;
	else
		hdl->attrs.group = op_ctx->creds->caller_gid;

	/* Use full timer resolution */
	now(&hdl->attrs.ctime);
	hdl->attrs.chgtime = hdl->attrs.ctime;

	if ((attrs && attrs->valid_mask & ATTR_ATIME) != 0)
		hdl->attrs.atime = attrs->atime;
	else
		hdl->attrs.atime = hdl->attrs.ctime;

	if ((attrs && attrs->valid_mask & ATTR_MTIME) != 0)
		hdl->attrs.mtime = attrs->mtime;
	else
		hdl->attrs.mtime = hdl->attrs.ctime;

	hdl->attrs.change =
		timespec_to_nsecs(&hdl->attrs.chgtime);

	switch (type) {
	case REGULAR_FILE:
		if ((attrs && attrs->valid_mask & ATTR_SIZE) != 0) {
			hdl->attrs.filesize = attrs->filesize;
			hdl->attrs.spaceused = attrs->filesize;
		} else {
			hdl->attrs.filesize = 0;
			hdl->attrs.spaceused = 0;
		}
		hdl->mh_file.length = hdl->attrs.filesize;
		hdl->attrs.numlinks = 1;
		break;
	case BLOCK_FILE:
	case CHARACTER_FILE:
		if ((attrs && attrs->valid_mask & ATTR_RAWDEV) != 0) {
			hdl->attrs.rawdev.major = attrs->rawdev.major;
			hdl->attrs.rawdev.minor = attrs->rawdev.minor;
		} else {
			hdl->attrs.rawdev.major = 0;
			hdl->attrs.rawdev.minor = 0;
		}
		hdl->attrs.numlinks = 1;
		break;
	case DIRECTORY:
		avltree_init(&hdl->mh_dir.avl_name, mem_n_cmpf, 0);
		avltree_init(&hdl->mh_dir.avl_index, mem_i_cmpf, 0);
		hdl->next_i = 2;
		hdl->attrs.numlinks = 2;
		hdl->mh_dir.numlinks = 2;
		break;
	default:
		hdl->attrs.numlinks = 1;
		break;
	}



	/* Set the mask at the end. */
	hdl->attrs.valid_mask = ATTRS_POSIX;
	hdl->attrs.supported = ATTRS_POSIX;

	fsal_obj_handle_init(&hdl->obj_handle, &mfe->export, type);
	mem_handle_ops_init(&hdl->obj_handle.obj_ops);

	if (parent != NULL) {
		/* Attach myself to my parent */
		mem_insert_obj(parent, hdl);
	}
#ifdef USE_LTTNG
	tracepoint(fsalmem, mem_alloc, func, line, hdl);
#endif
	return hdl;

 spcerr:

	if (hdl->m_name != NULL) {
		gsh_free(hdl->m_name);
		hdl->m_name = NULL;
	}

	gsh_free(hdl);		/* elvis has left the building */
	return NULL;
}

static fsal_status_t mem_int_lookup(struct mem_fsal_obj_handle *dir,
				    const char *path,
				    struct mem_fsal_obj_handle **entry)
{
	struct mem_fsal_obj_handle key[1];
	struct avltree_node *node;

	*entry = NULL;
	LogFullDebug(COMPONENT_FSAL, "Lookup %s in %p", path, dir);

	if (strcmp(path, "..") == 0) {
		/* lookup parent - lookupp */
		if (dir->parent == NULL) {
			return fsalstat(ERR_FSAL_NOENT, 0);
		}

		*entry = dir->parent;
		LogFullDebug(COMPONENT_FSAL,
			     "Found %s/%s hdl=%p",
			     dir->m_name, path, *entry);
		return fsalstat(ERR_FSAL_NO_ERROR, 0);
	} else if (strcmp(path, ".") == 0) {
		*entry = dir;
		return fsalstat(ERR_FSAL_NO_ERROR, 0);
	}

	key->m_name = (char *)path;
	node = avltree_lookup(&key->avl_n, &dir->mh_dir.avl_name);
	if (!node) {
		return fsalstat(ERR_FSAL_NOENT, 0);
	}
	*entry = avltree_container_of(node, struct mem_fsal_obj_handle, avl_n);

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

static fsal_status_t mem_create_obj(struct mem_fsal_obj_handle *parent,
				    object_file_type_t type,
				    const char *name,
				    struct attrlist *attrs_in,
				    struct fsal_obj_handle **new_obj,
				    struct attrlist *attrs_out)
{
	struct mem_fsal_export *mfe = container_of(op_ctx->fsal_export,
						   struct mem_fsal_export,
						   export);
	struct mem_fsal_obj_handle *hdl;
	fsal_status_t status;

	*new_obj = NULL;		/* poison it */

	if (parent->obj_handle.type != DIRECTORY) {
		LogCrit(COMPONENT_FSAL,
			"Parent handle is not a directory. hdl = 0x%p",
			parent);
		return fsalstat(ERR_FSAL_NOTDIR, 0);
	}

	status = mem_int_lookup(parent, name, &hdl);
	if (!FSAL_IS_ERROR(status)) {
		/* It already exists */
		return fsalstat(ERR_FSAL_EXIST, 0);
	} else if (status.major != ERR_FSAL_NOENT) {
		/* Some other error */
		return status;
	}

	/* allocate an obj_handle and fill it up */
	hdl = mem_alloc_handle(parent,
			       name,
			       type,
			       mfe,
			       attrs_in);
	if (!hdl)
		return fsalstat(ERR_FSAL_NOMEM, 0);

	*new_obj = &hdl->obj_handle;

	if (attrs_out != NULL)
		fsal_copy_attrs(attrs_out, &hdl->attrs, false);

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/* handle methods
 */

/**
 * @brief Lookup a file
 *
 * @param[in] parent	Parent directory
 * @param[in] path	Path to lookup
 * @param[out] handle	Found handle, on success
 * @param[out] attrs_out	Attributes of found handle
 * @return FSAL status
 */
static fsal_status_t mem_lookup(struct fsal_obj_handle *parent,
				const char *path,
				struct fsal_obj_handle **handle,
				struct attrlist *attrs_out)
{
	struct mem_fsal_obj_handle *myself, *hdl = NULL;
	fsal_status_t status;

	myself = container_of(parent,
			      struct mem_fsal_obj_handle,
			      obj_handle);

	/* Check if this context already holds the lock on
	 * this directory.
	 */
	if (op_ctx->fsal_private != parent)
		PTHREAD_RWLOCK_rdlock(&parent->obj_lock);
	else
		LogFullDebug(COMPONENT_FSAL,
			     "Skipping lock for %s",
			     myself->m_name);

	status = mem_int_lookup(myself, path, &hdl);
	if (FSAL_IS_ERROR(status)) {
		goto out;
	}

	*handle = &hdl->obj_handle;

out:
	if (op_ctx->fsal_private != parent)
		PTHREAD_RWLOCK_unlock(&parent->obj_lock);

	if (!FSAL_IS_ERROR(status) && attrs_out != NULL) {
		/* This is unlocked, however, for the most part, attributes
		 * are read-only. Come back later and do some lock protection.
		 */
		fsal_copy_attrs(attrs_out, &hdl->attrs, false);
	}

	return status;
}

/**
 * @brief Read a directory
 *
 * @param[in] dir_hdl	the directory to read
 * @param[in] whence	where to start (next)
 * @param[in] dir_state	pass thru of state to callback
 * @param[in] cb	callback function
 * @param[out] eof	eof marker true == end of dir
 */

static fsal_status_t mem_readdir(struct fsal_obj_handle *dir_hdl,
				 fsal_cookie_t *whence,
				 void *dir_state,
				 fsal_readdir_cb cb,
				 attrmask_t attrmask,
				 bool *eof)
{
	struct mem_fsal_obj_handle *myself, *hdl;
	struct avltree_node *node;
	fsal_cookie_t seekloc = 0;
	struct attrlist attrs;
	enum fsal_dir_result cb_rc;

	myself = container_of(dir_hdl,
			      struct mem_fsal_obj_handle,
			      obj_handle);

	if (whence != NULL)
		seekloc = *whence;
	else
		seekloc = 2;

	*eof = true;

	LogFullDebug(COMPONENT_FSAL,
		 "hdl=%p, name=%s",
		 myself, myself->m_name);

	PTHREAD_RWLOCK_rdlock(&dir_hdl->obj_lock);

	/* Use fsal_private to signal to lookup that we hold
	 * the lock.
	 */
	op_ctx->fsal_private = dir_hdl;

	for (node = avltree_first(&myself->mh_dir.avl_index);
	     node != NULL;
	     node = avltree_next(node)) {
		hdl = avltree_container_of(node,
					   struct mem_fsal_obj_handle,
					   avl_i);
		/* skip entries before seekloc */
		if (hdl->index < seekloc)
			continue;

		fsal_prepare_attrs(&attrs, attrmask);
		fsal_copy_attrs(&attrs, &hdl->attrs, false);

		cb_rc = cb(hdl->m_name, &hdl->obj_handle, &attrs,
			   dir_state, hdl->index + 1);

		fsal_release_attrs(&attrs);

		if (cb_rc >= DIR_TERMINATE) {
			*eof = false;
			break;
		}
	}

	op_ctx->fsal_private = NULL;

	PTHREAD_RWLOCK_unlock(&dir_hdl->obj_lock);

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Create a file
 *
 * @param[in] dir_hdl	Handle of parent directory
 * @param[in] name	Name of file to create
 * @param[in] attrs_in	Attributes to set on new file
 * @param[out] new_obj	Newly created file
 * @param[out] attrs_out Attributes of newlys created object
 *
 * @return FSAL status
 */
static fsal_status_t mem_create(struct fsal_obj_handle *dir_hdl,
				    const char *name, struct attrlist *attrs_in,
				    struct fsal_obj_handle **new_obj,
				    struct attrlist *attrs_out)
{
	struct mem_fsal_obj_handle *parent =
		container_of(dir_hdl, struct mem_fsal_obj_handle, obj_handle);

	LogDebug(COMPONENT_FSAL, "create %s", name);

	return mem_create_obj(parent, REGULAR_FILE, name, attrs_in, new_obj,
			      attrs_out);
}

/**
 * @brief Create a directory
 *
 * This function creates a new directory.
 *
 * While FSAL_MEM is a support_ex FSAL, it doesn't actually support
 * setting attributes, so only the mode attribute is relevant. Any other
 * attributes set on creation will be ignored. The owner and group will be
 * set from the active credentials.
 *
 * @param[in]     dir_hdl   Directory in which to create the directory
 * @param[in]     name      Name of directory to create
 * @param[in]     attrs_in  Attributes to set on newly created object
 * @param[out]    new_obj   Newly created object
 * @param[in,out] attrs_out Optional attributes for newly created object
 *
 * @note On success, @a new_obj has been ref'd
 *
 * @return FSAL status.
 */
static fsal_status_t mem_mkdir(struct fsal_obj_handle *dir_hdl,
			       const char *name,
			       struct attrlist *attrs_in,
			       struct fsal_obj_handle **new_obj,
			       struct attrlist *attrs_out)
{
	struct mem_fsal_obj_handle *parent =
		container_of(dir_hdl, struct mem_fsal_obj_handle, obj_handle);

	LogDebug(COMPONENT_FSAL, "mkdir %s", name);

	return mem_create_obj(parent, DIRECTORY, name, attrs_in, new_obj,
			      attrs_out);
}

/**
 * @brief Make a device node
 *
 * @param[in] dir_hdl	Parent directory handle
 * @param[in] name	Name of new node
 * @param[in] nodetype	Type of new node
 * @param[in] attrs_in	Attributes for new node
 * @param[out] new_obj	New object handle on success
 *
 * @note This returns an INITIAL ref'd entry on success
 * @return FSAL status
 */
static fsal_status_t mem_mknode(struct fsal_obj_handle *dir_hdl,
				const char *name, object_file_type_t nodetype,
				struct attrlist *attrs_in,
				struct fsal_obj_handle **new_obj,
				struct attrlist *attrs_out)
{
	struct mem_fsal_obj_handle *hdl, *parent =
		container_of(dir_hdl, struct mem_fsal_obj_handle, obj_handle);
	fsal_status_t status;

	LogDebug(COMPONENT_FSAL, "mknode %s", name);

	status = mem_create_obj(parent, nodetype, name, attrs_in, new_obj,
				attrs_out);
	if (unlikely(FSAL_IS_ERROR(status)))
		return status;

	hdl = container_of(*new_obj, struct mem_fsal_obj_handle, obj_handle);

	hdl->mh_node.nodetype = nodetype;
	hdl->mh_node.dev = attrs_in->rawdev; /* struct copy */

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Make a symlink
 *
 * @param[in] dir_hdl	Parent directory handle
 * @param[in] name	Name of new node
 * @param[in] link_path	Contents of symlink
 * @param[in] attrs_in	Attributes for new simlink
 * @param[out] new_obj	New object handle on success
 *
 * @note This returns an INITIAL ref'd entry on success
 * @return FSAL status
 */
static fsal_status_t mem_symlink(struct fsal_obj_handle *dir_hdl,
				 const char *name, const char *link_path,
				 struct attrlist *attrs_in,
				 struct fsal_obj_handle **new_obj,
				 struct attrlist *attrs_out)
{
	struct mem_fsal_obj_handle *hdl, *parent =
		container_of(dir_hdl, struct mem_fsal_obj_handle, obj_handle);
	fsal_status_t status;

	LogDebug(COMPONENT_FSAL, "symlink %s", name);

	status = mem_create_obj(parent, SYMBOLIC_LINK, name, attrs_in, new_obj,
				attrs_out);
	if (unlikely(FSAL_IS_ERROR(status)))
		return status;

	hdl = container_of(*new_obj, struct mem_fsal_obj_handle, obj_handle);

	hdl->mh_symlink.link_contents = gsh_strdup(link_path);

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Read a symlink
 *
 * @param[in] obj_hdl	Handle for symlink
 * @param[out] link_content	Buffer to fill with link contents
 * @param[in] refresh	If true, refresh attributes on symlink
 * @return FSAL status
 */
static fsal_status_t mem_readlink(struct fsal_obj_handle *obj_hdl,
				  struct gsh_buffdesc *link_content,
				  bool refresh)
{
	struct mem_fsal_obj_handle *myself =
		container_of(obj_hdl, struct mem_fsal_obj_handle, obj_handle);

	if (obj_hdl->type != SYMBOLIC_LINK) {
		LogCrit(COMPONENT_FSAL,
			"Handle is not a symlink. hdl = 0x%p",
			obj_hdl);
		return fsalstat(ERR_FSAL_INVAL, 0);
	}

	link_content->len = strlen(myself->mh_symlink.link_contents) + 1;
	link_content->addr = gsh_strdup(myself->mh_symlink.link_contents);

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Get attributes for a file
 *
 * @param[in] obj_hdl	File to get
 * @param[out] outattrs	Attributes for file
 * @return FSAL status
 */
static fsal_status_t mem_getattrs(struct fsal_obj_handle *obj_hdl,
				  struct attrlist *outattrs)
{
	struct mem_fsal_obj_handle *myself =
		container_of(obj_hdl, struct mem_fsal_obj_handle, obj_handle);

	if (myself->parent != NULL && !myself->inavl) {
		/* Removed entry - stale */
		LogDebug(COMPONENT_FSAL,
			 "Requesting attributes for removed entry %p, name=%s",
			 myself, myself->m_name);
		return fsalstat(ERR_FSAL_STALE, ESTALE);
	}

	/* We need to update the numlinks */
	myself->attrs.numlinks =
		atomic_fetch_uint32_t(&myself->mh_dir.numlinks);

#ifdef USE_LTTNG
	tracepoint(fsalmem, mem_getattrs, __func__, __LINE__, myself,
			   myself->m_name, myself->attrs.filesize,
			   myself->attrs.numlinks, myself->attrs.change);
#endif
	LogFullDebug(COMPONENT_FSAL,
		     "hdl=%p, name=%s numlinks %"PRIu32,
		     myself,
		     myself->m_name,
		     myself->attrs.numlinks);

	fsal_copy_attrs(outattrs, &myself->attrs, false);

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Set attributes on an object
 *
 * This function sets attributes on an object.  Which attributes are
 * set is determined by attrs_set->valid_mask. The FSAL must manage bypass
 * or not of share reservations, and a state may be passed.
 *
 * @param[in] obj_hdl    File on which to operate
 * @param[in] state      state_t to use for this operation
 * @param[in] attrs_set Attributes to set
 *
 * @return FSAL status.
 */
fsal_status_t mem_setattr2(struct fsal_obj_handle *obj_hdl,
			   bool bypass,
			   struct state_t *state,
			   struct attrlist *attrs_set)
{
	struct mem_fsal_obj_handle *myself =
		container_of(obj_hdl, struct mem_fsal_obj_handle, obj_handle);

	/* apply umask, if mode attribute is to be changed */
	if (FSAL_TEST_MASK(attrs_set->valid_mask, ATTR_MODE))
		attrs_set->mode &=
		    ~op_ctx->fsal_export->exp_ops.fs_umask(op_ctx->fsal_export);

	/* Test if size is being set, make sure file is regular and if so,
	 * require a read/write file descriptor.
	 */
	if (FSAL_TEST_MASK(attrs_set->valid_mask, ATTR_SIZE) &&
	    obj_hdl->type != REGULAR_FILE) {
		LogFullDebug(COMPONENT_FSAL,
			     "Setting size on non-regular file");
		return fsalstat(ERR_FSAL_INVAL, EINVAL);
	}

	/** TRUNCATE **/
	if (FSAL_TEST_MASK(attrs_set->valid_mask, ATTR_SIZE)) {
		myself->attrs.filesize = attrs_set->filesize;
	}

	/** CHMOD **/
	if (FSAL_TEST_MASK(attrs_set->valid_mask, ATTR_MODE)) {
		myself->attrs.mode = attrs_set->mode;
	}

	/**  CHOWN  **/
	if (FSAL_TEST_MASK(attrs_set->valid_mask, ATTR_OWNER)) {
		myself->attrs.owner = attrs_set->owner;
	}
	if (FSAL_TEST_MASK(attrs_set->valid_mask, ATTR_GROUP)) {
		myself->attrs.group = attrs_set->group;
	}

	/**  UTIME  **/
	if (FSAL_TEST_MASK(attrs_set->valid_mask, ATTRS_SET_TIME)) {
		if (FSAL_TEST_MASK(attrs_set->valid_mask, ATTR_ATIME_SERVER)) {
			myself->attrs.atime.tv_sec = 0;
			myself->attrs.atime.tv_nsec = UTIME_NOW;
		} else if (FSAL_TEST_MASK(attrs_set->valid_mask, ATTR_ATIME)) {
			myself->attrs.atime = attrs_set->atime;
		}

		/* Mtime */
		if (FSAL_TEST_MASK(attrs_set->valid_mask, ATTR_MTIME_SERVER)) {
			myself->attrs.mtime.tv_sec = 0;
			myself->attrs.mtime.tv_nsec = UTIME_NOW;
		} else if (FSAL_TEST_MASK(attrs_set->valid_mask, ATTR_MTIME)) {
			myself->attrs.mtime = attrs_set->mtime;
		}
	}

	/** ACL **/
	/* XXX TODO */

	return fsalstat(ERR_FSAL_NO_ERROR, EINVAL);
}

/**
 * @brief Unlink a file
 *
 * @param[in] dir_hdl	Parent directory handle
 * @param[in] obj_hdl	Object being removed
 * @param[in] name	Name of object to remove
 * @return FSAL status
 */
static fsal_status_t mem_unlink(struct fsal_obj_handle *dir_hdl,
				struct fsal_obj_handle *obj_hdl,
				const char *name)
{
	struct mem_fsal_obj_handle *parent, *myself;
	fsal_status_t status = {0, 0};
	uint32_t numlinks;

	parent = container_of(dir_hdl,
			      struct mem_fsal_obj_handle,
			      obj_handle);
	myself = container_of(obj_hdl,
			      struct mem_fsal_obj_handle,
			      obj_handle);

	PTHREAD_RWLOCK_wrlock(&dir_hdl->obj_lock);

	switch (obj_hdl->type) {
	case DIRECTORY:
		/* Check if directory is empty */
		numlinks = atomic_fetch_uint32_t(&myself->mh_dir.numlinks);
		if (numlinks > 2) {
			LogFullDebug(COMPONENT_FSAL,
				     "%s numlinks %"PRIu32,
				     myself->m_name, numlinks);
			status = fsalstat(ERR_FSAL_NOTEMPTY, 0);
			goto unlock;
		}

		break;
	case REGULAR_FILE:
		/* Openable. Make sure it's closed */
		if (myself->mh_file.fd.openflags != FSAL_O_CLOSED) {
			status = fsalstat(ERR_FSAL_FILE_OPEN, 0);
			goto unlock;
		}
		break;
	case SYMBOLIC_LINK:
	case SOCKET_FILE:
	case CHARACTER_FILE:
	case BLOCK_FILE:
	case FIFO_FILE:
		/* Unopenable.  Just clean up */
		break;
	default:
		break;
	}

	/* Remove from directory's name and index avls */
	mem_remove_obj_locked(parent, myself, false);

unlock:
	PTHREAD_RWLOCK_unlock(&dir_hdl->obj_lock);

	return status;
}

/**
 * @brief Close a file's global descriptor
 *
 * @param[in] obj_hdl    File on which to operate
 *
 * @return FSAL status.
 */

fsal_status_t mem_close(struct fsal_obj_handle *obj_hdl)
{
	struct mem_fsal_obj_handle *myself = container_of(obj_hdl,
				  struct mem_fsal_obj_handle, obj_handle);
	fsal_status_t status;

	assert(obj_hdl->type == REGULAR_FILE);

	/* Take write lock on object to protect file descriptor.
	 * This can block over an I/O operation.
	 */
	PTHREAD_RWLOCK_wrlock(&obj_hdl->obj_lock);

	status = mem_close_my_fd(&myself->mh_file.fd);

	PTHREAD_RWLOCK_unlock(&obj_hdl->obj_lock);

	return status;
}

/**
 * @brief Rename an object
 *
 * Rename the given object from @a old_name in @a olddir_hdl to @a new_name in
 * @a newdir_hdl.  The old and new directories may be the same.
 *
 * @param[in] obj_hdl	Object to rename
 * @param[in] olddir_hdl	Directory containing @a obj_hdl
 * @param[in] old_name	Current name of @a obj_hdl
 * @param[in] newdir_hdl	Directory to move @a obj_hdl to
 * @param[in] new_name	Name to rename @a obj_hdl to
 * @return FSAL status
 */
static fsal_status_t mem_rename(struct fsal_obj_handle *obj_hdl,
				struct fsal_obj_handle *olddir_hdl,
				const char *old_name,
				struct fsal_obj_handle *newdir_hdl,
				const char *new_name)
{
	struct mem_fsal_obj_handle *mem_olddir =
		container_of(olddir_hdl, struct mem_fsal_obj_handle,
			     obj_handle);
	struct mem_fsal_obj_handle *mem_newdir =
		container_of(newdir_hdl, struct mem_fsal_obj_handle,
			     obj_handle);
	struct mem_fsal_obj_handle *mem_obj =
		container_of(obj_hdl, struct mem_fsal_obj_handle, obj_handle);
	struct mem_fsal_obj_handle *mem_lookup_dst = NULL;
	fsal_status_t status;

	status = mem_int_lookup(mem_newdir, new_name, &mem_lookup_dst);
	if (!FSAL_IS_ERROR(status)) {
		uint32_t numlinks;

		if (mem_obj == mem_lookup_dst) {
			/* Same source and destination */
			return status;
		}

		if ((obj_hdl->type == DIRECTORY &&
		     mem_lookup_dst->obj_handle.type != DIRECTORY) ||
		    (obj_hdl->type != DIRECTORY &&
		     mem_lookup_dst->obj_handle.type == DIRECTORY)) {
			/* Types must be "compatible" */
			return fsalstat(ERR_FSAL_EXIST, 0);
		}

		numlinks = atomic_fetch_uint32_t(
				&mem_lookup_dst->mh_dir.numlinks);
		if (mem_lookup_dst->obj_handle.type == DIRECTORY &&
		    numlinks > 2) {
			/* Target dir must be empty */
			return fsalstat(ERR_FSAL_EXIST, 0);
		}

		/* Unlink destination */
		status = mem_unlink(newdir_hdl, &mem_lookup_dst->obj_handle,
				    new_name);
		if (FSAL_IS_ERROR(status)) {
			return status;
		}
	}

	/* Remove from old dir */
	mem_remove_obj(mem_olddir, mem_obj);

	/* Change name */
	gsh_free(mem_obj->m_name);
	mem_obj->m_name = gsh_strdup(new_name);

	/* Insert into new directory */
	mem_insert_obj(mem_newdir, mem_obj);

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Open a file descriptor for read or write and possibly create
 *
 * @param[in] obj_hdl               File to open or parent directory
 * @param[in,out] state             state_t to use for this operation
 * @param[in] openflags             Mode for open
 * @param[in] createmode            Mode for create
 * @param[in] name                  Name for file if being created or opened
 * @param[in] attrs_in              Attributes to set on created file
 * @param[in] verifier              Verifier to use for exclusive create
 * @param[in,out] new_obj           Newly created object
 * @param[in,out] attrs_out         Optional attributes for newly created object
 * @param[in,out] caller_perm_check The caller must do a permission check
 *
 * @return FSAL status.
 */
fsal_status_t mem_open2(struct fsal_obj_handle *obj_hdl,
			struct state_t *state,
			fsal_openflags_t openflags,
			enum fsal_create_mode createmode,
			const char *name,
			struct attrlist *attrs_set,
			fsal_verifier_t verifier,
			struct fsal_obj_handle **new_obj,
			struct attrlist *attrs_out,
			bool *caller_perm_check)
{
	fsal_status_t status = {0, 0};
	struct mem_fd *my_fd = NULL;
	struct mem_fsal_obj_handle *myself, *hdl = NULL;
	bool truncated;
	bool setattrs = attrs_set != NULL;
	bool created = false;
	struct attrlist verifier_attr;

	if (state != NULL)
		my_fd = (struct mem_fd *)(state + 1);

	myself = container_of(obj_hdl, struct mem_fsal_obj_handle, obj_handle);

	if (setattrs)
		LogAttrlist(COMPONENT_FSAL, NIV_FULL_DEBUG,
			    "attrs_set ", attrs_set, false);

	truncated = (openflags & FSAL_O_TRUNC) != 0;
	LogFullDebug(COMPONENT_FSAL,
		     truncated ? "Truncate" : "No truncate");

	/* Now fixup attrs for verifier if exclusive create */
	if (createmode >= FSAL_EXCLUSIVE) {
		if (!setattrs) {
			/* We need to use verifier_attr */
			attrs_set = &verifier_attr;
			memset(&verifier_attr, 0, sizeof(verifier_attr));
		}

		set_common_verifier(attrs_set, verifier);
	}

	if (name == NULL) {
		/* This is an open by handle */
		if (state != NULL) {
			/* Prepare to take the share reservation, but only if we
			 * are called with a valid state (if state is NULL the
			 * caller is a stateless create such as NFS v3 CREATE).
			 */

			/* This can block over an I/O operation. */
			PTHREAD_RWLOCK_wrlock(&obj_hdl->obj_lock);

			/* Check share reservation conflicts. */
			status = check_share_conflict(&myself->mh_file.share,
						      openflags,
						      false);

			if (FSAL_IS_ERROR(status)) {
				PTHREAD_RWLOCK_unlock(&obj_hdl->obj_lock);
				return status;
			}

			/* Take the share reservation now by updating the
			 * counters.
			 */
			update_share_counters(&myself->mh_file.share,
					      FSAL_O_CLOSED,
					      openflags);

			PTHREAD_RWLOCK_unlock(&obj_hdl->obj_lock);
		} else {
			/* We need to use the global fd to continue, and take
			 * the lock to protect it.
			 */
			my_fd = &hdl->mh_file.fd;
			PTHREAD_RWLOCK_wrlock(&obj_hdl->obj_lock);
		}

		my_fd->openflags = openflags;
		if (my_fd->openflags & FSAL_O_WRITE)
			my_fd->openflags |= FSAL_O_READ;
		my_fd->offset = 0;
		if (truncated)
			myself->attrs.filesize = myself->attrs.spaceused = 0;

		/* Now check verifier for exclusive, but not for
		 * FSAL_EXCLUSIVE_9P.
		 */
		if (createmode >= FSAL_EXCLUSIVE &&
		    createmode != FSAL_EXCLUSIVE_9P &&
		    !check_verifier_attrlist(&myself->attrs, verifier)) {
			/* Verifier didn't match, return EEXIST */
			status = fsalstat(posix2fsal_error(EEXIST), EEXIST);
		}

		if (state == NULL) {
			/* If no state, release the lock taken above and return
			 * status.
			 */
			PTHREAD_RWLOCK_unlock(&obj_hdl->obj_lock);
			return status;
		}

		if (!FSAL_IS_ERROR(status)) {
			/* Return success. */
			return status;
		}

		(void) mem_close_my_fd(my_fd);


		/* Can only get here with state not NULL and an error */

		/* On error we need to release our share reservation
		 * and undo the update of the share counters.
		 * This can block over an I/O operation.
		 */
		PTHREAD_RWLOCK_wrlock(&obj_hdl->obj_lock);

		update_share_counters(&myself->mh_file.share,
				      openflags,
				      FSAL_O_CLOSED);

		PTHREAD_RWLOCK_unlock(&obj_hdl->obj_lock);

		return status;
	}

	/* In this path where we are opening by name, we can't check share
	 * reservation yet since we don't have an object_handle yet. If we
	 * indeed create the object handle (there is no race with another
	 * open by name), then there CAN NOT be a share conflict, otherwise
	 * the share conflict will be resolved when the object handles are
	 * merged.
	 */

	status = mem_int_lookup(myself, name, &hdl);
	if (FSAL_IS_ERROR(status)) {
		struct fsal_obj_handle *new_obj;

		if (status.major != ERR_FSAL_NOENT) {
			/* Actual error from lookup */
			return status;
		}
		/* Doesn't exist, create it */
		status = mem_create_obj(myself, REGULAR_FILE, name, attrs_set,
					&new_obj, attrs_out);
		if (FSAL_IS_ERROR(status)) {
			return status;
		}
		hdl = container_of(new_obj, struct mem_fsal_obj_handle,
				   obj_handle);
		created = true;
	}

	*caller_perm_check = !created;

	/* If we didn't have a state above, use the global fd. At this point,
	 * since we just created the global fd, no one else can have a
	 * reference to it, and thus we can mamnipulate unlocked which is
	 * handy since we can then call setattr2 which WILL take the lock
	 * without a double locking deadlock.
	 */
	if (my_fd == NULL)
		my_fd = &hdl->mh_file.fd;

	my_fd->openflags = openflags;
	if (my_fd->openflags & FSAL_O_WRITE)
		my_fd->openflags |= FSAL_O_READ;

	*new_obj = &hdl->obj_handle;

	if (!created) {
		/* Create sets and gets attributes, so only do this if not
		 * creating */
		if (setattrs && attrs_set->valid_mask != 0) {
			mem_copy_attrs_mask(attrs_set, &hdl->attrs);
		}

		if (attrs_out != NULL) {
			status = (*new_obj)->obj_ops.getattrs(*new_obj,
							      attrs_out);
			if (FSAL_IS_ERROR(status) &&
			    (attrs_out->request_mask & ATTR_RDATTR_ERR) == 0) {
				/* Get attributes failed and caller expected
				 * to get the attributes. Otherwise continue
				 * with attrs_out indicating ATTR_RDATTR_ERR.
				 */
				return status;
			}
		}
	}

	if (state != NULL) {
		/* Prepare to take the share reservation, but only if we are
		 * called with a valid state (if state is NULL the caller is
		 * a stateless create such as NFS v3 CREATE).
		 */

		/* This can block over an I/O operation. */
		PTHREAD_RWLOCK_wrlock(&(*new_obj)->obj_lock);

		/* Take the share reservation now by updating the counters. */
		update_share_counters(&hdl->mh_file.share,
				      FSAL_O_CLOSED,
				      openflags);

		PTHREAD_RWLOCK_unlock(&(*new_obj)->obj_lock);
	}

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Re-open a file that may be already opened
 *
 * This function supports changing the access mode of a share reservation and
 * thus should only be called with a share state. The state_lock must be held.
 *
 * This MAY be used to open a file the first time if there is no need for
 * open by name or create semantics. One example would be 9P lopen.
 *
 * @param[in] obj_hdl     File on which to operate
 * @param[in] state       state_t to use for this operation
 * @param[in] openflags   Mode for re-open
 *
 * @return FSAL status.
 */
fsal_status_t mem_reopen2(struct fsal_obj_handle *obj_hdl,
			  struct state_t *state,
			  fsal_openflags_t openflags)
{
	struct mem_fsal_obj_handle *myself =
		container_of(obj_hdl, struct mem_fsal_obj_handle, obj_handle);
	fsal_status_t status = {0, 0};
	struct mem_fd *my_fd = (struct mem_fd *)(state + 1);
	fsal_openflags_t old_openflags;

	old_openflags = my_fd->openflags;

	/* This can block over an I/O operation. */
	PTHREAD_RWLOCK_wrlock(&obj_hdl->obj_lock);

	/* We can conflict with old share, so go ahead and check now. */
	status = check_share_conflict(&myself->mh_file.share, openflags, false);
	if (FSAL_IS_ERROR(status)) {
		PTHREAD_RWLOCK_unlock(&obj_hdl->obj_lock);
		return status;
	}

	/* Set up the new share so we can drop the lock and not have a
	 * conflicting share be asserted, updating the share counters.
	 */
	update_share_counters(&myself->mh_file.share, old_openflags, openflags);

	PTHREAD_RWLOCK_unlock(&obj_hdl->obj_lock);

	my_fd->openflags = openflags;
	my_fd->offset = 0;
	if (openflags & FSAL_O_TRUNC)
		myself->attrs.filesize = myself->attrs.spaceused = 0;

	return status;
}

/**
 * @brief Read data from a file
 *
 * This function reads data from the given file. The FSAL must be able to
 * perform the read whether a state is presented or not. This function also
 * is expected to handle properly bypassing or not share reservations.
 *
 * @param[in]     obj_hdl        File on which to operate
 * @param[in]     bypass         If state doesn't indicate a share reservation,
 *                               bypass any deny read
 * @param[in]     state          state_t to use for this operation
 * @param[in]     offset         Position from which to read
 * @param[in]     buffer_size    Amount of data to read
 * @param[out]    buffer         Buffer to which data are to be copied
 * @param[out]    read_amount    Amount of data read
 * @param[out]    end_of_file    true if the end of file has been reached
 * @param[in,out] info           more information about the data
 *
 * @return FSAL status.
 */

fsal_status_t mem_read2(struct fsal_obj_handle *obj_hdl,
			bool bypass,
			struct state_t *state,
			uint64_t offset,
			size_t buffer_size,
			void *buffer,
			size_t *read_amount,
			bool *end_of_file,
			struct io_info *info)
{
	struct mem_fsal_obj_handle *myself = container_of(obj_hdl,
				  struct mem_fsal_obj_handle, obj_handle);
	struct mem_fd *my_fd = NULL;
	fsal_status_t status = {ERR_FSAL_NO_ERROR, 0};

	if (info != NULL) {
		/* Currently we don't support READ_PLUS */
		return fsalstat(ERR_FSAL_NOTSUPP, 0);
	}

	/* Find an FD */
	if (state) {
		my_fd = (struct mem_fd *)(state + 1);
		if (!open_correct(my_fd->openflags, FSAL_O_READ)) {
			return fsalstat(ERR_FSAL_NOT_OPENED, 0);
		}
	} else {
		my_fd = &myself->mh_file.fd;
	}

	status = check_share_conflict(&myself->mh_file.share, FSAL_O_READ,
				      bypass);
	if (FSAL_IS_ERROR(status)) {
		return status;
	}

	if (offset > myself->mh_file.length) {
		buffer_size = 0;
	} else if (offset + buffer_size > myself->mh_file.length) {
		buffer_size = myself->mh_file.length - offset;
	}

	if (offset < myself->datasize) {
		size_t readsize;

		/* Data to read */
		readsize = MIN(buffer_size, myself->datasize - offset);
		memcpy(buffer, myself->data + offset, readsize);
		if (readsize < buffer_size)
			memset(buffer + readsize, 'a', buffer_size - readsize);
	} else {
		memset(buffer, 'a', buffer_size);
	}

	*read_amount = buffer_size;
	*end_of_file = (buffer_size == 0);
	now(&myself->attrs.atime);

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Write data to a file
 *
 * This function writes data to a file. The FSAL must be able to
 * perform the write whether a state is presented or not. This function also
 * is expected to handle properly bypassing or not share reservations. Even
 * with bypass == true, it will enforce a mandatory (NFSv4) deny_write if
 * an appropriate state is not passed).
 *
 * The FSAL is expected to enforce sync if necessary.
 *
 * @param[in]     obj_hdl        File on which to operate
 * @param[in]     bypass         If state doesn't indicate a share reservation,
 *                               bypass any non-mandatory deny write
 * @param[in]     state          state_t to use for this operation
 * @param[in]     offset         Position at which to write
 * @param[in]     buffer         Data to be written
 * @param[in,out] fsal_stable    In, if on, the fsal is requested to write data
 *                               to stable store. Out, the fsal reports what
 *                               it did.
 * @param[in,out] info           more information about the data
 *
 * @return FSAL status.
 */

fsal_status_t mem_write2(struct fsal_obj_handle *obj_hdl,
			 bool bypass,
			 struct state_t *state,
			 uint64_t offset,
			 size_t buffer_size,
			 void *buffer,
			 size_t *wrote_amount,
			 bool *fsal_stable,
			 struct io_info *info)
{
	struct mem_fsal_obj_handle *myself = container_of(obj_hdl,
				  struct mem_fsal_obj_handle, obj_handle);
	struct mem_fd *my_fd = NULL;
	fsal_status_t status = {ERR_FSAL_NO_ERROR, 0};

	if (info != NULL) {
		/* Currently we don't support WRITE_PLUS */
		return fsalstat(ERR_FSAL_NOTSUPP, 0);
	}

	/* Find an FD */
	if (state) {
		my_fd = (struct mem_fd *)(state + 1);
		if (!open_correct(my_fd->openflags, FSAL_O_WRITE)) {
			return fsalstat(ERR_FSAL_NOT_OPENED, 0);
		}
	} else {
		my_fd = &myself->mh_file.fd;
		status = check_share_conflict(&myself->mh_file.share,
					      FSAL_O_WRITE,
					      bypass);
		if (FSAL_IS_ERROR(status)) {
			return status;
		}
	}

	if (offset + buffer_size > myself->mh_file.length) {
		myself->attrs.filesize = myself->mh_file.length = offset +
			buffer_size;
	}

	if (offset < myself->datasize) {
		size_t writesize;

		/* Space to write */
		writesize = MIN(buffer_size, myself->datasize - offset);
		memcpy(myself->data + offset, buffer, writesize);
	}

	/* Update change stats */
	now(&myself->attrs.mtime);
	myself->attrs.chgtime = myself->attrs.mtime;
	myself->attrs.change =
		timespec_to_nsecs(&myself->attrs.chgtime);

	*wrote_amount = buffer_size;

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Commit written data
 *
 * This function flushes possibly buffered data to a file. This method
 * differs from commit due to the need to interact with share reservations
 * and the fact that the FSAL manages the state of "file descriptors". The
 * FSAL must be able to perform this operation without being passed a specific
 * state.
 *
 * @param[in] obj_hdl          File on which to operate
 * @param[in] state            state_t to use for this operation
 * @param[in] offset           Start of range to commit
 * @param[in] len              Length of range to commit
 *
 * @return FSAL status.
 */

fsal_status_t mem_commit2(struct fsal_obj_handle *obj_hdl,
			  off_t offset,
			  size_t len)
{
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Perform a lock operation
 *
 * This function performs a lock operation (lock, unlock, test) on a
 * file. This method assumes the FSAL is able to support lock owners,
 * though it need not support asynchronous blocking locks. Passing the
 * lock state allows the FSAL to associate information with a specific
 * lock owner for each file (which may include use of a "file descriptor".
 *
 * @param[in]  obj_hdl          File on which to operate
 * @param[in]  state            state_t to use for this operation
 * @param[in]  owner            Lock owner
 * @param[in]  lock_op          Operation to perform
 * @param[in]  request_lock     Lock to take/release/test
 * @param[out] conflicting_lock Conflicting lock
 *
 * @return FSAL status.
 */
fsal_status_t mem_lock_op2(struct fsal_obj_handle *obj_hdl,
			   struct state_t *state,
			   void *owner,
			   fsal_lock_op_t lock_op,
			   fsal_lock_param_t *request_lock,
			   fsal_lock_param_t *conflicting_lock)
{
	/* Stub out for now */
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Manage closing a file when a state is no longer needed.
 *
 * When the upper layers are ready to dispense with a state, this method is
 * called to allow the FSAL to close any file descriptors or release any other
 * resources associated with the state. A call to free_state should be assumed
 * to follow soon.
 *
 * @param[in] obj_hdl    File on which to operate
 * @param[in] state      state_t to use for this operation
 *
 * @return FSAL status.
 */

fsal_status_t mem_close2(struct fsal_obj_handle *obj_hdl,
			 struct state_t *state)
{
	struct mem_fd *my_fd = (struct mem_fd *)(state + 1);
	struct mem_fsal_obj_handle *myself = container_of(obj_hdl,
				  struct mem_fsal_obj_handle, obj_handle);
	fsal_status_t status;

	if (state->state_type == STATE_TYPE_SHARE ||
	    state->state_type == STATE_TYPE_NLM_SHARE ||
	    state->state_type == STATE_TYPE_9P_FID) {
		/* This is a share state, we must update the share counters */

		/* This can block over an I/O operation. */
		PTHREAD_RWLOCK_wrlock(&obj_hdl->obj_lock);

		update_share_counters(&myself->mh_file.share,
				      my_fd->openflags,
				      FSAL_O_CLOSED);

		PTHREAD_RWLOCK_unlock(&obj_hdl->obj_lock);
	}

	status = mem_close_my_fd(&myself->mh_file.fd);

	return status;
}

/**
 * @brief Get the wire version of a handle
 *
 * fill in the opaque f/s file handle part.
 * we zero the buffer to length first.  This MAY already be done above
 * at which point, remove memset here because the caller is zeroing
 * the whole struct.
 *
 * @param[in] obj_hdl	Handle to digest
 * @param[in] out_type	Type of digest to get
 * @param[out] fh_desc	Buffer to write digest into
 * @return FSAL status
 */
static fsal_status_t mem_handle_to_wire(const struct fsal_obj_handle *obj_hdl,
					fsal_digesttype_t output_type,
					struct gsh_buffdesc *fh_desc)
{
	const struct mem_fsal_obj_handle *myself;

	myself = container_of(obj_hdl,
			      const struct mem_fsal_obj_handle,
			      obj_handle);

	switch (output_type) {
	case FSAL_DIGEST_NFSV3:
	case FSAL_DIGEST_NFSV4:
		if (fh_desc->len < V4_FH_OPAQUE_SIZE) {
			LogMajor(COMPONENT_FSAL,
				 "Space too small for handle.  need %lu, have %zu",
				 ((unsigned long) V4_FH_OPAQUE_SIZE),
				 fh_desc->len);
			return fsalstat(ERR_FSAL_TOOSMALL, 0);
		}

		memcpy(fh_desc->addr, myself->handle, V4_FH_OPAQUE_SIZE);
		fh_desc->len = V4_FH_OPAQUE_SIZE;
		break;

	default:
		return fsalstat(ERR_FSAL_SERVERFAULT, 0);
	}

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/**
 * @brief Get the unique key for a handle
 *
 * return a handle descriptor into the handle in this object handle
 * @TODO reminder.  make sure things like hash keys don't point here
 * after the handle is released.
 *
 * @param[in] obj_hdl	Handle to digest
 * @param[out] fh_desc	Buffer to write key into
 */
static void mem_handle_to_key(struct fsal_obj_handle *obj_hdl,
			  struct gsh_buffdesc *fh_desc)
{
	struct mem_fsal_obj_handle *myself;

	myself = container_of(obj_hdl,
			      struct mem_fsal_obj_handle,
			      obj_handle);

	fh_desc->addr = myself->handle;
	fh_desc->len = V4_FH_OPAQUE_SIZE;
}

/**
 * @brief Release an object handle
 *
 * @param[in] obj_hdl	Handle to release
 */
static void mem_release(struct fsal_obj_handle *obj_hdl)
{
	struct mem_fsal_obj_handle *myself;

	myself = container_of(obj_hdl,
			      struct mem_fsal_obj_handle,
			      obj_handle);

	if (myself->parent == NULL || myself->inavl) {
		/* Entry is still live: NULL parent means export handle, inavl
		 * means used by parent */
#ifdef USE_LTTNG
		tracepoint(fsalmem, mem_inuse, __func__, __LINE__, myself,
			   myself->inavl);
#endif
		LogDebug(COMPONENT_FSAL,
			 "Releasing live hdl=%p, name=%s, don't deconstruct it",
			 myself, myself->m_name);
		return;
	}

	fsal_obj_handle_fini(obj_hdl);

	LogDebug(COMPONENT_FSAL,
		 "Releasing obj_hdl=%p, myself=%p, name=%s",
		 obj_hdl, myself, myself->m_name);

	switch (obj_hdl->type) {
	case DIRECTORY:
		/* Empty directory */
		mem_clean_dir_tree(myself);
		break;
	case REGULAR_FILE:
		break;
	case SYMBOLIC_LINK:
		gsh_free(myself->mh_symlink.link_contents);
		break;
	case SOCKET_FILE:
	case CHARACTER_FILE:
	case BLOCK_FILE:
	case FIFO_FILE:
		break;
	default:
		break;
	}

	mem_free_handle(myself);
}

void mem_handle_ops_init(struct fsal_obj_ops *ops)
{
	ops->release = mem_release;
	ops->lookup = mem_lookup;
	ops->readdir = mem_readdir;
	ops->create = mem_create;
	ops->mkdir = mem_mkdir;
	ops->mknode = mem_mknode;
	ops->symlink = mem_symlink;
	ops->readlink = mem_readlink;
	ops->getattrs = mem_getattrs;
	ops->setattr2 = mem_setattr2;
	/*ops->link = mdcache_link;*/ /* Not supported, currently */
	ops->rename = mem_rename;
	ops->unlink = mem_unlink;
	ops->close = mem_close;
	ops->open2 = mem_open2;
	ops->reopen2 = mem_reopen2;
	ops->read2 = mem_read2;
	ops->write2 = mem_write2;
	ops->commit2 = mem_commit2;
	ops->lock_op2 = mem_lock_op2;
	ops->close2 = mem_close2;
	ops->handle_to_wire = mem_handle_to_wire;
	ops->handle_to_key = mem_handle_to_key;
}

/* export methods that create object handles
 */

/* lookup_path
 * modelled on old api except we don't stuff attributes.
 * KISS
 */

fsal_status_t mem_lookup_path(struct fsal_export *exp_hdl,
			      const char *path,
			      struct fsal_obj_handle **obj_hdl,
			      struct attrlist *attrs_out)
{
	struct mem_fsal_export *mfe;
	struct attrlist attrs;

	mfe = container_of(exp_hdl, struct mem_fsal_export, export);

	if (strcmp(path, mfe->export_path) != 0) {
		/* Lookup of a path other than the export's root. */
		LogCrit(COMPONENT_FSAL,
			"Attempt to lookup non-root path %s",
			path);
		return fsalstat(ERR_FSAL_NOENT, ENOENT);
	}

	attrs.valid_mask = ATTR_MODE;
	attrs.mode = 0755;

	if (mfe->root_handle == NULL) {
		mfe->root_handle = mem_alloc_handle(NULL,
						    mfe->export_path,
						    DIRECTORY,
						    mfe,
						    &attrs);
	}

	*obj_hdl = &mfe->root_handle->obj_handle;

	if (attrs_out != NULL)
		fsal_copy_attrs(attrs_out, &mfe->root_handle->attrs, false);

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/* create_handle
 * Does what original FSAL_ExpandHandle did (sort of)
 * returns a ref counted handle to be later used in cache_inode etc.
 * NOTE! you must release this thing when done with it!
 * BEWARE! Thanks to some holes in the *AT syscalls implementation,
 * we cannot get an fd on an AF_UNIX socket, nor reliably on block or
 * character special devices.  Sorry, it just doesn't...
 * we could if we had the handle of the dir it is in, but this method
 * is for getting handles off the wire for cache entries that have LRU'd.
 * Ideas and/or clever hacks are welcome...
 */

fsal_status_t mem_create_handle(struct fsal_export *exp_hdl,
				struct gsh_buffdesc *hdl_desc,
				struct fsal_obj_handle **obj_hdl,
				struct attrlist *attrs_out)
{
	struct glist_head *glist;
	struct fsal_obj_handle *hdl;
	struct mem_fsal_obj_handle *my_hdl;

	*obj_hdl = NULL;

	if (hdl_desc->len != V4_FH_OPAQUE_SIZE) {
		LogCrit(COMPONENT_FSAL,
			"Invalid handle size %zu expected %lu",
			hdl_desc->len,
			((unsigned long) V4_FH_OPAQUE_SIZE));

		return fsalstat(ERR_FSAL_BADHANDLE, 0);
	}

	PTHREAD_RWLOCK_rdlock(&exp_hdl->fsal->lock);

	glist_for_each(glist, &exp_hdl->fsal->handles) {
		hdl = glist_entry(glist, struct fsal_obj_handle, handles);

		my_hdl = container_of(hdl,
				      struct mem_fsal_obj_handle,
				      obj_handle);

		if (memcmp(my_hdl->handle,
			   hdl_desc->addr,
			   V4_FH_OPAQUE_SIZE) == 0) {
			LogDebug(COMPONENT_FSAL,
				 "Found hdl=%p name=%s",
				 my_hdl, my_hdl->m_name);

			*obj_hdl = hdl;

			PTHREAD_RWLOCK_unlock(&exp_hdl->fsal->lock);

			if (attrs_out != NULL) {
				fsal_copy_attrs(attrs_out, &my_hdl->attrs,
						false);
			}

			return fsalstat(ERR_FSAL_NO_ERROR, 0);
		}
	}

	LogDebug(COMPONENT_FSAL,
		"Could not find handle");

	PTHREAD_RWLOCK_unlock(&exp_hdl->fsal->lock);

	return fsalstat(ERR_FSAL_STALE, ESTALE);
}
