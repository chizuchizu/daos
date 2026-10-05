/**
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
#define D_LOGFAC DD_FAC(tests)

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "place_obj_common.h"
#include <daos/object.h>

bool g_verbose;

/* Export/import a real map, rather than writing private in-memory structures. */
static void
weighted_maps(int nodes, int engines, int targets, int fdom, const char *spec,
	      struct pool_map **pool, struct pl_map **placement)
{
	struct pool_map        *original;
	struct pl_map          *original_pl;
	struct pool_buf        *buf;
	struct pl_map_init_attr attr = {.ia_type = PL_TYPE_JUMP_MAP};

	gen_maps_adv(1, nodes, engines, targets, fdom, &original, &original_pl);
	assert_success(pool_buf_extract(original, &buf));
	assert_success(pool_buf_set_rank_weights(buf, spec));
	assert_success(pool_map_create(buf, 1, pool));
	attr.ia_jump_map.domain = fdom;
	assert_success(pl_map_create(*pool, &attr, placement));
	pool_buf_free(buf);
	free_pool_and_placement_map(original, original_pl);
}

static void
parser_rejects_invalid_and_is_atomic(void **state)
{
	const char      *bad[] = {"",
				  "0=0",
				  "0=256",
				  "0=-1",
				  "0=+1",
				  "0=1,",
				  ",0=1",
				  "0=1,,1=2",
				  "0=1,0=2",
				  "0=1,00=2",
				  "2=1",
				  "0 =1",
				  "0= 1",
				  "0=1x",
				  "0=1,1=99999999999999999",
				  "4294967296=1",
				  "=1",
				  "0="};
	struct pool_map *pool;
	struct pl_map   *placement;
	struct pool_buf *buf, *snapshot;
	unsigned int     i;

	gen_maps(1, 2, 1, 8, &pool, &placement);
	assert_success(pool_buf_extract(pool, &buf));
	snapshot = pool_buf_dup(buf);
	assert_non_null(snapshot);
	assert_success(pool_buf_set_rank_weights(buf, NULL));
	for (i = 0; i < ARRAY_SIZE(bad); i++) {
		assert_int_equal(pool_buf_set_rank_weights(buf, bad[i]), -DER_INVAL);
		assert_memory_equal(buf, snapshot, pool_buf_size(buf->pb_nr));
	}
	assert_success(pool_buf_set_rank_weights(buf, "1=255"));
	assert_true(pool_buf_has_rank_weights(buf));
	assert_int_equal(buf->pb_comps[buf->pb_domain_nr].co_weight, 1);
	assert_int_equal(buf->pb_comps[buf->pb_domain_nr + 1].co_weight, 255);
	/* A populated weighted map cannot be reweighted through this creation API. */
	assert_int_equal(pool_buf_set_rank_weights(buf, "0=2,1=1"), -DER_INVAL);
	pool_buf_free(snapshot);
	pool_buf_free(buf);
	free_pool_and_placement_map(pool, placement);
}

static void
distribution(int fdom, int slow_weight, int fast_weight)
{
	struct pool_map      *pool;
	struct pl_map        *placement;
	struct pl_obj_layout *layout;
	struct pool_target   *target;
	char                  spec[32];
	uint32_t              counts[2] = {0}, target_counts[16] = {0}, i;
	const uint32_t        objects  = 20000;
	double                expected = (double)slow_weight / (slow_weight + fast_weight);

	snprintf(spec, sizeof(spec), "0=%d,1=%d", slow_weight, fast_weight);
	weighted_maps(2, 1, 8, fdom, spec, &pool, &placement);
	for (i = 0; i < objects; i++) {
		daos_obj_id_t oid;

		gen_oid(&oid, i, UINT64_MAX, OC_S1);
		assert_success(plt_obj_place(oid, 0, &layout, placement, false));
		assert_int_equal(layout->ol_nr, 1);
		assert_int_equal(
		    pool_map_find_target(pool, layout->ol_shards[0].po_target, &target), 1);
		assert_true(target->ta_comp.co_rank < 2);
		counts[target->ta_comp.co_rank]++;
		target_counts[target->ta_comp.co_id]++;
		pl_obj_layout_free(layout);
	}
	print_message("weights %s, fault level %d: %u / %u placements\n", spec, fdom, counts[0],
		      counts[1]);
	assert_true((double)counts[0] / objects > expected - 0.02);
	assert_true((double)counts[0] / objects < expected + 0.02);
	for (i = 0; i < ARRAY_SIZE(target_counts); i++)
		assert_true(target_counts[i] > 0);
	free_pool_and_placement_map(pool, placement);
}

static void
weighted_distribution_rank(void **state)
{
	distribution(PO_COMP_TP_RANK, 1, 2);
}

static void
weighted_distribution_node(void **state)
{
	distribution(PO_COMP_TP_NODE, 1, 2);
}

static void
weighted_distribution_reversed(void **state)
{
	distribution(PO_COMP_TP_RANK, 2, 1);
}

static void
equal_weights_keep_legacy_layouts(void **state)
{
	struct pool_map *normal, *weighted;
	struct pl_map   *normal_pl, *weighted_pl;
	unsigned int     i;
	daos_oclass_id_t classes[] = {OC_S1, OC_SX, OC_RP_2G1, OC_RP_2GX};

	gen_maps(1, 2, 1, 8, &normal, &normal_pl);
	weighted_maps(2, 1, 8, PO_COMP_TP_RANK, "0=7,1=7", &weighted, &weighted_pl);
	for (i = 0; i < 2000; i++) {
		struct pl_obj_layout *a, *b;
		daos_obj_id_t         oid;

		gen_oid(&oid, i, UINT64_MAX, classes[i % ARRAY_SIZE(classes)]);
		assert_success(plt_obj_place(oid, 0, &a, normal_pl, false));
		assert_success(plt_obj_place(oid, 0, &b, weighted_pl, false));
		assert_true(plt_obj_layout_match(a, b));
		pl_obj_layout_free(a);
		pl_obj_layout_free(b);
	}
	free_pool_and_placement_map(normal, normal_pl);
	free_pool_and_placement_map(weighted, weighted_pl);
}

static void
map_roundtrip_preserves_policy_and_layouts(void **state)
{
	struct pool_map        *pool, *restored;
	struct pl_map          *placement, *restored_pl;
	struct pool_buf        *buf, *again;
	struct pl_map_init_attr attr = {.ia_type = PL_TYPE_JUMP_MAP};
	unsigned int            i;

	weighted_maps(2, 1, 8, PO_COMP_TP_RANK, "0=1,1=2", &pool, &placement);
	assert_success(pool_buf_extract(pool, &buf));
	assert_true(pool_buf_has_rank_weights(buf));
	assert_success(pool_map_create(buf, 1, &restored));
	assert_true(pool_map_has_rank_weights(restored));
	assert_success(pool_buf_extract(restored, &again));
	assert_memory_equal(buf, again, pool_buf_size(buf->pb_nr));
	attr.ia_jump_map.domain = PO_COMP_TP_RANK;
	assert_success(pl_map_create(restored, &attr, &restored_pl));
	for (i = 0; i < 2000; i++) {
		struct pl_obj_layout *a, *b;
		daos_obj_id_t         oid;

		gen_oid(&oid, i, UINT64_MAX, OC_RP_2G1);
		assert_success(plt_obj_place(oid, 0, &a, placement, false));
		assert_success(plt_obj_place(oid, 0, &b, restored_pl, false));
		assert_true(plt_obj_layout_match(a, b));
		pl_obj_layout_free(a);
		pl_obj_layout_free(b);
	}
	pool_buf_free(buf);
	pool_buf_free(again);
	free_pool_and_placement_map(pool, placement);
	free_pool_and_placement_map(restored, restored_pl);
}

static void
replicas_remain_on_separate_nodes(void **state)
{
	struct pool_map *pool;
	struct pl_map   *placement;
	unsigned int     i;

	weighted_maps(2, 1, 8, PO_COMP_TP_NODE, "0=1,1=255", &pool, &placement);
	for (i = 0; i < 1000; i++) {
		struct pl_obj_layout *layout;
		struct pool_target   *a, *b;
		daos_obj_id_t         oid;

		gen_oid(&oid, i, UINT64_MAX, OC_RP_2G1);
		assert_success(plt_obj_place(oid, 0, &layout, placement, false));
		assert_int_equal(layout->ol_nr, 2);
		assert_int_equal(pool_map_find_target(pool, layout->ol_shards[0].po_target, &a), 1);
		assert_int_equal(pool_map_find_target(pool, layout->ol_shards[1].po_target, &b), 1);
		assert_int_not_equal(a->ta_comp.co_rank, b->ta_comp.co_rank);
		pl_obj_layout_free(layout);
	}
	free_pool_and_placement_map(pool, placement);
}

static void
full_width_uses_every_target(void **state)
{
	struct pool_map *pool;
	struct pl_map   *placement;
	unsigned int     i;

	weighted_maps(2, 1, 8, PO_COMP_TP_RANK, "0=1,1=2", &pool, &placement);
	for (i = 0; i < 1000; i++) {
		struct pl_obj_layout *layout;
		daos_obj_id_t         oid;
		uint32_t              seen = 0, j;

		gen_oid(&oid, i, UINT64_MAX, OC_SX);
		assert_success(plt_obj_place(oid, 0, &layout, placement, false));
		assert_int_equal(layout->ol_nr, 16);
		for (j = 0; j < layout->ol_nr; j++) {
			int target = layout->ol_shards[j].po_target;

			assert_true(target >= 0 && target < 16);
			assert_false(seen & (1U << target));
			seen |= 1U << target;
		}
		assert_int_equal(seen, 0xffff);
		pl_obj_layout_free(layout);
	}
	free_pool_and_placement_map(pool, placement);
}

static void
compatibility_and_unsupported_modes(void **state)
{
	struct pool_map        *pool, *unused         = NULL;
	struct pl_map          *placement, *unused_pl = NULL;
	struct pool_buf        *buf;
	struct pl_map_init_attr attr   = {.ia_type = PL_TYPE_RING};
	struct daos_obj_md      md     = {0};
	struct pl_obj_layout   *layout = NULL;

	weighted_maps(2, 1, 8, PO_COMP_TP_RANK, "0=1,1=2", &pool, &placement);
	assert_success(pool_buf_extract(pool, &buf));
	assert_int_equal(pool_buf_check_rank_weight_capability(buf, false), -DER_NOTSUPPORTED);
	assert_success(pool_buf_check_rank_weight_capability(buf, true));
	assert_int_equal(pool_map_extend(pool, 2, buf), -DER_NOTSUPPORTED);
	attr.ia_ring.domain = PO_COMP_TP_RANK;
	assert_int_equal(pl_map_create(pool, &attr, &unused_pl), -DER_NOTSUPPORTED);
	gen_oid(&md.omd_id, 0, UINT64_MAX, OC_S1);
	md.omd_ver = 1;
	assert_int_equal(pl_obj_place(placement, 1, &md, 0, NULL, &layout), -DER_NOTSUPPORTED);
	buf->pb_comps[buf->pb_domain_nr].co_weight = 0;
	assert_int_equal(pool_map_create(buf, 1, &unused), -DER_INVAL);
	buf->pb_version = 12345;
	assert_int_equal(pool_map_create(buf, 1, &unused), -DER_NOTSUPPORTED);
	buf->pb_version = POOL_MAP_VER_2;
	assert_success(pool_buf_check_rank_weight_capability(buf, false));
	pool_buf_free(buf);
	free_pool_and_placement_map(pool, placement);
}

static void
performance_domains_are_rejected(void **state)
{
	struct pool_map *pool;
	struct pl_map   *placement;
	struct pool_buf *buf;

	gen_maps(2, 2, 1, 8, &pool, &placement);
	assert_success(pool_buf_extract(pool, &buf));
	assert_int_equal(pool_buf_set_rank_weights(buf, "0=1,1=2"), -DER_NOTSUPPORTED);
	assert_false(pool_buf_has_rank_weights(buf));
	pool_buf_free(buf);
	free_pool_and_placement_map(pool, placement);
}

static void
legacy_padding_is_ignored(void **state)
{
	struct pool_map    *pool, *restored;
	struct pl_map      *placement;
	struct pool_buf    *buf;
	struct pool_domain *ranks;
	unsigned int        i;

	gen_maps(1, 2, 1, 8, &pool, &placement);
	assert_success(pool_buf_extract(pool, &buf));
	for (i = 0; i < buf->pb_nr; i++)
		buf->pb_comps[i].co_padding = 255;
	assert_success(pool_map_create(buf, 1, &restored));
	assert_false(pool_map_has_rank_weights(restored));
	assert_int_equal(pool_map_find_ranks(restored, PO_COMP_ID_ALL, &ranks), 2);
	assert_int_equal(ranks[0].do_comp.co_weight, 0);
	assert_int_equal(ranks[1].do_comp.co_weight, 0);
	pool_buf_free(buf);
	pool_map_decref(restored);
	free_pool_and_placement_map(pool, placement);
}

static void
failed_target_preserves_historical_layout(void **state)
{
	struct pool_map      *pool;
	struct pl_map        *placement;
	struct pl_obj_layout *before, *historical, *current;
	struct daos_obj_md    md  = {0};
	uint32_t              ver = 1, rebuild_ranks[2], shard_ids[2];
	int                   failed, rc;

	weighted_maps(3, 1, 8, PO_COMP_TP_NODE, "0=1,1=2,2=3", &pool, &placement);
	gen_oid(&md.omd_id, 42, UINT64_MAX, OC_RP_2G1);
	md.omd_ver = 1;
	assert_success(pl_obj_place(placement, 2, &md, 0, NULL, &before));
	failed = before->ol_shards[0].po_target;
	plt_fail_tgt(failed, &ver, pool, false);
	assert_success(pl_obj_place(placement, 2, &md, DAOS_OO_RO, NULL, &historical));
	assert_int_equal(historical->ol_shards[0].po_target, failed);
	md.omd_ver = ver;
	assert_success(pl_obj_place(placement, 2, &md, 0, NULL, &current));
	assert_int_not_equal(current->ol_shards[0].po_target, failed);
	assert_int_equal(current->ol_shards[0].po_rebuilding, 1);
	rc = pl_obj_find_rebuild(placement, 2, &md, NULL, ver, rebuild_ranks, shard_ids, 2);
	assert_int_equal(rc, 1);
	assert_int_equal(shard_ids[0], 0);
	pl_obj_layout_free(before);
	pl_obj_layout_free(historical);
	pl_obj_layout_free(current);
	free_pool_and_placement_map(pool, placement);
}

static void
engine_failure_keeps_replicas_separate(void **state)
{
	struct pool_map        *pool;
	struct pl_map          *placement;
	struct pl_map_init_attr attr = {.ia_type = PL_TYPE_JUMP_MAP};
	uint32_t                ver  = 1, i;

	weighted_maps(3, 1, 8, PO_COMP_TP_NODE, "0=1,1=2,2=3", &pool, &placement);
	plt_set_domain_status(2, PO_COMP_ST_DOWN, &ver, pool, false, PO_COMP_TP_NODE);
	pl_map_decref(placement);
	attr.ia_jump_map.domain = PO_COMP_TP_NODE;
	assert_success(pl_map_create(pool, &attr, &placement));
	for (i = 0; i < 1000; i++) {
		struct daos_obj_md    md = {.omd_ver = ver};
		struct pl_obj_layout *layout;
		struct pool_target   *a, *b;

		gen_oid(&md.omd_id, i, UINT64_MAX, OC_RP_2G1);
		assert_success(pl_obj_place(placement, 2, &md, 0, NULL, &layout));
		assert_int_equal(layout->ol_nr, 2);
		assert_int_equal(pool_map_find_target(pool, layout->ol_shards[0].po_target, &a), 1);
		assert_int_equal(pool_map_find_target(pool, layout->ol_shards[1].po_target, &b), 1);
		assert_true(a->ta_comp.co_rank < 2);
		assert_true(b->ta_comp.co_rank < 2);
		assert_int_not_equal(a->ta_comp.co_rank, b->ta_comp.co_rank);
		pl_obj_layout_free(layout);
	}
	free_pool_and_placement_map(pool, placement);
}

static void
ec_groups_keep_fault_domain_separation(void **state)
{
	struct pool_map *pool;
	struct pl_map   *placement;
	uint32_t         i;

	weighted_maps(6, 1, 4, PO_COMP_TP_NODE, "0=1,1=2,2=3,3=4,4=5,5=6", &pool, &placement);
	for (i = 0; i < 1000; i++) {
		struct pl_obj_layout *layout;
		daos_obj_id_t         oid;
		uint32_t              group, shard;

		gen_oid(&oid, i, UINT64_MAX, OC_EC_4P2G2);
		assert_success(plt_obj_place(oid, 0, &layout, placement, false));
		assert_int_equal(layout->ol_grp_size, 6);
		assert_int_equal(layout->ol_nr, 12);
		for (group = 0; group < 2; group++) {
			uint32_t seen = 0;

			for (shard = 0; shard < 6; shard++) {
				struct pool_target *target;
				int id = layout->ol_shards[group * 6 + shard].po_target;

				assert_int_equal(pool_map_find_target(pool, id, &target), 1);
				assert_true(target->ta_comp.co_rank < 6);
				assert_false(seen & (1U << target->ta_comp.co_rank));
				seen |= 1U << target->ta_comp.co_rank;
			}
			assert_int_equal(seen, 0x3f);
		}
		pl_obj_layout_free(layout);
	}
	free_pool_and_placement_map(pool, placement);
}

static int
setup(void **state)
{
	assert_success(obj_class_init());
	return pl_init();
}

static int
teardown(void **state)
{
	pl_fini();
	obj_class_fini();
	return 0;
}

int
main(void)
{
	const struct CMUnitTest tests[] = {
	    cmocka_unit_test(parser_rejects_invalid_and_is_atomic),
	    cmocka_unit_test(weighted_distribution_rank),
	    cmocka_unit_test(weighted_distribution_node),
	    cmocka_unit_test(weighted_distribution_reversed),
	    cmocka_unit_test(equal_weights_keep_legacy_layouts),
	    cmocka_unit_test(map_roundtrip_preserves_policy_and_layouts),
	    cmocka_unit_test(replicas_remain_on_separate_nodes),
	    cmocka_unit_test(full_width_uses_every_target),
	    cmocka_unit_test(compatibility_and_unsupported_modes),
	    cmocka_unit_test(performance_domains_are_rejected),
	    cmocka_unit_test(legacy_padding_is_ignored),
	    cmocka_unit_test(failed_target_preserves_historical_layout),
	    cmocka_unit_test(engine_failure_keeps_replicas_separate),
	    cmocka_unit_test(ec_groups_keep_fault_domain_separation),
	};
	int rc;

	assert_success(daos_debug_init(DAOS_LOG_DEFAULT));
	rc = cmocka_run_group_tests(tests, setup, teardown);
	daos_debug_fini();
	return rc;
}
