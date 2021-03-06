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
 * Copyright (c) 2008, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * libcfs/include/libcfs/libcfs_time.h
 *
 * Time functions.
 *
 */

#ifndef __LIBCFS_TIME_H__
#define __LIBCFS_TIME_H__

#include <libcfs/linux/linux-time.h>

/*
 * generic time manipulation functions.
 */

static inline cfs_time_t cfs_time_add(cfs_time_t t, cfs_duration_t d)
{
        return (cfs_time_t)(t + d);
}

static inline cfs_duration_t cfs_time_sub(cfs_time_t t1, cfs_time_t t2)
{
        return (cfs_time_t)(t1 - t2);
}

static inline int cfs_time_after(cfs_time_t t1, cfs_time_t t2)
{
        return cfs_time_before(t2, t1);
}

static inline int cfs_time_aftereq(cfs_time_t t1, cfs_time_t t2)
{
        return cfs_time_beforeq(t2, t1);
}

static inline cfs_time_t cfs_time_shift(int seconds)
{
        return cfs_time_add(cfs_time_current(), cfs_time_seconds(seconds));
}

#define CFS_TICK	1

#endif
