/*
 * Copyright (C) 1999  Internet Software Consortium.
 * 
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND INTERNET SOFTWARE CONSORTIUM DISCLAIMS
 * ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL INTERNET SOFTWARE
 * CONSORTIUM BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
 * ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 */

/*
 * $Id: tkey.c,v 1.19 2000/01/24 19:14:21 gson Exp $
 * Principal Author: Brian Wellington
 */


#include <config.h>

#include <stdlib.h>
#include <string.h>

#include <isc/assertions.h>
#include <isc/buffer.h>
#include <isc/error.h>
#include <isc/list.h>
#include <isc/log.h>
#include <isc/mem.h>
#include <isc/net.h>
#include <isc/result.h>
#include <isc/rwlock.h>
#include <isc/stdtime.h>
#include <isc/types.h>

#include <dns/dnssec.h>
#include <dns/keyvalues.h>
#include <dns/name.h>
#include <dns/message.h>
#include <dns/rdata.h>
#include <dns/rdatalist.h>
#include <dns/rdataset.h>
#include <dns/rdatastruct.h>
#include <dns/tkey.h>
#include <dns/tsig.h>
#include <dns/confctx.h>

#include <dst/dst.h>
#include <dst/result.h>

#define dns_tsigerror_badalg 21

#define TKEY_RANDOM_AMOUNT 16

#define RETERR(x) do { \
	result = (x); \
	if (result != ISC_R_SUCCESS) \
		goto failure; \
	} while (0)


isc_result_t
dns_tkeyctx_create(isc_mem_t *mctx, dns_tkey_ctx_t **tctx) {
	REQUIRE(mctx != NULL);
	REQUIRE(tctx != NULL);
	REQUIRE(*tctx == NULL);

	*tctx = isc_mem_get(mctx, sizeof(dns_tkey_ctx_t));
	if (*tctx == NULL)
		return (ISC_R_NOMEMORY);
	(*tctx)->mctx = mctx;
	(*tctx)->dhkey = NULL;
	(*tctx)->domain = NULL;

	return (ISC_R_SUCCESS);
}

void
dns_tkeyctx_destroy(dns_tkey_ctx_t **tctx) {
	isc_mem_t *mctx;

	REQUIRE(tctx != NULL);
	REQUIRE(*tctx != NULL);

	if ((*tctx)->dhkey != NULL)
		dst_key_free((*tctx)->dhkey);
	if ((*tctx)->domain != NULL) {
		dns_name_free((*tctx)->domain, (*tctx)->mctx);
		isc_mem_put((*tctx)->mctx, (*tctx)->domain, sizeof(dns_name_t));
	}

	mctx = (*tctx)->mctx;
	isc_mem_put(mctx, *tctx, sizeof(dns_tkey_ctx_t));
}

static isc_result_t
add_rdata_to_list(dns_message_t *msg, dns_name_t *name, dns_rdata_t *rdata,
		isc_uint32_t ttl, dns_namelist_t *namelist)
{
	isc_result_t result;
	isc_region_t r, newr;
	dns_rdata_t *newrdata = NULL;
	dns_name_t *newname = NULL;
	dns_rdatalist_t *newlist = NULL;
	dns_rdataset_t *newset = NULL;
	isc_buffer_t *tmprdatabuf = NULL, *tmpnamebuf = NULL;

	RETERR(dns_message_gettemprdata(msg, &newrdata));

	dns_rdata_toregion(rdata, &r);
	RETERR(isc_buffer_allocate(msg->mctx, &tmprdatabuf, r.length,
				   ISC_BUFFERTYPE_BINARY));
	isc_buffer_available(tmprdatabuf, &newr);
	memcpy(newr.base, r.base, r.length);
	dns_rdata_fromregion(newrdata, rdata->rdclass, rdata->type, &newr);
	dns_message_takebuffer(msg, &tmprdatabuf);

	dns_name_toregion(name, &r);
	RETERR(dns_message_gettempname(msg, &newname));
	dns_name_init(newname, NULL);
	RETERR(isc_buffer_allocate(msg->mctx, &tmpnamebuf, r.length,
				   ISC_BUFFERTYPE_BINARY));
	isc_buffer_available(tmpnamebuf, &newr);
	memcpy(newr.base, r.base, r.length);
	dns_name_fromregion(newname, &newr);
	dns_message_takebuffer(msg, &tmpnamebuf);

	RETERR(dns_message_gettemprdatalist(msg, &newlist));
	newlist->rdclass = newrdata->rdclass;
	newlist->type = newrdata->type;
	newlist->covers = 0;
	newlist->ttl = ttl;
	ISC_LIST_INIT(newlist->rdata);
	ISC_LIST_APPEND(newlist->rdata, newrdata, link);

	RETERR(dns_message_gettemprdataset(msg, &newset));
	dns_rdataset_init(newset);
	RETERR(dns_rdatalist_tordataset(newlist, newset));

	ISC_LIST_INIT(newname->list);
	ISC_LIST_APPEND(newname->list, newset, link);

	ISC_LIST_APPEND(*namelist, newname, link);

	return (ISC_R_SUCCESS);

 failure:
	if (newrdata != NULL)
		dns_message_puttemprdata(msg, &newrdata);
	if (newname != NULL)
		dns_message_puttempname(msg, &newname);
	if (newlist != NULL)
		dns_message_puttemprdatalist(msg, &newlist);
	if (newset != NULL) {
		dns_rdataset_disassociate(newset);
		dns_message_puttemprdataset(msg, &newset);
	}
	return (result);
}

static isc_result_t
compute_secret(isc_buffer_t *shared, isc_region_t *queryrandomness,
	       isc_region_t *serverrandomness, isc_buffer_t *secret)
{
	dst_context_t ctx;
	isc_result_t result;
	isc_region_t r, r2;
	char digests[32];
	isc_buffer_t b;
	unsigned int i;

	isc_buffer_init(&b, digests, sizeof(digests), ISC_BUFFERTYPE_BINARY);
	isc_buffer_used(shared, &r);

	/* MD5 ( query data | DH value ) */
	RETERR(dst_digest(DST_SIGMODE_INIT, DST_DIGEST_MD5, &ctx, NULL, NULL));
	RETERR(dst_digest(DST_SIGMODE_UPDATE, DST_DIGEST_MD5, &ctx,
			  queryrandomness, NULL));
	RETERR(dst_digest(DST_SIGMODE_UPDATE, DST_DIGEST_MD5, &ctx, &r, NULL));
	RETERR(dst_digest(DST_SIGMODE_FINAL, DST_DIGEST_MD5, &ctx, NULL, &b));
			
	/* MD5 ( server data | DH value ) */
	RETERR(dst_digest(DST_SIGMODE_INIT, DST_DIGEST_MD5, &ctx, NULL, NULL));
	RETERR(dst_digest(DST_SIGMODE_UPDATE, DST_DIGEST_MD5, &ctx,
			  serverrandomness, NULL));
	RETERR(dst_digest(DST_SIGMODE_UPDATE, DST_DIGEST_MD5, &ctx, &r, NULL));
	RETERR(dst_digest(DST_SIGMODE_FINAL, DST_DIGEST_MD5, &ctx, NULL, &b));

	/* XOR ( DH value, MD5-1 | MD5-2) */
	isc_buffer_available(secret, &r);
	isc_buffer_used(shared, &r2);
	if (r.length < sizeof(digests) || r.length < r2.length)
		return (ISC_R_NOSPACE);
	if (r2.length > sizeof(digests)) {
		memcpy(r.base, r2.base, r2.length);
		for (i = 0; i < sizeof(digests); i++)
			r.base[i] ^= digests[i];
		isc_buffer_add(secret, r2.length);
	}
	else {
		memcpy(r.base, digests, sizeof(digests));
		for (i = 0; i < r2.length; i++)
			r.base[i] ^= r2.base[i];
		isc_buffer_add(secret, sizeof(digests));
	}

 failure:
	return result;

}

static isc_result_t
process_dhtkey(dns_message_t *msg, dns_name_t *name,
	       dns_rdata_generic_tkey_t *tkeyin, dns_tkey_ctx_t *tctx,
	       dns_rdata_generic_tkey_t *tkeyout,
	       dns_tsig_keyring_t *ring, dns_namelist_t *namelist)
{
	isc_result_t result = ISC_R_SUCCESS;
	dns_name_t *keyname, ourname, signer, *creator;
	dns_rdataset_t *keyset;
	dns_rdata_t keyrdata, ourkeyrdata;
	isc_boolean_t found_key = ISC_FALSE, found_incompatible = ISC_FALSE;
	dst_key_t *pubkey = NULL;
	isc_buffer_t ourkeybuf, ournamein, ournameout, *shared = NULL;
	isc_region_t r, r2, ourkeyr;
	isc_uint32_t ourttl;
	unsigned char keydata[DST_KEY_MAXSIZE];
	unsigned char namedata[1024];
	dns_tsigkey_t *tsigkey;
	unsigned int sharedsize;
	isc_buffer_t randombuf, secret;
	unsigned char *randomdata = NULL, secretdata[256];

	/* Look for a DH KEY record that will work with ours */
	result = dns_message_firstname(msg, DNS_SECTION_ADDITIONAL);
	while (result == ISC_R_SUCCESS) {
		keyname = NULL;
		dns_message_currentname(msg, DNS_SECTION_ADDITIONAL, &keyname);
		keyset = NULL;
		result = dns_message_findtype(keyname, dns_rdatatype_key, 0,
					      &keyset);
		if (result == ISC_R_SUCCESS) {
			result = dns_rdataset_first(keyset);
			while (result == ISC_R_SUCCESS) {
				dns_rdataset_current(keyset, &keyrdata);
				pubkey = NULL;
				result = dns_dnssec_keyfromrdata(keyname,
								 &keyrdata,
								 msg->mctx,
								 &pubkey);
				if (result != ISC_R_SUCCESS) {
					result = dns_rdataset_next(keyset);
					continue;
				}
				if (dst_key_alg(pubkey) == DNS_KEYALG_DH) {
					if (dst_key_paramcompare(pubkey,
					    tctx->dhkey))
					{
						found_key = ISC_TRUE;
						goto got_key;
					}
					else
						found_incompatible = ISC_TRUE;
				}
				dst_key_free(pubkey);
				result = dns_rdataset_next(keyset);
			}
		}
		result = dns_message_nextname(msg, DNS_SECTION_ADDITIONAL);
	}

 got_key:
	if (!found_key) {
		if (found_incompatible) {
			tkeyout->error = dns_tsigerror_badkey;
			return (ISC_R_SUCCESS);
		}
		return (DNS_R_FORMERR);
	}

	RETERR(add_rdata_to_list(msg, keyname, &keyrdata, keyset->ttl,
				 namelist));

	isc_buffer_init(&ourkeybuf, keydata, sizeof(keydata),
			ISC_BUFFERTYPE_BINARY);
	RETERR(dst_key_todns(tctx->dhkey, &ourkeybuf));
	isc_buffer_used(&ourkeybuf, &ourkeyr);
	dns_rdata_fromregion(&ourkeyrdata, dns_rdataclass_in,
			     dns_rdatatype_key, &ourkeyr);
	isc_buffer_init(&ournamein, dst_key_name(tctx->dhkey),
		        strlen(dst_key_name(tctx->dhkey)), ISC_BUFFERTYPE_TEXT);
	isc_buffer_add(&ournamein, strlen(dst_key_name(tctx->dhkey)));
	isc_buffer_init(&ournameout, namedata, sizeof(namedata),
			ISC_BUFFERTYPE_BINARY);
	dns_name_init(&ourname, NULL);
	RETERR(dns_name_fromtext(&ourname, &ournamein, dns_rootname, ISC_FALSE,
				 &ournameout));
	ourttl = 0;
#if 0
	/* Not sure how to do this without a view... */
	db = NULL;
	result = dns_dbtable_find(client->view->dbtable, &ourname, &db);
	if (result == ISC_R_SUCCESS) {
		dns_rdataset_t set;
		dns_fixedname_t foundname;

		dns_rdataset_init(&set);
		dns_fixedname_init(&foundname);
		result = dns_db_find(db, &ourname, NULL, dns_rdatatype_key,
				     DNS_DBFIND_NOWILD, 0, NULL,
				     dns_fixedname_name(&foundname),
				     &set, NULL);
		if (result == ISC_R_SUCCESS) {
			ourttl = set.ttl;
			dns_rdataset_disassociate(&set);
		}
	}
#endif
	RETERR(add_rdata_to_list(msg, &ourname, &ourkeyrdata, ourttl,
				 namelist));

	RETERR(dst_secret_size(tctx->dhkey, &sharedsize));
	RETERR(isc_buffer_allocate(msg->mctx, &shared, sharedsize,
				   ISC_BUFFERTYPE_BINARY));

	RETERR(dst_computesecret(pubkey, tctx->dhkey, shared));

	isc_buffer_init(&secret, secretdata, sizeof(secretdata),
			ISC_BUFFERTYPE_BINARY);

	randomdata = isc_mem_get(tkeyout->mctx, TKEY_RANDOM_AMOUNT);
	if (randomdata == NULL) {
		result = ISC_R_NOMEMORY;
		goto failure;
	}
	isc_buffer_init(&randombuf, randomdata, TKEY_RANDOM_AMOUNT,
			ISC_BUFFERTYPE_BINARY);
	RETERR(dst_random_get(TKEY_RANDOM_AMOUNT, &randombuf));

	isc_buffer_used(&randombuf, &r);
	r2.base = tkeyin->key;
	r2.length = tkeyin->keylen;
	RETERR(compute_secret(shared, &r2, &r, &secret));

	dns_name_init(&signer, NULL);
	result = dns_message_signer(msg, &signer);
	/* handle DNS_R_NOTVERIFIEDYET */
	if (result == ISC_R_SUCCESS)
		creator = &signer;
	else
		creator = NULL;

	dst_key_free(pubkey);
	isc_buffer_used(&secret, &r);
	tsigkey = NULL;
	result = dns_tsigkey_create(name, &tkeyin->algorithm, r.base, r.length,
				    ISC_TRUE, creator, msg->mctx, ring,
				    NULL);
	isc_buffer_free(&shared);
	shared = NULL;
	if (result == ISC_R_NOTFOUND) {
		tkeyout->error = dns_tsigerror_badalg;
		return (ISC_R_SUCCESS);
	}
	if (result != ISC_R_SUCCESS)
		goto failure;

	/* This key is good for a long time */
	tkeyout->inception = 0;
	tkeyout->expire = 0x7FFFFFFF;

	tkeyout->key = randomdata;
	tkeyout->keylen = TKEY_RANDOM_AMOUNT;

	return (ISC_R_SUCCESS);

 failure:
	if (!ISC_LIST_EMPTY(*namelist)) {
		dns_name_t *tname = ISC_LIST_HEAD(*namelist);
		while (tname != NULL) {
			dns_name_t *next = ISC_LIST_NEXT(tname, link);
			dns_rdataset_t *tset;

			ISC_LIST_UNLINK(*namelist, tname, link);
			tset = ISC_LIST_HEAD(tname->list);
			dns_rdataset_disassociate(tset);
			dns_message_puttemprdataset(msg, &tset);
			dns_message_puttempname(msg, &tname);
			tname = next;
		}
	}
	if (shared != NULL)
		isc_buffer_free(&shared);
	return (result);
}

static isc_result_t
process_deletetkey(dns_message_t *msg, dns_name_t *name,
		   dns_rdata_generic_tkey_t *tkeyin,
		   dns_rdata_generic_tkey_t *tkeyout,
		   dns_tsig_keyring_t *ring,
		   dns_namelist_t *namelist)
{
	isc_result_t result;
	dns_tsigkey_t *tsigkey = NULL;
	dns_name_t signer;

	/* Unused variables */
	msg = msg;
	tkeyout = tkeyout;
	namelist = namelist;

	result = dns_tsigkey_find(&tsigkey, name, &tkeyin->algorithm, ring);
	if (result != ISC_R_SUCCESS)
		tkeyout->error = dns_tsigerror_badname;

	/*
	 * Only allow a delete if the identity that created the key is the
	 * same as the identity that signed the message.
	 */
	dns_name_init(&signer, NULL);
	result = dns_message_signer(msg, &signer);
	/* handle DNS_R_NOTVERIFIEDYET */
	if (result == DNS_R_NOIDENTITY) {
		/*
		 * Special case - there is no identity associated with the
		 * TSIG key that signed the message, but it's that key
		 * being deleted.  This is OK.
		 */
		if (!dns_name_equal(&signer, name))
			return (DNS_R_REFUSED);
	}
	else if (result != ISC_R_SUCCESS) {
		return (DNS_R_REFUSED);
	}
	else {
		dns_name_t *identity = dns_tsigkey_identity(tsigkey);
		if (identity == NULL || !dns_name_equal(identity, &signer))
			return (DNS_R_REFUSED);
	}

	/*
	 * Set the key to be deleted when no references are left.  If the key
	 * was not generated with TKEY and is in the config file, it may be
	 * reloaded later.
	 */
	dns_tsigkey_setdeleted(tsigkey);
	/* Release the reference */
	dns_tsigkey_free(&tsigkey);

	return (ISC_R_SUCCESS);
}

isc_result_t
dns_tkey_processquery(dns_message_t *msg, dns_tkey_ctx_t *tctx,
		      dns_tsig_keyring_t *ring)
{
	isc_result_t result = ISC_R_SUCCESS;
	dns_rdata_generic_tkey_t tkeyin, tkeyout;
	dns_name_t *qname, *name, *keyname, tempkeyname;
	dns_rdataset_t *tkeyset;
	dns_rdata_t tkeyrdata, *rdata = NULL;
	isc_buffer_t *dynbuf = NULL;
	dns_namelist_t namelist;

	REQUIRE(msg != NULL);
	REQUIRE(tctx != NULL);
	REQUIRE(ring != NULL);

	/* Need to do this to determine if this should be freed later */
	memset(&tkeyin, 0, sizeof(dns_rdata_generic_tkey_t));

	/* Interpret the question section */
	result = dns_message_firstname(msg, DNS_SECTION_QUESTION);
	INSIST(result == DNS_R_SUCCESS);

	qname = NULL;
	dns_message_currentname(msg, DNS_SECTION_QUESTION, &qname);

	/* Look for a TKEY record that matches the question */
	tkeyset = NULL;
	name = NULL;
	result = dns_message_findname(msg, DNS_SECTION_ADDITIONAL, qname,
				      dns_rdatatype_tkey, 0, &name, &tkeyset);
	if (result != ISC_R_SUCCESS) {
		result = DNS_R_FORMERR;
		goto failure;
	}
	result = dns_rdataset_first(tkeyset);
	if (result != ISC_R_SUCCESS) {
		result = DNS_R_FORMERR;
		goto failure;
	}
	dns_rdataset_current(tkeyset, &tkeyrdata);

	RETERR(dns_rdata_tostruct(&tkeyrdata, &tkeyin, msg->mctx));

	if (tkeyin.error != dns_rcode_noerror) {
		result = DNS_R_FORMERR;
		goto failure;
	}

	ISC_LIST_INIT(namelist);

	tkeyout.common.rdclass = tkeyin.common.rdclass;
	tkeyout.common.rdtype = tkeyin.common.rdtype;
	ISC_LINK_INIT(&tkeyout.common, link);
	tkeyout.mctx = msg->mctx;

	dns_name_init(&tkeyout.algorithm, NULL);
	RETERR(dns_name_dup(&tkeyin.algorithm, msg->mctx, &tkeyout.algorithm));

	tkeyout.inception = tkeyout.expire = 0;
	tkeyout.mode = tkeyin.mode;
	tkeyout.error = 0;
	tkeyout.keylen = tkeyout.otherlen = 0;
	tkeyout.key = tkeyout.other = NULL;

	/*
	 * A delete operation must have a fully specified key name.  If this
	 * is not a delete, we do the following:
	 * if (qname != ".")
	 *	keyname = qname + defaultdomain
	 * else
	 *	keyname = <random hex> + defaultdomain
	 */
	if (tkeyin.mode != DNS_TKEYMODE_DELETE) {
		dns_name_t prefix;
		isc_buffer_t *buf = NULL;
		unsigned char tdata[64];
		dns_tsigkey_t *tsigkey = NULL;

		dns_name_init(&tempkeyname, NULL);
		keyname = &tempkeyname;
		dns_name_init(&prefix, NULL);
		RETERR(isc_buffer_allocate(msg->mctx, &buf, 256,
					   ISC_BUFFERTYPE_BINARY));

		if (!dns_name_equal(qname, dns_rootname)) {
			unsigned int n = dns_name_countlabels(qname);
			dns_name_getlabelsequence(qname, 0, n - 1, &prefix);
		}
		else {
			static char hexdigits[16] = {
				'0', '1', '2', '3', '4', '5', '6', '7',
				'8', '9', 'A', 'B', 'C', 'D', 'E', 'F' };
			unsigned char randomtext[32];
			isc_buffer_t b, b2;
			int i;

			isc_buffer_init(&b, randomtext, sizeof(randomtext),
					ISC_BUFFERTYPE_BINARY);
			result = dst_random_get(sizeof(randomtext)/2, &b);
			if (result != ISC_R_SUCCESS) {
				dns_message_takebuffer(msg, &buf);
				goto failure;
			}

			for (i = sizeof(randomtext) - 2; i >= 0; i -= 2) {
				unsigned char val = randomtext[i/2];
				randomtext[i] = hexdigits[val >> 4];
				randomtext[i+1] = hexdigits[val & 0xF];
			}
			isc_buffer_init(&b, randomtext, sizeof(randomtext),
					ISC_BUFFERTYPE_TEXT);
			isc_buffer_init(&b2, tdata, sizeof(tdata),
					ISC_BUFFERTYPE_BINARY);
			isc_buffer_add(&b, sizeof(randomtext));
			result = dns_name_fromtext(&prefix, &b, NULL,
						   ISC_FALSE, &b2);
			if (result != ISC_R_SUCCESS) {
				dns_message_takebuffer(msg, &buf);
				goto failure;
			}
		}
		result = dns_name_concatenate(&prefix, tctx->domain,
					      keyname, buf);
		dns_message_takebuffer(msg, &buf);
		if (result != ISC_R_SUCCESS)
			goto failure;

		result = dns_tsigkey_find(&tsigkey, keyname, NULL, ring);
		if (result == ISC_R_SUCCESS) {
			tkeyout.error = dns_tsigerror_badname;
			dns_tsigkey_free(&tsigkey);
			goto failure_with_tkey;
		}
		else if (result != ISC_R_NOTFOUND)
			goto failure;
	}
	else
		keyname = qname;

	if (!dns_name_equal(&tkeyin.algorithm, DNS_TSIG_HMACMD5_NAME)) {
		tkeyout.error = dns_tsigerror_badkey;
		goto failure_with_tkey;
	}

	switch (tkeyin.mode) {
		case DNS_TKEYMODE_DIFFIEHELLMAN:
			RETERR(process_dhtkey(msg, keyname, &tkeyin, tctx,
					      &tkeyout, ring, &namelist));
			tkeyout.error = dns_rcode_noerror;
			break;
		case DNS_TKEYMODE_DELETE:
			RETERR(process_deletetkey(msg, keyname, &tkeyin,
						  &tkeyout, ring, &namelist));
			tkeyout.error = dns_rcode_noerror;
			break;
		case DNS_TKEYMODE_SERVERASSIGNED:
		case DNS_TKEYMODE_GSSAPI:
		case DNS_TKEYMODE_RESOLVERASSIGNED:
			result = DNS_R_NOTIMP;
			goto failure;
		default:
			tkeyout.error = dns_tsigerror_badmode;
	}

 failure_with_tkey:
	dns_rdata_freestruct(&tkeyin);

	RETERR(dns_message_gettemprdata(msg, &rdata));
	RETERR(isc_buffer_allocate(msg->mctx, &dynbuf, 128,
				   ISC_BUFFERTYPE_BINARY));
	result = dns_rdata_fromstruct(rdata, tkeyout.common.rdclass,
				      tkeyout.common.rdtype, &tkeyout, dynbuf);
	dns_rdata_freestruct(&tkeyout);
	if (result != ISC_R_SUCCESS)
		goto failure;

	RETERR(add_rdata_to_list(msg, keyname, rdata, 0, &namelist));

	isc_buffer_free(&dynbuf);

	RETERR(dns_message_reply(msg, ISC_TRUE));

	name = ISC_LIST_HEAD(namelist);
	while (name != NULL) {
		dns_name_t *next = ISC_LIST_NEXT(name, link);
		ISC_LIST_UNLINK(namelist, name, link);
		dns_message_addname(msg, name, DNS_SECTION_ADDITIONAL);
		name = next;
	}

	return (ISC_R_SUCCESS);

 failure:
	if (tkeyin.common.rdtype == dns_rdatatype_tkey)
		dns_rdata_freestruct(&tkeyin);
	if (rdata != NULL)
		dns_message_puttemprdata(msg, &rdata);
	if (dynbuf != NULL)
		isc_buffer_free(&dynbuf);
	return (result);
}

static isc_result_t
buildquery(dns_message_t *msg, dns_name_t *name,
	   dns_rdata_generic_tkey_t *tkey)
{
	dns_name_t *qname = NULL, *aname = NULL;
	dns_rdataset_t *question = NULL, *tkeyset = NULL;
	dns_rdatalist_t *tkeylist = NULL;
	dns_rdata_t *rdata = NULL;
	isc_buffer_t *dynbuf = NULL;
	isc_result_t result;

	REQUIRE(msg != NULL);
	REQUIRE(name != NULL);
	REQUIRE(tkey != NULL);

	RETERR(dns_message_gettempname(msg, &qname));
	RETERR(dns_message_gettempname(msg, &aname));

	RETERR(dns_message_gettemprdataset(msg, &question));
	dns_rdataset_init(question);
	dns_rdataset_makequestion(question, dns_rdataclass_in /**/,
				  dns_rdatatype_tkey);

	RETERR(isc_buffer_allocate(msg->mctx, &dynbuf, 512,
				   ISC_BUFFERTYPE_BINARY));
	RETERR(dns_message_gettemprdata(msg, &rdata));
	RETERR(dns_rdata_fromstruct(rdata, dns_rdataclass_in /**/,
				    dns_rdatatype_tkey, tkey, dynbuf));
	dns_message_takebuffer(msg, &dynbuf);

	RETERR(dns_message_gettemprdatalist(msg, &tkeylist));
	tkeylist->rdclass = dns_rdataclass_in /**/;
	tkeylist->type = dns_rdatatype_tkey;
	tkeylist->covers = 0;
	tkeylist->ttl = 0;
	ISC_LIST_INIT(tkeylist->rdata);
	ISC_LIST_APPEND(tkeylist->rdata, rdata, link);

	RETERR(dns_message_gettemprdataset(msg, &tkeyset));
	dns_rdataset_init(tkeyset);
	RETERR(dns_rdatalist_tordataset(tkeylist, tkeyset));

	dns_name_init(qname, NULL);
	dns_name_clone(name, qname);

	dns_name_init(aname, NULL);
	dns_name_clone(name, aname);

	ISC_LIST_APPEND(qname->list, question, link);
	ISC_LIST_APPEND(aname->list, tkeyset, link);

	dns_message_addname(msg, qname, DNS_SECTION_QUESTION);
	dns_message_addname(msg, aname, DNS_SECTION_ADDITIONAL);

	return (ISC_R_SUCCESS);

 failure:
	if (qname != NULL)
		dns_message_puttempname(msg, &qname);
	if (aname != NULL)
		dns_message_puttempname(msg, &aname);
	if (question != NULL) {
		dns_rdataset_disassociate(question);
		dns_message_puttemprdataset(msg, &question);
	}
	if (dynbuf != NULL)
		isc_buffer_free(&dynbuf);
	return (result);
}

isc_result_t
dns_tkey_builddhquery(dns_message_t *msg, dst_key_t *key, dns_name_t *name,
		      dns_name_t *algorithm, isc_buffer_t *nonce)
{
	dns_rdata_generic_tkey_t tkey;
	dns_rdata_t *rdata = NULL;
	isc_buffer_t src, *dynbuf = NULL;
	isc_region_t r;
	dns_name_t *keyname = NULL;
	dns_namelist_t namelist;
	isc_result_t result;

	REQUIRE(msg != NULL);
	REQUIRE(key != NULL);
	REQUIRE(dst_key_alg(key) == DNS_KEYALG_DH);
	REQUIRE(dst_key_isprivate(key));
	REQUIRE(name != NULL);
	REQUIRE(algorithm != NULL);

	tkey.common.rdclass = dns_rdataclass_in /**/;
	tkey.common.rdtype = dns_rdatatype_tkey;
	ISC_LINK_INIT(&tkey.common, link);
	tkey.mctx = msg->mctx;
	dns_name_init(&tkey.algorithm, NULL);
	dns_name_clone(algorithm, &tkey.algorithm);
	tkey.inception = tkey.expire = 0;
	tkey.mode = DNS_TKEYMODE_DIFFIEHELLMAN;
	isc_buffer_region(nonce, &r);
	tkey.error = 0;
	tkey.key = r.base;
	tkey.keylen =  r.length;
	tkey.other = NULL;
	tkey.otherlen = 0;

	RETERR(buildquery(msg, name, &tkey));

	RETERR(dns_message_gettemprdata(msg, &rdata));
	RETERR(isc_buffer_allocate(msg->mctx, &dynbuf, 1024,
                                   ISC_BUFFERTYPE_BINARY));
	RETERR(dst_key_todns(key, dynbuf));
	isc_buffer_used(dynbuf, &r);
	dns_rdata_fromregion(rdata, dns_rdataclass_in,
			     dns_rdatatype_key, &r);
	dns_message_takebuffer(msg, &dynbuf);
	RETERR(dns_message_gettempname(msg, &keyname));
	isc_buffer_init(&src, dst_key_name(key), strlen(dst_key_name(key)),
			ISC_BUFFERTYPE_TEXT);
	isc_buffer_add(&src, strlen(dst_key_name(key)));
	RETERR(isc_buffer_allocate(msg->mctx, &dynbuf, 1024,
                                   ISC_BUFFERTYPE_BINARY));
	dns_name_init(keyname, NULL);
	RETERR(dns_name_fromtext(keyname, &src, dns_rootname, ISC_FALSE,
				 dynbuf));
	dns_message_takebuffer(msg, &dynbuf);
	ISC_LIST_INIT(namelist);
	RETERR(add_rdata_to_list(msg, keyname, rdata, 0, &namelist));
	dns_message_addname(msg, ISC_LIST_HEAD(namelist),
			    DNS_SECTION_ADDITIONAL);

	return (ISC_R_SUCCESS);

 failure:

	if (dynbuf != NULL)
		isc_buffer_free(&dynbuf);
	return (result);
}

isc_result_t
dns_tkey_builddeletequery(dns_message_t *msg, dns_tsigkey_t *key) {
	dns_rdata_generic_tkey_t tkey;

	REQUIRE(msg != NULL);
	REQUIRE(key != NULL);

	tkey.common.rdclass = dns_rdataclass_in /**/;
	tkey.common.rdtype = dns_rdatatype_tkey;
	ISC_LINK_INIT(&tkey.common, link);
	tkey.mctx = msg->mctx;
	dns_name_init(&tkey.algorithm, NULL);
	dns_name_clone(&key->algorithm, &tkey.algorithm);
	tkey.inception = tkey.expire = 0;
	tkey.mode = DNS_TKEYMODE_DELETE;
	tkey.error = 0;
	tkey.keylen = tkey.otherlen = 0;
	tkey.key = tkey.other = NULL;

	return (buildquery(msg, &key->name, &tkey));
}

static isc_result_t
find_tkey(dns_message_t *msg, dns_name_t **name, dns_rdata_t *rdata) {
	dns_rdataset_t *tkeyset;
	isc_result_t result;

	result = dns_message_firstname(msg, DNS_SECTION_ADDITIONAL);
	while (result == ISC_R_SUCCESS) {
		*name = NULL;
		dns_message_currentname(msg, DNS_SECTION_ADDITIONAL, name);
		tkeyset = NULL;
		result = dns_message_findtype(*name, dns_rdatatype_tkey, 0,
					      &tkeyset);
		if (result == ISC_R_SUCCESS) {
			result = dns_rdataset_first(tkeyset);
			if (result != ISC_R_SUCCESS)
				return (result);
			dns_rdataset_current(tkeyset, rdata);
			return (ISC_R_SUCCESS);
		}
		result = dns_message_nextname(msg, DNS_SECTION_ADDITIONAL);
	}
	if (result == ISC_R_NOMORE)
		return (ISC_R_NOTFOUND);
	return (result);
}

isc_result_t
dns_tkey_processdhresponse(dns_message_t *qmsg, dns_message_t *rmsg,
			   dst_key_t *key, isc_buffer_t *nonce,
			   dns_tsigkey_t **outkey, dns_tsig_keyring_t *ring)
{
	dns_rdata_t qtkeyrdata, rtkeyrdata;
	dns_name_t keyname, *tkeyname, *theirkeyname, *ourkeyname, *tempname;
	dns_rdataset_t *theirkeyset = NULL, *ourkeyset = NULL;
	dns_rdata_t theirkeyrdata;
	dst_key_t *theirkey;
	dns_tsigkey_t *tsigkey;
	dns_rdata_generic_tkey_t qtkey, rtkey;
	unsigned char keydata[1024], secretdata[256];
	unsigned int sharedsize;
	isc_buffer_t keysrc, keybuf, *shared = NULL, secret;
	isc_region_t r, r2;
	isc_result_t result;

	REQUIRE(qmsg != NULL);
	REQUIRE(rmsg != NULL);
	REQUIRE(key != NULL);
	REQUIRE(dst_key_alg(key) == DNS_KEYALG_DH);
	REQUIRE(dst_key_isprivate(key));
	if (outkey != NULL)
		REQUIRE(*outkey == NULL);
	REQUIRE(ring != NULL);

	RETERR(find_tkey(rmsg, &tkeyname, &rtkeyrdata));
	RETERR(dns_rdata_tostruct(&rtkeyrdata, &rtkey, rmsg->mctx));

	RETERR(find_tkey(qmsg, &tempname, &qtkeyrdata));
	RETERR(dns_rdata_tostruct(&qtkeyrdata, &qtkey, qmsg->mctx));

	if (rtkey.error != dns_rcode_noerror ||
	    rtkey.mode != DNS_TKEYMODE_DIFFIEHELLMAN ||
	    rtkey.mode != qtkey.mode ||
	    !dns_name_equal(&rtkey.algorithm, &qtkey.algorithm) ||
	    rmsg->rcode != dns_rcode_noerror)
	{
		result = DNS_R_INVALIDTKEY;
		dns_rdata_freestruct(&rtkey);
		goto failure;
	}

	isc_buffer_init(&keysrc, dst_key_name(key), strlen(dst_key_name(key)),
			ISC_BUFFERTYPE_TEXT);
	isc_buffer_add(&keysrc, strlen(dst_key_name(key)));
	isc_buffer_init(&keybuf, keydata, sizeof(keydata),
			ISC_BUFFERTYPE_BINARY);
	dns_name_init(&keyname, NULL);
	RETERR(dns_name_fromtext(&keyname, &keysrc, dns_rootname,
				 ISC_FALSE, &keybuf));

	ourkeyname = NULL;
	ourkeyset = NULL;
	RETERR(dns_message_findname(rmsg, DNS_SECTION_ADDITIONAL, &keyname,
				    dns_rdatatype_key, 0, &ourkeyname,
				    &ourkeyset));

	result = dns_message_firstname(rmsg, DNS_SECTION_ADDITIONAL);
	while (result == ISC_R_SUCCESS) {
		theirkeyname = NULL;
		dns_message_currentname(rmsg, DNS_SECTION_ADDITIONAL,
					&theirkeyname);
		if (dns_name_equal(theirkeyname, ourkeyname))
			goto next;
		theirkeyset = NULL;
		result = dns_message_findtype(theirkeyname, dns_rdatatype_key,
					      0, &theirkeyset);
		if (result == ISC_R_SUCCESS) {
			RETERR(dns_rdataset_first(theirkeyset));
			break;
		}
 next:
                result = dns_message_nextname(rmsg, DNS_SECTION_ADDITIONAL);
        }

	if (theirkeyset == NULL) {
		result = ISC_R_NOTFOUND;
		goto failure;
	}

	dns_rdataset_current(theirkeyset, &theirkeyrdata);
	theirkey = NULL;
	RETERR(dns_dnssec_keyfromrdata(theirkeyname, &theirkeyrdata,
				       rmsg->mctx, &theirkey));

	RETERR(dst_secret_size(key, &sharedsize));
	RETERR(isc_buffer_allocate(rmsg->mctx, &shared, sharedsize,
				   ISC_BUFFERTYPE_BINARY));

	RETERR(dst_computesecret(theirkey, key, shared));

	isc_buffer_init(&secret, secretdata, sizeof(secretdata),
			ISC_BUFFERTYPE_BINARY);

	r.base = rtkey.key;
	r.length = rtkey.keylen;
	isc_buffer_region(nonce, &r2);
	RETERR(compute_secret(shared, &r2, &r, &secret));

	isc_buffer_used(&secret, &r);
	tsigkey = NULL;
	result = dns_tsigkey_create(tkeyname, &rtkey.algorithm,
				    r.base, r.length, ISC_TRUE,
				    NULL, rmsg->mctx, ring, outkey);
	isc_buffer_free(&shared);
	return (result);

 failure:
	if (shared != NULL)
		isc_buffer_free(&shared);

	return (result);
}

isc_result_t
dns_tkey_processdeleteresponse(dns_message_t *qmsg, dns_message_t *rmsg,
			       dns_tsig_keyring_t *ring)
{
	dns_rdata_t qtkeyrdata, rtkeyrdata;
	dns_name_t *tkeyname, *tempname;
	dns_rdata_generic_tkey_t qtkey, rtkey;
	dns_tsigkey_t *tsigkey = NULL;
	isc_result_t result;

	REQUIRE(qmsg != NULL);
	REQUIRE(rmsg != NULL);

	RETERR(find_tkey(rmsg, &tkeyname, &rtkeyrdata));
	RETERR(dns_rdata_tostruct(&rtkeyrdata, &rtkey, rmsg->mctx));

	RETERR(find_tkey(qmsg, &tempname, &qtkeyrdata));
	RETERR(dns_rdata_tostruct(&qtkeyrdata, &qtkey, qmsg->mctx));

	if (rtkey.error != dns_rcode_noerror ||
	    rtkey.mode != DNS_TKEYMODE_DELETE ||
	    rtkey.mode != qtkey.mode ||
	    !dns_name_equal(&rtkey.algorithm, &qtkey.algorithm) ||
	    rmsg->rcode != dns_rcode_noerror)
	{
		result = DNS_R_INVALIDTKEY;
		dns_rdata_freestruct(&rtkey);
		goto failure;
	}

	RETERR(dns_tsigkey_find(&tsigkey, tkeyname, &rtkey.algorithm, ring));

	/* Mark the key as deleted */
	dns_tsigkey_setdeleted(tsigkey);
	/* Release the reference */
	dns_tsigkey_free(&tsigkey);

 failure:
	return (result);
}
