// timer.c - A timer system
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//


#ifdef TIMER_TRACE
#define TRACE
#endif

#ifdef TIMER_DEBUG
#define DEBUG
#endif
#include <stddef.h>
#include "timer.h"
#include "thread.h"
#include "riscv.h"
#include "intr.h"
#include "conf.h"
#include "see.h" // for set_stcmp
#include "misc.h"
#include <stddef.h>


// EXPORTED GLOBAL VARIABLE DEFINITIONS
// 

char timer_initialized = 0;

// INTERNVAL GLOBAL VARIABLE DEFINITIONS
//

static struct alarm * sleep_list;

// INTERNAL FUNCTION DECLARATIONS
//

// EXPORTED FUNCTION DEFINITIONS
//

void timer_init(void) {
    set_stcmp(UINT64_MAX);
    timer_initialized = 1;
}

// void alarm_init(struct alarm * al, const char * name)
// Inputs: struct alarm * al - pointer to alarm struct to initialize
//         const char * name - name of the alarm 
// Outputs: None 
// Description: Does the proper initialization for an alarm   
// Side Effects: None
void alarm_init(struct alarm * al, const char * name) {
    // FIXME your code goes here
    if(name == NULL){
        name = "alarm";
    }

    condition_init(&al->cond, name); // init condition variable
    al->next = NULL; // init the next object
    al->twake = rdtime(); // first time it is used, so init time is twake 
}

// void alarm_sleep(struct alarm * al, unsigned long long tcnt)
// Inputs: struct alarm * al - pointer to alarm struct to initialize
//         unsigned long long tcnt - count until wake up  
// Outputs: None 
// Description: This function puts the current alarm to sleep and inserts it into the alarm list   
// Side Effects: None
void alarm_sleep(struct alarm * al, unsigned long long tcnt) {
    unsigned long long now;
    struct alarm * prev;
    int pie;

    now = rdtime();

    // If the tcnt is so large it wraps around, set it to UINT64_MAX

    if (UINT64_MAX - al->twake < tcnt)
        al->twake = UINT64_MAX;
    else
        al->twake += tcnt;
    
    // If the wake-up time has already passed, return

    if (al->twake < now)
        return;
    
    // FIXME your code goes here

    pie = disable_interrupts(); // disable interrupts so we dont enter ISR

    if(sleep_list == NULL || al->twake < sleep_list->twake){ // if sleep list is empty
        al->next = sleep_list; // set head to al 
        sleep_list = al;
    }
    else{
        struct alarm * cur;
        prev = sleep_list;
        cur = sleep_list->next;
        
        while(cur != NULL && cur->twake <= al->twake){ // iterate through sleep list
            prev = cur;
            cur = cur->next;
        }   

        al->next = cur; // insert node in list 
        prev->next = al;
    }

    if(al == sleep_list){ // if al is first alarm, interrupt threshold is set by al
        set_stcmp(al->twake);
    }

    restore_interrupts(pie); // re-enable prev interrupt state 

    condition_wait(&al->cond); // set current thread to sleep
}

// Resets the alarm so that the next sleep increment is relative to the time
// alarm_reset is called.

void alarm_reset(struct alarm * al) {
    al->twake = rdtime();
}

void alarm_sleep_sec(struct alarm * al, unsigned int sec) {
    alarm_sleep(al, sec * TIMER_FREQ);
}

void alarm_sleep_ms(struct alarm * al, unsigned long ms) {
    alarm_sleep(al, ms * (TIMER_FREQ / 1000));
}

void alarm_sleep_us(struct alarm * al, unsigned long us) {
    alarm_sleep(al, us * (TIMER_FREQ / 1000 / 1000));
}

void sleep_sec(unsigned int sec) {
    sleep_ms(1000UL * sec);
}

void sleep_ms(unsigned long ms) {
    sleep_us(1000UL * ms);
}

void sleep_us(unsigned long us) {
    struct alarm al;

    alarm_init(&al, "sleep");
    alarm_sleep_us(&al, us);
}

// void handle_timer_interrupt(void)
// Inputs: None 
// Outputs: None 
// Description: This function wakes all threads waiting on expired alarms, and removed expired
//              alarms from the alarm list 
// Side Effects: None
void handle_timer_interrupt(void) {
    struct alarm * head = sleep_list;
    struct alarm * next;
    uint64_t now;

    now = rdtime();

    trace("[%lu] %s()", now, __func__);
    debug("[%lu] mtcmp = %lu", now, rdtime());

    // FIXME your code goes here

    uint64_t new_timer = now + 20 * (TIMER_FREQ / 1000);

    while(head != NULL && head->twake <= now){ // while valid pointer and past wake up time
        next = head->next; // remove node by simply incrementing pointer 
        condition_broadcast(&head->cond); // wake threads waiting on head->twake
        sleep_list = next; // next node
        head = next;
    }

    if(sleep_list != NULL && sleep_list->twake <= new_timer){ // update threshold to next twake if twake is < periodic timer 
        set_stcmp(sleep_list->twake);
    }
    else{
        set_stcmp(new_timer); // set the next timer to 20ms from now if that is the soonest timer
    }
}