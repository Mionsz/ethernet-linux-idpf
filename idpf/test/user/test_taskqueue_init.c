#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

struct taskqueue { int unused; };
struct task { bool initialized; };
struct timeout_task { bool initialized; };
struct idpf_adapter {
	struct taskqueue *init_wq, *serv_wq, *mbx_wq, *stats_wq, *vc_event_wq;
	struct timeout_task init_task, vc_event_task;
	struct task mbx_task, stats_deferred;
	int serv_task, stats_task, mbx_poll_task;
	struct { int subsystem_device_id; } hw;
};
static int fail_at, attempts, live, failures;

static int
idpf_alloc_taskqueue(struct idpf_adapter *adapter, struct taskqueue **queue,
    const char *suffix)
{
	(void)adapter;
	(void)suffix;
	if (++attempts == fail_at)
		return (ENOMEM);
	*queue = calloc(1, sizeof(**queue));
	if (*queue == NULL)
		return (ENOMEM);
	live++;
	return (0);
}

static void
test_drain(bool initialized)
{
	if (!initialized) {
		fprintf(stderr, "FAIL uninitialized task drained (fault %d)\n", fail_at);
		failures++;
	}
}

static void
taskqueue_free(struct taskqueue *queue)
{
	free(queue);
	live--;
}

#define callout_init(callout, flags) ((void)0)
#define TIMEOUT_TASK_INIT(queue, task, priority, fn, ctx) ((task)->initialized = true)
#define TASK_INIT(task, priority, fn, ctx) ((task)->initialized = true)
#define taskqueue_drain_timeout(queue, task) test_drain((task)->initialized)
#define taskqueue_drain(queue, task) test_drain((task)->initialized)
#define IS_SILICON_DEVICE(device) (device)
#define device_printf(dev, ...) ((void)0)
#include "tasks_under_test.inc"

int
main(void)
{
	for (int silicon = 0; silicon < 2; silicon++) {
		for (fail_at = 1; fail_at <= 6; fail_at++) {
			struct idpf_adapter adapter = { 0 };
			adapter.hw.subsystem_device_id = silicon;
			attempts = 0;
			if (idpf_alloc_taskqueues(&adapter) == 0)
				idpf_free_taskqueues(&adapter);
			if (live != 0)
				failures++;
		}
	}
	printf("Taskqueue allocation/unwind: %d failures\n", failures);
	return (failures != 0);
}