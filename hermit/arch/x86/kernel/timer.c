/*
 * Copyright (c) 2010, Stefan Lankes, RWTH Aachen University
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *    * Neither the name of the University nor the names of its contributors
 *      may be used to endorse or promote products derived from this
 *      software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <hermit/stdio.h>
#include <hermit/string.h>
#include <hermit/processor.h>
#include <hermit/time.h>
#include <hermit/tasks.h>
#include <hermit/errno.h>
#include <hermit/spinlock.h>
#include <asm/irq.h>
#include <asm/irqflags.h>
#include <asm/io.h>

/* 
 * This will keep track of how many ticks the system
 * has been running for 
 */
extern volatile uint64_t timer_ticks;
extern uint32_t cpu_freq;
extern int32_t boot_processor;

static int8_t use_tickless = 0;
static uint64_t last_rdtsc = 0;

#if MAX_CORES > 1
static spinlock_irqsave_t ticks_lock = SPINLOCK_IRQSAVE_INIT;
#endif

void start_tickless(void)
{
	use_tickless = 1;
	if (has_rdtscp())
		last_rdtsc = rdtscp(NULL);
	else
		last_rdtsc = rdtsc();
	rmb();
}

void end_tickless(void)
{
	use_tickless = 0;
	last_rdtsc = 0;
}

void check_ticks(void)
{
	if (!use_tickless)
		return;

#if MAX_CORES > 1
	spinlock_irqsave_lock(&ticks_lock);
#endif

	if (has_rdtscp()){
		uint64_t curr_rdtsc = rdtscp(NULL);
		uint64_t diff;

		rmb();
		diff = ((curr_rdtsc - last_rdtsc) * (uint64_t)TIMER_FREQ) / (1000000ULL*(uint64_t)get_cpu_frequency());
		if (diff > 0) {
			timer_ticks += diff;
			last_rdtsc = curr_rdtsc;
			rmb();
		}
	} else {
		uint64_t curr_rdtsc = rdtsc();
		uint64_t diff;

		rmb();
		diff = ((curr_rdtsc - last_rdtsc) * (uint64_t)TIMER_FREQ) / (1000000ULL*(uint64_t)get_cpu_frequency());
		if (diff > 0) {
			timer_ticks += diff;
			last_rdtsc = curr_rdtsc;
			rmb();
		}
	}

#if MAX_CORES > 1
	spinlock_irqsave_unlock(&ticks_lock);
#endif

}

uint64_t get_clock_tick(void)
{
	return timer_ticks;
}

/* 
 * Handles the timer. In this case, it's very simple: We
 * increment the 'timer_ticks' variable every time the
 * timer fires. 
 */
static void timer_handler(struct state *s)
{
#if MAX_CORES > 1
	if (CORE_ID == boot_processor)
#endif
		/* Increment our 'tick counter' */
		timer_ticks++;

#if 0
	/*
	 * Every TIMER_FREQ clocks (approximately 1 second), we will
	 * display a message on the screen
	 */
	if (timer_ticks % TIMER_FREQ == 0) {
		kprintf("One second has passed %d\n", CORE_ID);
	}
#endif
}

int timer_wait(unsigned int ticks)
{
	uint64_t eticks = timer_ticks + ticks;
        
	task_t* curr_task = per_core(current_task);

	if (curr_task->status == TASK_IDLE)
	{
		/*
		 * This will continuously loop until the given time has
		 * been reached
		 */
		while (timer_ticks < eticks) {
			check_workqueues();

			// recheck break condition
			if (timer_ticks >= eticks)
				break;

			HALT;
		}
	} else if (timer_ticks < eticks) {
		check_workqueues();

		if (timer_ticks < eticks) {
			set_timer(eticks);
			reschedule();
		}
	}

	return 0;
}

#define LATCH(f)	((CLOCK_TICK_RATE + f/2) / f)
#define WAIT_SOME_TIME() do { uint64_t start = rdtsc(); mb(); \
			      while(rdtsc() - start < 1000000) ; \
			} while (0)

/* 
 * Sets up the system clock by installing the timer handler
 * into IRQ0 
 */
int timer_init(void)
{
	/* 
	 * Installs 'timer_handler' for the PIC and APIC timer,
	 * only one handler will be later used.
	 */
	irq_install_handler(32, timer_handler);
	irq_install_handler(123, timer_handler);

	if (cpu_freq) // do we need to configure the timer?
		return 0;

	/*
	 * Port 0x43 is for initializing the PIT:
	 *
	 * 0x34 means the following:
	 * 0b...     (step-by-step binary representation)
	 * ...  00  - channel 0
	 * ...  11  - write two values to counter register:
	 *            first low-, then high-byte
	 * ... 010  - mode number 2: "rate generator" / frequency divider
	 * ...   0  - binary counter (the alternative is BCD)
	 */
	outportb(0x43, 0x34);

	WAIT_SOME_TIME();

	/* Port 0x40 is for the counter register of channel 0 */

	outportb(0x40, LATCH(TIMER_FREQ) & 0xFF);   /* low byte  */

	WAIT_SOME_TIME();

	outportb(0x40, LATCH(TIMER_FREQ) >> 8);     /* high byte */

	return 0;
}
