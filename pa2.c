/**********************************************************************
 * Copyright (c) 2020
 *  Sang-Hoon Kim <sanghoonkim@ajou.ac.kr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTIABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 **********************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include "types.h"
#include "list_head.h"
#include "vm.h"

/**
 * Ready queue of the system
 */
extern struct list_head processes;

/**
 * Currently running process
 */
extern struct process *current;

/**
 * Page Table Base Register that MMU will walk through for address translation
 */
extern struct pagetable *ptbr;


/**
 * The number of mappings for each page frame. Can be used to determine how
 * many processes are using the page frames.
 */
extern unsigned int mapcounts[];


/**
 * alloc_page(@vpn, @rw)
 *
 * DESCRIPTION
 *   Allocate a page frame that is not allocated to any process, and map it
 *   to @vpn. When the system has multiple free pages, this function should
 *   allocate the page frame with the **smallest pfn**.
 *   You may construct the page table of the @current process. When the page
 *   is allocated with RW_WRITE flag, the page may be later accessed for writes.
 *   However, the pages populated with RW_READ only should not be accessed with
 *   RW_WRITE accesses.
 *
 * RETURN
 *   Return allocated page frame number.
 *   Return -1 if all page frames are allocated.
 */
unsigned int alloc_page(unsigned int vpn, unsigned int rw)
{
	// 
	unsigned int pd_index = vpn/NR_PTES_PER_PAGE;
	unsigned int pte_index = vpn%NR_PTES_PER_PAGE;
	
	// is page table valid?
	// page table doesn't exist!
	if(current->pagetable.outer_ptes[pd_index] == NULL){
		struct pte_directory *new_pd = malloc(sizeof(struct pte_directory));
		current->pagetable.outer_ptes[pd_index]=new_pd;
		//printf("current page table is empty. fill the page with next level page table(16 ptes).\n");
	}
	
	int i=0;
	for(i=0; i<NR_PAGEFRAMES; i++){
		// find the empty page frame of smallest #
		if(mapcounts[i]==0){
			// mapping vpn-pfn
			mapcounts[i]++;

			// record on pte
			current->pagetable.outer_ptes[pd_index]->ptes[pte_index].valid = true;
			if(rw==(RW_READ|RW_WRITE))
				current->pagetable.outer_ptes[pd_index]->ptes[pte_index].writable = true;
			else
				current->pagetable.outer_ptes[pd_index]->ptes[pte_index].writable = false;
			current->pagetable.outer_ptes[pd_index]->ptes[pte_index].pfn = i;
			current->pagetable.outer_ptes[pd_index]->ptes[pte_index].private = rw;

			return i;
		}
	}	
	return -1;
}


/**
 * free_page(@vpn)
 *
 * DESCRIPTION
 *   Deallocate the page from the current processor. Make sure that the fields
 *   for the corresponding PTE (valid, writable, pfn) is set @false or 0.
 *   Also, consider carefully for the case when a page is shared by two processes,
 *   and one process is to free the page.
 */
void free_page(unsigned int vpn)
{
	unsigned int pd_index = vpn/NR_PTES_PER_PAGE;
	unsigned int pte_index = vpn%NR_PTES_PER_PAGE;

	unsigned int pfn = current->pagetable.outer_ptes[pd_index]->ptes[pte_index].pfn;
	int process_cnt = mapcounts[pfn];	// the number of processes that refer to page frame #pfn
	
	// nothing to free
	if(process_cnt==0){
		return;
	}
	// something to free
	else{
		mapcounts[pfn]--;
		
		current->pagetable.outer_ptes[pd_index]->ptes[pte_index].valid=false;
		current->pagetable.outer_ptes[pd_index]->ptes[pte_index].writable=false;
		current->pagetable.outer_ptes[pd_index]->ptes[pte_index].pfn=0;
		current->pagetable.outer_ptes[pd_index]->ptes[pte_index].private=0;
	}

	// if second-level page table is empty, then free that table
	int i;
	// find valid pte
	for(i=0; i<NR_PTES_PER_PAGE && !(current->pagetable.outer_ptes[pd_index]->ptes[i].valid); i++);
	// all ptes are invalid, then free that page table
	if(i==NR_PTES_PER_PAGE){
		struct pte_directory *p_pte_dir = current->pagetable.outer_ptes[pd_index];
		current->pagetable.outer_ptes[pd_index]=NULL;
		free(p_pte_dir);
	}
	
}

/**
 * handle_page_fault()
 *
 * DESCRIPTION
 *   Handle the page fault for accessing @vpn for @rw. This function is called
 *   by the framework when the __translate() for @vpn fails. This implies;
 *   0. page directory is invalid
 *   1. pte is invalid
 *   2. pte is not writable but @rw is for write
 *   This function should identify the situation, and do the copy-on-write if
 *   necessary.
 *
 * RETURN
 *   @true on successful fault handling
 *   @false otherwise
 */
bool handle_page_fault(unsigned int vpn, unsigned int rw)
{
	int pd_index = vpn / NR_PTES_PER_PAGE;
	int pte_index = vpn % NR_PTES_PER_PAGE;

	// if	1. page table is invalid
	//		2. page directory is invalid
	//		3. pte is invalid
	// then, return false
	struct pagetable *pt=ptbr;
	struct pte_directory *pd;
	struct pte *pte;

	// case 1
	if(!pt)
		return false;

	pd=pt->outer_ptes[pd_index];
	
	// case 2
	if(!pd)
		return false;

	pte = &pt->outer_ptes[pd_index]->ptes[pte_index];
	// case 3
	if(!pte->valid)
		return false;

	// if pte is valid,
	// then check	1. whether the original access mode (private) of the pte is r or rw
	//				2. if original access mode is rw, then consider copy on write policy
	//					change pfn and pte information(access mode and pfn...)
	
	// case 1 - originally only readable
	if(pte->private == RW_READ)
		return false;
	
	// case 2
	int pfn = pte->pfn;
	// check the # of processes that refer to PA(mapcounts)
	int number_of_process = mapcounts[pfn];
	
	// only one process refer to PA[pfn]
	if(number_of_process==1){
		pte->writable=true;
		return true;
	}
	// many
	else if(number_of_process>1) {
		// new allocation
		int i;
		for(i=0; i<NR_PAGEFRAMES; i++){
			if(mapcounts[i]==0){
				mapcounts[i]++;

				pte->writable = true;
				pte->pfn = i;
				
				return true;
			}
		}
	}

	return false;
}


/**
 * switch_process()
 *
 * DESCRIPTION
 *   If there is a process with @pid in @processes, switch to the process;
 *   @current should be replaced to the requested process. Make sure that
 *   @ptbr is set properly.
 *
 *   If there is no process with @pid in the @processes list, fork a process
 *   from the @current. This implies the forked child process should have
 *   the identical page table entry 'values' to its parent's (i.e., @current)
 *   page table. 
 *   To implement the copy-on-write feature, you should manipulate the writable
 *   bit in PTE and mapcounts for shared pages. You can use pte->private to
 *   remember whether the PTE was originally writable or not.
 */
void switch_process(unsigned int pid)
{
	/** YOU CAN CHANGE EVERYTING (INCLUDING THE EXAMPLE) IN THIS FUNCTION **/

	struct process *p;
	int flag = 0;
	/* This example shows to iterate @processes to find a process with @pid */
	list_for_each_entry(p, &processes, list) {
		//if(processes==p->list && flag>0){
		//	break;
		//}
		// 만약 찾는 게 존재하지 않는다면 무한루프 돌 듯. 그러니 이따가 break 하는 거 추가하기
		if (p->pid == pid) {
			/* FOUND */
			// chage current process and processes list head
			current = p;
			ptbr = &p->pagetable;
			processes.next = p->list.next;
			processes.prev = p->list.prev;
			return;
		}
	}

	// the process that i want doesn't exist
	// fork
	p = malloc(sizeof(*p));		/* This example shows to create a process, */
	
	p->pid = pid;
	
	int i, j;
	struct pagetable *old_pt;
	struct pte_directory *old_pd;
	struct pte *old_pte;
	
	old_pt=&current->pagetable;
	struct pagetable *newpt = &p->pagetable;

	for(i=0; i<NR_PTES_PER_PAGE; i++){
		old_pd = old_pt->outer_ptes[i];
		if(old_pd != NULL){
			struct pte_directory *newpd = malloc(sizeof(struct pte_directory));
			newpt->outer_ptes[i] = newpd;

			for(j=0; j<NR_PTES_PER_PAGE; j++){
				old_pte = &old_pd->ptes[j];
				if(old_pte->valid){
					newpd->ptes[j].valid = true;
					
					old_pte->writable=false;
					newpd->ptes[j].writable=false;
					
					newpd->ptes[j].pfn = old_pte->pfn;
					mapcounts[old_pte->pfn]++;
					newpd->ptes[j].private = old_pte->private;
				}
			}

		}

	}


	current = p;
	ptbr = &p->pagetable;
	INIT_LIST_HEAD(&p->list);	/* initialize list_head, */
	list_add_tail(&p->list, &processes);
								/* and add it to the @processes list */
}

