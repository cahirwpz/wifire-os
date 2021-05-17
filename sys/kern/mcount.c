/*	$NetBSD: mcount.c,v 1.14 2019/08/27 22:48:53 kamil Exp $	*/

/*
 * Copyright (c) 2003, 2004 Wasabi Systems, Inc.
 * All rights reserved.
 *
 * Written by Nathan J. Williams for Wasabi Systems, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed for the NetBSD Project by
 *	Wasabi Systems, Inc.
 * 4. The name of Wasabi Systems, Inc. may not be used to endorse
 *    or promote products derived from this software without specific prior
 *    written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY WASABI SYSTEMS, INC. ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL WASABI SYSTEMS, INC
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*-
 * Copyright (c) 1983, 1992, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/gmon.h>
#include <sys/interrupt.h>
#include <sys/spinlock.h>
#include <sys/types.h>

static SPIN_DEFINE(mcount_lock, 0);

/*
 * The function is updating an array of linked lists, which stores 
 * how many times a functions have been called by another function.
 * 
 *  froms[X] - an array of linked list, index X encodes a function, the kept
 *             value is an index of tos entry (the first node in the list,
 *             0 - means an empty list), the list is sorted by most recently
 *             used function
 * 
 *  tos[N]   - a node, which let us know how many times function with address selfpc 
 *             have been called by X, and stores index of the next element in the list
 *             (0 - means the end of the list)
 * 
 *  tos[0] + 1  is the smallest index of an unused tos entry
 */

void __cyg_profile_func_enter(void *from, void *self) {
  u_long frompc = (u_long)from, selfpc = (u_long)self;
  u_short *frompcindex;
  tostruct_t *top, *prevtop;
  gmonparam_t *p = &_gmonparam;
  long toindex;

  /*
   * check that we are profiling
   * and that we aren't recursively invoked.
   */
  if (p->state != GMON_PROF_ON)
    return;

  WITH_SPIN_LOCK (&mcount_lock) {
    p->state = GMON_PROF_BUSY;
    /*
     * check that frompc is a reasonable pc value.
     * for example:	signal catchers get called from the stack,
     *		not from text space.  too bad.
     */
    frompc -= p->lowpc;
    if (frompc >= p->textsize)
      goto done;

    size_t index = (frompc / (HASHFRACTION * sizeof(*p->froms)));
    frompcindex = &p->froms[index];
    toindex = *frompcindex;
    /*
     *	First time profiling this calling function .
     */
    if (toindex == 0) {
      /*
       * Getting an unused node (the smallest unused tos entry index).
       */
      toindex = ++p->tos[0].link;
      if (toindex >= p->tolimit) {
        p->state = GMON_PROF_ERROR;
        goto done;
      }

      *frompcindex = (u_short)toindex;
      top = &p->tos[(size_t)toindex];
      top->selfpc = selfpc;
      top->count = 1;
      top->link = 0;
      goto done;
    }
    top = &p->tos[(size_t)toindex];
    /*
     * Node with our called function at front of chain; usual case.
     */
    if (top->selfpc == selfpc) {
      top->count++;
      goto done;
    }
    /*
     * Traversing the list and looking for node with our called function.
     */
    while (true) {
      /*
       * We reached the end of the list it does not contain a node with
       * the called function. Check if there are still available nodes to use,
       *  if so get one and add it to the list.
       */
      if (top->link == 0) {
        toindex = ++p->tos[0].link;
        if (toindex >= p->tolimit) {
          p->state = GMON_PROF_ERROR;
          goto done;
        }

        top = &p->tos[(size_t)toindex];
        top->selfpc = selfpc;
        top->count = 1;
        top->link = *frompcindex;
        *frompcindex = (u_short)toindex;
        goto done;
      }
      /*
       * Move to the next node.
       */
      prevtop = top;
      top = &p->tos[top->link];
      /*
       * We found our node,remove it from our list
       * and add it at the beginning of the list.
       */
      if (top->selfpc == selfpc) {
        top->count++;
        toindex = prevtop->link;
        prevtop->link = top->link;
        top->link = *frompcindex;
        *frompcindex = (u_short)toindex;
        goto done;
      }
    }
  done:
    if (p->state != GMON_PROF_ERROR)
      p->state = GMON_PROF_ON;
  }
}

void __cyg_profile_func_exit(void *this_fn, void *call_site) {
}
