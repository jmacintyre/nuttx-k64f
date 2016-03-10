/****************************************************************************
 * include/nuttx/tls.h
 *
 *   Copyright (C) 2016 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

#ifndef __INCLUDE_NUTTX_TLS_H
#define __INCLUDE_NUTTX_TLS_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>

#ifdef CONFIG_TLS

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/
/* Configuration ************************************************************/

#ifndef CONFIG_TLS_LOG2_MAXSTACK
#  error CONFIG_TLS_LOG2_MAXSTACK is not defined
#endif

/* TLS Definitions **********************************************************/

#define TLS_STACK_ALIGN   (1L << CONFIG_TLS_LOG2_MAXSTACK)
#define TLS_STACK_MASK    (TLS_STACK_ALIGN - 1)
#define TLS_MAXSTACK      (TLS_STACK_ALIGN)
#define TLS_INFO(sp)      ((FAR struct tls_info_s *)((sp) & TLS_STACK_MASK))

/****************************************************************************
 * Public Types
 ****************************************************************************/
/* When TLS is enabled, up_createstack() will align allocated stacks to the
 * TLS_STACK_ALIGN value.  An instance of the following structure will be
 * implicitly positioned at the "lower" end of the stack.  Assuming a
 * "push down" stack, this is at the "far" end of the stack (and can be
 * clobbered if the stack overflows).
 *
 * If an MCU has a "push up" then that TLS structure will lie at the top
 * of the stack and stack allocation and initialization logic must take
 * care to preserve this structure content.
 *
 * The stack memory is fully accessible to user mode threads but will
 * contain references to OS internal, private data structures (such as the
 * TCB)
 */

struct tcb_s; /* Forward reference */
struct tls_info_s
{
  FAR struct tcb_s *tl_tcb; /* The TCB of the current task */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: tls_cpu_index
 *
 * Description:
 *   Return an index in the range of 0 through (CONFIG_SMP_NCPUS-1) that
 *   corresponds to the currently executing CPU.  This is index is retained
 *   in the task TCB which is accessible via the tls_info_s structure.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   An integer index in the range of 0 through (CONFIG_SMP_NCPUS-1) that
 *   corresponds to the currently executing CPU.
 *
 ****************************************************************************/

#ifdef CONFIG_SMP
int tls_cpu_index(void);
#endif

#endif /* CONFIG_TLS */
#endif /* __INCLUDE_NUTTX_TLS_H */