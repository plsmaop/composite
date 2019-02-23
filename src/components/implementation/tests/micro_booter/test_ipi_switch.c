#include <stdint.h>

#include "micro_booter.h"

extern int _expect_llu(int predicate, char *str, long long unsigned a, long long unsigned b, char *errcmp, char *testname, char *file, int line);
extern int _expect_ll(int predicate, char *str, long long a, long long b, char *errcmp, char *testname, char *file, int line);
extern void clear_sched(int* rcvd, thdid_t* tid, int* blocked, cycles_t* cycles, tcap_time_t* thd_timeout);

/* Test RCV 1: Close Loop at lower priority => Measure Thread Switching + IPI */

#define TEST_RCV_CORE 0
#define TEST_SND_CORE 1
#define TEST_IPI_ITERS 1000000

static volatile asndcap_t asnd[NUM_CPU] = { 0 };
static volatile arcvcap_t rcv[NUM_CPU] = { 0 };
static volatile thdcap_t  thd[NUM_CPU] = { 0 };
static volatile thdid_t   tid[NUM_CPU] = { 0 };
static volatile int       blkd[NUM_CPU] = { 0 };

static volatile unsigned long long total_rcvd[NUM_CPU] = { 0 };
static volatile unsigned long long total_sent[NUM_CPU] = { 0 };

#define MAX_THRS 4
#define MIN_THRS 1

static volatile thdcap_t  spinner_thd[NUM_CPU] = { 0 };
static volatile cycles_t  global_time[2] = { 0 };

static volatile int       done_test = 0;
static volatile int       ret_enable = 1;
static volatile int       pending_rcv = 0;
static volatile int       ready = 0;


static void
test_rcv(arcvcap_t r)
{
	int pending = 0, rcvd = 0;

	pending = cos_rcv(r, RCV_ALL_PENDING, &rcvd);
	assert(pending == 0);

	total_rcvd[cos_cpuid()] += rcvd;
}

static void
test_asnd(asndcap_t s)
{
	int ret = 0;

	ret = cos_asnd(s, 1);
	assert(ret == 0 || ret == -EBUSY);
    ready = 1;
	if (!ret) total_sent[cos_cpuid()]++;
}

static void
test_sync_asnd(void)
{
	int i;

	while (!asnd[TEST_SND_CORE]) ;
	while (!asnd[TEST_RCV_CORE]) ;
}

// RCV TEST #1

static void
rcv_spinner(void *d)
{
    while (!done_test) {
        while(ready) ;
        rdtscll(global_time[0]);
    }

    while (1) cos_thd_switch(BOOT_CAPTBL_SELF_INITTHD_CPU_BASE);
}

static void
test_rcv_1(arcvcap_t r)
{
	int pending = 0, rcvd = 0;

	pending = cos_rcv(r, RCV_ALL_PENDING, &rcvd);
    rdtscll(global_time[1]);
    ready = 1;
    assert(pending == 0);

	total_rcvd[cos_cpuid()] += rcvd;
}

static void
test_rcv_fn(void *d)
{
	arcvcap_t r = rcv[cos_cpuid()];
	asndcap_t s = asnd[cos_cpuid()];

	while (1) {
		test_rcv_1(r);
		test_asnd(s);
	}
}

static void
test_asnd_fn(void *d)
{
    cycles_t tot = 0, mask = 0, time = 0,wc = 0, bc = 0;
    int iters = 0;

    arcvcap_t r = rcv[cos_cpuid()];
    asndcap_t s = asnd[cos_cpuid()];

    for(iters = 0; iters < TEST_IPI_ITERS; iters++) {

        test_asnd(s);
        test_rcv(r);

        time = (global_time[1] - global_time[0]);
        mask = (time >> (sizeof(int) * CHAR_BIT - 1));
        time = (time + mask) ^ mask;

        ready = 0;

        tot += time;
        if (time > wc) {
            wc = time;
        }
        if (time < bc || bc == 0) {
            bc = time;
        }
    }

#if defined(PERF)
    PRINTC("IPI Switch on RCV THD: %llu, BC: %llu, WC: %llu\n", tot/TEST_IPI_ITERS, bc, wc);
#endif
    EXPECT_LLU_LT((long long unsigned)((tot / TEST_IPI_ITERS) * MAX_THRS), wc , "Test IPI Switch MAX");
    EXPECT_LLU_LT(bc * (unsigned) MIN_THRS, (long long unsigned)(tot / TEST_IPI_ITERS), "Test IPI Switch MIN");
    done_test = 1;
    while(1) test_rcv(r);
}

static void
test_sched_loop(void)
{
    int blocked, rcvd, pending, ret;
    cycles_t cycles;
    tcap_time_t timeout, thd_timeout;
    thdid_t thdid;

    // Clear Scheduler
    clear_sched(&rcvd, &thdid, &blocked, &cycles, &thd_timeout);

    while (1) {

        if(cos_cpuid() == TEST_RCV_CORE) {
            do {
                ret = cos_switch(spinner_thd[cos_cpuid()], BOOT_CAPTBL_SELF_INITTCAP_CPU_BASE, TCAP_PRIO_MAX + 2, 0, 0, 0);
            } while (ret == -EAGAIN);
        }
        while ((pending = cos_sched_rcv(BOOT_CAPTBL_SELF_INITRCV_CPU_BASE, RCV_ALL_PENDING, 0,
                        &rcvd, &thdid, &blocked, &cycles, &thd_timeout)) >= 0) {
            if (!thdid)
                goto done;
            assert(thdid == tid[cos_cpuid()]);
            blkd[cos_cpuid()] = blocked;
done:
            if (!pending) break;
        }

        if (blkd[cos_cpuid()] && done_test) return;

        if(cos_cpuid() == TEST_SND_CORE && blkd[cos_cpuid()]) continue;

        if(cos_cpuid() == TEST_SND_CORE) {
            do {
                ret = cos_switch(thd[cos_cpuid()], BOOT_CAPTBL_SELF_INITTCAP_CPU_BASE, 0, 0, 0, 0);
            } while (ret == -EAGAIN);
        }
    }
}

void
test_ipi_switch(void)
{
    arcvcap_t r = 0;
    asndcap_t s = 0;
    thdcap_t t = 0;
    tcap_t  tcc = 0;


    if (cos_cpuid() == TEST_RCV_CORE) {

        // Test RCV 1: Close Loop at lower priority => Measure Thread Switching + IPI

        tcc = cos_tcap_alloc(&booter_info);
        if (EXPECT_LL_LT(1, tcc, "Allocation"))
            return;

        t = cos_thd_alloc(&booter_info, booter_info.comp_cap, test_rcv_fn, NULL);
        if (EXPECT_LL_LT(1, t, "Allocation"))
            return;

        r = cos_arcv_alloc(&booter_info, t, tcc, booter_info.comp_cap, BOOT_CAPTBL_SELF_INITRCV_CPU_BASE);
        if (EXPECT_LL_LT(1, r, "Allocation"))
            return;

        cos_tcap_transfer(r, BOOT_CAPTBL_SELF_INITTCAP_CPU_BASE, TCAP_RES_INF, TCAP_PRIO_MAX);

        thd[cos_cpuid()] = t;
        tid[cos_cpuid()] = cos_introspect(&booter_info, t, THD_GET_TID);
        rcv[cos_cpuid()] = r;
        while(!rcv[TEST_SND_CORE]) ;

        t = cos_thd_alloc(&booter_info, booter_info.comp_cap, rcv_spinner, NULL);
        if (EXPECT_LL_LT(1, t, "Allocation"))
            return;

        spinner_thd[cos_cpuid()] = t;

        s = cos_asnd_alloc(&booter_info, rcv[TEST_SND_CORE], booter_info.captbl_cap);
        if (EXPECT_LL_LT(1, s, "Allocation"))
            return;
        asnd[cos_cpuid()] = s;
        test_sync_asnd();
        test_sched_loop();

    } else {

        if (cos_cpuid() != TEST_SND_CORE) return;

        // Test RCV1: Corresponding Send

        t = cos_thd_alloc(&booter_info, booter_info.comp_cap, test_asnd_fn, NULL);
        if (EXPECT_LL_LT(1, t, "Allocation"))
            return;

        r = cos_arcv_alloc(&booter_info, t, BOOT_CAPTBL_SELF_INITTCAP_CPU_BASE, booter_info.comp_cap, BOOT_CAPTBL_SELF_INITRCV_CPU_BASE);
        if (EXPECT_LL_LT(1, r, "Allocation"))
            return;

        thd[cos_cpuid()] = t;
        tid[cos_cpuid()] = cos_introspect(&booter_info, t, THD_GET_TID);
        rcv[cos_cpuid()] = r;
        while(!rcv[TEST_RCV_CORE]) ;

        s = cos_asnd_alloc(&booter_info, rcv[TEST_RCV_CORE], booter_info.captbl_cap);
        if (EXPECT_LL_LT(1, s, "Allocation"))
            return;
        asnd[cos_cpuid()] = s;

        test_sync_asnd();
        test_sched_loop();
    }
}
