/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NTSYNC_GKI_COMPAT_H
#define NTSYNC_GKI_COMPAT_H

#include <linux/file.h>
#include <linux/slab.h>
#include <linux/version.h>

/* Prefer the UAPI bundled with the vendored driver over a kernel-tree copy. */
#include "include/uapi/linux/ntsync.h"

/* Android 5.10 lacks newer lockdep helpers and their disabled-config stubs. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)
#ifndef LOCK_STATE_UNKNOWN
#define LOCK_STATE_UNKNOWN -1
#define LOCK_STATE_NOT_HELD 0
#define LOCK_STATE_HELD 1
#endif

#ifndef lockdep_is_held
/* Avoid a false lock assertion when the target has CONFIG_LOCKDEP disabled. */
#define lockdep_is_held(lock) LOCK_STATE_UNKNOWN
#endif

#ifndef lockdep_assert
#define lockdep_assert(condition) \
	do { \
		WARN_ON(debug_locks && !(condition)); \
	} while (0)
#endif
#endif

/* kzalloc_obj() was introduced after the oldest supported GKI kernel. */
#ifndef kzalloc_obj
#define kzalloc_obj(object) kzalloc(sizeof(object), GFP_KERNEL)
#endif

/* FD_PREPARE() and its helpers are absent from Android GKI through 6.6. */
#ifndef FD_PREPARE
struct ntsync_compat_fd {
	struct file *file;
	int fd;
	int err;
};

static inline struct ntsync_compat_fd
ntsync_compat_fd_prepare(unsigned int flags, struct file *file)
{
	struct ntsync_compat_fd fdf = { .file = file, .fd = -1 };

	if (IS_ERR(file)) {
		fdf.err = PTR_ERR(file);
		return fdf;
	}

	fdf.fd = get_unused_fd_flags(flags);
	if (fdf.fd < 0) {
		fdf.err = fdf.fd;
		fput(file);
	}

	return fdf;
}

static inline int ntsync_compat_fd_publish(struct ntsync_compat_fd fdf)
{
	fd_install(fdf.fd, fdf.file);
	return fdf.fd;
}

#define FD_PREPARE(fdf, flags, getfile) \
	struct ntsync_compat_fd fdf = ntsync_compat_fd_prepare((flags), (getfile))
#define fd_prepare_file(fdf) ((fdf).file)
#define fd_publish(fdf) ntsync_compat_fd_publish(fdf)
#endif

/* kmalloc_flex() was introduced after the oldest supported GKI kernel. */
#ifndef kmalloc_flex
#define kmalloc_flex(object, member, count) \
	kmalloc(struct_size(&(object), member, count), GFP_KERNEL)
#endif

#endif
