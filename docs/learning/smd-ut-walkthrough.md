# smd_ut.c — line-by-line walkthrough

Source snapshot: `046caab85`. This walkthrough covers all **603 source lines** in [src/bio/smd/tests/smd_ut.c](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/smd/tests/smd_ut.c).


Read [the concepts and glossary](smd-bio-walkthrough.md) first if pointers, role masks, or DAOS return codes are unfamiliar.

This is a **test executable**, not the server's database implementation. It calls production SMD routines through an in-memory database adapter defined in this file. The adapter implements fetch/upsert/delete/traverse with linked lists. It does not supply real durable transactions, crash recovery, locks, or an NVMe device.

Most of the file predates the heterogeneous-weighting change. The additions are the BIO/weight includes and `ut_weight_capacity()`, plus its test registration. The other three tests are existing coverage.

The test group has **one setup and teardown for the whole group**. In particular, the replacement test uses devices created by `ut_device()`. The capacity regression uses a new UUID and different target IDs to avoid those existing records.

## Function and section index

- [L1: Headers and the in-memory database](#l1)
- [L45: db_name2list: choose a table](#l45)
- [L68: db_chain_alloc: allocate one record](#l68)
- [L86: db_chain_free: release a record](#l86)
- [L96: db_find: find a key](#l96)
- [L109: db_fetch: read a value](#l109)
- [L125: db_upsert: insert or update](#l125)
- [L146: db_delete: remove a record](#l146)
- [L161: db_traverse: visit every key](#l161)
- [L176: db_init: install the mock backend](#l176)
- [L191: db_fini and shared device IDs](#l191)
- [L211: smd_ut_setup: start the suite](#l211)
- [L233: smd_ut_teardown: stop the suite](#l233)
- [L242: verify_dev: check a device snapshot](#l242)
- [L261: ut_device: device and target mappings](#l261)
- [L336: verify_pool: check a pool snapshot](#l336)
- [L356: ut_pool: pool and blob mappings](#l356)
- [L464: ut_dev_replace: replace a failed device](#l464)
- [L508: ut_weight_capacity: the new regression](#l508)
- [L554: Register the four tests](#l554)
- [L561: Command-line help](#l561)
- [L569: main: initialize and run the suite](#l569)

Every source line appears once below, with its original number. Explanations refer to individual lines or short groups belonging to one statement or operation. Blank lines separate ideas; `{` and `}` open and close the surrounding scope. Copyright comments and repeated diagnostic formatting do not change runtime behavior.


<a id="l1"></a>

## L1–L44: Headers and the in-memory database

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/smd/tests/smd_ut.c#L1)

| Source lines | Explanation |
| --- | --- |
| L1–L8 | Copyright/license header and the `tests` logging category. `D_LOGFAC` makes this file's messages appear under the test facility. |
| L10–L16 | Standard C/POSIX declarations and cmocka. `stdarg.h` and `setjmp.h` support the cmocka interface; `getopt.h` parses command-line options. |
| L18 | DAOS common types, assertions, allocation helpers, and error definitions. |
| L19 | BIO declarations, including `BIO_SYS_TGT_ID`, used by the new capacity test. |
| L20–L21 | Public SMD APIs and internal table names. These tell the fake database which table an SMD call requests. |
| L22 | The actual weighting/capacity helper functions under test, rather than copies of their implementations. |
| L23–L24 | Test helpers and the `sys_db` backend interface whose function pointers the mock implements. |
| L26 | An unused path constant in this file. Its presence does not mean these tests format or write `/mnt/daos`. |
| L28 | Three role-specific target tables + three role-specific pool tables + three shared tables = nine list heads, since `SMD_DEV_TYPE_MAX` is 3. |
| L30–L33 | `ut_db` embeds the generic database interface and nine linked lists. Each list represents one table. |
| L35 | One static database instance for the suite. Static storage is zero-initialized, so unassigned optional callbacks remain NULL. |
| L37–L43 | `ut_chain` is one key/value record: linked-list node, owned key/value byte buffers, and their sizes. A `void *` stores bytes without imposing a C type. |

<details>
<summary>Numbered source for this section</summary>

```text
   1  /**
   2   * (C) Copyright 2018-2025 Intel Corporation.
   3   * (C) Copyright 2025 Hewlett Packard Enterprise Development LP
   4   *
   5   * SPDX-License-Identifier: BSD-2-Clause-Patent
   6   */
   7
   8  #define D_LOGFAC	DD_FAC(tests)
   9
  10  #include <stdio.h>
  11  #include <unistd.h>
  12  #include <stdlib.h>
  13  #include <stdarg.h>
  14  #include <setjmp.h>
  15  #include <cmocka.h>
  16  #include <getopt.h>
  17
  18  #include <daos/common.h>
  19  #include <daos_srv/bio.h>
  20  #include <daos_srv/smd.h>
  21  #include "../smd_internal.h"
  22  #include "../../bio_weight.h"
  23  #include <daos/tests_lib.h>
  24  #include <daos/sys_db.h>
  25
  26  #define SMD_STORAGE_PATH	"/mnt/daos"
  27  /* See db_name2list() */
  28  #define DB_LIST_NR              (SMD_DEV_TYPE_MAX * 2 + 3)
  29
  30  struct ut_db {
  31  	struct sys_db	ud_db;
  32  	d_list_t	ud_lists[DB_LIST_NR];
  33  };
  34
  35  static struct ut_db	ut_db;
  36
  37  struct ut_chain {
  38  	d_list_t	 uc_link;
  39  	void		*uc_key;
  40  	void		*uc_val;
  41  	int		 uc_key_size;
  42  	int		 uc_val_size;
  43  };
  44
```

</details>


<a id="l45"></a>

## L45–L67: db_name2list: choose a table

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/smd/tests/smd_ut.c#L45)

| Source lines | Explanation |
| --- | --- |
| L45–L49 | Return a list-head pointer. `container_of` recovers the enclosing `ut_db` from its embedded `sys_db` pointer. `st` will iterate role enum values. |
| L51–L52 | `strcmp` returns zero on equality; `!strcmp` therefore means “names match.” Device records use list 0. |
| L53–L54 | Extended metadata-pool records use list 1. This is specifically `TABLE_POOLS_EX[META]`, not every extended pool table. |
| L55–L56 | Controller model/serial information uses list 2. |
| L58–L60 | Match role-specific target tables. DATA/META/WAL map to list indexes 3/4/5. |
| L61–L62 | Match role-specific pool tables. DATA/META/WAL map to indexes 6/7/8. |
| L64–L65 | An unexpected table name is a test/backend programming error: assert, then return NULL as a fallback. This adapter supports only tables used by this suite. |

<details>
<summary>Numbered source for this section</summary>

```text
  45  d_list_t *
  46  db_name2list(struct sys_db *db, char *name)
  47  {
  48  	struct ut_db *ud = container_of(db, struct ut_db, ud_db);
  49  	enum smd_dev_type st;
  50
  51  	if (!strcmp(name, TABLE_DEV))
  52  		return &ud->ud_lists[0];
  53  	if (!strcmp(name, TABLE_POOLS_EX[SMD_DEV_TYPE_META]))
  54  		return &ud->ud_lists[1];
  55  	if (!strcmp(name, TABLE_CTRLR_DATA))
  56  		return &ud->ud_lists[2];
  57
  58  	for (st = SMD_DEV_TYPE_DATA; st < SMD_DEV_TYPE_MAX; st++) {
  59  		if (!strcmp(name, TABLE_TGTS[st]))
  60  			return &ud->ud_lists[st + 3];
  61  		if (!strcmp(name, TABLE_POOLS[st]))
  62  			return &ud->ud_lists[st + SMD_DEV_TYPE_MAX + 3];
  63  	}
  64  	D_ASSERT(0);
  65  	return NULL;
  66  }
  67
```

</details>


<a id="l68"></a>

## L68–L85: db_chain_alloc: allocate one record

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/smd/tests/smd_ut.c#L68)

| Source lines | Explanation |
| --- | --- |
| L68–L71 | Allocate and return a `ut_chain *`; the local pointer will own the new record. |
| L73–L74 | Allocate one zero-filled record header and assert success. `sizeof(*chain)` measures the pointed-to structure, not the pointer. |
| L75–L77 | Store the key length, allocate its owned buffer, and assert that allocation succeeded. |
| L79–L81 | Do the same for the value buffer. |
| L83 | Return ownership to the caller. The bytes will be copied into these buffers by `db_upsert()`. |

<details>
<summary>Numbered source for this section</summary>

```text
  68  struct ut_chain *
  69  db_chain_alloc(int key_size, int val_size)
  70  {
  71  	struct ut_chain	*chain;
  72
  73  	chain = calloc(1, sizeof(*chain));
  74  	D_ASSERT(chain);
  75  	chain->uc_key_size = key_size;
  76  	chain->uc_key = calloc(1, key_size);
  77  	D_ASSERT(chain->uc_key);
  78
  79  	chain->uc_val_size = val_size;
  80  	chain->uc_val = calloc(1, val_size);
  81  	D_ASSERT(chain->uc_val);
  82
  83  	return chain;
  84  }
  85
```

</details>


<a id="l86"></a>

## L86–L95: db_chain_free: release a record

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/smd/tests/smd_ut.c#L86)

| Source lines | Explanation |
| --- | --- |
| L86–L88 | Accept the record pointer to destroy; there is no return value. |
| L89–L93 | Free the key buffer, value buffer, and finally the containing record. This function does not unlink the record; callers must do that first. |

<details>
<summary>Numbered source for this section</summary>

```text
  86  void
  87  db_chain_free(struct ut_chain *chain)
  88  {
  89  	if (chain->uc_key)
  90  		free(chain->uc_key);
  91  	if (chain->uc_val)
  92  		free(chain->uc_val);
  93  	free(chain);
  94  }
  95
```

</details>


<a id="l96"></a>

## L96–L108: db_find: find a key

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/smd/tests/smd_ut.c#L96)

| Source lines | Explanation |
| --- | --- |
| L96–L99 | Search one table list for a `d_iov_t` key. An iov holds a byte-buffer pointer and lengths. |
| L101 | Iterate records using their embedded `uc_link` nodes. |
| L102 | This mock assumes all keys in a given table have the same length. A mismatched key length is an assertion failure. |
| L103–L104 | `memcmp` compares raw key bytes. Zero means equal, so return the matching record. |
| L106 | No match: return NULL. Nothing is allocated or modified. |

<details>
<summary>Numbered source for this section</summary>

```text
  96  struct ut_chain *
  97  db_find(d_list_t *head, d_iov_t *key)
  98  {
  99  	struct ut_chain *chain;
 100
 101  	d_list_for_each_entry(chain, head, uc_link) {
 102  		D_ASSERT(key->iov_len == chain->uc_key_size);
 103  		if (!memcmp(key->iov_buf, chain->uc_key, chain->uc_key_size))
 104  			return chain;
 105  	}
 106  	return NULL;
 107  }
 108
```

</details>


<a id="l109"></a>

## L109–L124: db_fetch: read a value

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/smd/tests/smd_ut.c#L109)

| Source lines | Explanation |
| --- | --- |
| L109–L113 | Resolve the table and prepare a record pointer. `val` is a caller-provided output descriptor. |
| L115–L116 | Look up the requested key and branch on whether it exists. |
| L117–L119 | Copy the stored bytes into the caller's output buffer, set the actual output length, and return success. This mock assumes the caller provided sufficient space. |
| L121–L122 | Clear the output length and return `-DER_NONEXIST` for a missing key. |

<details>
<summary>Numbered source for this section</summary>

```text
 109  static int
 110  db_fetch(struct sys_db *db, char *table, d_iov_t *key, d_iov_t *val)
 111  {
 112  	d_list_t	*head = db_name2list(db, table);
 113  	struct ut_chain *chain;
 114
 115  	chain = db_find(head, key);
 116  	if (chain) {
 117  		memcpy(val->iov_buf, chain->uc_val, chain->uc_val_size);
 118  		val->iov_len = chain->uc_val_size;
 119  		return 0;
 120  	}
 121  	val->iov_len = 0;
 122  	return -DER_NONEXIST;
 123  }
 124
```

</details>


<a id="l125"></a>

## L125–L145: db_upsert: insert or update

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/smd/tests/smd_ut.c#L125)

| Source lines | Explanation |
| --- | --- |
| L125–L129 | “Upsert” means update if present, otherwise insert. Find the table and reserve a temporary record pointer. |
| L131–L135 | Existing key: require the same value size, overwrite its bytes, and return success. This backend does not resize existing values. |
| L137–L138 | Missing key: allocate a new record large enough for both input byte sequences. |
| L140–L141 | Copy the input key/value. The database owns these copies independently of the caller's buffers. |
| L142–L143 | Append to the table's list and report success. The list node is part of the record itself. |

<details>
<summary>Numbered source for this section</summary>

```text
 125  static int
 126  db_upsert(struct sys_db *db, char *table, d_iov_t *key, d_iov_t *val)
 127  {
 128  	d_list_t	*head = db_name2list(db, table);
 129  	struct ut_chain *chain;
 130
 131  	chain = db_find(head, key);
 132  	if (chain) {
 133  		D_ASSERT(val->iov_len == chain->uc_val_size);
 134  		memcpy(chain->uc_val, val->iov_buf, val->iov_len);
 135  		return 0;
 136  	}
 137  	chain = db_chain_alloc(key->iov_len, val->iov_len);
 138  	D_ASSERT(chain);
 139
 140  	memcpy(chain->uc_key, key->iov_buf, key->iov_len);
 141  	memcpy(chain->uc_val, val->iov_buf, val->iov_len);
 142  	d_list_add_tail(&chain->uc_link, head);
 143  	return 0;
 144  }
 145
```

</details>


<a id="l146"></a>

## L146–L160: db_delete: remove a record

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/smd/tests/smd_ut.c#L146)

| Source lines | Explanation |
| --- | --- |
| L146–L150 | Resolve the table for a delete and declare a temporary record pointer. |
| L152–L156 | Find the record, unlink it, free all its memory, and return zero. |
| L158 | Deleting a nonexistent key reports `-DER_NONEXIST`. |

<details>
<summary>Numbered source for this section</summary>

```text
 146  static int
 147  db_delete(struct sys_db *db, char *table, d_iov_t *key)
 148  {
 149  	d_list_t	*head = db_name2list(db, table);
 150  	struct ut_chain *chain;
 151
 152  	chain = db_find(head, key);
 153  	if (chain) {
 154  		d_list_del(&chain->uc_link);
 155  		db_chain_free(chain);
 156  		return 0;
 157  	}
 158  	return -DER_NONEXIST;
 159  }
 160
```

</details>


<a id="l161"></a>

## L161–L175: db_traverse: visit every key

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/smd/tests/smd_ut.c#L161)

| Source lines | Explanation |
| --- | --- |
| L161–L165 | Receive the table, a callback function, and caller context `args`. Resolve the table list. |
| L167–L170 | For each record, construct an iov view of its key; `d_iov_set` points at existing bytes rather than copying them. |
| L171 | Call the traversal callback with the database, table, key, caller context, and NULL final argument. Its return value is ignored here; this mock is not a complete production traversal implementation. |
| L173 | Report traversal success after visiting every record. |

<details>
<summary>Numbered source for this section</summary>

```text
 161  static int
 162  db_traverse(struct sys_db *db, char *table, sys_db_trav_cb_t cb, void *args)
 163  {
 164  	d_list_t	*head = db_name2list(db, table);
 165  	struct ut_chain *chain;
 166
 167  	d_list_for_each_entry(chain, head, uc_link) {
 168  		d_iov_t		key;
 169
 170  		d_iov_set(&key, chain->uc_key, chain->uc_key_size);
 171  		cb(db, table, &key, args, NULL);
 172  	}
 173  	return 0;
 174  }
 175
```

</details>


<a id="l176"></a>

## L176–L190: db_init: install the mock backend

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/smd/tests/smd_ut.c#L176)

| Source lines | Explanation |
| --- | --- |
| L176–L179 | Initialize the mock backend; `i` is the table index. |
| L181–L184 | Install the four function pointers. A production `smd_dev_add_tgt()` call eventually reaches these callbacks through the generic SMD store layer. |
| L186–L188 | Initialize every circular list head and return zero. No real storage is opened. |

<details>
<summary>Numbered source for this section</summary>

```text
 176  static int
 177  db_init(void)
 178  {
 179  	int	i;
 180
 181  	ut_db.ud_db.sd_fetch	= db_fetch;
 182  	ut_db.ud_db.sd_upsert	= db_upsert;
 183  	ut_db.ud_db.sd_delete	= db_delete;
 184  	ut_db.ud_db.sd_traverse	= db_traverse;
 185
 186  	for (i = 0; i < DB_LIST_NR; i++)
 187  		D_INIT_LIST_HEAD(&ut_db.ud_lists[i]);
 188  	return 0;
 189  }
 190
```

</details>


<a id="l191"></a>

## L191–L210: db_fini and shared device IDs

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/smd/tests/smd_ut.c#L191)

| Source lines | Explanation |
| --- | --- |
| L191–L198 | During teardown, visit every table and keep removing records until its list is empty. |
| L200–L201 | Recover the first `ut_chain` from the first list node using the offset of its `uc_link` member. |
| L202–L203 | Unlink the record, then release its owned memory. The next loop iteration sees the new first record. |
| L208–L209 | Shared device UUIDs used by more than one test. This is why test order matters in the existing suite. |

<details>
<summary>Numbered source for this section</summary>

```text
 191  static void
 192  db_fini(void)
 193  {
 194  	int	i;
 195
 196  	for (i = 0; i < DB_LIST_NR; i++) {
 197  		while (!d_list_empty(&ut_db.ud_lists[i])) {
 198  			struct ut_chain	*chain;
 199
 200  			chain = d_list_entry(ut_db.ud_lists[i].next,
 201  					     struct ut_chain, uc_link);
 202  			d_list_del(&chain->uc_link);
 203  			db_chain_free(chain);
 204  		}
 205  	}
 206  }
 207
 208  uuid_t	dev_id1;
 209  uuid_t	dev_id2;
 210
```

</details>


<a id="l211"></a>

## L211–L232: smd_ut_setup: start the suite

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/smd/tests/smd_ut.c#L211)

| Source lines | Explanation |
| --- | --- |
| L211–L214 | Group setup callback. The `void **state` argument is the cmocka callback convention; this function does not use it. |
| L216–L220 | Initialize DAOS logging. On failure, print a diagnostic and return that error. |
| L221–L222 | Initialize the in-memory adapter, then give its interface to production SMD via `smd_init()`. |
| L224–L227 | If SMD initialization fails, print the error, shut down logging, and return failure. |
| L230 | Zero means the group can start running tests. |

<details>
<summary>Numbered source for this section</summary>

```text
 211  static int
 212  smd_ut_setup(void **state)
 213  {
 214  	int	rc;
 215
 216  	rc = daos_debug_init(DAOS_LOG_DEFAULT);
 217  	if (rc) {
 218  		print_error("Error initializing the debug instance\n");
 219  		return rc;
 220  	}
 221  	db_init();
 222  	rc = smd_init(&ut_db.ud_db);
 223
 224  	if (rc) {
 225  		print_error("Error initializing SMD store: %d\n", rc);
 226  		daos_debug_fini();
 227  		return rc;
 228  	}
 229
 230  	return 0;
 231  }
 232
```

</details>


<a id="l233"></a>

## L233–L241: smd_ut_teardown: stop the suite

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/smd/tests/smd_ut.c#L233)

| Source lines | Explanation |
| --- | --- |
| L233–L239 | End the SMD session, free all mock records, shut down logging, and return success. This is group teardown, not per-test teardown. |

<details>
<summary>Numbered source for this section</summary>

```text
 233  static int
 234  smd_ut_teardown(void **state)
 235  {
 236  	smd_fini();
 237  	db_fini();
 238  	daos_debug_fini();
 239  	return 0;
 240  }
 241
```

</details>


<a id="l242"></a>

## L242–L260: verify_dev: check a device snapshot

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/smd/tests/smd_ut.c#L242)

| Source lines | Explanation |
| --- | --- |
| L242–L247 | Check that a returned device-info snapshot has the expected UUID. `uuid_compare(...) == 0` means the identifiers match. |
| L248–L252 | Expected device 1: NORMAL state, three mappings, and target IDs 0, 1, 2 in insertion order. |
| L253–L257 | Expected device 2: FAULTY state, three mappings, and target IDs 3, 4, 5. `i - 3` converts the target number into array indexes 0, 1, 2. |

<details>
<summary>Numbered source for this section</summary>

```text
 242  static void
 243  verify_dev(struct smd_dev_info *dev_info, uuid_t id, int dev_idx)
 244  {
 245  	int	i;
 246
 247  	assert_int_equal(uuid_compare(dev_info->sdi_id, id), 0);
 248  	if (dev_idx == 1) {
 249  		assert_int_equal(dev_info->sdi_state, SMD_DEV_NORMAL);
 250  		assert_int_equal(dev_info->sdi_tgt_cnt, 3);
 251  		for (i = 0; i < 3; i++)
 252  			assert_int_equal(dev_info->sdi_tgts[i], i);
 253  	} else {
 254  		assert_int_equal(dev_info->sdi_state, SMD_DEV_FAULTY);
 255  		assert_int_equal(dev_info->sdi_tgt_cnt, 3);
 256  		for (i = 3; i < 6; i++)
 257  			assert_int_equal(dev_info->sdi_tgts[i - 3], i);
 258  	}
 259  }
 260
```

</details>


<a id="l261"></a>

## L261–L335: ut_device: device and target mappings

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/smd/tests/smd_ut.c#L261)

| Source lines | Explanation |
| --- | --- |
| L261–L268 | Declare output snapshots, a traversal list, an unused-in-store UUID, the role iterator, count, and return code. `tmp` supports safe deletion while traversing returned snapshots. |
| L270–L272 | Generate two device IDs to insert and a third ID that remains absent. |
| L274–L276 | Insert the first DATA mapping for device 1, target 0. The final NULL means no controller-info payload is supplied. |
| L278–L279 | Repeating the identical target/role assignment must return `-DER_EXIST`. |
| L281–L284 | Add DATA mappings for targets 1 and 2 to device 1. |
| L286–L287 | Target 1/DATA already belongs to device 1. Assigning that same target/role to device 2 must also return `-DER_EXIST`. |
| L289–L293 | Add three mappings to device 2: target 3/DATA, target 4/META, target 5/WAL. The ternary at L290 yields enum values 0, 1, 2 respectively. |
| L295–L296 | Mark device 2 FAULTY and expect the state update to succeed. |
| L298–L299 | Query the never-inserted UUID and expect `-DER_NONEXIST`. |
| L301–L305 | Read device 1, validate its contents with `verify_dev`, then free the allocated snapshot. Freeing a snapshot does not delete the stored record. |
| L307–L308 | Target 4 exists for META, not DATA. This lookup demonstrates that target lookup is role-specific. |
| L310–L316 | Look up each of device 2's three target/role pairs, verify the complete device snapshot, and free it. |
| L318–L321 | Initialize an output list, enumerate SMD devices, and expect exactly two devices. |
| L323–L329 | Walk returned snapshots with the safe iterator, identify each UUID, and verify it. Any third/unexpected UUID fails the test. |
| L331–L332 | Remove each returned snapshot from the temporary result list and free it. The underlying database remains populated for later tests. |

<details>
<summary>Numbered source for this section</summary>

```text
 261  static void
 262  ut_device(void **state)
 263  {
 264  	struct smd_dev_info	*dev_info, *tmp;
 265  	d_list_t		 dev_list;
 266  	uuid_t			 id3;
 267  	enum smd_dev_type	 st;
 268  	int			 i, dev_cnt = 0, rc;
 269
 270  	uuid_generate(dev_id1);
 271  	uuid_generate(dev_id2);
 272  	uuid_generate(id3);
 273
 274  	/* Assigned dev1 to target 0, 1, 2, dev2 to target 3. 4. 5 */
 275  	rc = smd_dev_add_tgt(dev_id1, 0, SMD_DEV_TYPE_DATA, NULL);
 276  	assert_rc_equal(rc, 0);
 277
 278  	rc = smd_dev_add_tgt(dev_id1, 0, SMD_DEV_TYPE_DATA, NULL);
 279  	assert_rc_equal(rc, -DER_EXIST);
 280
 281  	for (i = 1; i < 3; i++) {
 282  		rc = smd_dev_add_tgt(dev_id1, i, SMD_DEV_TYPE_DATA, NULL);
 283  		assert_rc_equal(rc, 0);
 284  	}
 285
 286  	rc = smd_dev_add_tgt(dev_id2, 1, SMD_DEV_TYPE_DATA, NULL);
 287  	assert_rc_equal(rc, -DER_EXIST);
 288
 289  	for (i = 3; i < 6; i++) {
 290  		st = (i < 4) ? SMD_DEV_TYPE_DATA : SMD_DEV_TYPE_DATA + i - 3;
 291  		rc = smd_dev_add_tgt(dev_id2, i, st, NULL);
 292  		assert_rc_equal(rc, 0);
 293  	}
 294
 295  	rc = smd_dev_set_state(dev_id2, SMD_DEV_FAULTY);
 296  	assert_rc_equal(rc, 0);
 297
 298  	rc = smd_dev_get_by_id(id3, &dev_info);
 299  	assert_rc_equal(rc, -DER_NONEXIST);
 300
 301  	rc = smd_dev_get_by_id(dev_id1, &dev_info);
 302  	assert_rc_equal(rc, 0);
 303  	verify_dev(dev_info, dev_id1, 1);
 304
 305  	smd_dev_free_info(dev_info);
 306
 307  	rc = smd_dev_get_by_tgt(4, SMD_DEV_TYPE_DATA, &dev_info);
 308  	assert_rc_equal(rc, -DER_NONEXIST);
 309
 310  	for (i = 3; i < 6; i++) {
 311  		st = (i < 4) ? SMD_DEV_TYPE_DATA : SMD_DEV_TYPE_DATA + i - 3;
 312  		rc = smd_dev_get_by_tgt(i, st, &dev_info);
 313  		assert_rc_equal(rc, 0);
 314  		verify_dev(dev_info, dev_id2, 2);
 315  		smd_dev_free_info(dev_info);
 316  	}
 317
 318  	D_INIT_LIST_HEAD(&dev_list);
 319  	rc = smd_dev_list(&dev_list, &dev_cnt);
 320  	assert_rc_equal(rc, 0);
 321  	assert_int_equal(dev_cnt, 2);
 322
 323  	d_list_for_each_entry_safe(dev_info, tmp, &dev_list, sdi_link) {
 324  		if (uuid_compare(dev_info->sdi_id, dev_id1) == 0)
 325  			verify_dev(dev_info, dev_id1, 1);
 326  		else if (uuid_compare(dev_info->sdi_id, dev_id2) == 0)
 327  			verify_dev(dev_info, dev_id2, 2);
 328  		else
 329  			assert_true(false);
 330
 331  		d_list_del(&dev_info->sdi_link);
 332  		smd_dev_free_info(dev_info);
 333  	}
 334  }
 335
```

</details>


<a id="l336"></a>

## L336–L355: verify_pool: check a pool snapshot

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/smd/tests/smd_ut.c#L336)

| Source lines | Explanation |
| --- | --- |
| L336–L340 | Helper for pool-info snapshots. `shift` chooses the synthetic blob-ID encoding; `scm_sz` is the expected SCM-size field. |
| L342–L346 | Check the pool UUID and SCM size, then require four DATA entries, one META entry, and one WAL entry. |
| L348–L350 | Enumerate target IDs 0–5. Targets 0–3 select DATA; 4 selects META; 5 selects WAL. DATA has four entries, so its index is `i`; the other roles have one entry each, so their index is zero. |
| L351–L352 | Verify the target ID and associated synthetic blob ID. `i << shift` means multiply `i` by 2 raised to `shift`; these values make mismatches easy to spot. |

<details>
<summary>Numbered source for this section</summary>

```text
 336  static void
 337  verify_pool(struct smd_pool_info *pool_info, uuid_t id, int shift, uint64_t scm_sz)
 338  {
 339  	enum smd_dev_type	st;
 340  	int			i, j;
 341
 342  	assert_int_equal(uuid_compare(pool_info->spi_id, id), 0);
 343  	assert_int_equal(pool_info->spi_scm_sz, scm_sz);
 344  	assert_int_equal(pool_info->spi_tgt_cnt[SMD_DEV_TYPE_DATA], 4);
 345  	assert_int_equal(pool_info->spi_tgt_cnt[SMD_DEV_TYPE_META], 1);
 346  	assert_int_equal(pool_info->spi_tgt_cnt[SMD_DEV_TYPE_WAL], 1);
 347
 348  	for (i = 0; i < 6; i++) {
 349  		st = (i < 4) ? SMD_DEV_TYPE_DATA : SMD_DEV_TYPE_DATA + i - 3;
 350  		j = (i < 4) ? i : 0;
 351  		assert_int_equal(pool_info->spi_tgts[st][j], i);
 352  		assert_int_equal(pool_info->spi_blobs[st][j], i << shift);
 353  	}
 354  }
 355
```

</details>


<a id="l356"></a>

## L356–L463: ut_pool: pool and blob mappings

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/smd/tests/smd_ut.c#L356)

| Source lines | Explanation |
| --- | --- |
| L356–L364 | Declare two populated pool IDs, one absent ID, a blob-ID output, result list, role, loop index, and status variables. |
| L366–L368 | Generate three independent pool UUIDs. |
| L370–L373 | Populate pool 1 across six target/role pairs. Blob IDs are `i << 10`, blob size is 100, SCM-size argument is 0, and `recreate` is false. |
| L375–L379 | Populate pool 2 with blob IDs `i << 20` and blob size 200. Its META entry gets SCM-size argument 50; other roles get zero. The returned aggregate SCM-size expectation is 50, checked below. Pool 1 falls back to its META blob size, 100, because a zero SCM-size argument did not create an extended META size record; see `smd_pool_get_info()`. |
| L382–L383 | Repeat pool 1's target 0/DATA entry and expect `-DER_EXIST`, even though a different blob ID is supplied. |
| L385–L386 | Try adding to the existing DATA table with blob size 200 instead of 100. The inconsistent blob size must return `-DER_INVAL`. |
| L388–L389 | Duplicate target 4/META must return `-DER_EXIST`. |
| L391–L392 | A new META target with a blob size inconsistent with that role's existing table must return `-DER_INVAL`. |
| L394–L395 | Duplicate target 5/WAL must return `-DER_EXIST`. |
| L397–L398 | A WAL entry with the wrong blob size must return `-DER_INVAL`. |
| L400–L403 | Fetch pool 1's combined info, expect success, verify ID/role counts/blob IDs/SCM size, and free the snapshot. |
| L405–L408 | Do the equivalent check for pool 2, using shift 20 and SCM size 50. |
| L410–L411 | Fetch the unpopulated pool ID and expect no record. |
| L413–L417 | Look up each pool 1 blob directly by pool, target, and role; verify the expected synthetic ID. |
| L419–L421 | Do the same for pool 2. |
| L424–L427 | Target 6 was not inserted. Each of its DATA/META/WAL lookups must fail with `-DER_NONEXIST`. |
| L429–L432 | Enumerate pools and expect two results. |
| L434–L440 | Safely traverse pool snapshots; verify each belongs to pool 1 or pool 2. Unexpected IDs fail the test. |
| L442–L443 | Unlink/free temporary snapshots; this does not remove pool records from SMD. |
| L446–L449 | Deleting nonexistent target 6 from any pool 1 role must report `-DER_NONEXIST`. |
| L451–L458 | Delete all six inserted target/role mappings from both pools and require success for each deletion. |
| L460–L461 | With its last entries removed, pool 1 should no longer have a discoverable pool-info record. |

<details>
<summary>Numbered source for this section</summary>

```text
 356  static void
 357  ut_pool(void **state)
 358  {
 359  	struct smd_pool_info	*pool_info, *tmp;
 360  	uuid_t			 id1, id2, id3;
 361  	uint64_t		 blob_id;
 362  	d_list_t		 pool_list;
 363  	enum smd_dev_type	 st;
 364  	int			 i, pool_cnt = 0, rc;
 365
 366  	uuid_generate(id1);
 367  	uuid_generate(id2);
 368  	uuid_generate(id3);
 369
 370  	for (i = 0; i < 6; i++) {
 371  		st = (i < 4) ? SMD_DEV_TYPE_DATA : SMD_DEV_TYPE_DATA + i - 3;
 372  		rc = smd_pool_add_tgt(id1, i, i << 10, st, 100, 0, false);
 373  		assert_rc_equal(rc, 0);
 374
 375  		if (st == SMD_DEV_TYPE_META)
 376  			rc = smd_pool_add_tgt(id2, i, i << 20, st, 200, 50, false);
 377  		else
 378  			rc = smd_pool_add_tgt(id2, i, i << 20, st, 200, 0, false);
 379  		assert_rc_equal(rc, 0);
 380  	}
 381
 382  	rc = smd_pool_add_tgt(id1, 0, 5000, SMD_DEV_TYPE_DATA, 100, 0, false);
 383  	assert_rc_equal(rc, -DER_EXIST);
 384
 385  	rc = smd_pool_add_tgt(id1, 4, 4 << 10, SMD_DEV_TYPE_DATA, 200, 0, false);
 386  	assert_rc_equal(rc, -DER_INVAL);
 387
 388  	rc = smd_pool_add_tgt(id1, 4, 5000, SMD_DEV_TYPE_META, 100, 0, false);
 389  	assert_rc_equal(rc, -DER_EXIST);
 390
 391  	rc = smd_pool_add_tgt(id1, 0, 4 << 10, SMD_DEV_TYPE_META, 200, 0, false);
 392  	assert_rc_equal(rc, -DER_INVAL);
 393
 394  	rc = smd_pool_add_tgt(id1, 5, 5000, SMD_DEV_TYPE_WAL, 100, 0, false);
 395  	assert_rc_equal(rc, -DER_EXIST);
 396
 397  	rc = smd_pool_add_tgt(id1, 0, 4 << 10, SMD_DEV_TYPE_WAL, 200, 0, false);
 398  	assert_rc_equal(rc, -DER_INVAL);
 399
 400  	rc = smd_pool_get_info(id1, &pool_info);
 401  	assert_rc_equal(rc, 0);
 402  	verify_pool(pool_info, id1, 10, 100);
 403  	smd_pool_free_info(pool_info);
 404
 405  	rc = smd_pool_get_info(id2, &pool_info);
 406  	assert_rc_equal(rc, 0);
 407  	verify_pool(pool_info, id2, 20, 50);
 408  	smd_pool_free_info(pool_info);
 409
 410  	rc = smd_pool_get_info(id3, &pool_info);
 411  	assert_rc_equal(rc, -DER_NONEXIST);
 412
 413  	for (i = 0; i < 6; i++) {
 414  		st = (i < 4) ? SMD_DEV_TYPE_DATA : SMD_DEV_TYPE_DATA + i - 3;
 415  		rc = smd_pool_get_blob(id1, i, st, &blob_id);
 416  		assert_rc_equal(rc, 0);
 417  		assert_int_equal(blob_id, i << 10);
 418
 419  		rc = smd_pool_get_blob(id2, i, st, &blob_id);
 420  		assert_rc_equal(rc, 0);
 421  		assert_int_equal(blob_id, i << 20);
 422  	}
 423
 424  	for (st = SMD_DEV_TYPE_DATA; st < SMD_DEV_TYPE_MAX; st++) {
 425  		rc = smd_pool_get_blob(id1, 6, st, &blob_id);
 426  		assert_rc_equal(rc, -DER_NONEXIST);
 427  	}
 428
 429  	D_INIT_LIST_HEAD(&pool_list);
 430  	rc = smd_pool_list(&pool_list, &pool_cnt);
 431  	assert_rc_equal(rc, 0);
 432  	assert_int_equal(pool_cnt, 2);
 433
 434  	d_list_for_each_entry_safe(pool_info, tmp, &pool_list, spi_link) {
 435  		if (uuid_compare(pool_info->spi_id, id1) == 0)
 436  			verify_pool(pool_info, id1, 10, 100);
 437  		else if (uuid_compare(pool_info->spi_id, id2) == 0)
 438  			verify_pool(pool_info, id2, 20, 50);
 439  		else
 440  			assert_true(false);
 441
 442  		d_list_del(&pool_info->spi_link);
 443  		smd_pool_free_info(pool_info);
 444  	}
 445
 446  	for (st = SMD_DEV_TYPE_DATA; st < SMD_DEV_TYPE_MAX; st++) {
 447  		rc = smd_pool_del_tgt(id1, 6, st);
 448  		assert_rc_equal(rc, -DER_NONEXIST);
 449  	}
 450
 451  	for (i = 0; i < 6; i++) {
 452  		st = (i < 4) ? SMD_DEV_TYPE_DATA : SMD_DEV_TYPE_DATA + i - 3;
 453  		rc = smd_pool_del_tgt(id1, i, st);
 454  		assert_rc_equal(rc, 0);
 455
 456  		rc = smd_pool_del_tgt(id2, i, st);
 457  		assert_rc_equal(rc, 0);
 458  	}
 459
 460  	rc = smd_pool_get_info(id1, &pool_info);
 461  	assert_rc_equal(rc, -DER_NONEXIST);
 462  }
 463
```

</details>


<a id="l464"></a>

## L464–L507: ut_dev_replace: replace a failed device

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/smd/tests/smd_ut.c#L464)

| Source lines | Explanation |
| --- | --- |
| L464–L470 | Declare snapshots, a replacement UUID, the result list, and status variables. The old devices still exist from `ut_device()`. |
| L472 | Generate a new, unused replacement device UUID. |
| L474–L476 | Attempt to replace NORMAL device 1. Replacement requires the appropriate failure state, so expect `-DER_INVAL`. |
| L478–L479 | Mark device 1 FAULTY to make replacement eligible. |
| L481–L483 | Reject device 2 as a replacement because it is already in use. |
| L485–L487 | Replace faulty device 1 with the new device using the DATA role bit mask and no controller payload. Expect success. |
| L489–L493 | Enumerate devices; replacement should leave two device records, not add a third alongside the old one. |
| L495–L501 | The replacement must inherit device 1's target list and be NORMAL; device 2 retains its FAULTY state and target list. Any other UUID fails. |
| L503–L504 | Free the enumeration snapshots after unlinking them from the result list. |

<details>
<summary>Numbered source for this section</summary>

```text
 464  static void
 465  ut_dev_replace(void **state)
 466  {
 467  	struct smd_dev_info	*dev_info, *tmp_dev;
 468  	uuid_t			 dev_id3;
 469  	d_list_t		 dev_list;
 470  	int			 rc, dev_cnt = 0;
 471
 472  	uuid_generate(dev_id3);
 473
 474  	/* Replace dev1 with dev3 without marking dev1 as faulty */
 475  	rc = smd_dev_replace(dev_id1, dev_id3, smd_dev_type2role(SMD_DEV_TYPE_DATA), NULL);
 476  	assert_rc_equal(rc, -DER_INVAL);
 477
 478  	rc = smd_dev_set_state(dev_id1, SMD_DEV_FAULTY);
 479  	assert_rc_equal(rc, 0);
 480
 481  	/* Replace dev1 with dev2 */
 482  	rc = smd_dev_replace(dev_id1, dev_id2, smd_dev_type2role(SMD_DEV_TYPE_DATA), NULL);
 483  	assert_rc_equal(rc, -DER_INVAL);
 484
 485  	/* Replace dev1 with dev3 */
 486  	rc = smd_dev_replace(dev_id1, dev_id3, smd_dev_type2role(SMD_DEV_TYPE_DATA), NULL);
 487  	assert_rc_equal(rc, 0);
 488
 489  	/* Verify device after replace */
 490  	D_INIT_LIST_HEAD(&dev_list);
 491  	rc = smd_dev_list(&dev_list, &dev_cnt);
 492  	assert_rc_equal(rc, 0);
 493  	assert_int_equal(dev_cnt, 2);
 494
 495  	d_list_for_each_entry_safe(dev_info, tmp_dev, &dev_list, sdi_link) {
 496  		if (uuid_compare(dev_info->sdi_id, dev_id3) == 0)
 497  			verify_dev(dev_info, dev_id3, 1);
 498  		else if (uuid_compare(dev_info->sdi_id, dev_id2) == 0)
 499  			verify_dev(dev_info, dev_id2, 2);
 500  		else
 501  			assert_true(false);
 502
 503  		d_list_del(&dev_info->sdi_link);
 504  		smd_dev_free_info(dev_info);
 505  	}
 506  }
 507
```

</details>


<a id="l508"></a>

## L508–L553: ut_weight_capacity: the new regression

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/smd/tests/smd_ut.c#L508)

| Source lines | Explanation |
| --- | --- |
| L508–L510 | New regression: check the capacity helper against counts produced by real SMD functions, backed by the in-memory adapter. |
| L512 | `info` receives a heap-allocated device-info snapshot from SMD. |
| L513 | `id` identifies this test's device, separate from the earlier tests. |
| L514 | `target` and `role` are loop counters; `used` holds the actual number of occupied SMD entries. |
| L515 | `rc` stores each SMD return code. |
| L517 | Generate a unique device ID. No physical device is created. |
| L518–L519 | Iterate target IDs 1000 through 1020 inclusive: 21 targets. These avoid the earlier tests' target IDs 0–5. |
| L520 | For each target, iterate DATA=0, META=1, WAL=2. MAX=3 is the exclusive loop bound, not another role. |
| L521 | Insert this exact device/target/role mapping through production `smd_dev_add_tgt()`. |
| L522 | Every insertion must succeed; failure aborts the test at the failing assertion. |
| L525–L526 | Fetch the resulting device-info snapshot and require success. |
| L527 | Read `sdi_tgt_cnt`, the count of SMD entries. It is 63, not 21, because every role mapping occupies one slot. |
| L528 | Release the snapshot after copying the count into `used`. |
| L529 | Assert `21 * 3 == 63` entries actually reached the device table. |
| L530 | At 63/64 occupancy, a complete three-role assignment must be rejected by `bio_weight_fits`. |
| L531 | A two-role assignment also cannot fit. |
| L532 | A one-role assignment can still fit in the final slot. |
| L534–L535 | Insert one META mapping for `BIO_SYS_TGT_ID` (1024 in this source). This deliberately tests one system mapping, not the system target's entire production role setup. It is a target ID, not a fourth storage role. |
| L536 | That final insertion must succeed. |
| L537–L539 | Fetch the device again and require exactly the limit, `SMD_MAX_TGT_CNT == 64`. |
| L540 | The helper must now reject even one additional mapping. |
| L541 | Release this second snapshot. |
| L542–L543 | Bypass the helper and directly try to insert target 1021/DATA. SMD's own hard limit must reject the operation with `-DER_OVERFLOW`. |
| L544–L545 | Verify that the rejected operation did not leave a new target-to-device mapping behind. |
| L546–L547 | Fetch the earlier target 1000/WAL mapping to check existing records survived the failure. |
| L548 | That old mapping must still be readable. |
| L549 | Its device UUID must still be this test's device, not another test device. |
| L550 | The device's entry count must still be 64; the failed addition must not increment it. |
| L551 | Free the last returned snapshot. Group teardown later removes the stored test records. |

<details>
<summary>Numbered source for this section</summary>

```text
 508  /* Exercise the capacity guard against real SMD accounting, not just a counter. */
 509  static void
 510  ut_weight_capacity(void **state)
 511  {
 512  	struct smd_dev_info *info;
 513  	uuid_t               id;
 514  	unsigned int         target, role, used;
 515  	int                  rc;
 516
 517  	uuid_generate(id);
 518  	/* Combined data/meta/WAL roles consume three entries per target. */
 519  	for (target = 1000; target < 1021; target++) {
 520  		for (role = SMD_DEV_TYPE_DATA; role < SMD_DEV_TYPE_MAX; role++) {
 521  			rc = smd_dev_add_tgt(id, target, role, NULL);
 522  			assert_rc_equal(rc, 0);
 523  		}
 524  	}
 525  	rc = smd_dev_get_by_id(id, &info);
 526  	assert_rc_equal(rc, 0);
 527  	used = info->sdi_tgt_cnt;
 528  	smd_dev_free_info(info);
 529  	assert_int_equal(used, 63);
 530  	assert_false(bio_weight_fits(used, 3, SMD_MAX_TGT_CNT));
 531  	assert_false(bio_weight_fits(used, 2, SMD_MAX_TGT_CNT));
 532  	assert_true(bio_weight_fits(used, 1, SMD_MAX_TGT_CNT));
 533
 534  	/* System-target mappings also consume a slot, despite not adding VOS load. */
 535  	rc = smd_dev_add_tgt(id, BIO_SYS_TGT_ID, SMD_DEV_TYPE_META, NULL);
 536  	assert_rc_equal(rc, 0);
 537  	rc = smd_dev_get_by_id(id, &info);
 538  	assert_rc_equal(rc, 0);
 539  	assert_int_equal(info->sdi_tgt_cnt, SMD_MAX_TGT_CNT);
 540  	assert_false(bio_weight_fits(info->sdi_tgt_cnt, 1, SMD_MAX_TGT_CNT));
 541  	smd_dev_free_info(info);
 542  	rc = smd_dev_add_tgt(id, 1021, SMD_DEV_TYPE_DATA, NULL);
 543  	assert_rc_equal(rc, -DER_OVERFLOW);
 544  	rc = smd_dev_get_by_tgt(1021, SMD_DEV_TYPE_DATA, &info);
 545  	assert_rc_equal(rc, -DER_NONEXIST);
 546  	/* A failed addition must leave existing role mappings readable. */
 547  	rc = smd_dev_get_by_tgt(1000, SMD_DEV_TYPE_WAL, &info);
 548  	assert_rc_equal(rc, 0);
 549  	assert_int_equal(uuid_compare(info->sdi_id, id), 0);
 550  	assert_int_equal(info->sdi_tgt_cnt, SMD_MAX_TGT_CNT);
 551  	smd_dev_free_info(info);
 552  }
 553
```

</details>


<a id="l554"></a>

## L554–L560: Register the four tests

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/smd/tests/smd_ut.c#L554)

| Source lines | Explanation |
| --- | --- |
| L554–L559 | Register four tests. The first three are existing tests; the fourth is new. NULL per-test setup/teardown fields mean the group setup/teardown below is used, so state is shared across tests. |

<details>
<summary>Numbered source for this section</summary>

```text
 554  static const struct CMUnitTest smd_uts[] = {
 555  	{ "smd_ut_device", ut_device, NULL, NULL},
 556  	{ "smd_ut_pool", ut_pool, NULL, NULL},
 557  	{ "smd_ut_dev_replace", ut_dev_replace, NULL, NULL},
 558  	{ "smd_ut_weight_capacity", ut_weight_capacity, NULL, NULL},
 559  };
 560
```

</details>


<a id="l561"></a>

## L561–L568: Command-line help

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/smd/tests/smd_ut.c#L561)

| Source lines | Explanation |
| --- | --- |
| L561–L567 | Print a short help banner and the supported help syntax. The long string at L565 contains embedded newline escapes. |

<details>
<summary>Numbered source for this section</summary>

```text
 561  static void
 562  print_usage(char *name)
 563  {
 564  	print_message(
 565  		"\n\nCOMMON TESTS\n==========================\n");
 566  	print_message("%s -h|--help\n", name);
 567  }
 568
```

</details>


<a id="l569"></a>

## L569–L603: main: initialize and run the suite

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/smd/tests/smd_ut.c#L569)

| Source lines | Explanation |
| --- | --- |
| L569 | Short-option string accepts `h` and `l`, but the switch below implements only `h`; `-l` falls into its failure default. This is existing behavior, not a new weighting option. |
| L570–L573 | `idx` receives the long-option index. The array declares `--help` as a no-argument option that produces `'h'`. The shown array has no explicit zero terminator; the walkthrough records existing code rather than changing command-line parsing. |
| L575–L578 | `main` receives command-line arguments and declares the exit status and current option. |
| L580–L584 | Initialize Argobots before SMD tests. On failure print a message and return the initialization status. |
| L586 | Consume options one at a time until `getopt_long` returns -1. |
| L587–L591 | For `-h`/`--help`, print help, set success, and jump to common cleanup. |
| L592–L594 | Any other parsed option sets status 1 and goes to cleanup. |
| L597–L598 | Run all four tests in order, with one group setup and one group teardown. The result becomes the executable exit status. Assertion failures are reported by cmocka. |
| L600 | Common cleanup label used by the help/error branches and normal completion. |
| L601 | Finalize the Argobots runtime after the suite or early option handling. |
| L602 | Return the final status to the shell: zero indicates success. |

<details>
<summary>Numbered source for this section</summary>

```text
 569  const char *s_opts = "hl";
 570  static int idx;
 571  static struct option l_opts[] = {
 572  	{"help", no_argument,	NULL, 'h'},
 573  };
 574
 575  int main(int argc, char **argv)
 576  {
 577  	int	rc;
 578  	int	opt;
 579
 580  	rc = ABT_init(0, NULL);
 581  	if (rc != 0) {
 582  		D_PRINT("Error initializing ABT\n");
 583  		return rc;
 584  	}
 585
 586  	while ((opt = getopt_long(argc, argv, s_opts, l_opts, &idx)) != -1) {
 587  		switch (opt) {
 588  		case 'h':
 589  			print_usage(argv[0]);
 590  			rc = 0;
 591  			goto out;
 592  		default:
 593  			rc = 1;
 594  			goto out;
 595  		}
 596  	}
 597  	rc = cmocka_run_group_tests_name("SMD unit tests", smd_uts,
 598  					 smd_ut_setup, smd_ut_teardown);
 599
 600  out:
 601  	ABT_finalize();
 602  	return rc;
 603  }
```

</details>
