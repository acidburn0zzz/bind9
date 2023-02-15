/*
 * Copyright (C) Internet Systems Consortium, Inc. ("ISC")
 *
 * SPDX-License-Identifier: MPL-2.0
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, you can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * See the COPYRIGHT file distributed with this work for additional
 * information regarding copyright ownership.
 */

/*
 * This is an implementation of the Robin Hood hash table algorithm as
 * described in [a] with simple linear searching, and backwards shift
 * deletion algorithm as described in [b] and [c].
 *
 * Further work:
 * 1. Implement 4.1 Speeding up Searches - 4.4 Smart Search [a]
 * 2. Implement A Fast Concurrent and Resizable Robin Hood Hash Table [b]
 *
 * a. https://cs.uwaterloo.ca/research/tr/1986/CS-86-14.pdf paper.
 * b. https://dspace.mit.edu/bitstream/handle/1721.1/130693/1251799942-MIT.pdf
 * c.
 * https://codecapsule.com/2013/11/17/robin-hood-hashing-backward-shift-deletion/
 */

#include <ctype.h>
#include <inttypes.h>
#include <string.h>

#include <isc/ascii.h>
#include <isc/entropy.h>
#include <isc/hash.h>
#include <isc/hashmap.h>
#include <isc/magic.h>
#include <isc/mem.h>
#include <isc/result.h>
#include <isc/siphash.h>
#include <isc/types.h>
#include <isc/util.h>

#define APPROX_99_PERCENT(x) (((x)*1013) >> 10)
#define APPROX_95_PERCENT(x) (((x)*972) >> 10)
#define APPROX_90_PERCENT(x) (((x)*921) >> 10)
#define APPROX_85_PERCENT(x) (((x)*870) >> 10)
#define APPROX_40_PERCENT(x) (((x)*409) >> 10)
#define APPROX_35_PERCENT(x) (((x)*359) >> 10)
#define APPROX_30_PERCENT(x) (((x)*308) >> 10)
#define APPROX_25_PERCENT(x) (((x)*256) >> 10)
#define APPROX_20_PERCENT(x) (((x)*205) >> 10)
#define APPROX_15_PERCENT(x) (((x)*154) >> 10)
#define APPROX_10_PERCENT(x) (((x)*103) >> 10)
#define APPROX_05_PERCENT(x) (((x)*52) >> 10)
#define APPROX_01_PERCENT(x) (((x)*11) >> 10)

#define ISC_HASHMAP_MAGIC	   ISC_MAGIC('H', 'M', 'a', 'p')
#define ISC_HASHMAP_VALID(hashmap) ISC_MAGIC_VALID(hashmap, ISC_HASHMAP_MAGIC)

/* We have two tables for incremental rehashing */
#define HASHMAP_NUM_TABLES 2

#define HASHSIZE(bits) (UINT64_C(1) << (bits))

#define HASHMAP_NO_BITS	 0U
#define HASHMAP_MIN_BITS 1U
#define HASHMAP_MAX_BITS 32U

typedef struct hashmap_node {
	const uint8_t *key;
	void *value;
	uint32_t hashval;
	uint32_t psl;
	uint16_t keysize;
} hashmap_node_t;

typedef struct hashmap_table {
	size_t size;
	uint8_t hashbits;
	uint32_t hashmask;
	hashmap_node_t *table;
} hashmap_table_t;

struct isc_hashmap {
	unsigned int magic;
	bool case_sensitive;
	uint8_t hindex;
	uint32_t hiter; /* rehashing iterator */
	isc_mem_t *mctx;
	size_t count;
	uint8_t hash_key[16];
	hashmap_table_t tables[HASHMAP_NUM_TABLES];
};

struct isc_hashmap_iter {
	isc_hashmap_t *hashmap;
	size_t i;
	uint8_t hindex;
	hashmap_node_t *cur;
};

static isc_result_t
hashmap_add(isc_hashmap_t *hashmap, const uint32_t hashval, const uint8_t *key,
	    const uint32_t keysize, void *value, uint8_t idx);

static void
hashmap_rehash_one(isc_hashmap_t *hashmap);
static void
hashmap_rehash_start_grow(isc_hashmap_t *hashmap);
static void
hashmap_rehash_start_shrink(isc_hashmap_t *hashmap);
static bool
over_threshold(isc_hashmap_t *hashmap);
static bool
under_threshold(isc_hashmap_t *hashmap);

static uint8_t
hashmap_nexttable(uint8_t idx) {
	return ((idx == 0) ? 1 : 0);
}

static bool
rehashing_in_progress(const isc_hashmap_t *hashmap) {
	return (hashmap->tables[hashmap_nexttable(hashmap->hindex)].table !=
		NULL);
}

static bool
try_nexttable(const isc_hashmap_t *hashmap, uint8_t idx) {
	return (idx == hashmap->hindex && rehashing_in_progress(hashmap));
}

static void
hashmap_node_init(hashmap_node_t *node, const uint32_t hashval,
		  const uint8_t *key, const uint32_t keysize, void *value) {
	REQUIRE(key != NULL && keysize <= UINT16_MAX);

	*node = (hashmap_node_t){
		.value = value,
		.hashval = hashval,
		.key = key,
		.keysize = keysize,
		.psl = 0,
	};
}

static void __attribute__((__unused__))
hashmap_dump_table(const isc_hashmap_t *hashmap, const uint8_t idx) {
	fprintf(stderr,
		"====== %" PRIu8 " (bits = %" PRIu8 ", size = %zu =====\n", idx,
		hashmap->tables[idx].hashbits, hashmap->tables[idx].size);
	for (size_t i = 0; i < hashmap->tables[idx].size; i++) {
		hashmap_node_t *node = &hashmap->tables[idx].table[i];
		if (node->key != NULL) {
			uint32_t hash = isc_hash_bits32(
				node->hashval, hashmap->tables[idx].hashbits);
			fprintf(stderr,
				"%p: %zu -> %p"
				", value = %p"
				", hash = %" PRIu32 ", hashval = %" PRIu32
				", psl = %" PRIu32 ", key = %s\n",
				hashmap, i, node, node->value, hash,
				node->hashval, node->psl, (char *)node->key);
		}
	}
	fprintf(stderr, "================\n\n");
}

static void
hashmap_create_table(isc_hashmap_t *hashmap, const uint8_t idx,
		     const uint8_t bits) {
	size_t size;

	REQUIRE(hashmap->tables[idx].hashbits == HASHMAP_NO_BITS);
	REQUIRE(hashmap->tables[idx].table == NULL);
	REQUIRE(bits >= HASHMAP_MIN_BITS);
	REQUIRE(bits <= HASHMAP_MAX_BITS);

	hashmap->tables[idx] = (hashmap_table_t){
		.hashbits = bits,
		.hashmask = HASHSIZE(bits) - 1,
		.size = HASHSIZE(bits),
	};

	size = hashmap->tables[idx].size *
	       sizeof(hashmap->tables[idx].table[0]);

	hashmap->tables[idx].table = isc_mem_getx(hashmap->mctx, size,
						  ISC_MEM_ZERO);
}

static void
hashmap_free_table(isc_hashmap_t *hashmap, const uint8_t idx, bool cleanup) {
	size_t size;

	if (cleanup) {
		for (size_t i = 0; i < hashmap->tables[idx].size; i++) {
			hashmap_node_t *node = &hashmap->tables[idx].table[i];
			if (node->key != NULL) {
				*node = (hashmap_node_t){ 0 };
				hashmap->count--;
			}
		}
	}

	size = hashmap->tables[idx].size *
	       sizeof(hashmap->tables[idx].table[0]);
	isc_mem_put(hashmap->mctx, hashmap->tables[idx].table, size);

	hashmap->tables[idx] = (hashmap_table_t){
		.hashbits = HASHMAP_NO_BITS,
	};
}

void
isc_hashmap_create(isc_mem_t *mctx, uint8_t bits, unsigned int options,
		   isc_hashmap_t **hashmapp) {
	isc_hashmap_t *hashmap = isc_mem_get(mctx, sizeof(*hashmap));
	bool case_sensitive = ((options & ISC_HASHMAP_CASE_INSENSITIVE) == 0);

	REQUIRE(hashmapp != NULL && *hashmapp == NULL);
	REQUIRE(mctx != NULL);
	REQUIRE(bits >= HASHMAP_MIN_BITS && bits <= HASHMAP_MAX_BITS);

	*hashmap = (isc_hashmap_t){
		.magic = ISC_HASHMAP_MAGIC,
		.hash_key = { 0, 1 },
		.case_sensitive = case_sensitive,
	};
	isc_mem_attach(mctx, &hashmap->mctx);

#if !defined(FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION) && !defined(UNIT_TESTING)
	isc_entropy_get(hashmap->hash_key, sizeof(hashmap->hash_key));
#endif /* if FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION */

	hashmap_create_table(hashmap, 0, bits);

	hashmap->magic = ISC_HASHMAP_MAGIC;

	*hashmapp = hashmap;
}

void
isc_hashmap_destroy(isc_hashmap_t **hashmapp) {
	isc_hashmap_t *hashmap;

	REQUIRE(hashmapp != NULL && *hashmapp != NULL);
	REQUIRE(ISC_HASHMAP_VALID(*hashmapp));

	hashmap = *hashmapp;
	*hashmapp = NULL;

	hashmap->magic = 0;

	for (size_t i = 0; i < HASHMAP_NUM_TABLES; i++) {
		if (hashmap->tables[i].table != NULL) {
			hashmap_free_table(hashmap, i, true);
		}
	}
	INSIST(hashmap->count == 0);

	isc_mem_putanddetach(&hashmap->mctx, hashmap, sizeof(*hashmap));
}

static bool
hashmap_match(hashmap_node_t *node, const uint32_t hashval, const uint8_t *key,
	      const uint32_t keysize, const bool case_sensitive) {
	return (node->hashval == hashval && node->keysize == keysize &&
		(case_sensitive
			 ? (memcmp(node->key, key, keysize) == 0)
			 : (isc_ascii_lowerequal(node->key, key, keysize))));
}

static hashmap_node_t *
hashmap_find(const isc_hashmap_t *hashmap, const uint32_t hashval,
	     const uint8_t *key, uint32_t keysize, uint32_t *pslp,
	     uint8_t *idxp) {
	uint32_t hash;
	uint32_t psl;
	uint8_t idx = *idxp;
	uint32_t pos;

nexttable:
	psl = 0;
	hash = isc_hash_bits32(hashval, hashmap->tables[idx].hashbits);

	while (true) {
		hashmap_node_t *node = NULL;

		pos = (hash + psl) & hashmap->tables[idx].hashmask;

		node = &hashmap->tables[idx].table[pos];

		if (node->key == NULL || psl > node->psl) {
			break;
		}

		if (hashmap_match(node, hashval, key, keysize,
				  hashmap->case_sensitive))
		{
			*pslp = psl;
			*idxp = idx;
			return (node);
		}

		psl++;
	}
	if (try_nexttable(hashmap, idx)) {
		idx = hashmap_nexttable(idx);
		goto nexttable;
	}

	return (NULL);
}

uint32_t
isc_hashmap_hash(const isc_hashmap_t *hashmap, const void *key,
		 uint32_t keysize) {
	REQUIRE(ISC_HASHMAP_VALID(hashmap));

	uint32_t hashval;

	isc_halfsiphash24(hashmap->hash_key, key, keysize,
			  hashmap->case_sensitive, (uint8_t *)&hashval);

	return (hashval);
}

isc_result_t
isc_hashmap_find(const isc_hashmap_t *hashmap, const uint32_t *hashvalp,
		 const void *key, uint32_t keysize, void **valuep) {
	REQUIRE(ISC_HASHMAP_VALID(hashmap));
	REQUIRE(key != NULL && keysize <= UINT16_MAX);
	REQUIRE(valuep == NULL || *valuep == NULL);

	hashmap_node_t *node = NULL;
	uint8_t idx = hashmap->hindex;
	uint32_t hashval = (hashvalp != NULL)
				   ? *hashvalp
				   : isc_hashmap_hash(hashmap, key, keysize);

	node = hashmap_find(hashmap, hashval, key, keysize, &(uint32_t){ 0 },
			    &idx);
	if (node == NULL) {
		return (ISC_R_NOTFOUND);
	}

	INSIST(node->key != NULL);
	if (valuep != NULL) {
		*valuep = node->value;
	}
	return (ISC_R_SUCCESS);
}

static void
hashmap_delete_node(isc_hashmap_t *hashmap, hashmap_node_t *entry,
		    uint32_t hashval, uint32_t psl, const uint8_t idx) {
	uint32_t pos;
	uint32_t hash;

	hashmap->count--;

	hash = isc_hash_bits32(hashval, hashmap->tables[idx].hashbits);
	pos = hash + psl;

	while (true) {
		hashmap_node_t *node = NULL;

		pos = (pos + 1) & hashmap->tables[idx].hashmask;
		INSIST(pos < hashmap->tables[idx].size);

		node = &hashmap->tables[idx].table[pos];

		if (node->key == NULL || node->psl == 0) {
			break;
		}

		node->psl--;
		*entry = *node;
		entry = &hashmap->tables[idx].table[pos];
	}

	*entry = (hashmap_node_t){ 0 };
}

static void
hashmap_rehash_one(isc_hashmap_t *hashmap) {
	uint8_t oldidx = hashmap_nexttable(hashmap->hindex);
	uint32_t oldsize = hashmap->tables[oldidx].size;
	hashmap_node_t *oldtable = hashmap->tables[oldidx].table;
	hashmap_node_t node;
	isc_result_t result;

	/* Find first non-empty node */
	while (hashmap->hiter < oldsize && oldtable[hashmap->hiter].key == NULL)
	{
		hashmap->hiter++;
	}

	/* Rehashing complete */
	if (hashmap->hiter == oldsize) {
		hashmap_free_table(hashmap, hashmap_nexttable(hashmap->hindex),
				   false);
		hashmap->hiter = 0;
		return;
	}

	/* Move the first non-empty node from old table to new table */
	node = oldtable[hashmap->hiter];

	hashmap_delete_node(hashmap, &oldtable[hashmap->hiter], node.hashval,
			    node.psl, oldidx);

	result = hashmap_add(hashmap, node.hashval, node.key, node.keysize,
			     node.value, hashmap->hindex);
	INSIST(result == ISC_R_SUCCESS);

	/*
	 * we don't increase the hiter here because the table has been reordered
	 * when we deleted the old node
	 */
}

static uint32_t
grow_bits(isc_hashmap_t *hashmap) {
	uint32_t newbits = hashmap->tables[hashmap->hindex].hashbits + 1;
	size_t newsize = HASHSIZE(newbits);

	while (hashmap->count > APPROX_40_PERCENT(newsize)) {
		newbits += 1;
		newsize = HASHSIZE(newbits);
	}
	if (newbits > HASHMAP_MAX_BITS) {
		newbits = HASHMAP_MAX_BITS;
	}

	return (newbits);
}

static uint32_t
shrink_bits(isc_hashmap_t *hashmap) {
	uint32_t newbits = hashmap->tables[hashmap->hindex].hashbits - 1;

	if (newbits <= HASHMAP_MIN_BITS) {
		newbits = HASHMAP_MIN_BITS;
	}

	return (newbits);
}

static void
hashmap_rehash_start_grow(isc_hashmap_t *hashmap) {
	uint32_t newbits;
	uint8_t oldindex = hashmap->hindex;
	uint32_t oldbits = hashmap->tables[oldindex].hashbits;
	uint8_t newindex = hashmap_nexttable(oldindex);

	REQUIRE(!rehashing_in_progress(hashmap));

	newbits = grow_bits(hashmap);

	if (newbits > oldbits) {
		hashmap_create_table(hashmap, newindex, newbits);
		hashmap->hindex = newindex;
	}
}

static void
hashmap_rehash_start_shrink(isc_hashmap_t *hashmap) {
	uint32_t newbits;
	uint8_t oldindex = hashmap->hindex;
	uint32_t oldbits = hashmap->tables[oldindex].hashbits;
	uint8_t newindex = hashmap_nexttable(oldindex);

	REQUIRE(!rehashing_in_progress(hashmap));

	newbits = shrink_bits(hashmap);

	if (newbits < oldbits) {
		hashmap_create_table(hashmap, newindex, newbits);
		hashmap->hindex = newindex;
	}
}

isc_result_t
isc_hashmap_delete(isc_hashmap_t *hashmap, const uint32_t *hashvalp,
		   const void *key, uint32_t keysize) {
	REQUIRE(ISC_HASHMAP_VALID(hashmap));
	REQUIRE(key != NULL && keysize <= UINT16_MAX);

	hashmap_node_t *node;
	isc_result_t result = ISC_R_NOTFOUND;
	uint32_t psl = 0;
	uint8_t idx;
	uint32_t hashval = (hashvalp != NULL)
				   ? *hashvalp
				   : isc_hashmap_hash(hashmap, key, keysize);

	if (rehashing_in_progress(hashmap)) {
		hashmap_rehash_one(hashmap);
	} else if (under_threshold(hashmap)) {
		hashmap_rehash_start_shrink(hashmap);
		hashmap_rehash_one(hashmap);
	}

	/* Initialize idx after possible shrink start */
	idx = hashmap->hindex;

	node = hashmap_find(hashmap, hashval, key, keysize, &psl, &idx);
	if (node != NULL) {
		INSIST(node->key != NULL);
		hashmap_delete_node(hashmap, node, hashval, psl, idx);
		result = ISC_R_SUCCESS;
	}

	return (result);
}

static bool
over_threshold(isc_hashmap_t *hashmap) {
	uint32_t bits = hashmap->tables[hashmap->hindex].hashbits;
	if (bits == HASHMAP_MAX_BITS) {
		return (false);
	}
	size_t threshold = APPROX_90_PERCENT(HASHSIZE(bits));
	return (hashmap->count > threshold);
}

static bool
under_threshold(isc_hashmap_t *hashmap) {
	uint32_t bits = hashmap->tables[hashmap->hindex].hashbits;
	if (bits == HASHMAP_MIN_BITS) {
		return (false);
	}
	size_t threshold = APPROX_20_PERCENT(HASHSIZE(bits));
	return (hashmap->count < threshold);
}

static isc_result_t
hashmap_add(isc_hashmap_t *hashmap, const uint32_t hashval, const uint8_t *key,
	    const uint32_t keysize, void *value, uint8_t idx) {
	uint32_t hash;
	uint32_t psl = 0;
	hashmap_node_t node;
	hashmap_node_t *current = NULL;
	uint32_t pos;

	hash = isc_hash_bits32(hashval, hashmap->tables[idx].hashbits);

	/* Initialize the node to be store to 'node' */
	hashmap_node_init(&node, hashval, key, keysize, value);

	psl = 0;
	while (true) {
		pos = (hash + psl) & hashmap->tables[idx].hashmask;

		current = &hashmap->tables[idx].table[pos];

		/* Found an empty node */
		if (current->key == NULL) {
			break;
		}

		if (hashmap_match(current, hashval, key, keysize,
				  hashmap->case_sensitive))
		{
			return (ISC_R_EXISTS);
		}
		/* Found rich node */
		if (node.psl > current->psl) {
			/* Swap the poor with the rich node */
			ISC_SWAP(*current, node);
		}

		node.psl++;
		psl++;
	}

	/*
	 * Possible optimalization - start growing when the poor node is too far
	 */
#if ISC_HASHMAP_GROW_FAST
	if (psl > hashmap->hashbits[idx]) {
		if (!rehashing_in_progress(hashmap)) {
			hashmap_rehash_start_grow(hashmap);
		}
	}
#endif

	hashmap->count++;

	/* We found an empty place, store entry into current node */
	*current = node;

	return (ISC_R_SUCCESS);
}

isc_result_t
isc_hashmap_add(isc_hashmap_t *hashmap, const uint32_t *hashvalp,
		const void *key, uint32_t keysize, void *value) {
	REQUIRE(ISC_HASHMAP_VALID(hashmap));
	REQUIRE(key != NULL && keysize <= UINT16_MAX);

	isc_result_t result;
	uint32_t hashval = (hashvalp != NULL)
				   ? *hashvalp
				   : isc_hashmap_hash(hashmap, key, keysize);

	if (rehashing_in_progress(hashmap)) {
		hashmap_rehash_one(hashmap);
	} else if (over_threshold(hashmap)) {
		hashmap_rehash_start_grow(hashmap);
		hashmap_rehash_one(hashmap);
	}

	if (rehashing_in_progress(hashmap)) {
		uint8_t fidx = hashmap_nexttable(hashmap->hindex);
		uint32_t psl;

		/* Look for the value in the old table */
		if (hashmap_find(hashmap, hashval, key, keysize, &psl, &fidx)) {
			return (ISC_R_EXISTS);
		}
	}

	result = hashmap_add(hashmap, hashval, key, keysize, value,
			     hashmap->hindex);
	switch (result) {
	case ISC_R_SUCCESS:
	case ISC_R_EXISTS:
		return (result);
	default:
		UNREACHABLE();
	}
}

void
isc_hashmap_iter_create(isc_hashmap_t *hashmap, isc_hashmap_iter_t **iterp) {
	isc_hashmap_iter_t *iter;

	REQUIRE(ISC_HASHMAP_VALID(hashmap));
	REQUIRE(iterp != NULL && *iterp == NULL);

	iter = isc_mem_get(hashmap->mctx, sizeof(*iter));
	*iter = (isc_hashmap_iter_t){
		.hashmap = hashmap,
		.hindex = hashmap->hindex,
	};

	*iterp = iter;
}

void
isc_hashmap_iter_destroy(isc_hashmap_iter_t **iterp) {
	isc_hashmap_iter_t *iter;
	isc_hashmap_t *hashmap;

	REQUIRE(iterp != NULL && *iterp != NULL);

	iter = *iterp;
	*iterp = NULL;
	hashmap = iter->hashmap;
	isc_mem_put(hashmap->mctx, iter, sizeof(*iter));
}

static isc_result_t
isc__hashmap_iter_next(isc_hashmap_iter_t *iter) {
	isc_hashmap_t *hashmap = iter->hashmap;

	while (iter->i < hashmap->tables[iter->hindex].size &&
	       hashmap->tables[iter->hindex].table[iter->i].key == NULL)
	{
		iter->i++;
	}

	if (iter->i < hashmap->tables[iter->hindex].size) {
		iter->cur = &hashmap->tables[iter->hindex].table[iter->i];

		return (ISC_R_SUCCESS);
	}

	if (try_nexttable(hashmap, iter->hindex)) {
		iter->hindex = hashmap_nexttable(iter->hindex);
		iter->i = 0;
		return (isc__hashmap_iter_next(iter));
	}

	return (ISC_R_NOMORE);
}

isc_result_t
isc_hashmap_iter_first(isc_hashmap_iter_t *iter) {
	REQUIRE(iter != NULL);

	iter->hindex = iter->hashmap->hindex;
	iter->i = 0;

	return (isc__hashmap_iter_next(iter));
}

isc_result_t
isc_hashmap_iter_next(isc_hashmap_iter_t *iter) {
	REQUIRE(iter != NULL);
	REQUIRE(iter->cur != NULL);

	iter->i++;

	return (isc__hashmap_iter_next(iter));
}

isc_result_t
isc_hashmap_iter_delcurrent_next(isc_hashmap_iter_t *iter) {
	REQUIRE(iter != NULL);
	REQUIRE(iter->cur != NULL);

	hashmap_node_t *node =
		&iter->hashmap->tables[iter->hindex].table[iter->i];

	hashmap_delete_node(iter->hashmap, node, node->hashval, node->psl,
			    iter->hindex);

	return (isc__hashmap_iter_next(iter));
}

void
isc_hashmap_iter_current(isc_hashmap_iter_t *it, void **valuep) {
	REQUIRE(it != NULL);
	REQUIRE(it->cur != NULL);
	REQUIRE(valuep != NULL && *valuep == NULL);

	*valuep = it->cur->value;
}

void
isc_hashmap_iter_currentkey(isc_hashmap_iter_t *it, const unsigned char **key,
			    size_t *keysize) {
	REQUIRE(it != NULL);
	REQUIRE(it->cur != NULL);
	REQUIRE(key != NULL && *key == NULL);

	*key = it->cur->key;
	*keysize = it->cur->keysize;
}

unsigned int
isc_hashmap_count(isc_hashmap_t *hashmap) {
	REQUIRE(ISC_HASHMAP_VALID(hashmap));

	return (hashmap->count);
}
