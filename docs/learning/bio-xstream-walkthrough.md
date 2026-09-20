# bio_xstream.c — line-by-line walkthrough

Source snapshot: `046caab85`. This walkthrough covers all **2235 source lines** in [src/bio/bio_xstream.c](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c).


Read [the concepts and glossary](smd-bio-walkthrough.md) alongside this file. `bio_xstream.c` is production BIO lifecycle and device-assignment code; it is not a test executable. “xstream” means execution stream. This file connects a target's execution context to SPDK threads, device records, shared blobstores, and per-context I/O channels.

For the heterogeneous feature, start with [weight validation](#l1092), [device selection](#l1340), [role assignment](#l1419), and [existing-mapping lookup](#l1485). Then read [context allocation](#l1828) to see who calls them. The remaining functions explain the setup, ownership, completion, and teardown rules around that path.

**Separate these objects:** `bio_bdev` represents a device; `bio_blobstore` is BIO's shared wrapper for its store; `spdk_blob_store` is SPDK's actual store handle; `bio_xs_blobstore` is one execution context's role-specific attachment; `bio_xs_context` represents that execution context. The short variable names `d_bdev`, `bbs`, `bs`, `bxb`, and `ctxt` typically refer to those five objects in that order.

**What changed:** the weight-helper include; the environment parsing in `create_bio_bdev()`; `validate_bio_weights()` and its startup call; weighted/capacity-aware selection and its diagnostics in `choose_device()`. Existing SPDK, hotplug, role-assignment, and teardown machinery is explained for context, not attributed to this patch.

## Function and section index

- [L1: Headers, constants, and process-wide state](#l1)
- [L102: bio_spdk_conf_read()](#l102)
- [L154: bio_spdk_env_init()](#l154)
- [L239: bypass_health_collect()](#l239)
- [L245: init_chk_cnt()](#l245)
- [L253: bio_nvme_init_ext()](#l253)
- [L434: bio_nvme_init()](#l434)
- [L442: bio_spdk_env_fini()](#l442)
- [L452: bio_nvme_fini()](#l452)
- [L465: is_bbs_owner()](#l465)
- [L473: init_thread()](#l473)
- [L479: init_xs_context()](#l479)
- [L485: is_server_started()](#l485)
- [L491: bio_bdev_list()](#l491)
- [L497: is_init_xstream()](#l497)
- [L504: default_cluster_sz()](#l504)
- [L510: bio_need_nvme_poll()](#l510)
- [L528: drain_inflight_ios()](#l528)
- [L549: common_prep_arg()](#l549)
- [L557: common_init_cb()](#l557)
- [L574: subsys_init_cb()](#l574)
- [L594: common_fini_cb()](#l594)
- [L603: common_bs_cb()](#l603)
- [L616: xs_poll_completion()](#l616)
- [L648: load_blobstore()](#l648)
- [L713: unload_blobstore()](#l713)
- [L730: free_bio_blobstore()](#l730)
- [L743: destroy_bio_bdev()](#l743)
- [L764: lookup_dev_by_id()](#l764)
- [L776: lookup_dev_by_name()](#l776)
- [L788: bio_release_bdev()](#l788)
- [L812: teardown_bio_bdev()](#l812)
- [L846: bio_bdev_event_cb()](#l846)
- [L896: replace_bio_bdev()](#l896)
- [L917: bdev_name2roles()](#l917)
- [L949: create_bio_bdev()](#l949)
- [L1092: validate_bio_weights()](#l1092)
- [L1128: init_bio_bdevs()](#l1128)
- [L1196: put_bio_blobstore()](#l1196)
- [L1243: fini_bio_bdevs()](#l1243)
- [L1254: alloc_bio_blobstore()](#l1254)
- [L1291: get_bio_blobstore()](#l1291)
- [L1319: is_role_match()](#l1319)
- [L1328: bio_nvme_configured()](#l1328)
- [L1340: choose_device()](#l1340)
- [L1404: alloc_xs_blobstore()](#l1404)
- [L1419: assign_roles()](#l1419)
- [L1485: assign_xs_bdev()](#l1485)
- [L1540: init_xs_blobstore_ctxt()](#l1540)
- [L1647: bio_xsctxt_free()](#l1647)
- [L1758: subsystem_init_cb()](#l1758)
- [L1776: load_config_cb()](#l1776)
- [L1788: bio_xsctxt_init_by_config()](#l1788)
- [L1828: bio_xsctxt_alloc()](#l1828)
- [L1985: bio_nvme_ctl()](#l1985)
- [L2004: reset_media_errors()](#l2004)
- [L2018: setup_bio_bdev()](#l2018)
- [L2062: scan_bio_bdevs()](#l2062)
- [L2162: bio_led_reset_on_timeout()](#l2162)
- [L2194: bio_nvme_poll()](#l2194)

Every source line appears once below, with its original number. Explanations refer to individual lines or short groups belonging to one statement or operation. Blank lines separate ideas; `{` and `}` open and close the surrounding scope. Copyright comments and repeated diagnostic formatting do not change runtime behavior.


<a id="l1"></a>

## L1–L101: Headers, constants, and process-wide state

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L1)

| Source lines | Explanation |
| --- | --- |
| L1–L8 | License/copyright information and the BIO logging category. This is production engine code, not a test program. |
| L10–L14 | Operating-system types/files, UUID operations, and Argobots synchronization declarations. |
| L15–L27 | SPDK environment, threads, devices, blobs, NVMe/VMD, RPC, file-loading, DPDK integration, and version declarations. These headers expose the asynchronous interfaces used below. |
| L28 | Internal BIO structures and helpers: device records, execution-stream contexts, health state, and storage roles. |
| L29 | Added weighting parser, exact comparator, and mapping-slot guard. These helpers are defined in `bio_weight.h`. |
| L30–L32 | SMD mapping APIs and generated protobuf declarations used by this module. |
| L34–L41 | Compile-time defaults: message-ring size, blobstore cluster size, initial DMA allocation percentage, and minimum/maximum chunk counts. `DAOS_MSG_RING_SZ` is defined but not referenced again in this file. |
| L43–L48 | Per-channel operation limit 4096, polling threshold 2048, and back-pressure threshold 4000. They concern pending I/O, not target assignment weights. |
| L50–L58 | Globals for DMA chunk size (in pages), maximum chunk count, NUMA placement, and initial allocation percentage. |
| L59–L69 | Runtime knobs: SCM RDMA, SPDK initialization flag, subsystem shutdown timeout, NVMe power management, unmap batching, maximum asynchronous size, and I/O timeout. Values are initialized here and may be overridden later. |
| L71–L74 | `bio_nvme_data` contains process-wide BIO state. Its Argobots mutex and condition variable coordinate initialization and teardown. |
| L75–L83 | Device class, number of attached execution streams, the designated initialization SPDK thread/context, and default blobstore options. |
| L84–L87 | The intrusive list of BIO device records and the timestamp of the last hotplug scan. |
| L88–L92 | Owned configuration-file path, memory budget, and configured storage-role bit mask. |
| L93–L98 | Lifecycle flag, health-collection bypass, and optional JSON-RPC enable/address fields. |
| L100–L101 | `nvme_glb` is the static process-wide instance; `glb_criteria` stores device auto-fault thresholds. These describe one engine process, not the whole cluster. |

<details>
<summary>Numbered source for this section</summary>

```text
   1  /**
   2   * (C) Copyright 2018-2024 Intel Corporation.
   3   * (C) Copyright 2025 Google LLC
   4   * (C) Copyright 2025-2026 Hewlett Packard Enterprise Development LP
   5   *
   6   * SPDX-License-Identifier: BSD-2-Clause-Patent
   7   */
   8  #define D_LOGFAC	DD_FAC(bio)
   9
  10  #include <sys/types.h>
  11  #include <sys/stat.h>
  12  #include <fcntl.h>
  13  #include <uuid/uuid.h>
  14  #include <abt.h>
  15  #include <spdk/log.h>
  16  #include <spdk/env.h>
  17  #include <spdk/init.h>
  18  #include <spdk/nvme.h>
  19  #include <spdk/vmd.h>
  20  #include <spdk/thread.h>
  21  #include <spdk/bdev.h>
  22  #include <spdk/blob_bdev.h>
  23  #include <spdk/blob.h>
  24  #include <spdk/rpc.h>
  25  #include <spdk/file.h>
  26  #include <spdk/env_dpdk.h>
  27  #include <spdk/version.h>
  28  #include "bio_internal.h"
  29  #include "bio_weight.h"
  30  #include <daos_srv/smd.h>
  31
  32  #include "smd.pb-c.h"
  33
  34  /* These Macros should be turned into DAOS configuration in the future */
  35  #define DAOS_MSG_RING_SZ	4096
  36  /* Default cluster size in MB */
  37  #define DAOS_DEFAULT_CLUSTER_MB 128
  38  /* DMA buffer parameters */
  39  #define DAOS_DMA_CHUNK_INIT_PCT 50      /* Default per-xstream init chunks, in percentage */
  40  #define DAOS_DMA_CHUNK_CNT_MAX	128	/* Default per-xstream max chunks, 1GB */
  41  #define DAOS_DMA_CHUNK_CNT_MIN	32	/* Per-xstream min chunks, 256MB */
  42
  43  /* Max in-flight blob IOs per io channel */
  44  #define BIO_BS_MAX_CHANNEL_OPS	(4096)
  45  /* Schedule a NVMe poll when so many blob IOs queued for an io channel */
  46  #define BIO_BS_POLL_WATERMARK	(2048)
  47  /* Stop issuing new IO when queued blob IOs reach a threshold */
  48  #define BIO_BS_STOP_WATERMARK	(4000)
  49
  50  /* Chunk size of DMA buffer in pages */
  51  unsigned int bio_chk_sz;
  52  /* Per-xstream maximum DMA buffer size (in chunk count) */
  53  unsigned int bio_chk_cnt_max;
  54  /* NUMA node affinity */
  55  unsigned int bio_numa_node;
  56  /* Per-xstream initial DMA buffer size (in percentage) */
  57  static unsigned int bio_chk_init_pct;
  58  /* Diret RDMA over SCM */
  59  bool bio_scm_rdma;
  60  /* Whether SPDK inited */
  61  bool                bio_spdk_inited;
  62  /* SPDK subsystem fini timeout */
  63  unsigned int bio_spdk_subsys_timeout = 25000;	/* ms */
  64  /* SPDK NVMe power management value, use bits 0-4 as per NVMe spec */
  65  unsigned int        bio_spdk_power_mgmt_val = NVME_POWER_MGMT_UNINIT;
  66  /* How many blob unmap calls can be called in a row */
  67  unsigned int bio_spdk_max_unmap_cnt = 32;
  68  unsigned int bio_max_async_sz = (1UL << 15) /* 32k */;
  69  unsigned int        bio_io_timeout         = 120000000; /* us, 120 seconds */
  70
  71  struct bio_nvme_data {
  72  	ABT_mutex		 bd_mutex;
  73  	ABT_cond		 bd_barrier;
  74  	/* SPDK bdev type */
  75  	int			 bd_bdev_class;
  76  	/* How many xstreams has initialized NVMe context */
  77  	int			 bd_xstream_cnt;
  78  	/* The thread responsible for SPDK bdevs init/fini */
  79  	struct spdk_thread	*bd_init_thread;
  80  	/* The xstream context for init thread */
  81  	struct bio_xs_context   *bd_init_xs;
  82  	/* Default SPDK blobstore options */
  83  	struct spdk_bs_opts	 bd_bs_opts;
  84  	/* All bdevs can be used by DAOS server */
  85  	d_list_t		 bd_bdevs;
  86  	uint64_t		 bd_scan_age;
  87  	/* Path to input SPDK JSON NVMe config file */
  88  	char			*bd_nvme_conf;
  89  	/* When using SPDK primary mode, specifies memory allocation in MB */
  90  	int			 bd_mem_size;
  91  	unsigned int		 bd_nvme_roles;
  92  	bool			 bd_started;
  93  	bool			 bd_bypass_health_collect;
  94  	/* Setting to enable SPDK JSON-RPC server */
  95  	bool			 bd_enable_rpc_srv;
  96  	const char		*bd_rpc_srv_addr;
  97  };
  98
  99  static struct bio_nvme_data nvme_glb;
 100  struct bio_faulty_criteria  glb_criteria;
 101
```

</details>


<a id="l102"></a>

## L102–L153: bio_spdk_conf_read()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L102)

| Source lines | Explanation |
| --- | --- |
| L102–L108 | Read the configured device/environment settings into the caller's SPDK options. Initialize temporary flags, role mask, and return code. |
| L110–L115 | Parse/allocate the allowed PCI-device configuration, propagate errors, and remember the configured role mask. The address of `roles` lets the helper write its output. |
| L117–L121 | Install the configured hotplug filter; fail initialization if that helper fails. |
| L123–L127 | Read acceleration properties and propagate any error. |
| L129–L134 | Read optional SPDK JSON-RPC settings, including the address stored in global state. |
| L135–L141 | In a release build, reject enabling this SPDK RPC server. Otherwise store the requested flag. This is the SPDK diagnostic/control endpoint, not DAOS's normal management service. |
| L143–L151 | Load automatic-fault criteria into the global thresholds; return the error or zero. |

<details>
<summary>Numbered source for this section</summary>

```text
 102  static int
 103  bio_spdk_conf_read(struct spdk_env_opts *opts)
 104  {
 105  	bool			enable_rpc_srv = false;
 106  	bool                    vmd_enabled    = false;
 107  	int			roles = 0;
 108  	int                     rc;
 109
 110  	rc = bio_add_allowed_alloc(nvme_glb.bd_nvme_conf, opts, &roles, &vmd_enabled);
 111  	if (rc != 0) {
 112  		DL_ERROR(rc, "Failed to add allowed devices to SPDK env");
 113  		return rc;
 114  	}
 115  	nvme_glb.bd_nvme_roles = roles;
 116
 117  	rc = bio_set_hotplug_filter(nvme_glb.bd_nvme_conf);
 118  	if (rc != 0) {
 119  		DL_ERROR(rc, "Failed to set hotplug filter");
 120  		return rc;
 121  	}
 122
 123  	rc = bio_read_accel_props(nvme_glb.bd_nvme_conf);
 124  	if (rc != 0) {
 125  		DL_ERROR(rc, "Failed to read acceleration properties");
 126  		return rc;
 127  	}
 128
 129  	rc = bio_read_rpc_srv_settings(nvme_glb.bd_nvme_conf, &enable_rpc_srv,
 130  				       &nvme_glb.bd_rpc_srv_addr);
 131  	if (rc != 0) {
 132  		DL_ERROR(rc, "Failed to read SPDK JSON-RPC server settings");
 133  		return rc;
 134  	}
 135  #ifdef DAOS_BUILD_RELEASE
 136  	if (enable_rpc_srv) {
 137  		D_ERROR("SPDK JSON-RPC server may not be enabled for release builds.\n");
 138  		return -DER_INVAL;
 139  	}
 140  #endif
 141  	nvme_glb.bd_enable_rpc_srv = enable_rpc_srv;
 142
 143  	rc = bio_read_auto_faulty_criteria(nvme_glb.bd_nvme_conf, &glb_criteria.fc_enabled,
 144  					   &glb_criteria.fc_max_io_errs,
 145  					   &glb_criteria.fc_max_csum_errs);
 146  	if (rc != 0) {
 147  		DL_ERROR(rc, "Failed to read NVMe auto-faulty criteria");
 148  		return rc;
 149  	}
 150
 151  	return 0;
 152  }
 153
```

</details>


<a id="l154"></a>

## L154–L238: bio_spdk_env_init()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L154)

| Source lines | Explanation |
| --- | --- |
| L154–L161 | Declare SPDK options and logging levels initialized to DAOS defaults. |
| L163–L169 | Read and validate the SPDK print level. The warning text references DPDK even though this branch validates SPDK; the assignment resets `spdk_level` correctly. This is existing code. |
| L171–L179 | Read and range-check the DPDK level; log the selected SPDK/DPDK values. |
| L181–L189 | Apply the SPDK print level and construct DPDK command-line options. Allocation failure goes to common cleanup. |
| L192–L195 | Set the options structure size, fill SPDK defaults, identify the application, and attach the DPDK option string. |
| L197–L202 | Existing TODO explains why the memory-size field is not set here. This comment does not execute anything. |
| L204–L210 | If an NVMe configuration is present, enrich the options with the device settings parsed by `bio_spdk_conf_read()`. |
| L212–L214 | Non-root execution selects virtual-address IOVA mode. This is an SPDK-environment workaround, not an I/O scheduling policy. |
| L216–L222 | Initialize/reinitialize the SPDK environment using the appropriate options argument. Convert its -1 failure into a DAOS error. |
| L224–L226 | Report the compiled SPDK version and remove the current thread's SPDK-set affinity. |
| L228–L236 | Initialize the SPDK thread library; tear down the environment if that fails. Free the temporary PCI allow-list and return status. |

<details>
<summary>Numbered source for this section</summary>

```text
 154  static int
 155  bio_spdk_env_init(void)
 156  {
 157  	struct spdk_env_opts opts;
 158  	const char          *dpdk_opts;
 159  	unsigned int         spdk_level = DAOS_SPDK_LOG_DEFAULT;
 160  	unsigned int         dpdk_level = DAOS_DPDK_LOG_DEFAULT;
 161  	int                  rc;
 162
 163  	/* Check for SPDK log level from environment */
 164  	d_getenv_uint("DAOS_SPDK_LOG_LEVEL", &spdk_level);
 165  	if (spdk_level > DAOS_SPDK_LOG_MAX) {
 166  		D_WARN("Invalid DAOS_DPDK_LOG_LEVEL=%u, using default (%u)\n", dpdk_level,
 167  		       DAOS_SPDK_LOG_DEFAULT);
 168  		spdk_level = DAOS_SPDK_LOG_DEFAULT;
 169  	}
 170
 171  	/* Check for DPDK log level from environment */
 172  	d_getenv_uint("DAOS_DPDK_LOG_LEVEL", &dpdk_level);
 173  	if (dpdk_level < DAOS_DPDK_LOG_MIN || dpdk_level > DAOS_DPDK_LOG_MAX) {
 174  		D_WARN("Invalid DAOS_DPDK_LOG_LEVEL=%u, using default (%u)\n", dpdk_level,
 175  		       DAOS_DPDK_LOG_DEFAULT);
 176  		dpdk_level = DAOS_DPDK_LOG_DEFAULT;
 177  	}
 178
 179  	D_INFO("SPDK log level: %u, DPDK log level: %u\n", spdk_level, dpdk_level);
 180
 181  	/* Set SPDK log print level to configured value */
 182  	spdk_log_set_print_level(spdk_level);
 183
 184  	/* Build DPDK options with specified log level for all DPDK log facilities */
 185  	dpdk_opts = dpdk_cli_build_opts(dpdk_level, dpdk_level);
 186  	if (dpdk_opts == NULL) {
 187  		D_ERROR("Failed to build DPDK options\n");
 188  		rc = -DER_NOMEM;
 189  		goto out;
 190  	}
 191
 192  	opts.opts_size = sizeof(opts);
 193  	spdk_env_opts_init(&opts);
 194  	opts.name = "daos_engine";
 195  	opts.env_context = (char *)dpdk_opts;
 196
 197  	/**
 198  	 * TODO: Set opts.mem_size to nvme_glb.bd_mem_size
 199  	 * Currently we can't guarantee clean shutdown (no hugepages leaked).
 200  	 * Setting mem_size could cause EAL: Not enough memory available error,
 201  	 * and DPDK will fail to initialize.
 202  	 */
 203
 204  	if (bio_nvme_configured(SMD_DEV_TYPE_MAX)) {
 205  		rc = bio_spdk_conf_read(&opts);
 206  		if (rc != 0) {
 207  			DL_ERROR(rc, "Failed to process nvme config");
 208  			goto out;
 209  		}
 210  	}
 211
 212  	if (geteuid() != 0) {
 213  		opts.iova_mode = "va"; // workaround for spdk issue #2683 when running as non-root
 214  	}
 215
 216  	/* Don't pass opt for reinitialization, otherwise it will fail */
 217  	rc = spdk_env_init(spdk_env_dpdk_external_init() ? &opts : NULL);
 218  	if (rc != 0) {
 219  		rc = -DER_INVAL; /* spdk_env_init() returns -1 */
 220  		D_ERROR("Failed to initialize SPDK env, "DF_RC"\n", DP_RC(rc));
 221  		goto out;
 222  	}
 223
 224  	D_INFO("Initialized " SPDK_VERSION_STRING "\n");
 225
 226  	spdk_unaffinitize_thread();
 227
 228  	rc = spdk_thread_lib_init(NULL, 0);
 229  	if (rc != 0) {
 230  		rc = -DER_INVAL;
 231  		D_ERROR("Failed to init SPDK thread lib, "DF_RC"\n", DP_RC(rc));
 232  		spdk_env_fini();
 233  	}
 234  out:
 235  	D_FREE(opts.pci_allowed);
 236  	return rc;
 237  }
 238
```

</details>


<a id="l239"></a>

## L239–L244: bypass_health_collect()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L239)

| Source lines | Explanation |
| --- | --- |
| L239–L243 | Return the configured health-collection bypass flag. No state changes. |

<details>
<summary>Numbered source for this section</summary>

```text
 239  bool
 240  bypass_health_collect()
 241  {
 242  	return nvme_glb.bd_bypass_health_collect;
 243  }
 244
```

</details>


<a id="l245"></a>

## L245–L252: init_chk_cnt()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L245)

| Source lines | Explanation |
| --- | --- |
| L245–L251 | Compute initial DMA chunks as maximum chunks times the configured percentage, using integer division. Guarantee at least one chunk even if rounding produces zero. |

<details>
<summary>Numbered source for this section</summary>

```text
 245  static inline unsigned int
 246  init_chk_cnt()
 247  {
 248  	unsigned init_cnt = (bio_chk_cnt_max * bio_chk_init_pct / 100);
 249
 250  	return (init_cnt == 0) ? 1 : init_cnt;
 251  }
 252
```

</details>


<a id="l253"></a>

## L253–L433: bio_nvme_init_ext()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L253)

| Source lines | Explanation |
| --- | --- |
| L253–L261 | Extended global initialization entry point. Parameters supply configuration, NUMA/memory/hugepage budgets, target count, health bypass, and whether SPDK itself should be initialized. |
| L263–L266 | Reject zero targets before dividing the memory budget by target count. |
| L268–L271 | A nonempty NVMe configuration requires a nonzero hugepage memory budget in this path. |
| L273–L281 | Reset lifecycle pointers/counters, choose initial flags, and initialize the global device-list head. |
| L283–L286 | Create the global mutex. Convert Argobots failure to a DAOS error and return. |
| L288–L292 | Create the condition variable. If this fails, jump to the label that frees the already-created mutex. |
| L294–L300 | Set default auto-fault criteria. Checksum-error auto-faulting is effectively deferred with a UINT32_MAX threshold, as the comment explains. |
| L302–L304 | Initialize DMA settings. Shift MiB to bytes with `<< 20`, then divide by page size using `>> BIO_DMA_PAGE_SHIFT`. |
| L306–L312 | Read SCM RDMA and NVMe power-management knobs and report them. These do not supply heterogeneous-device weights. |
| L314–L315 | Read/log the subsystem shutdown timeout in milliseconds. |
| L317–L320 | Read/log unmap batching; zero is interpreted as UINT32_MAX rather than “do no unmaps.” |
| L322–L323 | Read/log the maximum asynchronous data size. |
| L325–L333 | Read the I/O timeout in seconds. Only values 30–300 override the current default; multiply by one million to store microseconds. |
| L335–L341 | With no hugepage memory, leave DMA defaults in place and return without NVMe environment setup. |
| L343–L350 | Probe readability of the configuration file and close the descriptor if opened. The failure branch logs a warning; it does not itself return, despite the warning's wording. |
| L352–L358 | Require a positive hugepage size and calculate the per-target maximum DMA chunk count. Reject a budget below the minimum. |
| L360–L365 | Read initial preallocation percentage, reset zero or values at least 100 to the default, and report the derived DMA budget. |
| L367–L375 | Read/validate blobstore cluster size, initialize SPDK blobstore options, convert MiB to bytes, and set the maximum operations per channel. |
| L377–L382 | If `VOS_BDEV_CLASS` is `AIO` (case-insensitive), select the file-backed AIO class. Free the environment-string allocation afterward. |
| L384–L389 | Positive NUMA IDs select that node; -1 permits any node. Otherwise the earlier default of node 0 remains. |
| L391–L392 | When the caller requested settings-only initialization, return before initializing SPDK. |
| L394–L401 | Save the memory budget and duplicate the configuration path into owned storage. Allocation failure enters cleanup. |
| L403–L409 | Initialize the SPDK environment. On error, free the owned configuration string, clear the pointer, and release synchronization resources. |
| L411–L420 | Without metadata-on-SSD, override cluster size to 1 GiB for the documented PMem-mode performance reason. Report the final mode and cluster size. |
| L422–L424 | Mark SPDK successfully initialized and return zero. |
| L426–L431 | Cleanup labels release condition variable, then mutex, and return the saved error. Jump labels let callers skip freeing resources they never acquired. |

<details>
<summary>Numbered source for this section</summary>

```text
 253  int
 254  bio_nvme_init_ext(const char *nvme_conf, int numa_node, unsigned int mem_size,
 255  		  unsigned int hugepage_size, unsigned int tgt_nr, bool bypass_health_collect,
 256  		  bool init_spdk)
 257  {
 258  	char		*env;
 259  	int		 rc, fd;
 260  	unsigned int     size_mb = BIO_DMA_CHUNK_MB, io_timeout_secs = 0;
 261  	unsigned int     cluster_mb = DAOS_DEFAULT_CLUSTER_MB;
 262
 263  	if (tgt_nr <= 0) {
 264  		D_ERROR("tgt_nr: %u should be > 0\n", tgt_nr);
 265  		return -DER_INVAL;
 266  	}
 267
 268  	if (nvme_conf && strlen(nvme_conf) > 0 && mem_size == 0) {
 269  		D_ERROR("Hugepages must be configured when NVMe SSD is configured\n");
 270  		return -DER_INVAL;
 271  	}
 272
 273  	bio_numa_node = 0;
 274  	nvme_glb.bd_xstream_cnt = 0;
 275  	nvme_glb.bd_init_thread = NULL;
 276  	nvme_glb.bd_init_xs               = NULL;
 277  	nvme_glb.bd_nvme_conf = NULL;
 278  	nvme_glb.bd_bypass_health_collect = bypass_health_collect;
 279  	nvme_glb.bd_enable_rpc_srv = false;
 280  	nvme_glb.bd_rpc_srv_addr = NULL;
 281  	D_INIT_LIST_HEAD(&nvme_glb.bd_bdevs);
 282
 283  	rc = ABT_mutex_create(&nvme_glb.bd_mutex);
 284  	if (rc != ABT_SUCCESS) {
 285  		return dss_abterr2der(rc);
 286  	}
 287
 288  	rc = ABT_cond_create(&nvme_glb.bd_barrier);
 289  	if (rc != ABT_SUCCESS) {
 290  		rc = dss_abterr2der(rc);
 291  		goto free_mutex;
 292  	}
 293
 294  	glb_criteria.fc_enabled     = true;
 295  	glb_criteria.fc_max_io_errs = 10;
 296  	/*
 297  	 * FIXME: Don't enable csum error criterion by default otherwise targets will be
 298  	 *	  unexpectedly down in CSUM tests.
 299  	 */
 300  	glb_criteria.fc_max_csum_errs = UINT32_MAX;
 301
 302  	bio_chk_init_pct = DAOS_DMA_CHUNK_INIT_PCT;
 303  	bio_chk_cnt_max = DAOS_DMA_CHUNK_CNT_MAX;
 304  	bio_chk_sz = ((uint64_t)size_mb << 20) >> BIO_DMA_PAGE_SHIFT;
 305
 306  	d_getenv_bool("DAOS_SCM_RDMA_ENABLED", &bio_scm_rdma);
 307  	D_INFO("RDMA to SCM is %s\n", bio_scm_rdma ? "enabled" : "disabled");
 308
 309  	d_getenv_uint("DAOS_NVME_POWER_MGMT", &bio_spdk_power_mgmt_val);
 310  	if (bio_spdk_power_mgmt_val != NVME_POWER_MGMT_UNINIT)
 311  		D_INFO("NVMe power management setting to be applied is %u\n",
 312  		       bio_spdk_power_mgmt_val);
 313
 314  	d_getenv_uint("DAOS_SPDK_SUBSYS_TIMEOUT", &bio_spdk_subsys_timeout);
 315  	D_INFO("SPDK subsystem fini timeout is %u ms\n", bio_spdk_subsys_timeout);
 316
 317  	d_getenv_uint("DAOS_SPDK_MAX_UNMAP_CNT", &bio_spdk_max_unmap_cnt);
 318  	if (bio_spdk_max_unmap_cnt == 0)
 319  		bio_spdk_max_unmap_cnt = UINT32_MAX;
 320  	D_INFO("SPDK batch blob unmap call count is %u\n", bio_spdk_max_unmap_cnt);
 321
 322  	d_getenv_uint("DAOS_MAX_ASYNC_SZ", &bio_max_async_sz);
 323  	D_INFO("Max async data size is set to %u bytes\n", bio_max_async_sz);
 324
 325  	d_getenv_uint("DAOS_SPDK_IO_TIMEOUT", &io_timeout_secs);
 326  	if (io_timeout_secs > 0) {
 327  		if (io_timeout_secs < 30 || io_timeout_secs > 300)
 328  			D_WARN("DAOS_SPDK_IO_TIMEOUT(%u) is invalid. Min:30,Max:300,Default:120\n",
 329  			       io_timeout_secs);
 330  		else
 331  			bio_io_timeout = io_timeout_secs * 1000000; /* convert to us */
 332  	}
 333  	D_INFO("SPDK IO timeout set to %u us\n", bio_io_timeout);
 334
 335  	/* Hugepages disabled */
 336  	if (mem_size == 0) {
 337  		D_INFO("Set per-xstream DMA buffer upper bound to %u %uMB chunks\n",
 338  			bio_chk_cnt_max, size_mb);
 339  		D_INFO("Hugepages are not specified, skip NVMe setup.\n");
 340  		return 0;
 341  	}
 342
 343  	if (nvme_conf && strlen(nvme_conf) > 0) {
 344  		fd = open(nvme_conf, O_RDONLY);
 345  		if (fd < 0)
 346  			D_WARN("Open %s failed, skip DAOS NVMe setup "DF_RC"\n",
 347  			       nvme_conf, DP_RC(daos_errno2der(errno)));
 348  		else
 349  			close(fd);
 350  	}
 351
 352  	D_ASSERT(hugepage_size > 0);
 353  	bio_chk_cnt_max = (mem_size / tgt_nr) / size_mb;
 354  	if (bio_chk_cnt_max < DAOS_DMA_CHUNK_CNT_MIN) {
 355  		D_ERROR("%uMB hugepages are not enough for %u targets (256MB per target)\n",
 356  			mem_size, tgt_nr);
 357  		return -DER_INVAL;
 358  	}
 359
 360  	d_getenv_uint("DAOS_DMA_INIT_PCT", &bio_chk_init_pct);
 361  	if (bio_chk_init_pct == 0 || bio_chk_init_pct >= 100)
 362  		bio_chk_init_pct = DAOS_DMA_CHUNK_INIT_PCT;
 363
 364  	D_INFO("Set per-xstream DMA buffer upper bound to %u %uMB chunks, prealloc %u chunks\n",
 365  	       bio_chk_cnt_max, size_mb, init_chk_cnt());
 366
 367  	d_getenv_uint("DAOS_BS_CLUSTER_MB", &cluster_mb);
 368  	if (cluster_mb < 32 || cluster_mb > 1024) {
 369  		D_WARN("DAOS_BS_CLUSTER_MB %u is invalid, default %u is used\n", cluster_mb,
 370  		       DAOS_DEFAULT_CLUSTER_MB);
 371  		cluster_mb = DAOS_DEFAULT_CLUSTER_MB;
 372  	}
 373  	spdk_bs_opts_init(&nvme_glb.bd_bs_opts, sizeof(nvme_glb.bd_bs_opts));
 374  	nvme_glb.bd_bs_opts.cluster_sz      = (cluster_mb << 20);
 375  	nvme_glb.bd_bs_opts.max_channel_ops = BIO_BS_MAX_CHANNEL_OPS;
 376
 377  	d_agetenv_str(&env, "VOS_BDEV_CLASS");
 378  	if (env && strcasecmp(env, "AIO") == 0) {
 379  		D_WARN("AIO device(s) will be used!\n");
 380  		nvme_glb.bd_bdev_class = BDEV_CLASS_AIO;
 381  	}
 382  	d_freeenv_str(&env);
 383
 384  	if (numa_node > 0) {
 385  		bio_numa_node = (unsigned int)numa_node;
 386  	} else if (numa_node == -1) {
 387  		D_WARN("DMA buffer will be allocated from any NUMA node available\n");
 388  		bio_numa_node = SPDK_ENV_SOCKET_ID_ANY;
 389  	}
 390
 391  	if (!init_spdk)
 392  		return 0;
 393
 394  	nvme_glb.bd_mem_size = mem_size;
 395  	if (nvme_conf) {
 396  		D_STRNDUP(nvme_glb.bd_nvme_conf, nvme_conf, strlen(nvme_conf));
 397  		if (nvme_glb.bd_nvme_conf == NULL) {
 398  			rc = -DER_NOMEM;
 399  			goto free_cond;
 400  		}
 401  	}
 402
 403  	rc = bio_spdk_env_init();
 404  	if (rc) {
 405  		D_ERROR("Failed to init SPDK environment\n");
 406  		D_FREE(nvme_glb.bd_nvme_conf);
 407  		nvme_glb.bd_nvme_conf = NULL;
 408  		goto free_cond;
 409  	}
 410
 411  	/*
 412  	 * Let's keep using large cluster size(1GB) for pmem mode, the SPDK blobstore
 413  	 * loading time is unexpected long for smaller cluster size(32MB), see DAOS-13694.
 414  	 */
 415  	if (!bio_nvme_configured(SMD_DEV_TYPE_META))
 416  		nvme_glb.bd_bs_opts.cluster_sz = (1UL << 30);	/* 1GB */
 417
 418  	D_INFO("MD on SSD is %s, %u cluster size is used\n",
 419  	       bio_nvme_configured(SMD_DEV_TYPE_META) ? "enabled" : "disabled",
 420  	       nvme_glb.bd_bs_opts.cluster_sz);
 421
 422  	bio_spdk_inited = true;
 423
 424  	return 0;
 425
 426  free_cond:
 427  	ABT_cond_free(&nvme_glb.bd_barrier);
 428  free_mutex:
 429  	ABT_mutex_free(&nvme_glb.bd_mutex);
 430
 431  	return rc;
 432  }
 433
```

</details>


<a id="l434"></a>

## L434–L441: bio_nvme_init()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L434)

| Source lines | Explanation |
| --- | --- |
| L434–L440 | Public convenience wrapper: forward every parameter and request full SPDK initialization with the final `true`. |

<details>
<summary>Numbered source for this section</summary>

```text
 434  int
 435  bio_nvme_init(const char *nvme_conf, int numa_node, unsigned int mem_size,
 436  	      unsigned int hugepage_size, unsigned int tgt_nr, bool bypass_health_collect)
 437  {
 438  	return bio_nvme_init_ext(nvme_conf, numa_node, mem_size, hugepage_size, tgt_nr,
 439  				 bypass_health_collect, true);
 440  }
 441
```

</details>


<a id="l442"></a>

## L442–L451: bio_spdk_env_fini()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L442)

| Source lines | Explanation |
| --- | --- |
| L442–L450 | If SPDK was initialized, finalize its thread library and environment, then clear the flag. The guard avoids repeating this teardown. |

<details>
<summary>Numbered source for this section</summary>

```text
 442  static void
 443  bio_spdk_env_fini(void)
 444  {
 445  	if (bio_spdk_inited) {
 446  		spdk_thread_lib_fini();
 447  		spdk_env_fini();
 448  		bio_spdk_inited = false;
 449  	}
 450  }
 451
```

</details>


<a id="l452"></a>

## L452–L464: bio_nvme_fini()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L452)

| Source lines | Explanation |
| --- | --- |
| L452–L462 | Finish global NVMe state: finalize SPDK, free synchronization objects, assert every xstream/device has already been released, then free the configuration path. |

<details>
<summary>Numbered source for this section</summary>

```text
 452  void
 453  bio_nvme_fini(void)
 454  {
 455  	bio_spdk_env_fini();
 456  	ABT_cond_free(&nvme_glb.bd_barrier);
 457  	ABT_mutex_free(&nvme_glb.bd_mutex);
 458  	D_ASSERT(nvme_glb.bd_xstream_cnt == 0);
 459  	D_ASSERT(nvme_glb.bd_init_thread == NULL);
 460  	D_ASSERT(nvme_glb.bd_init_xs == NULL);
 461  	D_ASSERT(d_list_empty(&nvme_glb.bd_bdevs));
 462  	D_FREE(nvme_glb.bd_nvme_conf);
 463  }
 464
```

</details>


<a id="l465"></a>

## L465–L472: is_bbs_owner()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L465)

| Source lines | Explanation |
| --- | --- |
| L465–L471 | Check whether this execution-stream context owns the shared BIO blobstore. Assertions require both pointers to be valid; equality compares pointer identity. |

<details>
<summary>Numbered source for this section</summary>

```text
 465  static inline bool
 466  is_bbs_owner(struct bio_xs_context *ctxt, struct bio_blobstore *bbs)
 467  {
 468  	D_ASSERT(ctxt != NULL);
 469  	D_ASSERT(bbs != NULL);
 470  	return bbs->bb_owner_xs == ctxt;
 471  }
 472
```

</details>


<a id="l473"></a>

## L473–L478: init_thread()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L473)

| Source lines | Explanation |
| --- | --- |
| L473–L477 | Return the SPDK thread designated for global initialization and hotplug work. |

<details>
<summary>Numbered source for this section</summary>

```text
 473  inline struct spdk_thread *
 474  init_thread(void)
 475  {
 476  	return nvme_glb.bd_init_thread;
 477  }
 478
```

</details>


<a id="l479"></a>

## L479–L484: init_xs_context()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L479)

| Source lines | Explanation |
| --- | --- |
| L479–L483 | Return that thread's enclosing BIO execution-stream context. |

<details>
<summary>Numbered source for this section</summary>

```text
 479  inline struct bio_xs_context *
 480  init_xs_context(void)
 481  {
 482  	return nvme_glb.bd_init_xs;
 483  }
 484
```

</details>


<a id="l485"></a>

## L485–L490: is_server_started()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L485)

| Source lines | Explanation |
| --- | --- |
| L485–L489 | Return the engine-started lifecycle flag used to suppress hotplug/health transitions during startup and shutdown. |

<details>
<summary>Numbered source for this section</summary>

```text
 485  inline bool
 486  is_server_started(void)
 487  {
 488  	return nvme_glb.bd_started;
 489  }
 490
```

</details>


<a id="l491"></a>

## L491–L496: bio_bdev_list()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L491)

| Source lines | Explanation |
| --- | --- |
| L491–L495 | Return the address of the global device-list head, not a copy of the device list. |

<details>
<summary>Numbered source for this section</summary>

```text
 491  inline d_list_t *
 492  bio_bdev_list(void)
 493  {
 494  	return &nvme_glb.bd_bdevs;
 495  }
 496
```

</details>


<a id="l497"></a>

## L497–L503: is_init_xstream()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L497)

| Source lines | Explanation |
| --- | --- |
| L497–L502 | Require a context, then compare its SPDK thread with the initialization thread. |

<details>
<summary>Numbered source for this section</summary>

```text
 497  inline bool
 498  is_init_xstream(struct bio_xs_context *ctxt)
 499  {
 500  	D_ASSERT(ctxt != NULL);
 501  	return ctxt->bxc_thread == nvme_glb.bd_init_thread;
 502  }
 503
```

</details>


<a id="l504"></a>

## L504–L509: default_cluster_sz()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L504)

| Source lines | Explanation |
| --- | --- |
| L504–L508 | Return the configured SPDK blobstore cluster size in bytes. |

<details>
<summary>Numbered source for this section</summary>

```text
 504  inline uint32_t
 505  default_cluster_sz(void)
 506  {
 507  	return nvme_glb.bd_bs_opts.cluster_sz;
 508  }
 509
```

</details>


<a id="l510"></a>

## L510–L527: bio_need_nvme_poll()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L510)

| Source lines | Explanation |
| --- | --- |
| L510–L517 | Decide whether a context needs more NVMe polling. A NULL context needs none. |
| L519–L525 | Check each role's per-xstream blobstore; return true if its pending blob read/write count exceeds 2048. If none do, return false. |

<details>
<summary>Numbered source for this section</summary>

```text
 510  bool
 511  bio_need_nvme_poll(struct bio_xs_context *ctxt)
 512  {
 513  	enum smd_dev_type	 st;
 514  	struct bio_xs_blobstore	*bxb;
 515
 516  	if (ctxt == NULL)
 517  		return false;
 518
 519  	for (st = SMD_DEV_TYPE_DATA; st < SMD_DEV_TYPE_MAX; st++) {
 520  		bxb = ctxt->bxc_xs_blobstores[st];
 521  		if (bxb && bxb->bxb_blob_rw > BIO_BS_POLL_WATERMARK)
 522  			return true;
 523  	}
 524
 525  	return false;
 526  }
 527
```

</details>


<a id="l528"></a>

## L528–L548: drain_inflight_ios()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L528)

| Source lines | Explanation |
| --- | --- |
| L528–L533 | Skip draining when context/blobstore is absent or pending count is at most the polling watermark. |
| L535–L540 | Otherwise poll directly for self-polling contexts, or yield to the normal scheduler. Repeat while pending I/O is at least 4000. Because this is `do/while`, counts between 2049 and 3999 still cause one poll/yield. |
| L543–L547 | Shared callback result structure: one pending-operation counter, its eventual DAOS status, and an optional returned blobstore pointer. It is unrelated to SMD target counts. |

<details>
<summary>Numbered source for this section</summary>

```text
 528  void
 529  drain_inflight_ios(struct bio_xs_context *ctxt, struct bio_xs_blobstore *bxb)
 530  {
 531
 532  	if (ctxt == NULL || bxb == NULL || bxb->bxb_blob_rw <= BIO_BS_POLL_WATERMARK)
 533  		return;
 534
 535  	do {
 536  		if (ctxt->bxc_self_polling)
 537  			spdk_thread_poll(ctxt->bxc_thread, 0, 0);
 538  		else
 539  			bio_yield(NULL);
 540  	} while (bxb->bxb_blob_rw >= BIO_BS_STOP_WATERMARK);
 541  }
 542
 543  struct common_cp_arg {
 544  	unsigned int		 cca_inflights;
 545  	int			 cca_rc;
 546  	struct spdk_blob_store	*cca_bs;
 547  };
 548
```

</details>


<a id="l549"></a>

## L549–L556: common_prep_arg()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L549)

| Source lines | Explanation |
| --- | --- |
| L549–L555 | Prepare one outstanding asynchronous operation: pending=1, status=0, result=NULL. |

<details>
<summary>Numbered source for this section</summary>

```text
 549  static void
 550  common_prep_arg(struct common_cp_arg *arg)
 551  {
 552  	arg->cca_inflights = 1;
 553  	arg->cca_rc = 0;
 554  	arg->cca_bs = NULL;
 555  }
 556
```

</details>


<a id="l557"></a>

## L557–L573: common_init_cb()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L557)

| Source lines | Explanation |
| --- | --- |
| L557–L565 | Completion callback casts its opaque context, asserts a single unfinished operation, decrements the counter, and translates SPDK's error into DAOS's error namespace. |
| L568–L572 | Configuration-initialization callback context owns JSON bytes and their length, plus a pointer to the outer completion state. |

<details>
<summary>Numbered source for this section</summary>

```text
 557  static void
 558  common_init_cb(void *arg, int rc)
 559  {
 560  	struct common_cp_arg *cp_arg = arg;
 561
 562  	D_ASSERT(cp_arg->cca_inflights == 1);
 563  	D_ASSERT(cp_arg->cca_rc == 0);
 564  	cp_arg->cca_inflights--;
 565  	cp_arg->cca_rc = daos_errno2der(-rc);
 566  }
 567
 568  struct subsystem_init_arg {
 569  	struct common_cp_arg *cp_arg;
 570  	void                 *json_data;
 571  	ssize_t               json_data_size;
 572  };
 573
```

</details>


<a id="l574"></a>

## L574–L593: subsys_init_cb()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L574)

| Source lines | Explanation |
| --- | --- |
| L574–L582 | Final subsystem-init callback recovers its context, frees loaded JSON, and clears that pointer. |
| L584–L591 | Log any failure, complete the outer pending operation through `common_init_cb`, free the callback context, and return. |

<details>
<summary>Numbered source for this section</summary>

```text
 574  static void
 575  subsys_init_cb(int rc, void *arg)
 576  {
 577  	struct subsystem_init_arg *init_arg = arg;
 578
 579  	if (init_arg->json_data != NULL) {
 580  		free(init_arg->json_data);
 581  		init_arg->json_data = NULL;
 582  	}
 583
 584  	if (rc)
 585  		D_ERROR("subsystem init failed: %d\n", rc);
 586
 587  	common_init_cb(init_arg->cp_arg, rc);
 588
 589  	D_FREE(init_arg);
 590
 591  	return;
 592  }
 593
```

</details>


<a id="l594"></a>

## L594–L602: common_fini_cb()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L594)

| Source lines | Explanation |
| --- | --- |
| L594–L601 | Finalization callback has no status argument. It requires one outstanding operation and marks it complete by decrementing the counter. |

<details>
<summary>Numbered source for this section</summary>

```text
 594  static void
 595  common_fini_cb(void *arg)
 596  {
 597  	struct common_cp_arg *cp_arg = arg;
 598
 599  	D_ASSERT(cp_arg->cca_inflights == 1);
 600  	cp_arg->cca_inflights--;
 601  }
 602
```

</details>


<a id="l603"></a>

## L603–L615: common_bs_cb()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L603)

| Source lines | Explanation |
| --- | --- |
| L603–L613 | Blobstore callback validates the initial result state, marks completion, converts the status, and stores the returned SPDK blobstore pointer. |

<details>
<summary>Numbered source for this section</summary>

```text
 603  static void
 604  common_bs_cb(void *arg, struct spdk_blob_store *bs, int rc)
 605  {
 606  	struct common_cp_arg *cp_arg = arg;
 607
 608  	D_ASSERT(cp_arg->cca_inflights == 1);
 609  	D_ASSERT(cp_arg->cca_rc == 0);
 610  	D_ASSERT(cp_arg->cca_bs == NULL);
 611  	cp_arg->cca_inflights--;
 612  	cp_arg->cca_rc = daos_errno2der(-rc);
 613  	cp_arg->cca_bs = bs;
 614  }
 615
```

</details>


<a id="l616"></a>

## L616–L647: xs_poll_completion()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L616)

| Source lines | Explanation |
| --- | --- |
| L616–L626 | Polling helper accepts a context, address of an outstanding-operation counter, and an optional timeout. It initializes the start time only if timeout is nonzero. |
| L628–L634 | Repeatedly poll the context's SPDK thread so callbacks can execute. Return success once the callback reduces the counter to zero. |
| L636–L641 | With a timeout, compare current coarse time against start+timeout and return `-DER_TIMEDOUT` if exceeded. Zero timeout means no timeout check. |
| L645 | An already-zero counter also succeeds without polling. |

<details>
<summary>Numbered source for this section</summary>

```text
 616  int
 617  xs_poll_completion(struct bio_xs_context *ctxt, unsigned int *inflights,
 618  		   uint64_t timeout)
 619  {
 620  	uint64_t	start_time, cur_time;
 621
 622  	D_ASSERT(inflights != NULL);
 623  	D_ASSERT(ctxt != NULL);
 624
 625  	if (timeout != 0)
 626  		start_time = daos_getmtime_coarse();
 627
 628  	/* Wait for the completion callback done or timeout */
 629  	while (*inflights != 0) {
 630  		spdk_thread_poll(ctxt->bxc_thread, 0, 0);
 631
 632  		/* Completion is executed */
 633  		if (*inflights == 0)
 634  			return 0;
 635
 636  		/* Timeout */
 637  		if (timeout != 0) {
 638  			cur_time = daos_getmtime_coarse();
 639
 640  			if (cur_time > (start_time + timeout))
 641  				return -DER_TIMEDOUT;
 642  		}
 643  	}
 644
 645  	return 0;
 646  }
 647
```

</details>


<a id="l648"></a>

## L648–L712: load_blobstore()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L648)

| Source lines | Explanation |
| --- | --- |
| L648–L657 | Load or create a blobstore. `create` chooses initialization versus opening; `async` chooses callback delivery versus polling to completion. The supplied callback receives the result in asynchronous mode. |
| L659–L669 | Wrap the named SPDK block device as a blobstore device, with an event callback. SPDK owns cleanup of this wrapper after successful transfer to init/load or their error paths. |
| L671–L681 | Copy default options and use the 16-byte blobstore type field as the DAOS device UUID. No UUID means clear the type filter; a UUID means copy those exact bytes. |
| L683–L691 | Asynchronous path: require a callback, submit either init or load, and return NULL immediately. Here NULL is not by itself an error: the result arrives through the callback. |
| L694–L700 | Synchronous wrapper: submit with a stack-local completion structure, then keep polling with no timeout until the callback completes. SPDK's operation is still asynchronous underneath. |
| L702–L710 | On callback failure log the init/load status and return NULL. Otherwise assert a valid result and return the loaded blobstore. |

<details>
<summary>Numbered source for this section</summary>

```text
 648  struct spdk_blob_store *
 649  load_blobstore(struct bio_xs_context *ctxt, char *bdev_name, uuid_t *bs_uuid,
 650  	       bool create, bool async,
 651  	       void (*async_cb)(void *arg, struct spdk_blob_store *bs, int rc),
 652  	       void *async_arg)
 653  {
 654  	struct spdk_bs_dev	*bs_dev;
 655  	struct spdk_bs_opts	 bs_opts;
 656  	struct common_cp_arg	 cp_arg;
 657  	int			 rc;
 658
 659  	/*
 660  	 * bdev will be closed and bs_dev will be freed during
 661  	 * spdk_bs_unload(), or in the internal error handling code of
 662  	 * spdk_bs_init/load().
 663  	 */
 664  	rc = spdk_bdev_create_bs_dev_ext(bdev_name, bio_bdev_event_cb, NULL,
 665  					 &bs_dev);
 666  	if (rc != 0) {
 667  		D_ERROR("failed to create bs_dev %s, %d\n", bdev_name, rc);
 668  		return NULL;
 669  	}
 670
 671  	bs_opts = nvme_glb.bd_bs_opts;
 672  	/*
 673  	 * A little hack here, we store a UUID in the 16 bytes 'bstype' and
 674  	 * use it as the block device ID.
 675  	 */
 676  	D_ASSERT(SPDK_BLOBSTORE_TYPE_LENGTH == 16);
 677  	if (bs_uuid == NULL)
 678  		strncpy(bs_opts.bstype.bstype, "", SPDK_BLOBSTORE_TYPE_LENGTH);
 679  	else
 680  		memcpy(bs_opts.bstype.bstype, bs_uuid,
 681  		       SPDK_BLOBSTORE_TYPE_LENGTH);
 682
 683  	if (async) {
 684  		D_ASSERT(async_cb != NULL);
 685
 686  		if (create)
 687  			spdk_bs_init(bs_dev, &bs_opts, async_cb, async_arg);
 688  		else
 689  			spdk_bs_load(bs_dev, &bs_opts, async_cb, async_arg);
 690
 691  		return NULL;
 692  	}
 693
 694  	common_prep_arg(&cp_arg);
 695  	if (create)
 696  		spdk_bs_init(bs_dev, &bs_opts, common_bs_cb, &cp_arg);
 697  	else
 698  		spdk_bs_load(bs_dev, &bs_opts, common_bs_cb, &cp_arg);
 699  	rc = xs_poll_completion(ctxt, &cp_arg.cca_inflights, 0);
 700  	D_ASSERT(rc == 0);
 701
 702  	if (cp_arg.cca_rc != 0) {
 703  		D_CDEBUG(bs_uuid == NULL, DB_IO, DLOG_ERR,
 704  			 "%s blobstore failed %d\n", create ? "init" : "load",
 705  			 cp_arg.cca_rc);
 706  		return NULL;
 707  	}
 708
 709  	D_ASSERT(cp_arg.cca_bs != NULL);
 710  	return cp_arg.cca_bs;
 711  }
 712
```

</details>


<a id="l713"></a>

## L713–L729: unload_blobstore()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L713)

| Source lines | Explanation |
| --- | --- |
| L713–L727 | Submit asynchronous unload, poll until its callback completes, report any callback error, and return that error. The polling return and operation return are distinct. |

<details>
<summary>Numbered source for this section</summary>

```text
 713  int
 714  unload_blobstore(struct bio_xs_context *ctxt, struct spdk_blob_store *bs)
 715  {
 716  	struct common_cp_arg	cp_arg;
 717  	int			rc;
 718
 719  	common_prep_arg(&cp_arg);
 720  	spdk_bs_unload(bs, common_init_cb, &cp_arg);
 721  	rc = xs_poll_completion(ctxt, &cp_arg.cca_inflights, 0);
 722  	D_ASSERT(rc == 0);
 723
 724  	if (cp_arg.cca_rc != 0)
 725  		D_ERROR("failed to unload blobstore %d\n", cp_arg.cca_rc);
 726
 727  	return cp_arg.cca_rc;
 728  }
 729
```

</details>


<a id="l730"></a>

## L730–L742: free_bio_blobstore()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L730)

| Source lines | Explanation |
| --- | --- |
| L730–L740 | Free a BIO blobstore wrapper only after the SPDK store is closed and reference count is zero. Release condition variable, mutex, context array, and wrapper allocation. |

<details>
<summary>Numbered source for this section</summary>

```text
 730  static void
 731  free_bio_blobstore(struct bio_blobstore *bb)
 732  {
 733  	D_ASSERT(bb->bb_bs == NULL);
 734  	D_ASSERT(bb->bb_ref == 0);
 735
 736  	ABT_cond_free(&bb->bb_barrier);
 737  	ABT_mutex_free(&bb->bb_mutex);
 738  	D_FREE(bb->bb_xs_ctxts);
 739
 740  	D_FREE(bb);
 741  }
 742
```

</details>


<a id="l743"></a>

## L743–L763: destroy_bio_bdev()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L743)

| Source lines | Explanation |
| --- | --- |
| L743–L747 | Destroy a BIO device only after removal from its list and when no replacement operation is active. |
| L749–L752 | Close any held SPDK bdev descriptor and clear the pointer. |
| L754–L761 | Free the attached BIO blobstore wrapper if present, then the owned device-name string and the device record. This is in-memory cleanup, not SMD record deletion. |

<details>
<summary>Numbered source for this section</summary>

```text
 743  void
 744  destroy_bio_bdev(struct bio_bdev *d_bdev)
 745  {
 746  	D_ASSERT(d_list_empty(&d_bdev->bb_link));
 747  	D_ASSERT(!d_bdev->bb_replacing);
 748
 749  	if (d_bdev->bb_desc != NULL) {
 750  		spdk_bdev_close(d_bdev->bb_desc);
 751  		d_bdev->bb_desc = NULL;
 752  	}
 753
 754  	if (d_bdev->bb_blobstore != NULL) {
 755  		free_bio_blobstore(d_bdev->bb_blobstore);
 756  		d_bdev->bb_blobstore = NULL;
 757  	}
 758
 759  	D_FREE(d_bdev->bb_name);
 760
 761  	D_FREE(d_bdev);
 762  }
 763
```

</details>


<a id="l764"></a>

## L764–L775: lookup_dev_by_id()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L764)

| Source lines | Explanation |
| --- | --- |
| L764–L773 | Linear search of BIO's device list by persisted UUID. Return the matching record or NULL. UUID equality differs from SPDK name equality. |

<details>
<summary>Numbered source for this section</summary>

```text
 764  struct bio_bdev *
 765  lookup_dev_by_id(uuid_t dev_id)
 766  {
 767  	struct bio_bdev	*d_bdev;
 768
 769  	d_list_for_each_entry(d_bdev, &nvme_glb.bd_bdevs, bb_link) {
 770  		if (uuid_compare(d_bdev->bb_uuid, dev_id) == 0)
 771  			return d_bdev;
 772  	}
 773  	return NULL;
 774  }
 775
```

</details>


<a id="l776"></a>

## L776–L787: lookup_dev_by_name()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L776)

| Source lines | Explanation |
| --- | --- |
| L776–L785 | Linear search by exact current SPDK device name; return the matching record or NULL. |

<details>
<summary>Numbered source for this section</summary>

```text
 776  static struct bio_bdev *
 777  lookup_dev_by_name(const char *bdev_name)
 778  {
 779  	struct bio_bdev	*d_bdev;
 780
 781  	d_list_for_each_entry(d_bdev, &nvme_glb.bd_bdevs, bb_link) {
 782  		if (strcmp(d_bdev->bb_name, bdev_name) == 0)
 783  			return d_bdev;
 784  	}
 785  	return NULL;
 786  }
 787
```

</details>


<a id="l788"></a>

## L788–L811: bio_release_bdev()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L788)

| Source lines | Explanation |
| --- | --- |
| L788–L800 | Release callback recovers its device pointer, skips startup/shutdown, and returns if the descriptor is already closed. |
| L802–L809 | Close the descriptor only for an actually removed device. Fault teardown alone does not necessarily mean physical removal. |

<details>
<summary>Numbered source for this section</summary>

```text
 788  void
 789  bio_release_bdev(void *arg)
 790  {
 791  	struct bio_bdev	*d_bdev = arg;
 792
 793  	if (!is_server_started()) {
 794  		D_INFO("Skip device release on server start/shutdown\n");
 795  		return;
 796  	}
 797
 798  	D_ASSERT(d_bdev != NULL);
 799  	if (d_bdev->bb_desc == NULL)
 800  		return;
 801
 802  	/*
 803  	 * It could be called from faulty device teardown procedure, where
 804  	 * the device is still plugged.
 805  	 */
 806  	if (d_bdev->bb_removed) {
 807  		spdk_bdev_close(d_bdev->bb_desc);
 808  		d_bdev->bb_desc = NULL;
 809  	}
 810  }
 811
```

</details>


<a id="l812"></a>

## L812–L845: teardown_bio_bdev()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L812)

| Source lines | Explanation |
| --- | --- |
| L812–L822 | Device-teardown callback obtains the associated BIO blobstore and skips state transitions during startup/shutdown. |
| L824–L829 | A NORMAL or SETUP blobstore is moved to TEARDOWN; state-transition success is asserted. |
| L830–L839 | An already-OUT blobstore schedules descriptor release on the initialization thread, then deliberately falls through to the diagnostic shared by FAULTY/TEARDOWN states. |
| L840–L842 | Report an unexpected state. This switch does not itself migrate data or rebuild an object. |

<details>
<summary>Numbered source for this section</summary>

```text
 812  static void
 813  teardown_bio_bdev(void *arg)
 814  {
 815  	struct bio_bdev		*d_bdev = arg;
 816  	struct bio_blobstore	*bbs = d_bdev->bb_blobstore;
 817  	int			 rc;
 818
 819  	if (!is_server_started()) {
 820  		D_INFO("Skip device teardown on server start/shutdown\n");
 821  		return;
 822  	}
 823
 824  	switch (bbs->bb_state) {
 825  	case BIO_BS_STATE_NORMAL:
 826  	case BIO_BS_STATE_SETUP:
 827  		rc = bio_bs_state_set(bbs, BIO_BS_STATE_TEARDOWN);
 828  		D_ASSERT(rc == 0);
 829  		break;
 830  	case BIO_BS_STATE_OUT:
 831  		D_ASSERT(init_thread() != NULL);
 832  		spdk_thread_send_msg(init_thread(), bio_release_bdev, bbs->bb_dev);
 833  		/* fallthrough */
 834  	case BIO_BS_STATE_FAULTY:
 835  	case BIO_BS_STATE_TEARDOWN:
 836  		D_DEBUG(DB_MGMT, "Device "DF_UUID"(%s) is already in "
 837  			"%s state\n", DP_UUID(d_bdev->bb_uuid),
 838  			d_bdev->bb_name, bio_state_enum_to_str(bbs->bb_state));
 839  		break;
 840  	default:
 841  		D_ERROR("Invalid BS state %d\n", bbs->bb_state);
 842  		break;
 843  	}
 844  }
 845
```

</details>


<a id="l846"></a>

## L846–L895: bio_bdev_event_cb()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L846)

| Source lines | Explanation |
| --- | --- |
| L846–L857 | SPDK event callback: recover the BIO device; ignore events without a context and events other than REMOVE; log accepted events. |
| L859–L869 | During normal runtime, require an open descriptor, mark the device removed, and emit a device-unplugged RAS notification. |
| L871–L878 | If the device record is still under construction and not in the global list, avoid normal teardown and return after checking the expected state. |
| L880–L888 | An unused device with no blobstore and no replacement in progress can be unlinked and destroyed immediately. |
| L891–L893 | A used device must have teardown scheduled on its blobstore owner's SPDK thread. The callback does not perform that teardown on the event thread. |

<details>
<summary>Numbered source for this section</summary>

```text
 846  void
 847  bio_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
 848  		  void *event_ctx)
 849  {
 850  	struct bio_bdev		*d_bdev = event_ctx;
 851  	struct bio_blobstore	*bbs;
 852
 853  	if (d_bdev == NULL || type != SPDK_BDEV_EVENT_REMOVE)
 854  		return;
 855
 856  	D_DEBUG(DB_MGMT, "Got SPDK event(%d) for dev %s\n", type,
 857  		spdk_bdev_get_name(bdev));
 858
 859  	if (!is_server_started()) {
 860  		D_INFO("Skip device remove cb on server start/shutdown\n");
 861  		return;
 862  	}
 863
 864  	D_ASSERT(d_bdev->bb_desc != NULL);
 865  	d_bdev->bb_removed = 1;
 866
 867  	ras_notify_eventf(RAS_DEVICE_UNPLUGGED, RAS_TYPE_INFO, RAS_SEV_NOTICE, NULL, NULL, NULL,
 868  			  NULL, NULL, NULL, NULL, NULL, NULL, "Device: " DF_UUID " unplugged",
 869  			  DP_UUID(d_bdev->bb_uuid));
 870
 871  	/* The bio_bdev is still under construction */
 872  	if (d_list_empty(&d_bdev->bb_link)) {
 873  		D_ASSERT(d_bdev->bb_blobstore == NULL);
 874  		D_DEBUG(DB_MGMT, "bio_bdev for "DF_UUID"(%s) is still "
 875  			"under construction\n", DP_UUID(d_bdev->bb_uuid),
 876  			d_bdev->bb_name);
 877  		return;
 878  	}
 879
 880  	bbs = d_bdev->bb_blobstore;
 881  	/* A new device isn't used by DAOS yet */
 882  	if (bbs == NULL && !d_bdev->bb_replacing) {
 883  		D_DEBUG(DB_MGMT, "Removed device "DF_UUID"(%s)\n",
 884  			DP_UUID(d_bdev->bb_uuid), d_bdev->bb_name);
 885
 886  		d_list_del_init(&d_bdev->bb_link);
 887  		destroy_bio_bdev(d_bdev);
 888  		return;
 889  	}
 890
 891  	if (bbs != NULL)
 892  		spdk_thread_send_msg(owner_thread(bbs), teardown_bio_bdev,
 893  				     d_bdev);
 894  }
 895
```

</details>


<a id="l896"></a>

## L896–L916: replace_bio_bdev()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L896)

| Source lines | Explanation |
| --- | --- |
| L896–L904 | Transfer the old device's BIO blobstore to a new device record, update its back-pointer, copy roles, and clear old ownership to avoid a double free. |
| L906–L907 | Move the runtime target-load count to the new record and zero the old one. This function does not copy `bb_weight`; the new record's weight came from its current name at creation. |
| L909–L914 | Destroy an already-removed old record; otherwise retain it but mark it faulty. This function alone does not rewrite all persistent SMD mappings. |

<details>
<summary>Numbered source for this section</summary>

```text
 896  void
 897  replace_bio_bdev(struct bio_bdev *old_dev, struct bio_bdev *new_dev)
 898  {
 899  	D_ASSERT(old_dev->bb_blobstore != NULL);
 900
 901  	new_dev->bb_blobstore = old_dev->bb_blobstore;
 902  	new_dev->bb_blobstore->bb_dev = new_dev;
 903  	new_dev->bb_roles = old_dev->bb_roles;
 904  	old_dev->bb_blobstore = NULL;
 905
 906  	new_dev->bb_tgt_cnt = old_dev->bb_tgt_cnt;
 907  	old_dev->bb_tgt_cnt = 0;
 908
 909  	if (old_dev->bb_removed) {
 910  		d_list_del_init(&old_dev->bb_link);
 911  		destroy_bio_bdev(old_dev);
 912  	} else {
 913  		old_dev->bb_faulty = 1;
 914  	}
 915  }
 916
```

</details>


<a id="l917"></a>

## L917–L948: bdev_name2roles()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L917)

| Source lines | Explanation |
| --- | --- |
| L917–L922 | Parse the role mask encoded after the final underscore in a generated bdev name. `strrchr` finds that last delimiter. |
| L924–L925 | Missing delimiter returns `-DER_NONEXIST`. |
| L927–L932 | Advance past the underscore, parse an unsigned number with base autodetection, and accept either end-of-string or `n` as the following character (namespace suffix). This is the existing role-name parser, not the strict weight parser. |
| L934–L938 | Reject mask bits outside DATA/META/WAL; otherwise log and return the mask. A nonnegative result is a role mask, not a conventional zero-only success code. |
| L941–L948 | Comment for the next function: device creation can load or initialize a blobstore and must poll itself before ordinary device pollers exist. |

<details>
<summary>Numbered source for this section</summary>

```text
 917  int
 918  bdev_name2roles(const char *name)
 919  {
 920  	const char	*dst = strrchr(name, '_');
 921  	char		*ptr_parse_end = NULL;
 922  	unsigned	 int value;
 923
 924  	if (dst == NULL)
 925  		return -DER_NONEXIST;
 926
 927  	dst++;
 928  	value = strtoul(dst, &ptr_parse_end, 0);
 929  	if (ptr_parse_end && *ptr_parse_end != 'n' && *ptr_parse_end != '\0') {
 930  		D_ERROR("invalid numeric value: %s (name %s)\n", dst, name);
 931  		return -DER_INVAL;
 932  	}
 933
 934  	if (value & (~NVME_ROLE_ALL))
 935  		return -DER_INVAL;
 936
 937  	D_INFO("bdev name:%s, bdev role:%u\n", name, value);
 938  	return value;
 939  }
 940
 941  /*
 942   * Create bio_bdev from SPDK bdev. It checks if the bdev has existing
 943   * blobstore, if it doesn't have, it'll create one automatically.
 944   *
 945   * This function is only called by 'Init' xstream on server start or
 946   * a device is hot plugged, so it has to do self poll since the poll
 947   * xstream for this device hasn't been established yet.
 948   */
```

</details>


<a id="l949"></a>

## L949–L1091: create_bio_bdev()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L949)

| Source lines | Explanation |
| --- | --- |
| L949–L960 | Declare the new/previous device records, SPDK handles, UUID, status, creation flag, and the newly added temporary weight-configuration string. |
| L962–L971 | SPDK names can change after replug. Reject an already-known current name rather than inserting a duplicate BIO record. |
| L973–L978 | Allocate a zero-filled device record and initialize its list node; fail with `-DER_NOMEM` if allocation fails. |
| L979 | Added: obtain an allocated copy of `DAOS_NVME_DEVICE_WEIGHTS` into `weights`. |
| L980 | Added: parse the configuration and write this named device's weight to `bb_weight`; missing names or an unset variable use 1. |
| L981 | Free the copied environment string after parsing. The device stores an integer, not a pointer into that string. |
| L982–L985 | Invalid weight syntax becomes `-DER_INVAL`; go to shared device cleanup before opening its descriptor/blobstore. |
| L987–L991 | Store the role mask and copy the device name into owned memory. Clean up if the string allocation fails. |
| L993–L995 | Resolve the SPDK device and record whether it supports unmap/discard commands. |
| L997–L1009 | Hold an open SPDK descriptor to keep the bdev alive, registering the removal callback and this BIO record as its context. Translate any open failure and clean up. |
| L1011–L1015 | Try opening an existing blobstore without an expected UUID/type. If this returns NULL, the current code enters its creation path. |
| L1017–L1027 | Generate a UUID and initialize a new blobstore. Fail and clean up if initialization fails; otherwise remember it was newly created. This existing path does not distinguish every possible load failure from “no existing store.” |
| L1030–L1035 | Read the loaded/created blobstore's 16-byte type field back into the device UUID and log whether it was created or loaded. |
| L1037–L1041 | Unload this temporary probe handle. A target's owner will load the store again later for I/O. |
| L1043–L1048 | Reject an all-zero device UUID as an invalid legacy/non-DAOS blobstore identity. |
| L1050–L1059 | Store the UUID and check if a BIO device already has that identity. During initial startup, a duplicate UUID is an error. |
| L1061–L1067 | At runtime, the same UUID can mean a device was replugged. If the old descriptor is still held, discard the newly created record and wait for teardown to finish. |
| L1068–L1077 | Otherwise require the old record to be removed, transfer its blobstore to the new record, add the new record to the global list, and return it through `dev_out` so the caller can resume setup. |
| L1080–L1085 | For a genuinely new device, log its UUID/roles, append to the global list, and return success. `dev_out` is not filled for this ordinary-new-device case. |
| L1087–L1089 | Common failure path destroys whatever this partially constructed device owns and returns the saved error. |

<details>
<summary>Numbered source for this section</summary>

```text
 949  static int
 950  create_bio_bdev(struct bio_xs_context *ctxt, const char *bdev_name, unsigned int roles,
 951  		struct bio_bdev **dev_out)
 952  {
 953  	struct bio_bdev			*d_bdev, *old_dev;
 954  	struct spdk_blob_store		*bs = NULL;
 955  	struct spdk_bs_type		 bstype;
 956  	struct spdk_bdev		*bdev;
 957  	uuid_t				 bs_uuid;
 958  	int				 rc;
 959  	bool				 new_bs = false;
 960  	char                            *weights = NULL;
 961
 962  	/*
 963  	 * SPDK guarantees uniqueness of bdev name. When a device is hot
 964  	 * removed then plugged back to same slot, a new bdev with different
 965  	 * name will be generated.
 966  	 */
 967  	d_bdev = lookup_dev_by_name(bdev_name);
 968  	if (d_bdev != NULL) {
 969  		D_ERROR("Device %s is already created\n", bdev_name);
 970  		return -DER_EXIST;
 971  	}
 972
 973  	D_ALLOC_PTR(d_bdev);
 974  	if (d_bdev == NULL) {
 975  		return -DER_NOMEM;
 976  	}
 977
 978  	D_INIT_LIST_HEAD(&d_bdev->bb_link);
 979  	d_agetenv_str(&weights, BIO_NVME_WEIGHTS_ENV);
 980  	rc = bio_weight_get(weights, bdev_name, &d_bdev->bb_weight);
 981  	d_freeenv_str(&weights);
 982  	if (rc != 0) {
 983  		D_ERROR("Invalid %s configuration\n", BIO_NVME_WEIGHTS_ENV);
 984  		D_GOTO(error, rc = -DER_INVAL);
 985  	}
 986
 987  	d_bdev->bb_roles = roles;
 988  	D_STRNDUP(d_bdev->bb_name, bdev_name, strlen(bdev_name));
 989  	if (d_bdev->bb_name == NULL) {
 990  		D_GOTO(error, rc = -DER_NOMEM);
 991  	}
 992
 993  	bdev = spdk_bdev_get_by_name(d_bdev->bb_name);
 994  	D_ASSERT(bdev != NULL);
 995  	d_bdev->bb_unmap_supported = spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_UNMAP);
 996
 997  	/*
 998  	 * Hold the SPDK bdev by an open descriptor, otherwise, the bdev
 999  	 * could be deconstructed by SPDK on device hot remove.
1000  	 */
1001  	rc =
1002  	    spdk_bdev_open_ext(d_bdev->bb_name, false, bio_bdev_event_cb, d_bdev, &d_bdev->bb_desc);
1003  	if (rc != 0) {
1004  		D_ERROR("Failed to hold bdev %s, %d\n", d_bdev->bb_name, rc);
1005  		rc = daos_errno2der(-rc);
1006  		goto error;
1007  	}
1008
1009  	D_ASSERT(d_bdev->bb_desc != NULL);
1010
1011  	/* Try to load blobstore without specifying 'bstype' first */
1012  	bs = load_blobstore(ctxt, d_bdev->bb_name, NULL, false, false,
1013  			    NULL, NULL);
1014  	if (bs == NULL) {
1015  		D_DEBUG(DB_MGMT, "Creating bs for %s\n", d_bdev->bb_name);
1016
1017  		/* Create blobstore if it wasn't created before */
1018  		uuid_generate(bs_uuid);
1019  		bs = load_blobstore(ctxt, d_bdev->bb_name, &bs_uuid, true,
1020  				    false, NULL, NULL);
1021  		if (bs == NULL) {
1022  			D_ERROR("Failed to create blobstore on dev: "
1023  				""DF_UUID"\n", DP_UUID(bs_uuid));
1024  			rc = -DER_INVAL;
1025  			goto error;
1026  		}
1027  		new_bs = true;
1028  	}
1029
1030  	/* Get the 'bstype' (device ID) of blobstore */
1031  	bstype = spdk_bs_get_bstype(bs);
1032  	memcpy(bs_uuid, bstype.bstype, sizeof(bs_uuid));
1033  	D_DEBUG(DB_MGMT, "%s :"DF_UUID"\n",
1034  		new_bs ? "Created new blobstore" : "Loaded blobstore",
1035  		DP_UUID(bs_uuid));
1036
1037  	rc = unload_blobstore(ctxt, bs);
1038  	if (rc != 0) {
1039  		D_ERROR("Unable to unload blobstore\n");
1040  		goto error;
1041  	}
1042
1043  	/* Verify if the blobstore was created by DAOS */
1044  	if (uuid_is_null(bs_uuid)) {
1045  		D_ERROR("The bdev has old blobstore not created by DAOS!\n");
1046  		rc = -DER_INVAL;
1047  		goto error;
1048  	}
1049
1050  	uuid_copy(d_bdev->bb_uuid, bs_uuid);
1051  	/* Verify if any duplicated device ID */
1052  	old_dev = lookup_dev_by_id(bs_uuid);
1053  	if (old_dev != NULL) {
1054  		/* If it's in server xstreams start phase, report error */
1055  		if (!is_server_started()) {
1056  			D_ERROR("Dup device "DF_UUID" detected!\n",
1057  				DP_UUID(bs_uuid));
1058  			rc = -DER_EXIST;
1059  			goto error;
1060  		}
1061  		/* Old device is plugged back */
1062  		D_INFO("Device "DF_UUID" is plugged back\n", DP_UUID(bs_uuid));
1063
1064  		if (old_dev->bb_desc != NULL) {
1065  			D_INFO("Device "DF_UUID"(%s) isn't torndown\n",
1066  			       DP_UUID(old_dev->bb_uuid), old_dev->bb_name);
1067  			destroy_bio_bdev(d_bdev);
1068  		} else {
1069  			D_ASSERT(old_dev->bb_removed);
1070  			replace_bio_bdev(old_dev, d_bdev);
1071  			d_list_add(&d_bdev->bb_link, &nvme_glb.bd_bdevs);
1072  			/* Inform caller to trigger device setup */
1073  			D_ASSERT(dev_out != NULL);
1074  			*dev_out = d_bdev;
1075  		}
1076
1077  		return 0;
1078  	}
1079
1080  	D_DEBUG(DB_MGMT, "Create DAOS bdev "DF_UUID", role:%u\n",
1081  		DP_UUID(bs_uuid), d_bdev->bb_roles);
1082
1083  	d_list_add_tail(&d_bdev->bb_link, &nvme_glb.bd_bdevs);
1084
1085  	return 0;
1086
1087  error:
1088  	destroy_bio_bdev(d_bdev);
1089  	return rc;
1090  }
1091
```

</details>


<a id="l1092"></a>

## L1092–L1127: validate_bio_weights()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L1092)

| Source lines | Explanation |
| --- | --- |
| L1092–L1100 | New validation function. `config` owns the environment copy; `cursor` tracks parsing; `name` and `len` identify a slice inside that copy; `bdev` scans known SPDK devices; `weight` and `rc` receive parser outputs. |
| L1102 | Read `DAOS_NVME_DEVICE_WEIGHTS`. An unset variable leaves a NULL configuration and is valid. |
| L1103 | Start parsing at the beginning of the configuration. |
| L1104 | `bio_weight_next()` returns positive for an entry, zero at the end, and negative for malformed input. The loop processes one `name=weight` entry at a time. |
| L1105–L1106 | Scan the currently discovered SPDK bdevs and read each actual name. |
| L1108 | Require that the device belongs to this engine's selected bdev class, such as NVMe or AIO. |
| L1109 | Require identical name length and bytes. Checking length prevents a prefix such as `a` from matching `aa`. |
| L1110 | Stop the inner scan on a matching device. The parsed weight has already been range-checked by the parser. |
| L1112–L1116 | If the scan ended with no matching device, log the exact name slice with `%.*s`, set a failure status, and stop parsing. The slice need not be NUL-terminated. |
| L1119 | Release the environment copy on both successful parsing and validation failure. |
| L1120–L1123 | Translate a parser or unknown-device failure into `-DER_INVAL`, with a syntax/range diagnostic. |
| L1125 | Return success for an unset option or for a fully valid configuration. Validation does not create a device or assign any target. |

<details>
<summary>Numbered source for this section</summary>

```text
1092  static int
1093  validate_bio_weights(void)
1094  {
1095  	char             *config = NULL;
1096  	const char       *cursor, *name;
1097  	struct spdk_bdev *bdev;
1098  	size_t            len;
1099  	unsigned int      weight;
1100  	int               rc;
1101
1102  	d_agetenv_str(&config, BIO_NVME_WEIGHTS_ENV);
1103  	cursor = config;
1104  	while ((rc = bio_weight_next(config, &cursor, &name, &len, &weight)) > 0) {
1105  		for (bdev = spdk_bdev_first(); bdev != NULL; bdev = spdk_bdev_next(bdev)) {
1106  			const char *bdev_name = spdk_bdev_get_name(bdev);
1107
1108  			if (nvme_glb.bd_bdev_class == get_bdev_type(bdev) &&
1109  			    strlen(bdev_name) == len && memcmp(bdev_name, name, len) == 0)
1110  				break;
1111  		}
1112  		if (bdev == NULL) {
1113  			D_ERROR("Unknown device in %s: %.*s\n", BIO_NVME_WEIGHTS_ENV, (int)len,
1114  				name);
1115  			rc = -1;
1116  			break;
1117  		}
1118  	}
1119  	d_freeenv_str(&config);
1120  	if (rc < 0) {
1121  		D_ERROR("Invalid %s; expected unique bdev=weight entries, weights 1..%u\n",
1122  			BIO_NVME_WEIGHTS_ENV, BIO_NVME_WEIGHT_MAX);
1123  		return -DER_INVAL;
1124  	}
1125  	return 0;
1126  }
1127
```

</details>


<a id="l1128"></a>

## L1128–L1195: init_bio_bdevs()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L1128)

| Source lines | Explanation |
| --- | --- |
| L1128–L1134 | Initialize BIO's device list from SPDK discovery. Declare BIO/SPDK device pointers, the current name, and status. |
| L1136–L1140 | This is a startup-only operation. With no discovered SPDK devices, return `-DER_NONEXIST`. |
| L1141–L1144 | Added: validate all configured weights/names before this function opens or creates any blobstore. Return early for a typo rather than discovering it after initialization has progressed. |
| L1146–L1150 | Enumerate devices, skip other bdev classes, and obtain each eligible name. |
| L1152–L1156 | Attempt device power-management setup. Unsupported settings are tolerated; other failures are warned about rather than immediately aborting this loop. |
| L1158–L1163 | Parse the device role mask from the generated name. Negative values are errors. |
| L1165–L1167 | Create/load the BIO device using the parsed mask. Here `rc` is first passed as the role-mask argument, then overwritten with the function's status. |
| L1170–L1178 | Second pass over matching SPDK devices: locate the BIO record created in the first pass. If one is missing, report failure. |
| L1180–L1193 | Reset preexisting device LED state through the management helper. Propagate LED-reset failures; return zero after all devices succeed. |

<details>
<summary>Numbered source for this section</summary>

```text
1128  static int
1129  init_bio_bdevs(struct bio_xs_context *ctxt)
1130  {
1131  	struct bio_bdev  *d_bdev;
1132  	struct spdk_bdev *bdev;
1133  	const char       *bdev_name;
1134  	int rc = 0;
1135
1136  	D_ASSERT(!is_server_started());
1137  	if (spdk_bdev_first() == NULL) {
1138  		D_ERROR("No SPDK bdevs found!\n");
1139  		return -DER_NONEXIST;
1140  	}
1141  	/* Reject configuration errors before opening or creating any blobstores. */
1142  	rc = validate_bio_weights();
1143  	if (rc != 0)
1144  		return rc;
1145
1146  	for (bdev = spdk_bdev_first(); bdev != NULL; bdev = spdk_bdev_next(bdev)) {
1147  		if (nvme_glb.bd_bdev_class != get_bdev_type(bdev))
1148  			continue;
1149
1150  		bdev_name = spdk_bdev_get_name(bdev);
1151
1152  		/* Apply NVMe power management settings */
1153  		rc = bio_set_power_mgmt(ctxt, bdev_name);
1154  		if (rc != 0 && rc != -DER_NOTSUPPORTED)
1155  			D_WARN("Failed to set power management for device %s: " DF_RC "\n",
1156  			       bdev_name, DP_RC(rc));
1157
1158  		rc = bdev_name2roles(bdev_name);
1159  		if (rc < 0) {
1160  			D_ERROR("Failed to get role from bdev name '%s', "DF_RC"\n", bdev_name,
1161  				DP_RC(rc));
1162  			return rc;
1163  		}
1164
1165  		rc = create_bio_bdev(ctxt, bdev_name, rc, NULL);
1166  		if (rc)
1167  			return rc;
1168  	}
1169
1170  	for (bdev = spdk_bdev_first(); bdev != NULL; bdev = spdk_bdev_next(bdev)) {
1171  		if (nvme_glb.bd_bdev_class != get_bdev_type(bdev))
1172  			continue;
1173
1174  		d_bdev = lookup_dev_by_name(spdk_bdev_get_name(bdev));
1175  		if (d_bdev == NULL) {
1176  			D_ERROR("Device %s doesn't exist\n", spdk_bdev_get_name(bdev));
1177  			return -DER_EXIST;
1178  		}
1179
1180  		D_DEBUG(DB_MGMT, "Device " DF_UUID " LED reset on init\n",
1181  			DP_UUID(d_bdev->bb_uuid));
1182
1183  		/* Clear any pre-existing LED state */
1184  		rc = bio_led_manage(ctxt, NULL, d_bdev->bb_uuid,
1185  				    (unsigned int)CTL__LED_ACTION__RESET, NULL, 0);
1186  		if (rc != 0) {
1187  			DL_ERROR(rc, "Reset LED on device:" DF_UUID " failed",
1188  				 DP_UUID(d_bdev->bb_uuid));
1189  			return rc;
1190  		}
1191  	}
1192
1193  	return 0;
1194  }
1195
```

</details>


<a id="l1196"></a>

## L1196–L1242: put_bio_blobstore()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L1196)

| Source lines | Explanation |
| --- | --- |
| L1196–L1202 | Release one xstream's reference to a shared BIO blobstore. `bs` remembers an SPDK store that this owner may need to unload after other users finish. |
| L1204–L1208 | Detach each per-pool I/O context from the xstream's list. Warn if a pool blob is still open. This loop does not itself close that blob. |
| L1210 | Lock the shared blobstore's mutex before changing reference/owner state. |
| L1212–L1216 | Only the owner takes responsibility for unloading the SPDK store, and not if unloading is already in progress. Clear the shared pointer so it is not reused. |
| L1218–L1224 | Find this xstream's registered array entry, clear it, and assert that the reference really existed. |
| L1226–L1227 | Require a positive reference count and decrement it once. |
| L1229–L1233 | An owner with an SPDK handle waits for other references to disappear. The last releaser wakes waiters. `ABT_cond_wait` releases/reacquires the mutex while waiting. |
| L1235–L1240 | Unlock, require no external holdings, then unload the saved SPDK store on its owning xstream. |

<details>
<summary>Numbered source for this section</summary>

```text
1196  static void
1197  put_bio_blobstore(struct bio_xs_blobstore *bxb, struct bio_xs_context *ctxt)
1198  {
1199  	struct bio_blobstore	*bbs = bxb->bxb_blobstore;
1200  	struct spdk_blob_store	*bs = NULL;
1201  	struct bio_io_context	*ioc, *tmp;
1202  	int			i, xs_cnt_max = BIO_XS_CNT_MAX;
1203
1204  	d_list_for_each_entry_safe(ioc, tmp, &bxb->bxb_io_ctxts, bic_link) {
1205  		d_list_del_init(&ioc->bic_link);
1206  		if (ioc->bic_blob != NULL)
1207  			D_WARN("Pool isn't closed. tgt:%d\n", ctxt->bxc_tgt_id);
1208  	}
1209
1210  	ABT_mutex_lock(bbs->bb_mutex);
1211  	/* Unload the blobstore in the same xstream where it was loaded. */
1212  	if (is_bbs_owner(ctxt, bbs) && bbs->bb_bs != NULL) {
1213  		if (!bbs->bb_unloading)
1214  			bs = bbs->bb_bs;
1215  		bbs->bb_bs = NULL;
1216  	}
1217
1218  	for (i = 0; i < xs_cnt_max; i++) {
1219  		if (bbs->bb_xs_ctxts[i] == ctxt) {
1220  			bbs->bb_xs_ctxts[i] = NULL;
1221  			break;
1222  		}
1223  	}
1224  	D_ASSERT(i < xs_cnt_max);
1225
1226  	D_ASSERT(bbs->bb_ref > 0);
1227  	bbs->bb_ref--;
1228
1229  	/* Wait for other xstreams to put_bio_blobstore() first */
1230  	if (bs != NULL && bbs->bb_ref)
1231  		ABT_cond_wait(bbs->bb_barrier, bbs->bb_mutex);
1232  	else if (bbs->bb_ref == 0)
1233  		ABT_cond_broadcast(bbs->bb_barrier);
1234
1235  	ABT_mutex_unlock(bbs->bb_mutex);
1236
1237  	if (bs != NULL) {
1238  		D_ASSERT(bbs->bb_holdings == 0);
1239  		unload_blobstore(ctxt, bs);
1240  	}
1241  }
1242
```

</details>


<a id="l1243"></a>

## L1243–L1253: fini_bio_bdevs()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L1243)

| Source lines | Explanation |
| --- | --- |
| L1243–L1252 | Walk the global device list with a deletion-safe iterator, unlink every record, and destroy it. The `ctxt` parameter is unused in this body. |

<details>
<summary>Numbered source for this section</summary>

```text
1243  static void
1244  fini_bio_bdevs(struct bio_xs_context *ctxt)
1245  {
1246  	struct bio_bdev *d_bdev, *tmp;
1247
1248  	d_list_for_each_entry_safe(d_bdev, tmp, &nvme_glb.bd_bdevs, bb_link) {
1249  		d_list_del_init(&d_bdev->bb_link);
1250  		destroy_bio_bdev(d_bdev);
1251  	}
1252  }
1253
```

</details>


<a id="l1254"></a>

## L1254–L1290: alloc_bio_blobstore()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L1254)

| Source lines | Explanation |
| --- | --- |
| L1254–L1263 | Allocate a zero-filled shared BIO blobstore wrapper for a device. Assert a valid owner context and return NULL on allocation failure. |
| L1265–L1267 | Allocate its bounded xstream-pointer array. This `BIO_XS_CNT_MAX` limit is separate from the SMD role-entry limit. |
| L1269–L1275 | Create wrapper-local mutex and condition variable, unwinding earlier allocations on failure. |
| L1277–L1280 | Start with zero references, record the owner xstream and device pointer, and return the wrapper. Getting a reference is a separate call. |
| L1282–L1288 | Cleanup labels free mutex, context array, and wrapper in reverse allocation order, then return NULL. |

<details>
<summary>Numbered source for this section</summary>

```text
1254  static struct bio_blobstore *
1255  alloc_bio_blobstore(struct bio_xs_context *ctxt, struct bio_bdev *d_bdev)
1256  {
1257  	struct bio_blobstore	*bb;
1258  	int			 rc, xs_cnt_max = BIO_XS_CNT_MAX;
1259
1260  	D_ASSERT(ctxt != NULL);
1261  	D_ALLOC_PTR(bb);
1262  	if (bb == NULL)
1263  		return NULL;
1264
1265  	D_ALLOC_ARRAY(bb->bb_xs_ctxts, xs_cnt_max);
1266  	if (bb->bb_xs_ctxts == NULL)
1267  		goto out_bb;
1268
1269  	rc = ABT_mutex_create(&bb->bb_mutex);
1270  	if (rc != ABT_SUCCESS)
1271  		goto out_ctxts;
1272
1273  	rc = ABT_cond_create(&bb->bb_barrier);
1274  	if (rc != ABT_SUCCESS)
1275  		goto out_mutex;
1276
1277  	bb->bb_ref = 0;
1278  	bb->bb_owner_xs = ctxt;
1279  	bb->bb_dev = d_bdev;
1280  	return bb;
1281
1282  out_mutex:
1283  	ABT_mutex_free(&bb->bb_mutex);
1284  out_ctxts:
1285  	D_FREE(bb->bb_xs_ctxts);
1286  out_bb:
1287  	D_FREE(bb);
1288  	return NULL;
1289  }
1290
```

</details>


<a id="l1291"></a>

## L1291–L1318: get_bio_blobstore()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L1291)

| Source lines | Explanation |
| --- | --- |
| L1291–L1296 | Acquire a reference to a shared wrapper for this xstream under the wrapper's mutex. |
| L1298–L1302 | Reject registering the same xstream twice. Unlock before returning NULL. |
| L1303–L1306 | On the first empty context slot, store the pointer, increment `bb_ref`, and stop searching. |
| L1310–L1316 | Unlock; if all slots were occupied, report too many xstreams and return NULL. Otherwise return the shared wrapper. |

<details>
<summary>Numbered source for this section</summary>

```text
1291  static struct bio_blobstore *
1292  get_bio_blobstore(struct bio_blobstore *bb, struct bio_xs_context *ctxt)
1293  {
1294  	int	i, xs_cnt_max = BIO_XS_CNT_MAX;
1295
1296  	ABT_mutex_lock(bb->bb_mutex);
1297
1298  	for (i = 0; i < xs_cnt_max; i++) {
1299  		if (bb->bb_xs_ctxts[i] == ctxt) {
1300  			D_ERROR("Dup xstream context!\n");
1301  			ABT_mutex_unlock(bb->bb_mutex);
1302  			return NULL;
1303  		} else if (bb->bb_xs_ctxts[i] == NULL) {
1304  			bb->bb_xs_ctxts[i] = ctxt;
1305  			bb->bb_ref++;
1306  			break;
1307  		}
1308  	}
1309
1310  	ABT_mutex_unlock(bb->bb_mutex);
1311
1312  	if (i == xs_cnt_max) {
1313  		D_ERROR("Too many xstreams per device!\n");
1314  		return NULL;
1315  	}
1316  	return bb;
1317  }
1318
```

</details>


<a id="l1319"></a>

## L1319–L1327: is_role_match()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L1319)

| Source lines | Explanation |
| --- | --- |
| L1319–L1326 | Role eligibility helper. A zero mask means legacy DATA-only; otherwise a nonzero bitwise AND means the requested role is present. This operation tests masks, not enum indexes. |

<details>
<summary>Numbered source for this section</summary>

```text
1319  static inline bool
1320  is_role_match(unsigned int roles, unsigned int req_role)
1321  {
1322  	if (roles == 0)
1323  		return NVME_ROLE_DATA & req_role;
1324
1325  	return roles & req_role;
1326  }
1327
```

</details>


<a id="l1328"></a>

## L1328–L1339: bio_nvme_configured()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L1328)

| Source lines | Explanation |
| --- | --- |
| L1328–L1333 | Determine whether NVMe is configured. With no saved configuration path, return false regardless of requested role. |
| L1335–L1338 | A sentinel type at least MAX asks “is any NVMe configured?” and returns true here. For DATA/META/WAL, translate the enum to a bit mask and test it against the configured global roles. |

<details>
<summary>Numbered source for this section</summary>

```text
1328  bool
1329  bio_nvme_configured(enum smd_dev_type type)
1330  {
1331  	if (nvme_glb.bd_nvme_conf == NULL)
1332  		return false;
1333
1334  	if (type >= SMD_DEV_TYPE_MAX)
1335  		return true;
1336
1337  	return is_role_match(nvme_glb.bd_nvme_roles, smd_dev_type2role(type));
1338  }
1339
```

</details>


<a id="l1340"></a>

## L1340–L1403: choose_device()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L1340)

| Source lines | Explanation |
| --- | --- |
| L1340–L1341 | Select a device for an unmapped target/role. Returns a device pointer or NULL; it does not write SMD mappings itself. |
| L1343 | Loop variable pointing to the device currently being considered. |
| L1344 | The best eligible device so far starts as NULL; the first eligible candidate establishes it. |
| L1345–L1346 | `rc` stores SMD lookup status and `dev_info` receives a temporary device-info snapshot. |
| L1348 | This internal function assumes the global device list is nonempty. Its caller checks initialization state. |
| L1349–L1353 | Comment describes count/weight balancing and the existing-mapping bypass in `assign_xs_bdev()`. |
| L1354 | Traverse devices in their current list order. That order breaks exact score ties. |
| L1355 | For this candidate, initialize `used` (persisted slots already occupied) and `needed` (slots the whole new role assignment requires) to zero. |
| L1356 | Iterator over the DATA, META, and WAL enum values. |
| L1358–L1359 | Skip devices unable to serve the requested role, even if they have the best weight or most free slots. |
| L1361–L1362 | Count all the role mappings that `assign_roles()` would append for this candidate, not merely the requested role. |
| L1363–L1364 | Ignore roles absent from the candidate's role mask. |
| L1365 | Each matching role consumes one additional SMD device-table entry. |
| L1366–L1367 | In the legacy mode without metadata-on-SSD, only the first matching assignment is made; mirror `assign_roles()` by stopping here too. |
| L1370–L1371 | Read actual SMD occupancy. The runtime score counter is not a reliable slot counter because newly added system-target mappings do not increment it. |
| L1372–L1373 | On success, require a valid snapshot with at least one existing entry. |
| L1374 | Copy its persisted mapping-entry count into `used`. |
| L1375 | Free the snapshot after copying the count. No persistent data is removed. |
| L1376–L1380 | A never-mapped device (`-DER_NONEXIST`) retains `used=0`. Other database errors abort selection; they must not be treated as an empty device. |
| L1381–L1384 | Initialize the device's cached balancing count from SMD once. Nuance: the initial seed includes persisted system entries if present; subsequent `assign_roles()` increments exclude the system target. The counter is not a pure, universally system-free count. |
| L1385–L1387 | Added capacity guard: skip a device unless its entire new role assignment fits within 64 entries. A high weight cannot override this hard constraint. This checks mapping slots, not bytes free on the SSD. |
| L1388–L1391 | Pick the first eligible device, or replace the current best if the candidate's count/weight ratio is strictly lower. `\|\|` short-circuiting avoids dereferencing a NULL `chosen_bdev`. |
| L1392 | Remember the winning device pointer. Nothing is persisted at this point. |
| L1396–L1398 | Log the proposed target, requested role enum, chosen name, weight, and count before assignment. Actual SMD insertion can still fail afterward. |
| L1399–L1401 | If no candidate survived, report failure; return the best pointer or NULL. The caller runs `assign_roles()` only after a non-NULL result. |

<details>
<summary>Numbered source for this section</summary>

```text
1340  static struct bio_bdev *
1341  choose_device(int tgt_id, enum smd_dev_type st)
1342  {
1343  	struct bio_bdev     *d_bdev;
1344  	struct bio_bdev     *chosen_bdev = NULL;
1345  	int                  rc;
1346  	struct smd_dev_info *dev_info = NULL;
1347
1348  	D_ASSERT(!d_list_empty(&nvme_glb.bd_bdevs));
1349  	/*
1350  	 * Balance mapped targets per unit of configured throughput. Unit weights
1351  	 * preserve the legacy assignment order. Existing SMD mappings bypass this
1352  	 * path, so changing weights never silently relocates persistent data.
1353  	 */
1354  	d_list_for_each_entry(d_bdev, &nvme_glb.bd_bdevs, bb_link) {
1355  		unsigned int      used = 0, needed = 0;
1356  		enum smd_dev_type role;
1357
1358  		if (!is_role_match(d_bdev->bb_roles, smd_dev_type2role(st)))
1359  			continue;
1360
1361  		/* Count every SMD slot assign_roles() will append, including system targets. */
1362  		for (role = SMD_DEV_TYPE_DATA; role < SMD_DEV_TYPE_MAX; role++) {
1363  			if (!is_role_match(d_bdev->bb_roles, smd_dev_type2role(role)))
1364  				continue;
1365  			needed++;
1366  			if (!bio_nvme_configured(SMD_DEV_TYPE_META))
1367  				break;
1368  		}
1369
1370  		/* Use persistent occupancy: bb_tgt_cnt deliberately excludes system targets. */
1371  		rc = smd_dev_get_by_id(d_bdev->bb_uuid, &dev_info);
1372  		if (rc == 0) {
1373  			D_ASSERT(dev_info != NULL && dev_info->sdi_tgt_cnt != 0);
1374  			used = dev_info->sdi_tgt_cnt;
1375  			smd_dev_free_info(dev_info);
1376  		} else if (rc != -DER_NONEXIST) {
1377  			D_ERROR("Unable to get dev info for " DF_UUID "\n",
1378  				DP_UUID(d_bdev->bb_uuid));
1379  			return NULL;
1380  		}
1381  		if (!d_bdev->bb_tgt_cnt_init) {
1382  			d_bdev->bb_tgt_cnt      = used;
1383  			d_bdev->bb_tgt_cnt_init = 1;
1384  		}
1385  		/* Check the entire role assignment before its first persistent update. */
1386  		if (!bio_weight_fits(used, needed, SMD_MAX_TGT_CNT))
1387  			continue;
1388  		/* Compare ratios with integer products, retaining list-order ties. */
1389  		if (chosen_bdev == NULL ||
1390  		    bio_weight_less(d_bdev->bb_tgt_cnt, d_bdev->bb_weight, chosen_bdev->bb_tgt_cnt,
1391  				    chosen_bdev->bb_weight)) {
1392  			chosen_bdev = d_bdev;
1393  		}
1394  	}
1395
1396  	if (chosen_bdev != NULL)
1397  		D_INFO("Assign target %d type %u to %s (weight %u, mapped count %d)\n", tgt_id, st,
1398  		       chosen_bdev->bb_name, chosen_bdev->bb_weight, chosen_bdev->bb_tgt_cnt);
1399  	else
1400  		D_ERROR("No eligible device has SMD slots for target %d type %u\n", tgt_id, st);
1401  	return chosen_bdev;
1402  }
1403
```

</details>


<a id="l1404"></a>

## L1404–L1418: alloc_xs_blobstore()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L1404)

| Source lines | Explanation |
| --- | --- |
| L1404–L1411 | Allocate a per-xstream/per-role BIO blobstore context and return NULL on allocation failure. |
| L1413–L1416 | Initialize its pending-I/O and pool-I/O-context lists, then return it. The shared device/blobstore and channel are attached later. |

<details>
<summary>Numbered source for this section</summary>

```text
1404  struct bio_xs_blobstore *
1405  alloc_xs_blobstore(void)
1406  {
1407  	struct bio_xs_blobstore *bxb;
1408
1409  	D_ALLOC_PTR(bxb);
1410  	if (bxb == NULL)
1411  		return NULL;
1412
1413  	D_INIT_LIST_HEAD(&bxb->bxb_pending_ios);
1414  	D_INIT_LIST_HEAD(&bxb->bxb_io_ctxts);
1415
1416  	return bxb;
1417  }
1418
```

</details>


<a id="l1419"></a>

## L1419–L1484: assign_roles()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L1419)

| Source lines | Explanation |
| --- | --- |
| L1419–L1425 | Assign all supported roles on one selected device to a target. `failed_st` records where insertion fails; `assigned` distinguishes “no matching roles” from success. |
| L1427–L1431 | Allocate a temporary device-info description, including controller information needed for SMD. Return `-DER_NOMEM` if it cannot be created. |
| L1433–L1435 | Iterate role enums, skipping roles the selected device does not provide. |
| L1437 | Persist one `(target, role) → device UUID` mapping and update the device's entry table through SMD. Pass controller data along with it. |
| L1438–L1443 | On insertion error, log the context, remember the failed role, and enter cleanup. |
| L1444 | Record that at least one role has been assigned successfully. |
| L1445–L1460 | Existing design rationale: the internal system target should share a device with normal targets rather than consume a normal target's load-balancing increment. |
| L1461–L1462 | Increment the runtime balancing count once per successfully inserted role for ordinary targets only. SMD occupancy still increases for system-target entries. |
| L1464–L1466 | Log the successful persisted mapping and the updated runtime count. |
| L1468–L1469 | Stop after one role in the legacy mode without NVMe metadata; otherwise continue to the next supported role. |
| L1472–L1474 | Free temporary device/controller info. Return success if at least one role was assigned, otherwise `-DER_INVAL`. |
| L1475–L1482 | Error cleanup frees temporary info, but the rollback loop contains only a TODO. It does not undo earlier successful role insertions. The new capacity guard prevents a predictable full-table failure partway through this loop; it does not make all role updates atomic against arbitrary errors. |

<details>
<summary>Numbered source for this section</summary>

```text
1419  static int
1420  assign_roles(struct bio_bdev *d_bdev, unsigned int tgt_id)
1421  {
1422  	enum smd_dev_type	st, failed_st;
1423  	struct bio_dev_info    *b_info;
1424  	bool			assigned = false;
1425  	int			rc;
1426
1427  	b_info = alloc_dev_info(d_bdev->bb_uuid, d_bdev, NULL);
1428  	if (b_info == NULL) {
1429  		D_ERROR("Failed to alloc bio_dev_info.");
1430  		return -DER_NOMEM;
1431  	}
1432
1433  	for (st = SMD_DEV_TYPE_DATA; st < SMD_DEV_TYPE_MAX; st++) {
1434  		if (!is_role_match(d_bdev->bb_roles, smd_dev_type2role(st)))
1435  			continue;
1436
1437  		rc = smd_dev_add_tgt(d_bdev->bb_uuid, tgt_id, st, b_info->bdi_ctrlr);
1438  		if (rc) {
1439  			D_ERROR("Failed to map dev "DF_UUID" type:%u to tgt %d. "DF_RC"\n",
1440  				DP_UUID(d_bdev->bb_uuid), st, tgt_id, DP_RC(rc));
1441  			failed_st = st;
1442  			goto error;
1443  		}
1444  		assigned = true;
1445  		/*
1446  		 * Now a device will be assigned to SYS_TGT_ID for RDB
1447  		 * (the mapping will be recorded in target table), but we should not
1448  		 * treat the SYS_TGT mapping equally with other VOS targets mappings.
1449  		 *
1450  		 * Let's take an example, if there is a config having 4 meta SSDs and 3 targets,
1451  		 * how should we assign SSDs?
1452  		 *
1453  		 * 1. Assign 3 SSDs to 3 VOS targets and sys target (sys target share SSD with
1454  		 * one of VOS target), leave one SSD unused, or;
1455  		 * 2. Assign 1 SSD to sys target, assign the other 3 SSDs to VOS targets
1456  		 *
1457  		 * We use the 1st policy to assign SSDs and @bb_tgt_cnt won't be increased for
1458  		 * sys tgt id.
1459  		 *
1460  		 */
1461  		if (tgt_id != BIO_SYS_TGT_ID)
1462  			d_bdev->bb_tgt_cnt++;
1463
1464  		D_DEBUG(DB_MGMT, "Successfully mapped dev "DF_UUID"/%d/%u to tgt %d role %u\n",
1465  			DP_UUID(d_bdev->bb_uuid), d_bdev->bb_tgt_cnt, d_bdev->bb_roles,
1466  			tgt_id, smd_dev_type2role(st));
1467
1468  		if (!bio_nvme_configured(SMD_DEV_TYPE_META))
1469  			break;
1470  	}
1471
1472  	bio_free_dev_info(b_info);
1473
1474  	return assigned ? 0 : -DER_INVAL;
1475  error:
1476  	bio_free_dev_info(b_info);
1477  	for (st = SMD_DEV_TYPE_DATA; st < failed_st; st++) {
1478  		if (!is_role_match(d_bdev->bb_roles, smd_dev_type2role(st)))
1479  			continue;
1480  		/* TODO Error cleanup by smd_dev_del_tgt() */
1481  	}
1482  	return rc;
1483  }
1484
```

</details>


<a id="l1485"></a>

## L1485–L1539: assign_xs_bdev()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L1485)

| Source lines | Explanation |
| --- | --- |
| L1485–L1491 | Resolve an existing mapping or create a new one. `dev_state` is an output pointer; `ctxt` is not used directly in this function body. |
| L1493 | Initialize the output state to NORMAL for a newly assigned device. |
| L1494 | First ask SMD for the target's existing mapping for this role. |
| L1495–L1500 | Only a missing mapping invokes weighted `choose_device()`. A selection failure returns NULL without attempting assignment. |
| L1502–L1508 | Persist roles on the chosen device. Report insertion failure or return the chosen device on success. |
| L1509–L1513 | Propagate other SMD lookup failures as a NULL result with a diagnostic. Do not overwrite an unreadable mapping. |
| L1515–L1516 | For an existing mapping, require a snapshot and copy its persisted device state into the output. |
| L1517–L1526 | Comment explains why a mapped UUID might be absent from the in-memory device list: missing hardware or inconsistent stored configuration. This path does not silently choose another device. |
| L1527 | Find the current BIO device by the UUID in SMD, bypassing weighting entirely. |
| L1528–L1534 | If that UUID is missing, emit an unplugged-device RAS event with identity/target/role details. |
| L1535–L1537 | Free the snapshot and return the device pointer, possibly NULL. This is the key reason changing weights does not remap existing targets. |

<details>
<summary>Numbered source for this section</summary>

```text
1485  static struct bio_bdev *
1486  assign_xs_bdev(struct bio_xs_context *ctxt, int tgt_id, enum smd_dev_type st,
1487  	       unsigned int *dev_state)
1488  {
1489  	struct bio_bdev		*d_bdev;
1490  	struct smd_dev_info	*dev_info = NULL;
1491  	int			 rc;
1492
1493  	*dev_state = SMD_DEV_NORMAL;
1494  	rc = smd_dev_get_by_tgt(tgt_id, st, &dev_info);
1495  	if (rc == -DER_NONEXIST) {
1496  		d_bdev = choose_device(tgt_id, st);
1497  		if (d_bdev == NULL) {
1498  			D_ERROR("Failed to choose bdev for tgt:%u type:%u\n", tgt_id, st);
1499  			return NULL;
1500  		}
1501
1502  		rc = assign_roles(d_bdev, tgt_id);
1503  		if (rc) {
1504  			D_ERROR("Failed to assign roles. "DF_RC"\n", DP_RC(rc));
1505  			return NULL;
1506  		}
1507
1508  		return d_bdev;
1509  	} else if (rc) {
1510  		D_ERROR("Failed to get device info for tgt:%u type:%u, "DF_RC"\n",
1511  			tgt_id, st, DP_RC(rc));
1512  		return NULL;
1513  	}
1514
1515  	D_ASSERT(dev_info != NULL);
1516  	*dev_state = dev_info->sdi_state;
1517  	/*
1518  	 * Two cases leading to the inconsistency between SMD information and
1519  	 * in-memory bio_bdev list:
1520  	 * 1. The SMD data is stale (server started with new SSD/Target
1521  	 *    configuration but old SMD data are not erased) or corrupted.
1522  	 * 2. The device is not plugged.
1523  	 *
1524  	 * We can't differentiate these two cases for now, so let's just abort
1525  	 * starting and ask admin to plug the device or fix the SMD manually.
1526  	 */
1527  	d_bdev = lookup_dev_by_id(dev_info->sdi_id);
1528  	if (d_bdev == NULL)
1529  		ras_notify_eventf(RAS_DEVICE_UNPLUGGED, RAS_TYPE_INFO, RAS_SEV_ERROR, NULL, NULL,
1530  				  NULL, NULL, NULL, NULL, NULL, NULL, NULL,
1531  				  "Device: " DF_UUID " "
1532  				  "[model:%s, serial:%s] for target:%d type:%d is unplugged",
1533  				  DP_UUID(dev_info->sdi_id), dev_info->sdi_model,
1534  				  dev_info->sdi_serial, tgt_id, st);
1535  	smd_dev_free_info(dev_info);
1536
1537  	return d_bdev;
1538  }
1539
```

</details>


<a id="l1540"></a>

## L1540–L1646: init_xs_blobstore_ctxt()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L1540)

| Source lines | Explanation |
| --- | --- |
| L1540–L1548 | Build one role's per-xstream storage context. Local variables separate the BIO device, shared BIO wrapper, SPDK store, per-xstream wrapper, persisted state, and status. |
| L1550–L1556 | Require that this role is not already initialized; reject an empty device list with an uninitialized-storage error. |
| L1558–L1562 | Allocate the per-xstream wrapper and store it in the role-indexed array. Caller cleanup handles partially built contexts on later errors. |
| L1564–L1566 | Resolve or assign the device through `assign_xs_bdev()`. A missing result stops initialization. |
| L1568–L1577 | Require a device name. The first xstream using this device allocates a shared blobstore wrapper and becomes its owner. |
| L1579–L1585 | Register this xstream in the shared wrapper and hold its reference. Cache the shared wrapper pointer for subsequent setup. |
| L1591–L1600 | Only the owner initializes shared state: SMD NORMAL becomes BIO NORMAL; SMD FAULTY becomes BIO OUT; other persisted states are rejected. |
| L1602–L1608 | Initialize device health monitoring, propagating failures. |
| L1610–L1611 | An OUT device keeps its management representation but is not opened for ordinary storage I/O. |
| L1613–L1622 | For a usable device, load its existing SPDK blobstore with the expected UUID/type, store the handle, and log it. This call has `create=false`. |
| L1624–L1625 | Non-owner xstreams also skip channel allocation for an OUT device. |
| L1627–L1637 | Require a loaded store and no preexisting channel; allocate this xstream's SPDK I/O channel or return `-DER_NOMEM`. Success completes this role context. |
| L1640–L1646 | Documentation for the next function: release an xstream's NVMe context and participating SPDK resources. |

<details>
<summary>Numbered source for this section</summary>

```text
1540  static int
1541  init_xs_blobstore_ctxt(struct bio_xs_context *ctxt, int tgt_id, enum smd_dev_type st)
1542  {
1543  	struct bio_bdev		*d_bdev;
1544  	struct bio_blobstore	*bbs;
1545  	struct spdk_blob_store	*bs;
1546  	struct bio_xs_blobstore	*bxb;
1547  	unsigned int		 dev_state;
1548  	int			 rc;
1549
1550  	D_ASSERT(ctxt->bxc_xs_blobstores[st] == NULL);
1551
1552  	if (d_list_empty(&nvme_glb.bd_bdevs)) {
1553  		D_ERROR("No available SPDK bdevs, please check whether "
1554  			"VOS_BDEV_CLASS is set properly.\n");
1555  		return -DER_UNINIT;
1556  	}
1557
1558  	ctxt->bxc_xs_blobstores[st] = alloc_xs_blobstore();
1559  	if (ctxt->bxc_xs_blobstores[st] == NULL) {
1560  		D_ERROR("Failed to allocate memory for xs blobstore\n");
1561  		return -DER_NOMEM;
1562  	}
1563
1564  	d_bdev = assign_xs_bdev(ctxt, tgt_id, st, &dev_state);
1565  	if (d_bdev == NULL)
1566  		return -DER_NONEXIST;
1567
1568  	D_ASSERT(d_bdev->bb_name != NULL);
1569  	/*
1570  	 * If no bbs (BIO blobstore) is attached to the device, attach one and
1571  	 * set current xstream as bbs owner.
1572  	 */
1573  	if (d_bdev->bb_blobstore == NULL) {
1574  		d_bdev->bb_blobstore = alloc_bio_blobstore(ctxt, d_bdev);
1575  		if (d_bdev->bb_blobstore == NULL)
1576  			return -DER_NOMEM;
1577  	}
1578
1579  	bxb = ctxt->bxc_xs_blobstores[st];
1580  	/* Hold bbs refcount for current xstream */
1581  	bxb->bxb_blobstore = get_bio_blobstore(d_bdev->bb_blobstore, ctxt);
1582  	if (bxb->bxb_blobstore == NULL)
1583  		return -DER_NOMEM;
1584
1585  	bbs = bxb->bxb_blobstore;
1586
1587  	/*
1588  	 * bbs owner xstream is responsible to initialize monitoring context
1589  	 * and open SPDK blobstore.
1590  	 */
1591  	if (is_bbs_owner(ctxt, bbs)) {
1592  		/* Initialize BS state according to SMD state */
1593  		if (dev_state == SMD_DEV_NORMAL) {
1594  			bbs->bb_state = BIO_BS_STATE_NORMAL;
1595  		} else if (dev_state == SMD_DEV_FAULTY) {
1596  			bbs->bb_state = BIO_BS_STATE_OUT;
1597  		} else {
1598  			D_ERROR("Invalid SMD state:%d\n", dev_state);
1599  			return -DER_INVAL;
1600  		}
1601
1602  		/* Initialize health monitor */
1603  		rc = bio_init_health_monitoring(bbs, d_bdev->bb_name);
1604  		if (rc != 0) {
1605  			D_ERROR("BIO health monitor init failed. "DF_RC"\n",
1606  				DP_RC(rc));
1607  			return rc;
1608  		}
1609
1610  		if (bbs->bb_state == BIO_BS_STATE_OUT)
1611  			return 0;
1612
1613  		/* Load blobstore with bstype specified for sanity check */
1614  		bs = load_blobstore(ctxt, d_bdev->bb_name, &d_bdev->bb_uuid,
1615  				    false, false, NULL, NULL);
1616  		if (bs == NULL)
1617  			return -DER_INVAL;
1618  		bbs->bb_bs = bs;
1619
1620  		D_DEBUG(DB_MGMT, "Loaded bs, tgt_id:%d, xs:%p dev:%s\n",
1621  			tgt_id, ctxt, d_bdev->bb_name);
1622  	}
1623
1624  	if (bbs->bb_state == BIO_BS_STATE_OUT)
1625  		return 0;
1626
1627  	/* Open IO channel for current xstream */
1628  	bs = bbs->bb_bs;
1629  	D_ASSERT(bs != NULL);
1630  	D_ASSERT(bxb->bxb_io_channel == NULL);
1631  	bxb->bxb_io_channel = spdk_bs_alloc_io_channel(bs);
1632  	if (bxb->bxb_io_channel == NULL) {
1633  		D_ERROR("Failed to create io channel\n");
1634  		return -DER_NOMEM;
1635  	}
1636
1637  	return 0;
1638  }
1639
1640  /*
1641   * Finalize per-xstream NVMe context and SPDK env.
1642   *
1643   * \param[IN] ctxt	Per-xstream NVMe context
1644   *
1645   * \returns		N/A
1646   */
```

</details>


<a id="l1647"></a>

## L1647–L1757: bio_xsctxt_free()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L1647)

| Source lines | Explanation |
| --- | --- |
| L1647–L1656 | Teardown entry point initializes locals and treats a NULL context as already finished. |
| L1658–L1661 | Walk role slots and skip those that were never initialized. |
| L1663–L1668 | Free the role's I/O channel, clear its pointer, and detach the role slot from the xstream. |
| L1670–L1678 | Release its shared blobstore reference, finish health monitoring if this is the owner, clear the shared pointer, and free the per-xstream wrapper. |
| L1681–L1683 | Lock global lifecycle state and decrement the number of attached xstreams when positive. |
| L1685–L1691 | If the init thread still exists and this is it, close the optional SPDK RPC server before global teardown. |
| L1693–L1701 | The init xstream waits for all other xstreams to finish before removing shared devices/subsystems. The condition wait releases the mutex while blocked. |
| L1703 | Now destroy the global BIO device records, after per-xstream resources are gone. |
| L1705–L1717 | Start asynchronous subsystem finalization and poll with a timeout. If it cannot finish, set the skip-draining flag to avoid an unbounded later drain. |
| L1719–L1720 | Clear global initialization thread/context pointers. |
| L1722–L1724 | A non-init xstream that is the last remaining user wakes the waiting init xstream. |
| L1727 | Unlock global lifecycle state before finishing the local SPDK thread and DMA buffer. |
| L1729–L1738 | If there is a thread and draining was not disabled, poll until the thread is idle. Logs report the target and draining mode. |
| L1740–L1747 | Request thread exit, poll until exited unless skipping draining, then destroy the SPDK thread and clear the pointer. |
| L1750–L1755 | Destroy any DMA buffer and free the BIO execution-stream context itself. |

<details>
<summary>Numbered source for this section</summary>

```text
1647  void
1648  bio_xsctxt_free(struct bio_xs_context *ctxt)
1649  {
1650  	int			 rc = 0;
1651  	enum smd_dev_type	 st;
1652  	struct bio_xs_blobstore	*bxb;
1653
1654  	/* NVMe context setup was skipped */
1655  	if (ctxt == NULL)
1656  		return;
1657
1658  	for (st = SMD_DEV_TYPE_DATA; st < SMD_DEV_TYPE_MAX; st++) {
1659  		bxb = ctxt->bxc_xs_blobstores[st];
1660  		if (bxb == NULL)
1661  			continue;
1662
1663  		if (bxb->bxb_io_channel != NULL) {
1664  			spdk_bs_free_io_channel(bxb->bxb_io_channel);
1665  			bxb->bxb_io_channel = NULL;
1666  		}
1667
1668  		ctxt->bxc_xs_blobstores[st] = NULL;
1669
1670  		if (bxb->bxb_blobstore != NULL) {
1671  			put_bio_blobstore(bxb, ctxt);
1672
1673  			if (is_bbs_owner(ctxt, bxb->bxb_blobstore))
1674  				bio_fini_health_monitoring(ctxt, bxb->bxb_blobstore);
1675
1676  			bxb->bxb_blobstore = NULL;
1677  		}
1678  		D_FREE(bxb);
1679  	}
1680
1681  	ABT_mutex_lock(nvme_glb.bd_mutex);
1682  	if (nvme_glb.bd_xstream_cnt > 0)
1683  		nvme_glb.bd_xstream_cnt--;
1684
1685  	if (nvme_glb.bd_init_thread != NULL) {
1686  		if (is_init_xstream(ctxt)) {
1687  			struct common_cp_arg	cp_arg;
1688
1689  			/* Close SPDK JSON-RPC server if it has been enabled. */
1690  			if (nvme_glb.bd_enable_rpc_srv)
1691  				spdk_rpc_finish();
1692
1693  			/*
1694  			 * The xstream initialized SPDK env will have to
1695  			 * wait for all other xstreams finalized first.
1696  			 */
1697  			if (nvme_glb.bd_xstream_cnt != 0) {
1698  				D_DEBUG(DB_MGMT, "Init xs waits\n");
1699  				ABT_cond_wait(nvme_glb.bd_barrier,
1700  					      nvme_glb.bd_mutex);
1701  			}
1702
1703  			fini_bio_bdevs(ctxt);
1704
1705  			common_prep_arg(&cp_arg);
1706  			D_DEBUG(DB_MGMT, "Finalizing SPDK subsystems\n");
1707  			spdk_subsystem_fini(common_fini_cb, &cp_arg);
1708  			/*
1709  			 * spdk_subsystem_fini() won't run to completion if
1710  			 * any bdev is held by open blobs, set a timeout as
1711  			 * temporary workaround.
1712  			 */
1713  			rc = xs_poll_completion(ctxt, &cp_arg.cca_inflights,
1714  						bio_spdk_subsys_timeout);
1715  			DL_CDEBUG(rc == 0, DB_MGMT, DLOG_ERR, rc, "SPDK subsystems finalized");
1716  			if (rc != 0)
1717  				ctxt->bxc_skip_draining = 1;
1718
1719  			nvme_glb.bd_init_thread = NULL;
1720  			nvme_glb.bd_init_xs     = NULL;
1721
1722  		} else if (nvme_glb.bd_xstream_cnt == 0) {
1723  			ABT_cond_broadcast(nvme_glb.bd_barrier);
1724  		}
1725  	}
1726
1727  	ABT_mutex_unlock(nvme_glb.bd_mutex);
1728
1729  	if (ctxt->bxc_thread != NULL) {
1730  		D_DEBUG(DB_MGMT, "Finalizing SPDK thread, tgt_id:%d, skip_draining:%u",
1731  			ctxt->bxc_tgt_id, ctxt->bxc_skip_draining);
1732
1733  		/*
1734  		 * Don't drain events if we are asked to skip this (usually
1735  		 * due to an earlier error).
1736  		 */
1737  		while (!ctxt->bxc_skip_draining && !spdk_thread_is_idle(ctxt->bxc_thread))
1738  			spdk_thread_poll(ctxt->bxc_thread, 0, 0);
1739
1740  		D_DEBUG(DB_MGMT, "SPDK thread finalized, tgt_id:%d",
1741  			ctxt->bxc_tgt_id);
1742
1743  		spdk_thread_exit(ctxt->bxc_thread);
1744  		while (!ctxt->bxc_skip_draining && !spdk_thread_is_exited(ctxt->bxc_thread))
1745  			spdk_thread_poll(ctxt->bxc_thread, 0, 0);
1746  		spdk_thread_destroy(ctxt->bxc_thread);
1747  		ctxt->bxc_thread = NULL;
1748  	}
1749
1750  	if (ctxt->bxc_dma_buf != NULL) {
1751  		dma_buffer_destroy(ctxt->bxc_dma_buf);
1752  		ctxt->bxc_dma_buf = NULL;
1753  	}
1754
1755  	D_FREE(ctxt);
1756  }
1757
```

</details>


<a id="l1758"></a>

## L1758–L1775: subsystem_init_cb()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L1758)

| Source lines | Explanation |
| --- | --- |
| L1758–L1766 | Intermediate subsystem-init callback: on failure, delegate to the final completion/cleanup callback and stop. |
| L1768–L1773 | On success, enter SPDK RPC RUNTIME state and replay the configuration for methods valid only in runtime, using `subsys_init_cb` as the final completion callback. |

<details>
<summary>Numbered source for this section</summary>

```text
1758  static void
1759  subsystem_init_cb(int rc, void *arg)
1760  {
1761  	struct subsystem_init_arg *init_arg;
1762
1763  	if (rc) {
1764  		subsys_init_cb(rc, arg);
1765  		return;
1766  	}
1767
1768  	init_arg = arg;
1769
1770  	/* Set RUNTIME state and load config again for RUNTIME methods */
1771  	spdk_rpc_set_state(SPDK_RPC_RUNTIME);
1772  	spdk_subsystem_load_config(init_arg->json_data, init_arg->json_data_size, subsys_init_cb,
1773  				   init_arg, true);
1774  }
1775
```

</details>


<a id="l1776"></a>

## L1776–L1787: load_config_cb()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L1776)

| Source lines | Explanation |
| --- | --- |
| L1776–L1785 | Initial configuration-load callback: failures go directly to final cleanup; success starts subsystem initialization and passes `subsystem_init_cb` as the next callback. |

<details>
<summary>Numbered source for this section</summary>

```text
1776  static void
1777  load_config_cb(int rc, void *arg)
1778  {
1779  	if (rc) {
1780  		subsys_init_cb(rc, arg);
1781  		return;
1782  	}
1783
1784  	/* init subsystem */
1785  	spdk_subsystem_init(subsystem_init_cb, arg);
1786  }
1787
```

</details>


<a id="l1788"></a>

## L1788–L1827: bio_xsctxt_init_by_config()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L1788)

| Source lines | Explanation |
| --- | --- |
| L1788–L1799 | Load JSON configuration bytes from the owned config path. Declare their buffer/length and the heap callback context; NULL file-load result is reported as `-DER_NOMEM` by this code. |
| L1801–L1812 | Existing comment explains repeated SPDK initialization in tools such as DDB. Explicitly reset the RPC state to STARTUP before applying initial configuration methods. |
| L1814–L1818 | Allocate the callback context; if it fails, free the already-loaded JSON and return failure. |
| L1820–L1825 | Save the outer completion pointer and JSON ownership in the context, submit asynchronous configuration loading, and return after submission. The caller still has to poll for completion. |

<details>
<summary>Numbered source for this section</summary>

```text
1788  static int
1789  bio_xsctxt_init_by_config(struct common_cp_arg *cp_arg)
1790  {
1791  	struct subsystem_init_arg *init_arg;
1792  	void                      *json_data;
1793  	size_t                     json_data_size;
1794
1795  	json_data = spdk_posix_file_load_from_name(nvme_glb.bd_nvme_conf, &json_data_size);
1796  	if (json_data == NULL) {
1797  		D_ERROR("failed to load nvme conf %s\n", nvme_glb.bd_nvme_conf);
1798  		return -DER_NOMEM;
1799  	}
1800
1801  	/**
1802  	 * Initially, this was called internally spdk_subsystem_load_config() -> ... ->
1803  	 * spdk_rpc_initialize(). However, since commit
1804  	 * https://github.com/spdk/spdk/commit/fba209c7324a11b9230533144c02e7a66bc738ea (>=v24.01)
1805  	 * SPDK_RPC_STARTUP has become the initial value of the underlying global variable and it
1806  	 * is no longer reset automatically. This makes no difference for applications that
1807  	 * initialize SPDK only once during the lifetime of the process. But some BIO module
1808  	 * consumers—such as DDB—expect to be able to initialize, finalize, and then reinitialize
1809  	 * SPDK multiple times within the same process, for example when inspecting multiple pools
1810  	 * sequentially. For those use cases, the RPC state must now be reset explicitly.
1811  	 */
1812  	spdk_rpc_set_state(SPDK_RPC_STARTUP);
1813
1814  	D_ALLOC_PTR(init_arg);
1815  	if (init_arg == NULL) {
1816  		free(json_data);
1817  		return -DER_NOMEM;
1818  	}
1819
1820  	init_arg->cp_arg         = cp_arg;
1821  	init_arg->json_data      = json_data;
1822  	init_arg->json_data_size = (ssize_t)json_data_size;
1823  	spdk_subsystem_load_config(json_data, (ssize_t)json_data_size, load_config_cb, init_arg,
1824  				   true);
1825  	return 0;
1826  }
1827
```

</details>


<a id="l1828"></a>

## L1828–L1984: bio_xsctxt_alloc()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L1828)

| Source lines | Explanation |
| --- | --- |
| L1828–L1837 | Allocate one BIO execution-stream context. `pctxt` is an output pointer-to-pointer; `tgt_id` identifies the target; `self_polling` controls who drives completions. Declare the context/device/blobstore pointers, thread-name buffer, status, and role iterator. |
| L1839–L1844 | Allocate the context, reject allocation failure, and record target ID and polling mode. |
| L1846–L1856 | Without NVMe configuration, allocate only a DMA buffer. On failure free the context and clear the output; on success return the minimal context immediately. |
| L1858–L1862 | Lock global initialization state, increment the xstream count, and log the start. This lock also serializes the later device-selection and assignment work on this path. |
| L1864–L1877 | Create/register the SPDK thread before loading devices so configuration commands and blobstore operations can be polled. Set it as the calling execution context's current SPDK thread. |
| L1879–L1887 | Only the first xstream performs global discovery. A missing init thread identifies that first caller, and the xstream-count assertion checks the assumption. |
| L1889–L1895 | Prepare the completion state and start the JSON/configuration callback chain. Submission failure goes to cleanup. |
| L1897–L1907 | Poll until the initialization callback finishes. Propagate its error and skip later draining if subsystem startup failed, to avoid the existing potential hang described in the comment. |
| L1910–L1913 | Drain already-ready SPDK events. The lone semicolon is an intentionally empty loop body: the polling call itself performs the work. |
| L1915–L1922 | Record the initialization thread/context and initialize BIO devices. This calls the new weight validator and device-weight loader before any target selection. |
| L1924–L1927 | If the optional SPDK JSON-RPC server is enabled, choose its configured address or a default for an empty address. |
| L1929–L1939 | Start the endpoint; on failure go to cleanup. On success set RPC state to RUNTIME and log the listening address. |
| L1943–L1945 | Reset the previously-used-device pointer and walk DATA, META, WAL role slots for this target. |
| L1947–L1948 | Skip a DATA blobstore context for the internal system target. |
| L1949–L1951 | If the previous device already serves this role, reuse its shared context rather than creating another reference/channel here. Role lookup elsewhere accounts for this sharing. |
| L1952–L1954 | In legacy mode with no metadata-on-SSD, stop before trying META or WAL contexts. |
| L1956–L1958 | Initialize this role's context; this eventually reaches `assign_xs_bdev()` and, only for an unmapped target/role, weighted selection. |
| L1960–L1965 | Recover the just-created wrapper, shared blobstore, and device; assert that all exist, and remember the device for the next role's sharing check. |
| L1968–L1975 | Allocate one initial DMA chunk for the system target, otherwise use the calculated initial chunk count. Failure goes to common cleanup. |
| L1976–L1979 | Release the global mutex first; on error call full context teardown, which takes that mutex internally. |
| L1981–L1982 | Set the caller's output pointer to NULL on failure or to the completed context on success, then return the status. |

<details>
<summary>Numbered source for this section</summary>

```text
1828  int
1829  bio_xsctxt_alloc(struct bio_xs_context **pctxt, int tgt_id, bool self_polling)
1830  {
1831  	struct bio_xs_context	*ctxt;
1832  	struct bio_xs_blobstore	*bxb;
1833  	struct bio_blobstore	*bbs;
1834  	struct bio_bdev		*d_bdev;
1835  	char			 th_name[32];
1836  	int			 rc = 0;
1837  	enum smd_dev_type	 st;
1838
1839  	D_ALLOC_PTR(ctxt);
1840  	if (ctxt == NULL)
1841  		return -DER_NOMEM;
1842
1843  	ctxt->bxc_tgt_id = tgt_id;
1844  	ctxt->bxc_self_polling = self_polling;
1845
1846  	/* Skip NVMe context setup if the daos_nvme.conf isn't present */
1847  	if (!bio_nvme_configured(SMD_DEV_TYPE_MAX)) {
1848  		ctxt->bxc_dma_buf = dma_buffer_create(init_chk_cnt(), tgt_id);
1849  		if (ctxt->bxc_dma_buf == NULL) {
1850  			D_FREE(ctxt);
1851  			*pctxt = NULL;
1852  			return -DER_NOMEM;
1853  		}
1854  		*pctxt = ctxt;
1855  		return 0;
1856  	}
1857
1858  	ABT_mutex_lock(nvme_glb.bd_mutex);
1859  	nvme_glb.bd_xstream_cnt++;
1860
1861  	D_INFO("Initialize NVMe context, tgt_id:%d, init_thread:%p\n",
1862  	       tgt_id, nvme_glb.bd_init_thread);
1863
1864  	/*
1865  	 * Register SPDK thread beforehand, it could be used for poll device
1866  	 * admin commands completions and hotplugged events in following
1867  	 * spdk_subsystem_init_from_json_config() call, it also could be used
1868  	 * for blobstore metadata io channel in init_bio_bdevs() call.
1869  	 */
1870  	snprintf(th_name, sizeof(th_name), "daos_spdk_%d", tgt_id);
1871  	ctxt->bxc_thread = spdk_thread_create((const char *)th_name, NULL);
1872  	if (ctxt->bxc_thread == NULL) {
1873  		D_ERROR("failed to alloc SPDK thread\n");
1874  		rc = -DER_NOMEM;
1875  		goto out;
1876  	}
1877  	spdk_set_thread(ctxt->bxc_thread);
1878
1879  	/*
1880  	 * The first started xstream will scan all bdevs and create blobstores,
1881  	 * it's a prerequisite for all per-xstream blobstore initialization.
1882  	 */
1883  	if (nvme_glb.bd_init_thread == NULL) {
1884  		struct common_cp_arg cp_arg;
1885
1886  		D_ASSERTF(nvme_glb.bd_xstream_cnt == 1, "%d",
1887  			  nvme_glb.bd_xstream_cnt);
1888
1889  		/* Initialize all registered subsystems: bdev, vmd, copy. */
1890  		common_prep_arg(&cp_arg);
1891  		rc = bio_xsctxt_init_by_config(&cp_arg);
1892  		if (rc != 0) {
1893  			D_ERROR("failed to load nvme conf %s\n", nvme_glb.bd_nvme_conf);
1894  			goto out;
1895  		}
1896
1897  		rc = xs_poll_completion(ctxt, &cp_arg.cca_inflights, 0);
1898  		D_ASSERT(rc == 0);
1899  		if (cp_arg.cca_rc != 0) {
1900  			rc = cp_arg.cca_rc;
1901  			DL_ERROR(rc, "failed to init bdevs");
1902  			/*
1903  			 * We're afraid that draining the thread might never
1904  			 * complete (DAOS-17442).
1905  			 */
1906  			ctxt->bxc_skip_draining = 1;
1907  			goto out;
1908  		}
1909
1910  		/* Continue poll until no more events */
1911  		while (spdk_thread_poll(ctxt->bxc_thread, 0, 0) > 0)
1912  			;
1913  		D_DEBUG(DB_MGMT, "SPDK bdev initialized, tgt_id:%d", tgt_id);
1914
1915  		nvme_glb.bd_init_thread = ctxt->bxc_thread;
1916  		nvme_glb.bd_init_xs     = ctxt;
1917  		rc = init_bio_bdevs(ctxt);
1918  		if (rc != 0) {
1919  			D_ERROR("failed to init bio_bdevs, "DF_RC"\n",
1920  				DP_RC(rc));
1921  			goto out;
1922  		}
1923
1924  		/* After bio_bdevs are initialized, restart SPDK JSON-RPC server if required. */
1925  		if (nvme_glb.bd_enable_rpc_srv) {
1926  			if ((!nvme_glb.bd_rpc_srv_addr) || (strlen(nvme_glb.bd_rpc_srv_addr) == 0))
1927  				nvme_glb.bd_rpc_srv_addr = SPDK_DEFAULT_RPC_ADDR;
1928
1929  			rc = spdk_rpc_initialize(nvme_glb.bd_rpc_srv_addr, NULL);
1930  			if (rc != 0) {
1931  				D_ERROR("failed to start SPDK JSON-RPC server at %s, "DF_RC"\n",
1932  					nvme_glb.bd_rpc_srv_addr, DP_RC(daos_errno2der(-rc)));
1933  				goto out;
1934  			}
1935
1936  			/* Set SPDK JSON-RPC server state to receive and process RPCs */
1937  			spdk_rpc_set_state(SPDK_RPC_RUNTIME);
1938  			D_DEBUG(DB_MGMT, "SPDK JSON-RPC server listening at %s\n",
1939  				nvme_glb.bd_rpc_srv_addr);
1940  		}
1941  	}
1942
1943  	d_bdev = NULL;
1944  	/* Initialize per-xstream blobstore context */
1945  	for (st = SMD_DEV_TYPE_DATA; st < SMD_DEV_TYPE_MAX; st++) {
1946  		/* No Data blobstore for sys xstream */
1947  		if (st == SMD_DEV_TYPE_DATA && tgt_id == BIO_SYS_TGT_ID)
1948  			continue;
1949  		/* Share the same device/blobstore used by previous type */
1950  		if (d_bdev && is_role_match(d_bdev->bb_roles, smd_dev_type2role(st)))
1951  			continue;
1952  		/* No Meta/WAL blobstore if Metadata on SSD is not configured */
1953  		if (st != SMD_DEV_TYPE_DATA && !bio_nvme_configured(SMD_DEV_TYPE_META))
1954  			break;
1955
1956  		rc = init_xs_blobstore_ctxt(ctxt, tgt_id, st);
1957  		if (rc)
1958  			goto out;
1959
1960  		bxb = ctxt->bxc_xs_blobstores[st];
1961  		D_ASSERT(bxb != NULL);
1962  		bbs = bxb->bxb_blobstore;
1963  		D_ASSERT(bbs != NULL);
1964  		d_bdev = bbs->bb_dev;
1965  		D_ASSERT(d_bdev != NULL);
1966  	}
1967
1968  	/* Sys target only needs very limited DMA buffer for the WAL of RDB */
1969  	ctxt->bxc_dma_buf = dma_buffer_create(tgt_id == BIO_SYS_TGT_ID ? 1 : init_chk_cnt(),
1970  					      tgt_id);
1971  	if (ctxt->bxc_dma_buf == NULL) {
1972  		D_ERROR("failed to initialize dma buffer\n");
1973  		rc = -DER_NOMEM;
1974  		goto out;
1975  	}
1976  out:
1977  	ABT_mutex_unlock(nvme_glb.bd_mutex);
1978  	if (rc != 0)
1979  		bio_xsctxt_free(ctxt);
1980
1981  	*pctxt = (rc != 0) ? NULL : ctxt;
1982  	return rc;
1983  }
1984
```

</details>


<a id="l1985"></a>

## L1985–L2003: bio_nvme_ctl()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L1985)

| Source lines | Explanation |
| --- | --- |
| L1985–L1995 | Small control entry point. `BIO_CTL_NOTIFY_STARTED` updates the lifecycle flag under the global mutex. `*((bool *)arg)` casts the opaque argument to a bool pointer and reads the pointed-to value. |
| L1996–L2001 | Unknown commands return `-DER_INVAL`; the supported command returns zero. |

<details>
<summary>Numbered source for this section</summary>

```text
1985  int
1986  bio_nvme_ctl(unsigned int cmd, void *arg)
1987  {
1988  	int	rc = 0;
1989
1990  	switch (cmd) {
1991  	case BIO_CTL_NOTIFY_STARTED:
1992  		ABT_mutex_lock(nvme_glb.bd_mutex);
1993  		nvme_glb.bd_started = *((bool *)arg);
1994  		ABT_mutex_unlock(nvme_glb.bd_mutex);
1995  		break;
1996  	default:
1997  		D_ERROR("Invalid ctl cmd %d\n", cmd);
1998  		rc = -DER_INVAL;
1999  		break;
2000  	}
2001  	return rc;
2002  }
2003
```

</details>


<a id="l2004"></a>

## L2004–L2017: reset_media_errors()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L2004)

| Source lines | Explanation |
| --- | --- |
| L2004–L2008 | Obtain pointers to the blobstore's health record and its nested NVMe statistics, without copying either structure. |
| L2010–L2015 | Clear stalled-I/O, read/write/unmap/checksum-error counters and the faulty-completion marker before a device is set up again. This is in-memory monitoring state. |

<details>
<summary>Numbered source for this section</summary>

```text
2004  static inline void
2005  reset_media_errors(struct bio_blobstore *bbs)
2006  {
2007  	struct bio_dev_health *bdh       = &bbs->bb_dev_health;
2008  	struct nvme_stats     *dev_stats = &bdh->bdh_health_state;
2009
2010  	bdh->bdh_io_stalled       = 0;
2011  	dev_stats->bio_read_errs = 0;
2012  	dev_stats->bio_write_errs = 0;
2013  	dev_stats->bio_unmap_errs = 0;
2014  	dev_stats->checksum_errs = 0;
2015  	bbs->bb_faulty_done = 0;
2016  }
2017
```

</details>


<a id="l2018"></a>

## L2018–L2061: setup_bio_bdev()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L2018)

| Source lines | Explanation |
| --- | --- |
| L2018–L2024 | Runtime setup callback for a reappearing device; recover the BIO device and shared wrapper from the opaque argument. |
| L2026–L2031 | Skip setup during startup/shutdown and require the wrapper to be OUT before attempting reactivation. |
| L2033–L2038 | Query SMD for this device UUID; missing or unreadable persistent state prevents setup. |
| L2040–L2047 | A device still marked FAULTY in SMD is not automatically made healthy merely because it reappeared. Invalid states also exit without setup. |
| L2049–L2053 | For a NORMAL stored device, clear monitoring errors, transition the wrapper to SETUP, assert success, and free the SMD snapshot. |
| L2056–L2061 | Comment for the scanner: it runs on the init xstream and must not block progress with inappropriate waits, because it also helps drive NVMe completions. |

<details>
<summary>Numbered source for this section</summary>

```text
2018  void
2019  setup_bio_bdev(void *arg)
2020  {
2021  	struct smd_dev_info	*dev_info;
2022  	struct bio_bdev		*d_bdev = arg;
2023  	struct bio_blobstore	*bbs = d_bdev->bb_blobstore;
2024  	int			 rc;
2025
2026  	if (!is_server_started()) {
2027  		D_INFO("Skip device setup on server start/shutdown\n");
2028  		return;
2029  	}
2030
2031  	D_ASSERT(bbs->bb_state == BIO_BS_STATE_OUT);
2032
2033  	rc = smd_dev_get_by_id(d_bdev->bb_uuid, &dev_info);
2034  	if (rc != 0) {
2035  		D_ERROR("Original dev "DF_UUID" not in SMD. "DF_RC"\n",
2036  			DP_UUID(d_bdev->bb_uuid), DP_RC(rc));
2037  		return;
2038  	}
2039
2040  	if (dev_info->sdi_state == SMD_DEV_FAULTY) {
2041  		D_INFO("Faulty dev "DF_UUID" is plugged back\n",
2042  		       DP_UUID(d_bdev->bb_uuid));
2043  		goto out;
2044  	} else if (dev_info->sdi_state != SMD_DEV_NORMAL) {
2045  		D_ERROR("Invalid dev state %d\n", dev_info->sdi_state);
2046  		goto out;
2047  	}
2048
2049  	reset_media_errors(bbs);
2050  	rc = bio_bs_state_set(bbs, BIO_BS_STATE_SETUP);
2051  	D_ASSERT(rc == 0);
2052  out:
2053  	smd_dev_free_info(dev_info);
2054  }
2055
2056  /*
2057   * Scan the SPDK bdev list and compare it with bio_bdev list to see if any
2058   * device is hot plugged. This function is periodically called by the 'init'
2059   * xstream, be careful on using mutex or any blocking functions, that could
2060   * block the NVMe poll and lead to deadlock at the end.
2061   */
```

</details>


<a id="l2062"></a>

## L2062–L2161: scan_bio_bdevs()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L2062)

| Source lines | Explanation |
| --- | --- |
| L2062–L2071 | Declare SPDK/BIO iterators, default role mask, status, and a static scan period retained across calls. |
| L2073–L2074 | Return early if the next scan is not due yet. |
| L2076–L2085 | Enumerate matching SPDK devices and skip any current name already represented in BIO's global list. |
| L2087–L2093 | Report a newly seen device through console and RAS notifications. Set the scan-period marker to zero so another scan happens sooner. |
| L2095–L2101 | Select initial roles based on storage mode: explicit DATA in metadata-on-SSD mode, legacy zero mask otherwise. Replacement later inherits old device roles. |
| L2103–L2107 | Build the BIO record with `create_bio_bdev()`, which also reads the weight keyed by this current bdev name. Log failure and stop this scan's discovery loop on error. |
| L2109–L2114 | A NULL output means a new unused device was registered, or old-device teardown has not finished; no setup message is needed yet. |
| L2116–L2123 | For a returned replacement/replug record, require an open descriptor and check for a shared wrapper. If no wrapper exists, there is no previous active BIO store to reactivate. |
| L2125 | Send setup work to the device-owner thread rather than doing it on the scanner thread. |
| L2128–L2132 | Next, scan BIO's list for records marked removed, using an iterator safe for deletions. Skip unaffected devices. |
| L2134–L2142 | Removed, unused, non-replacing devices can be unlinked and destroyed immediately. |
| L2144–L2146 | A NULL descriptor means teardown has already released the device; skip rescheduling it. |
| L2148–L2151 | Request a shorter next scan and schedule teardown on the owner for devices with an attached wrapper. |
| L2154–L2159 | Convert the activity marker into the short or normal scan interval, and remember this scan's timestamp. |

<details>
<summary>Numbered source for this section</summary>

```text
2062  static void
2063  scan_bio_bdevs(struct bio_xs_context *ctxt, uint64_t now)
2064  {
2065  	struct bio_blobstore	*bbs;
2066  	struct bio_bdev		*d_bdev, *tmp;
2067  	struct spdk_bdev	*bdev;
2068  	const char		*bdev_name;
2069  	unsigned int             roles       = 0;
2070  	static uint64_t		 scan_period = NVME_MONITOR_PERIOD;
2071  	int			 rc;
2072
2073  	if (nvme_glb.bd_scan_age + scan_period >= now)
2074  		return;
2075
2076  	/* Iterate SPDK bdevs to detect hot plugged device */
2077  	for (bdev = spdk_bdev_first(); bdev != NULL; bdev = spdk_bdev_next(bdev)) {
2078  		if (nvme_glb.bd_bdev_class != get_bdev_type(bdev))
2079  			continue;
2080
2081  		bdev_name = spdk_bdev_get_name(bdev);
2082
2083  		d_bdev = lookup_dev_by_name(bdev_name);
2084  		if (d_bdev != NULL)
2085  			continue;
2086
2087  		/* Print a console message */
2088  		D_PRINT("Detected hot plugged device %s\n", bdev_name);
2089  		ras_notify_eventf(RAS_DEVICE_PLUGGED, RAS_TYPE_INFO, RAS_SEV_NOTICE, NULL, NULL,
2090  				  NULL, NULL, NULL, NULL, NULL, NULL, NULL,
2091  				  "Detected hot plugged device: %s", bdev_name);
2092
2093  		scan_period = 0;
2094
2095  		/*
2096  		 * Assign default roles based on operating mode (MD-on-SSD or PMem), roles will
2097  		 * subsequently be updated based on "old" device on "replace".
2098  		 */
2099
2100  		if (bio_nvme_configured(SMD_DEV_TYPE_META))
2101  			roles = NVME_ROLE_DATA;
2102
2103  		rc = create_bio_bdev(ctxt, bdev_name, roles, &d_bdev);
2104  		if (rc) {
2105  			D_ERROR("Failed to init hot plugged device %s\n", bdev_name);
2106  			break;
2107  		}
2108
2109  		/*
2110  		 * The plugged device is a new device, or teardown procedure for
2111  		 * old bio_bdev isn't finished.
2112  		 */
2113  		if (d_bdev == NULL)
2114  			continue;
2115
2116  		D_ASSERT(d_bdev->bb_desc != NULL);
2117  		bbs = d_bdev->bb_blobstore;
2118  		/* The device isn't used by DAOS yet */
2119  		if (bbs == NULL) {
2120  			D_INFO("New device "DF_UUID" is plugged back\n",
2121  			       DP_UUID(d_bdev->bb_uuid));
2122  			continue;
2123  		}
2124
2125  		spdk_thread_send_msg(owner_thread(bbs), setup_bio_bdev, d_bdev);
2126  	}
2127
2128  	/* Iterate bio_bdev list to trigger teardown on hot removed device */
2129  	d_list_for_each_entry_safe(d_bdev, tmp, &nvme_glb.bd_bdevs, bb_link) {
2130  		/* Device isn't removed */
2131  		if (!d_bdev->bb_removed)
2132  			continue;
2133
2134  		bbs = d_bdev->bb_blobstore;
2135  		/* Device not used by DAOS */
2136  		if (bbs == NULL && !d_bdev->bb_replacing) {
2137  			D_DEBUG(DB_MGMT, "Removed device "DF_UUID"(%s)\n",
2138  				DP_UUID(d_bdev->bb_uuid), d_bdev->bb_name);
2139  			d_list_del_init(&d_bdev->bb_link);
2140  			destroy_bio_bdev(d_bdev);
2141  			continue;
2142  		}
2143
2144  		/* Device is already torndown */
2145  		if (d_bdev->bb_desc == NULL)
2146  			continue;
2147
2148  		scan_period = 0;
2149  		if (bbs != NULL)
2150  			spdk_thread_send_msg(owner_thread(bbs),
2151  					     teardown_bio_bdev, d_bdev);
2152  	}
2153
2154  	if (scan_period == 0)
2155  		scan_period = NVME_MONITOR_SHORT_PERIOD;
2156  	else
2157  		scan_period = NVME_MONITOR_PERIOD;
2158
2159  	nvme_glb.bd_scan_age = now;
2160  }
2161
```

</details>


<a id="l2162"></a>

## L2162–L2193: bio_led_reset_on_timeout()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L2162)

| Source lines | Explanation |
| --- | --- |
| L2162–L2166 | Periodic LED helper: declare the device iterator and management-call status. |
| L2168–L2172 | Visit each BIO device. A nonzero expiry strictly earlier than `now` means an identify/blink timeout has expired. |
| L2174–L2180 | Ask the LED helper to restore the normal/fault-indicating state, reporting errors. This has no effect on weighted assignment. |
| L2185–L2193 | Comment describes the polling entry point and its work/no-work/exited return convention. The current implementation has early-return cases discussed below. |

<details>
<summary>Numbered source for this section</summary>

```text
2162  void
2163  bio_led_reset_on_timeout(struct bio_xs_context *ctxt, uint64_t now)
2164  {
2165  	struct bio_bdev         *d_bdev;
2166  	int			 rc;
2167
2168  	/* Scan all devices present in bio_bdev list */
2169  	d_list_for_each_entry(d_bdev, bio_bdev_list(), bb_link) {
2170  		if ((d_bdev->bb_led_expiry_time != 0) && (d_bdev->bb_led_expiry_time < now)) {
2171  			D_DEBUG(DB_MGMT, "Clearing LED QUICK_BLINK state for " DF_UUID "\n",
2172  				DP_UUID(d_bdev->bb_uuid));
2173
2174  			/* LED will be reset to faulty or normal state based on SSDs bio_bdevs. */
2175  			rc = bio_led_manage(ctxt, NULL, d_bdev->bb_uuid,
2176  					    (unsigned int)CTL__LED_ACTION__RESET, NULL, 0);
2177  			if (rc != 0) {
2178  				DL_ERROR(rc, "Reset LED on device:" DF_UUID " failed",
2179  					 DP_UUID(d_bdev->bb_uuid));
2180  			}
2181  		}
2182  	}
2183  }
2184
2185  /*
2186   * Execute the messages on msg ring, call all registered pollers.
2187   *
2188   * \param[IN] ctxt	Per-xstream NVMe context
2189   *
2190   * \returns		0: If mo work was done
2191   *			1: If work was done
2192   *			-1: If thread has exited
2193   */
```

</details>


<a id="l2194"></a>

## L2194–L2235: bio_nvme_poll()

[Open this source location](https://github.com/chizuchizu/daos/blob/046caab85bb523d26104b7a6b109c9ac027bf5b2/src/bio/bio_xstream.c#L2194)

| Source lines | Explanation |
| --- | --- |
| L2194–L2200 | Obtain the current monitoring time and declare role/context/status locals. |
| L2202–L2207 | If NVMe is not configured, do nothing. Otherwise require a valid context/thread and poll SPDK once, saving its result. |
| L2209–L2216 | During startup/shutdown, skip health and hotplug handling to avoid races with changing device/context lists. Return zero here even if the earlier SPDK poll did work. |
| L2218–L2223 | For each role wrapper owned by this xstream, run shared blobstore health monitoring. Non-owner xstreams do not duplicate the owner-only work. |
| L2225–L2229 | Only the initialization xstream scans hotplug state and resets timed-out identification LEDs. |
| L2231–L2234 | Monitor stalled I/O, then return the saved SPDK poll result. Weight parsing and new-target assignment are not performed on this ordinary I/O-progress path. |

<details>
<summary>Numbered source for this section</summary>

```text
2194  int
2195  bio_nvme_poll(struct bio_xs_context *ctxt)
2196  {
2197  	uint64_t		 now = d_timeus_secdiff(0);
2198  	enum smd_dev_type	 st;
2199  	int			 rc;
2200  	struct bio_xs_blobstore	*bxb;
2201
2202  	/* NVMe context setup was skipped */
2203  	if (!bio_nvme_configured(SMD_DEV_TYPE_MAX))
2204  		return 0;
2205
2206  	D_ASSERT(ctxt != NULL && ctxt->bxc_thread != NULL);
2207  	rc = spdk_thread_poll(ctxt->bxc_thread, 0, 0);
2208
2209  	/*
2210  	 * To avoid complicated race handling (init xstream and starting
2211  	 * VOS xstream concurrently access global device list & xstream
2212  	 * context array), we just simply disable faulty device detection
2213  	 * and hot remove/plug processing during server start/shutdown.
2214  	 */
2215  	if (!is_server_started())
2216  		return 0;
2217
2218  	/* Monitor device health on the device owner xstream */
2219  	for (st = SMD_DEV_TYPE_DATA; st < SMD_DEV_TYPE_MAX; st++) {
2220  		bxb = ctxt->bxc_xs_blobstores[st];
2221  		if (bxb && bxb->bxb_blobstore && is_bbs_owner(ctxt, bxb->bxb_blobstore))
2222  			bio_bs_monitor(ctxt, st, now);
2223  	}
2224
2225  	/* Detect new plugged device, manage LED on init xstream */
2226  	if (is_init_xstream(ctxt)) {
2227  		scan_bio_bdevs(ctxt, now);
2228  		bio_led_reset_on_timeout(ctxt, now);
2229  	}
2230
2231  	/* Detect stalled I/Os */
2232  	bio_io_monitor(ctxt, now);
2233
2234  	return rc;
2235  }
```

</details>
