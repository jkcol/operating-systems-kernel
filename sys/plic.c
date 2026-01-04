// plic.c - RISC-V PLIC
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef PLIC_TRACE
#define TRACE
#endif

#ifdef PLIC_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "plic.h"
#include "misc.h"

#include <stdint.h>

// INTERNAL MACRO DEFINITIONS
//

// CTX(i,0) is hartid /i/ M-mode context
// CTX(i,1) is hartid /i/ S-mode context

#define CTX(i,s) (2*(i)+(s))

// INTERNAL TYPE DEFINITIONS
// 


struct plic_regs {
	union {
		uint32_t priority[PLIC_SRC_CNT]; /**< Interrupt Priorities registers */
		char _reserved_priority[0x1000];
	};

	union {
		uint32_t pending[PLIC_SRC_CNT/32]; /**< Interrupt Pending Bits registers */
		char _reserved_pending[0x1000];
	};

	union {
		uint32_t enable[PLIC_CTX_CNT][32]; /**< Interrupt Enables registers */
		char _reserved_enable[0x200000-0x2000];
	};

	struct {
		union {
			struct {
				uint32_t threshold;	/**< Priority Thresholds registers */
				uint32_t claim;	/**< Interrupt Claim/Completion registers */
			};
			
			char _reserved_ctxctl[0x1000];
		};
	} ctx[PLIC_CTX_CNT];
};

#define PLIC (*(volatile struct plic_regs*)PLIC_MMIO_BASE)

// INTERNAL FUNCTION DECLARATIONS
//

static void plic_set_source_priority (
	uint_fast32_t srcno, uint_fast32_t level);

static int plic_source_pending(uint_fast32_t srcno);

static void plic_enable_source_for_context (
	uint_fast32_t ctxno, uint_fast32_t srcno);

static void plic_disable_source_for_context (
	uint_fast32_t ctxno, uint_fast32_t srcno);

static void plic_set_context_threshold (
	uint_fast32_t ctxno, uint_fast32_t level);

static uint_fast32_t plic_claim_context_interrupt (
	uint_fast32_t ctxno);

static void plic_complete_context_interrupt (
	uint_fast32_t ctxno, uint_fast32_t srcno);


static void plic_enable_all_sources_for_context(uint_fast32_t ctxno);

static void plic_disable_all_sources_for_context(uint_fast32_t ctxno);

// We currently only support single-hart operation, sending interrupts to S mode
// on hart 0 (context 0). The low-level PLIC functions already understand
// contexts, so we only need to modify the high-level functions (plit_init,
// plic_claim_request, plic_finish_request)to add support for multiple harts.

// EXPORTED FUNCTION DEFINITIONS
// 

void plic_init(void) {
	int i;

	// Disable all sources by setting priority to 0

	for (i = 0; i < PLIC_SRC_CNT; i++)
		plic_set_source_priority(i, 0);
	
	// Route all sources to S mode on hart 0 only

	for (int i = 0; i < PLIC_CTX_CNT; i++)
		plic_disable_all_sources_for_context(i);
	
	plic_enable_all_sources_for_context(CTX(0,1));
}

extern void plic_enable_source(int srcno, int prio) {
	trace("%s(srcno=%d,prio=%d)", __func__, srcno, prio);
	assert (0 < srcno && srcno <= PLIC_SRC_CNT);
	assert (prio > 0);

	plic_set_source_priority(srcno, prio);
}

extern void plic_disable_source(int irqno) {
	if (0 < irqno)
		plic_set_source_priority(irqno, 0);
	else
		debug("plic_disable_irq called with irqno = %d", irqno);
}

extern int plic_claim_interrupt(void) {
	trace("%s()", __func__);
	return plic_claim_context_interrupt(CTX(0,1));
}

extern void plic_finish_interrupt(int irqno) {
	trace("%s(irqno=%d)", __func__, irqno);
	plic_complete_context_interrupt(CTX(0,1), irqno);
}

// INTERNAL FUNCTION DEFINITIONS
//

// static inline void plic_set_source_priority(uint_fast32_t srcno, uint_fast32_t level)
// Inputs: uint_fast32_t srcno - interrupt source number
//         uint_fast32_t level - level to set priority 
// Outputs: None
// Description: Sets the priority of a given source  
// Side Effects: None

static inline void plic_set_source_priority(uint_fast32_t srcno, uint_fast32_t level) {
	// FIXME your code goes here
	if(srcno > 0 && srcno <= PLIC_SRC_CNT){ // set source prioirty if valid source #
		PLIC.priority[srcno] = level; 
	}

	return;
}

// static inline int plic_source_pending(uint_fast32_t srcno)
// Inputs: uint_fast32_t srcno - interrupt source number
// Outputs: Returns a 1 if the source is pending and 0 else
// Description: Checks if the given source has a pending interrupt 
// Side Effects: None

static inline int plic_source_pending(uint_fast32_t srcno) {
	// FIXME your code goes here

	if(srcno == 0 || srcno >= PLIC_SRC_CNT){ // check valid args
		return 0;
	}

	uint32_t mask = 1u << (srcno % 32);
		
	if((PLIC.pending[srcno/32] & mask) == mask){ // check specific bit 
		return 1;
	}
	
	return 0;
}

// static inline void plic_enable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcno)
// Inputs: uint_fast32_t ctxno - The context handling interrupt 
//         uint_fast32_t srcno - Interrupt source number 
// Outputs: None
// Description: Enables a context to handle a source's interrupt  
// Side Effects: None

static inline void plic_enable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcno) {
	// FIXME your code goes here

	if(ctxno >= PLIC_CTX_CNT || srcno <= 0 || srcno >= PLIC_SRC_CNT){ // check valid args
		return;
	}

	uint32_t mask = 1u << (srcno % 32);

	PLIC.enable[ctxno][srcno/32] |= mask; // set specific bit to 1

	return;
}

// static inline void plic_disable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcid)
// Inputs: uint_fast32_t ctxno - The context handling interrupt 
//         uint_fast32_t srcid - Interrupt source number 
// Outputs: None
// Description: Disables a context to handle a source's interrupt  
// Side Effects: None

static inline void plic_disable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcid) {
	// FIXME your code goes here

	if(ctxno >= PLIC_CTX_CNT || srcid <= 0 || srcid >= PLIC_SRC_CNT){ //check valid args
		return;
	}

	uint32_t mask = 1u << (srcid % 32);

	PLIC.enable[ctxno][srcid/32] &= ~mask; // set specific bit to 0

	return;
}

// static inline void plic_set_context_threshold(uint_fast32_t ctxno, uint_fast32_t level)
// Inputs: uint_fast32_t ctxno - The context handling interrupt 
//         uint_fast32_t level - The threshold level 
// Outputs: None
// Description: Sets the threshold to a givel level for a given context  
// Side Effects: None

static inline void plic_set_context_threshold(uint_fast32_t ctxno, uint_fast32_t level) {
	// FIXME your code goes here

	if(ctxno >= PLIC_CTX_CNT){ // check valid ctx
		return;
	}	

	PLIC.ctx[ctxno].threshold = level; // set threshold

	return;
}

// static inline uint_fast32_t plic_claim_context_interrupt(uint_fast32_t ctxno)
// Inputs: uint_fast32_t ctxno - The context handling interrupt 
// Outputs: Returns the interrupt ID of the highest pending interrupt 
// Description: Claims the interrupt with the highest priotiry 
// Side Effects: None

static inline uint_fast32_t plic_claim_context_interrupt(uint_fast32_t ctxno) {
	// FIXME your code goes here

	if(ctxno >= PLIC_CTX_CNT){ // check valid ctx
		return 0;
	}	

	return PLIC.ctx[ctxno].claim; // read from claim reg
}

// static inline void plic_complete_context_interrupt(uint_fast32_t ctxno, uint_fast32_t srcno)
// Inputs: uint_fast32_t ctxno - The context handling interrupt
//         uint_fast32_t srcno - The source number of the handled interrupt  
// Outputs: None 
// Description: Writes the srcno back to the claim register, signaling that the interrupt is complete  
// Side Effects: None

static inline void plic_complete_context_interrupt(uint_fast32_t ctxno, uint_fast32_t srcno) {
	// FIXME your code goes here

	if(ctxno >= PLIC_CTX_CNT || srcno <= 0 || srcno >= PLIC_SRC_CNT){ // check valid args
		return;
	}

	PLIC.ctx[ctxno].claim = srcno; // write back to claim reg
	
	return;
}

// static void plic_enable_all_sources_for_context(uint_fast32_t ctxno)
// Inputs: uint_fast32_t ctxno - The context handling interrupt  
// Outputs: None 
// Description: Enables all of the interrupt sources for a context   
// Side Effects: None

static void plic_enable_all_sources_for_context(uint_fast32_t ctxno) {
	// FIXME your code goes here

	if(ctxno >= PLIC_CTX_CNT){ // check valid ctx
		return;
	}

	for(uint32_t i = 1; i < PLIC_SRC_CNT; i++){ // loop and enable every source
		plic_enable_source_for_context(ctxno, i);
	}

	return;
}

// static void plic_disable_all_sources_for_context(uint_fast32_t ctxno)
// Inputs: uint_fast32_t ctxno - The context handling interrupt  
// Outputs: None 
// Description: Disables all of the interrupt sources for a context   
// Side Effects: None

static void plic_disable_all_sources_for_context(uint_fast32_t ctxno) {
	// FIXME your code goes here

	if(ctxno >= PLIC_CTX_CNT){ // check valid ctx
		return;
	}

	for(uint32_t i = 1; i < PLIC_SRC_CNT; i++){ // loop and enable every source
		plic_disable_source_for_context(ctxno, i);
	}

	return;
}
