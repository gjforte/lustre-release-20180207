/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.gnu.org/licenses/gpl-2.0.html
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2017, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/mdd/mdd_trans.c
 *
 * Lustre Metadata Server (mdd) routines
 *
 * Author: Wang Di <wangdi@clusterfs.com>
 */

#define DEBUG_SUBSYSTEM S_MDS

#include <obd_class.h>
#include <lprocfs_status.h>
#include <lustre_mds.h>
#include <lustre_barrier.h>

#include "mdd_internal.h"

struct thandle *mdd_trans_create(const struct lu_env *env,
                                 struct mdd_device *mdd)
{
	/* If blocked by the write barrier, then return "-EINPROGRESS"
	 * to the caller. Usually, such error will be forwarded to the
	 * client, and the expected behaviour is to re-try such modify
	 * RPC some time later until the barrier is thawed or expired. */
	if (unlikely(!barrier_entry(mdd->mdd_bottom)))
		return ERR_PTR(-EINPROGRESS);

        return mdd_child_ops(mdd)->dt_trans_create(env, mdd->mdd_child);
}

int mdd_trans_start(const struct lu_env *env, struct mdd_device *mdd,
                    struct thandle *th)
{
        return mdd_child_ops(mdd)->dt_trans_start(env, mdd->mdd_child, th);
}

int mdd_trans_stop(const struct lu_env *env, struct mdd_device *mdd,
		   int result, struct thandle *handle)
{
	int rc;

	handle->th_result = result;
	rc = mdd_child_ops(mdd)->dt_trans_stop(env, mdd->mdd_child, handle);
	barrier_exit(mdd->mdd_bottom);

	/* if operation failed, return \a result, otherwise return status of
	 * dt_trans_stop */
	return result ?: rc;
}
