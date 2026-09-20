/**
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
/* Keep checks active in release test builds as well. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <limits.h>
#include <stdio.h>

#include "../bio_weight.h"

static void
test_config(void)
{
	const char *invalid[] = {
	    "",        "a",        "=1",       "a=",          "a=0",
	    "a=-1",    "a=+1",     "a=1.5",    "a=65536",     "a=999999999999999999999",
	    "a=1,",    ",a=1",     "a=1,,b=2", "a=1,b=2,a=3", "a=1,a=1",
	    "a=1 b=2", " a=1",     "a =1",     "a= 1",        "a=1\n",
	    "a=1,b=0", "a=1,b=2=3"};
	unsigned int weight;
	size_t       i;

	assert(bio_weight_get(NULL, "a", &weight) == 0 && weight == 1);
	assert(bio_weight_get("a=1,aa=2,b=65535", "aa", &weight) == 0 && weight == 2);
	assert(bio_weight_get("a=1,aa=2,b=65535", "b", &weight) == 0 && weight == 65535);
	assert(bio_weight_get("a=1,aa=2,b=65535", "c", &weight) == 0 && weight == 1);
	assert(bio_weight_get("Nvme_0_1n1=2", "Nvme_0_1n1", &weight) == 0 && weight == 2);
	for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
		assert(bio_weight_get(invalid[i], "a", &weight) == -1);
}

static void
assign(unsigned int *counts, const unsigned int *weights, unsigned int devices,
       unsigned int targets)
{
	unsigned int t, i, best;

	for (t = 0; t < targets; t++) {
		best = 0;
		for (i = 1; i < devices; i++) {
			if (bio_weight_less(counts[i], weights[i], counts[best], weights[best]))
				best = i;
		}
		counts[best]++;
	}
}

static void
test_distribution(void)
{
	unsigned int       counts[]       = {0, 0, 0};
	const unsigned int weights[]      = {1, 1, 2};
	const unsigned int equal[]        = {1, 1, 1};
	unsigned int       legacy[]       = {0, 0, 0};
	unsigned int       scaled[]       = {0, 0, 0};
	const unsigned int proportional[] = {100, 100, 200};
	unsigned int       i, t;

	assign(counts, weights, 3, 8);
	assert(counts[0] == 2 && counts[1] == 2 && counts[2] == 4);
	assign(counts, weights, 3, 792);
	assert(counts[0] == 200 && counts[1] == 200 && counts[2] == 400);
	assign(scaled, proportional, 3, 800);
	assert(memcmp(counts, scaled, sizeof(counts)) == 0);

	/* Every prefix of a unit-weight assignment matches legacy round-robin. */
	for (t = 0; t < 100; t++) {
		assign(legacy, equal, 3, 1);
		for (i = 0; i < 3; i++)
			assert(legacy[i] == (t + 1) / 3 + (i < (t + 1) % 3));
	}
	/* Seed from pre-existing mappings: only add targets, never move them. */
	counts[0] = 4;
	counts[1] = 4;
	counts[2] = 0;
	assign(counts, weights, 3, 8);
	assert(counts[0] == 4 && counts[1] == 4 && counts[2] == 8);
	assert(!bio_weight_less(2, 1, 4, 2));
	assert(bio_weight_less(INT_MAX, 65535, INT_MAX, 65534));
	assert(!bio_weight_less(INT_MAX, 65534, INT_MAX, 65535));
}

static void
test_two_devices(void)
{
	unsigned int       baseline[]   = {0, 0};
	unsigned int       weighted[]   = {0, 0};
	unsigned int       reversed[]   = {0, 0};
	const unsigned int equal[]      = {1, 1};
	const unsigned int fast_first[] = {2, 1};
	const unsigned int slow_first[] = {1, 2};

	/* Six targets give exact ratios for both test configurations. */
	assign(baseline, equal, 2, 6);
	assert(baseline[0] == 3 && baseline[1] == 3);
	assign(weighted, fast_first, 2, 6);
	assert(weighted[0] == 4 && weighted[1] == 2);
	assign(reversed, slow_first, 2, 6);
	assert(reversed[0] == 2 && reversed[1] == 4);

	/* Eight targets cannot represent 2:1 exactly; document the rounding. */
	assign(weighted, fast_first, 2, 2);
	assert(weighted[0] == 5 && weighted[1] == 3);
	assign(weighted, fast_first, 2, 592);
	assert(weighted[0] == 400 && weighted[1] == 200);
}

static void
test_capacity(void)
{
	/* Data-only, combined roles, system reservations, and corrupt occupancy. */
	assert(bio_weight_fits(63, 1, 64));
	assert(!bio_weight_fits(64, 1, 64));
	assert(bio_weight_fits(61, 3, 64));
	assert(!bio_weight_fits(62, 3, 64));
	assert(!bio_weight_fits(63, 2, 64));
	assert(!bio_weight_fits(65, 1, 64));
	assert(!bio_weight_fits(UINT_MAX, 1, 64));
	assert(!bio_weight_fits(0, UINT_MAX, 64));
	assert(!bio_weight_fits(0, 0, 64));
}

int
main(void)
{
	test_config();
	test_capacity();
	test_distribution();
	test_two_devices();
	puts("BIO weight tests passed");
	return 0;
}
