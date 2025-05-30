/*-------------------------------------------------------------------------
 *
 * latch.h
 *	  Routines for interprocess latches
 *
 * A latch is a boolean variable, with operations that let processes sleep
 * until it is set. A latch can be set from another process, or a signal
 * handler within the same process.
 *
 * The latch interface is a reliable replacement for the common pattern of
 * using pg_usleep() or select() to wait until a signal arrives, where the
 * signal handler sets a flag variable. Because on some platforms an
 * incoming signal doesn't interrupt sleep, and even on platforms where it
 * does there is a race condition if the signal arrives just before
 * entering the sleep, the common pattern must periodically wake up and
 * poll the flag variable. The pselect() system call was invented to solve
 * this problem, but it is not portable enough. Latches are designed to
 * overcome these limitations, allowing you to sleep without polling and
 * ensuring quick response to signals from other processes.
 *
 * There are two kinds of latches: local and shared. A local latch is
 * initialized by InitLatch, and can only be set from the same process.
 * A local latch can be used to wait for a signal to arrive, by calling
 * SetLatch in the signal handler. A shared latch resides in shared memory,
 * and must be initialized at postmaster startup by InitSharedLatch. Before
 * a shared latch can be waited on, it must be associated with a process
 * with OwnLatch. Only the process owning the latch can wait on it, but any
 * process can set it.
 *
 * There are three basic operations on a latch:
 *
 * SetLatch		- Sets the latch
 * ResetLatch	- Clears the latch, allowing it to be set again
 * WaitLatch	- Waits for the latch to become set
 *
 * WaitLatch includes a provision for timeouts (which should be avoided
 * when possible, as they incur extra overhead) and a provision for
 * postmaster child processes to wake up immediately on postmaster death.
 * See latch.c for detailed specifications for the exported functions.
 *
 * The correct pattern to wait for event(s) is:
 *
 * for (;;)
 * {
 *	   ResetLatch();
 *	   if (work to do)
 *		   Do Stuff();
 *	   WaitLatch();
 * }
 *
 * It's important to reset the latch *before* checking if there's work to
 * do. Otherwise, if someone sets the latch between the check and the
 * ResetLatch call, you will miss it and Wait will incorrectly block.
 *
 * Another valid coding pattern looks like:
 *
 * for (;;)
 * {
 *	   if (work to do)
 *		   Do Stuff(); // in particular, exit loop if some condition satisfied
 *	   WaitLatch();
 *	   ResetLatch();
 * }
 *
 * This is useful to reduce latch traffic if it's expected that the loop's
 * termination condition will often be satisfied in the first iteration;
 * the cost is an extra loop iteration before blocking when it is not.
 * What must be avoided is placing any checks for asynchronous events after
 * WaitLatch and before ResetLatch, as that creates a race condition.
 *
 * To wake up the waiter, you must first set a global flag or something
 * else that the wait loop tests in the "if (work to do)" part, and call
 * SetLatch *after* that. SetLatch is designed to return quickly if the
 * latch is already set.
 *
 * On some platforms, signals will not interrupt the latch wait primitive
 * by themselves.  Therefore, it is critical that any signal handler that
 * is meant to terminate a WaitLatch wait calls SetLatch.
 *
 * Note that use of the process latch (PGPROC.procLatch) is generally better
 * than an ad-hoc shared latch for signaling auxiliary processes.  This is
 * because generic signal handlers will call SetLatch on the process latch
 * only, so using any latch other than the process latch effectively precludes
 * use of any generic handler.
 *
 *
 * See also WaitEventSets in waiteventset.h. They allow to wait for latches
 * being set and additional events - postmaster dying and socket readiness of
 * several sockets currently - at the same time.  On many platforms using a
 * long lived event set is more efficient than using WaitLatch or
 * WaitLatchOrSocket.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/latch.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LATCH_H
#define LATCH_H

#include <signal.h>

#include "storage/waiteventset.h"	/* for WL_* arguments to WaitLatch */

/*
 * Latch structure should be treated as opaque and only accessed through
 * the public functions. It is defined here to allow embedding Latches as
 * part of bigger structs.
 */
typedef struct Latch
{
	sig_atomic_t is_set;
	sig_atomic_t maybe_sleeping;
	bool		is_shared;
	int			owner_pid;
#ifdef WIN32
	HANDLE		event;
#endif
} Latch;

/*
 * prototypes for functions in latch.c
 */
extern void InitLatch(Latch *latch);
extern void InitSharedLatch(Latch *latch);
extern void OwnLatch(Latch *latch);
extern void DisownLatch(Latch *latch);
extern void SetLatch(Latch *latch);
extern void ResetLatch(Latch *latch);

extern int	WaitLatch(Latch *latch, int wakeEvents, long timeout,
					  uint32 wait_event_info);
extern int	WaitLatchOrSocket(Latch *latch, int wakeEvents,
							  pgsocket sock, long timeout, uint32 wait_event_info);
extern void InitializeLatchWaitSet(void);

#endif							/* LATCH_H */
