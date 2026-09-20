/**
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
#ifndef __BIO_WEIGHT_H__
#define __BIO_WEIGHT_H__

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>

/* Bounded weights keep comparisons exact even with INT_MAX mapped targets. */
#define BIO_NVME_WEIGHT_MAX  65535U
#define BIO_NVME_WEIGHTS_ENV "DAOS_NVME_DEVICE_WEIGHTS"

/* Parse one name=weight entry without modifying the environment string.
 * Return 1 for an entry, 0 at end, -1 for invalid input (including duplicates).
 */
static inline int
bio_weight_next(const char *config, const char **cursor, const char **name, size_t *name_len,
		unsigned int *weight)
{
	const char  *start = *cursor, *p, *prior;
	unsigned int value = 0;

	if (start == NULL)
		return 0;

	p = start;
	while (*p && *p != '=' && *p != ',') {
		if (isspace((unsigned char)*p))
			return -1;
		p++;
	}
	if (p == start || *p != '=')
		return -1;
	*name     = start;
	*name_len = p - start;
	/* Previous entries have already been parsed successfully. */
	for (prior = config; prior < start; prior = strchr(prior, ',') + 1) {
		const char *eq = strchr(prior, '=');

		if ((size_t)(eq - prior) == *name_len && memcmp(prior, start, *name_len) == 0)
			return -1;
	}
	p++;
	if (*p < '0' || *p > '9')
		return -1;
	while (*p >= '0' && *p <= '9') {
		value = value * 10 + (*p++ - '0');
		if (value > BIO_NVME_WEIGHT_MAX)
			return -1;
	}
	if (value == 0 || (*p != '\0' && *p != ',') || (*p == ',' && p[1] == '\0'))
		return -1;
	*weight = value;
	*cursor = *p == ',' ? p + 1 : NULL;
	return 1;
}

static inline int
bio_weight_get(const char *config, const char *device, unsigned int *weight)
{
	const char  *cursor = config, *name;
	size_t       len;
	unsigned int value;
	int          rc;

	*weight = 1;
	while ((rc = bio_weight_next(config, &cursor, &name, &len, &value)) > 0) {
		if (strlen(device) == len && memcmp(device, name, len) == 0)
			*weight = value;
	}
	return rc;
}

/* Reserve a complete role assignment without overflowing the persistent table. */
static inline bool
bio_weight_fits(unsigned int used, unsigned int needed, unsigned int limit)
{
	return needed > 0 && used <= limit && needed <= limit - used;
}

/* Preserve list-order ties and the legacy least-target policy for unit weights. */
static inline bool
bio_weight_less(unsigned int count, unsigned int weight, unsigned int best_count,
		unsigned int best_weight)
{
	return (uint64_t)count * best_weight < (uint64_t)best_count * weight;
}

#endif /* __BIO_WEIGHT_H__ */
