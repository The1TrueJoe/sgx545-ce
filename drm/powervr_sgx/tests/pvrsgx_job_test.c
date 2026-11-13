// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * KUnit tests of the job path's pure parts, included from pvrsgx_job.c: which
 * earlier jobs of a submit a wait depends on, and how many completions a
 * counting microkernel reports before a job's.
 */

#include <kunit/test.h>

static void job_test_intra_timeline(struct kunit *test)
{
	struct pvrsgx_signal s0[] = { { .handle = 5, .point = 3 } };
	struct pvrsgx_signal s1[] = { { .handle = 6, .point = 1 } };
	struct job_args prev[] = {
		{ .signals = s0, .nsignals = 1 },
		{ .signals = s1, .nsignals = 1 },
	};
	u32 intra[2];
	struct job_args ja = { .intra = intra };

	/* A later point than any signalled: not this submit's to satisfy. */
	KUNIT_EXPECT_FALSE(test, intra_waits(&ja, prev, 2, 5, 4));
	KUNIT_EXPECT_EQ(test, ja.nintra, 0);
	/* An earlier point: the job that signals past it, not the other handle's. */
	KUNIT_EXPECT_TRUE(test, intra_waits(&ja, prev, 2, 5, 2));
	KUNIT_EXPECT_EQ(test, ja.nintra, 1);
	KUNIT_EXPECT_EQ(test, ja.intra[0], 0);
	/* Waited on again: listed once. */
	KUNIT_EXPECT_TRUE(test, intra_waits(&ja, prev, 2, 5, 3));
	KUNIT_EXPECT_EQ(test, ja.nintra, 1);
}

static void job_test_intra_every_signaller(struct kunit *test)
{
	struct pvrsgx_signal s0[] = { { .handle = 7, .point = 1 } };
	struct pvrsgx_signal s1[] = { { .handle = 8 }, { .handle = 7, .point = 2 } };
	struct job_args prev[] = {
		{ .signals = s0, .nsignals = 1 },
		{ .signals = s1, .nsignals = 2 },
	};
	u32 intra[2];
	struct job_args ja = { .intra = intra };

	/* Point 0 (a binary syncobj, or a timeline's latest): every signaller. */
	KUNIT_EXPECT_TRUE(test, intra_waits(&ja, prev, 2, 7, 0));
	KUNIT_EXPECT_EQ(test, ja.nintra, 2);
	KUNIT_EXPECT_EQ(test, ja.intra[0], 0);
	KUNIT_EXPECT_EQ(test, ja.intra[1], 1);
}

static void job_test_completions(struct kunit *test)
{
	struct pvrsgx_device *sdev = kunit_kzalloc(test, sizeof(*sdev), GFP_KERNEL);
	struct pvrsgx_job job = { .noops_before = 10 };

	KUNIT_ASSERT_NOT_NULL(test, sdev);
	sdev->noops_seen = 10;
	/* No no-op ahead of it: its own completion. */
	KUNIT_EXPECT_EQ(test, job_completions(sdev, &job), 1);
	/* Two split invalidates sent ahead of it: they complete first. */
	job.noops_before = 12;
	KUNIT_EXPECT_EQ(test, job_completions(sdev, &job), 3);
	/* Across the counter's wrap. */
	sdev->noops_seen = 0xffffffff;
	job.noops_before = 1;
	KUNIT_EXPECT_EQ(test, job_completions(sdev, &job), 3);
}

/* fdinfo's engine time: render and transfer jobs, from send to end; not NULL jobs. */
static void job_test_account(struct kunit *test)
{
	struct pvrsgx_context *ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	struct pvrsgx_job job = { .start_ns = 100 };

	KUNIT_ASSERT_NOT_NULL(test, ctx);
	ctx->usage = pvrsgx_usage_new();
	KUNIT_ASSERT_NOT_NULL(test, ctx->usage);
	job.ctx = ctx;

	job.type = DRM_POWERVR_SGX_JOB_TYPE_RENDER;
	job_account(&job, 350);
	KUNIT_EXPECT_EQ(test, atomic64_read(&ctx->usage->busy_ns), 250);
	job.type = DRM_POWERVR_SGX_JOB_TYPE_TRANSFER;
	job_account(&job, 150);
	KUNIT_EXPECT_EQ(test, atomic64_read(&ctx->usage->busy_ns), 300);
	job.type = DRM_POWERVR_SGX_JOB_TYPE_NULL;
	job_account(&job, 1000);
	KUNIT_EXPECT_EQ(test, atomic64_read(&ctx->usage->busy_ns), 300);
	/* A clock read before the send adds nothing, rather than wrapping. */
	job.type = DRM_POWERVR_SGX_JOB_TYPE_RENDER;
	job_account(&job, 50);
	KUNIT_EXPECT_EQ(test, atomic64_read(&ctx->usage->busy_ns), 300);
	pvrsgx_usage_put(ctx->usage);
}

static struct kunit_case pvrsgx_job_test_cases[] = {
	KUNIT_CASE(job_test_intra_timeline),
	KUNIT_CASE(job_test_intra_every_signaller),
	KUNIT_CASE(job_test_completions),
	KUNIT_CASE(job_test_account),
	{}
};

static struct kunit_suite pvrsgx_job_test_suite = {
	.name = "powervr_sgx_job",
	.test_cases = pvrsgx_job_test_cases,
};

kunit_test_suite(pvrsgx_job_test_suite);
