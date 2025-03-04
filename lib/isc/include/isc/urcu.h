/*
 * Copyright (C) Internet Systems Consortium, Inc. ("ISC")
 *
 * SPDX-License-Identifier: MPL-2.0
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, you can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * See the COPYRIGHT file distributed with this work for additional
 * information regarding copyright ownership.
 */

#pragma once

#include <isc/util.h>

/* when urcu is not installed in a system header location */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

#if defined(RCU_MEMBARRIER) || defined(RCU_MB) || defined(RCU_SIGNAL)
#include <urcu.h>
#elif defined(RCU_QSBR)
#include <urcu-qsbr.h>
#elif defined(RCU_BP)
#include <urcu-bp.h>
#endif

#include <urcu-pointer.h>

#include <urcu/compiler.h>
#include <urcu/rculfhash.h>
#include <urcu/rculist.h>
#include <urcu/wfstack.h>

#pragma GCC diagnostic pop

/*
 * Help thread sanitizer to understand `call_rcu()`:
 *
 * The `rcu_head` argument to `call_rcu()` is expected to be embedded
 * in a structure defined by the caller, which is named `_rcuctx` in
 * these macros. The callback function uses `caa_container_of()` to
 * recover the `rcuctx` pointer from the `_rcu_head` pointer that is
 * passed to the callback.
 *
 * We explain the ordering dependency to tsan by releasing `_rcuctx`
 * pointer before `call_rcu()` and acquiring it in the callback
 * funtion. We pass the outer `_rcuctx` pointer to the `__tsan_`
 * barriers, because it should match a pointer that is known by tsan
 * to have been returned by `malloc()`.
 */

#define isc_urcu_cleanup(ptr, member, func)     \
	{                                       \
		__tsan_release(ptr);            \
		call_rcu(&(ptr)->member, func); \
	}

#define isc_urcu_container(ptr, type, member)                     \
	({                                                        \
		type *_ptr = caa_container_of(ptr, type, member); \
		__tsan_acquire(_ptr);                             \
		_ptr;                                             \
	})

#if defined(RCU_QSBR)

/*
 * Define wrappers that allows us to make the thread online without any extra
 * heavy tooling around libuv callbacks.
 */

#define isc_qsbr_read_lock()                       \
	{                                          \
		if (!urcu_qsbr_read_ongoing()) {   \
			urcu_qsbr_thread_online(); \
		}                                  \
		urcu_qsbr_read_lock();             \
	}

#undef rcu_read_lock
#define rcu_read_lock() isc_qsbr_read_lock()

#define isc_qsbr_call_rcu(rcu_head, func)           \
	{                                           \
		if (!urcu_qsbr_read_ongoing()) {    \
			urcu_qsbr_thread_online();  \
		}                                   \
		urcu_qsbr_call_rcu(rcu_head, func); \
	}

#undef call_rcu
#define call_rcu(rcu_head, func) isc_qsbr_call_rcu(rcu_head, func)

#define isc_qsbr_synchronize_rcu()                 \
	{                                          \
		if (!urcu_qsbr_read_ongoing()) {   \
			urcu_qsbr_thread_online(); \
		}                                  \
		urcu_qsbr_synchronize_rcu();       \
	}

#undef synchronize_rcu
#define synchronize_rcu() isc_qsbr_syncronize_rcu()

#define isc_qsbr_rcu_dereference(ptr)              \
	({                                         \
		if (!urcu_qsbr_read_ongoing()) {   \
			urcu_qsbr_thread_online(); \
		}                                  \
		_rcu_dereference(ptr);             \
	})

#undef rcu_dereference
#define rcu_dereference(ptr) isc_qsbr_rcu_dereference(ptr)

#endif /* RCU_QSBR */
