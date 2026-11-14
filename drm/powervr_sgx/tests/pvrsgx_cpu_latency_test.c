// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/* Included from pvrsgx_job.c for its static CPU latency helpers. */

#include <kunit/test.h>

/*
 * The CPU latency limit: taken with the first job, kept while any is in
 * flight, released only by the delayed work after the last one.
 */
static void job_test_cpu_latency(struct kunit *test)
{
	struct pvrsgx_cpu_latency *cl = kunit_kzalloc(test, sizeof(*cl), GFP_KERNEL);

	if (!IS_ENABLED(CONFIG_CPU_IDLE))
		kunit_skip(test, "no CPU latency QoS without CONFIG_CPU_IDLE");
	KUNIT_ASSERT_NOT_NULL(test, cl);
	cpu_latency_init(cl, 50);
	cpu_latency_get(cl);
	KUNIT_EXPECT_TRUE(test, cpu_latency_qos_request_active(&cl->req));
	KUNIT_EXPECT_LE(test, cpu_latency_qos_limit(), 50);
	/* one of two freed: still held */
	cpu_latency_get(cl);
	cpu_latency_put(cl, 1);
	flush_delayed_work(&cl->release);
	KUNIT_EXPECT_TRUE(test, cpu_latency_qos_request_active(&cl->req));
	/* the last one: held until the delayed release runs */
	cpu_latency_put(cl, 1);
	KUNIT_EXPECT_TRUE(test, cpu_latency_qos_request_active(&cl->req));
	flush_delayed_work(&cl->release);
	KUNIT_EXPECT_FALSE(test, cpu_latency_qos_request_active(&cl->req));
	/* a submit during a pending release keeps the limit */
	cpu_latency_get(cl);
	cpu_latency_put(cl, 1);
	cpu_latency_get(cl);
	flush_delayed_work(&cl->release);
	KUNIT_EXPECT_TRUE(test, cpu_latency_qos_request_active(&cl->req));
	cpu_latency_put(cl, 1);
	cpu_latency_fini(cl);
	KUNIT_EXPECT_FALSE(test, cpu_latency_qos_request_active(&cl->req));
}

/* A core without a limit never adds a request. */
static void job_test_no_cpu_latency(struct kunit *test)
{
	struct pvrsgx_cpu_latency *cl = kunit_kzalloc(test, sizeof(*cl), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, cl);
	cpu_latency_init(cl, 0);
	cpu_latency_get(cl);
	KUNIT_EXPECT_FALSE(test, cpu_latency_qos_request_active(&cl->req));
	cpu_latency_put(cl, 1);
	cpu_latency_fini(cl);
}

static struct kunit_case pvrsgx_cpu_latency_test_cases[] = {
	KUNIT_CASE(job_test_cpu_latency),
	KUNIT_CASE(job_test_no_cpu_latency),
	{}
};

static struct kunit_suite pvrsgx_cpu_latency_test_suite = {
	.name = "powervr_sgx_cpu_latency",
	.test_cases = pvrsgx_cpu_latency_test_cases,
};

kunit_test_suite(pvrsgx_cpu_latency_test_suite);
