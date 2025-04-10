/*
 * contrib/btree_gist/btree_bytea.c
 */
#include "postgres.h"

#include "btree_gist.h"
#include "btree_utils_var.h"
#include "utils/fmgrprotos.h"
#include "utils/sortsupport.h"

/* GiST support functions */
PG_FUNCTION_INFO_V1(gbt_bytea_compress);
PG_FUNCTION_INFO_V1(gbt_bytea_union);
PG_FUNCTION_INFO_V1(gbt_bytea_picksplit);
PG_FUNCTION_INFO_V1(gbt_bytea_consistent);
PG_FUNCTION_INFO_V1(gbt_bytea_penalty);
PG_FUNCTION_INFO_V1(gbt_bytea_same);
PG_FUNCTION_INFO_V1(gbt_bytea_sortsupport);


/* define for comparison */

static bool
gbt_byteagt(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2(byteagt,
											PointerGetDatum(a),
											PointerGetDatum(b)));
}

static bool
gbt_byteage(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2(byteage,
											PointerGetDatum(a),
											PointerGetDatum(b)));
}

static bool
gbt_byteaeq(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2(byteaeq,
											PointerGetDatum(a),
											PointerGetDatum(b)));
}

static bool
gbt_byteale(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2(byteale,
											PointerGetDatum(a),
											PointerGetDatum(b)));
}

static bool
gbt_bytealt(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	return DatumGetBool(DirectFunctionCall2(bytealt,
											PointerGetDatum(a),
											PointerGetDatum(b)));
}

static int32
gbt_byteacmp(const void *a, const void *b, Oid collation, FmgrInfo *flinfo)
{
	return DatumGetInt32(DirectFunctionCall2(byteacmp,
											 PointerGetDatum(a),
											 PointerGetDatum(b)));
}

static const gbtree_vinfo tinfo =
{
	gbt_t_bytea,
	0,
	true,
	gbt_byteagt,
	gbt_byteage,
	gbt_byteaeq,
	gbt_byteale,
	gbt_bytealt,
	gbt_byteacmp,
	NULL
};


/**************************************************
 * GiST support functions
 **************************************************/

Datum
gbt_bytea_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(gbt_var_compress(entry, &tinfo));
}

Datum
gbt_bytea_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	void	   *query = DatumGetByteaP(PG_GETARG_DATUM(1));
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	bool		retval;
	GBT_VARKEY *key = (GBT_VARKEY *) DatumGetPointer(entry->key);
	GBT_VARKEY_R r = gbt_var_key_readable(key);

	/* All cases served by this function are exact */
	*recheck = false;

	retval = gbt_var_consistent(&r, query, strategy, PG_GET_COLLATION(),
								GIST_LEAF(entry), &tinfo, fcinfo->flinfo);
	PG_RETURN_BOOL(retval);
}

Datum
gbt_bytea_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	int32	   *size = (int *) PG_GETARG_POINTER(1);

	PG_RETURN_POINTER(gbt_var_union(entryvec, size, PG_GET_COLLATION(),
									&tinfo, fcinfo->flinfo));
}

Datum
gbt_bytea_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);

	gbt_var_picksplit(entryvec, v, PG_GET_COLLATION(),
					  &tinfo, fcinfo->flinfo);
	PG_RETURN_POINTER(v);
}

Datum
gbt_bytea_same(PG_FUNCTION_ARGS)
{
	Datum		d1 = PG_GETARG_DATUM(0);
	Datum		d2 = PG_GETARG_DATUM(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = gbt_var_same(d1, d2, PG_GET_COLLATION(), &tinfo, fcinfo->flinfo);
	PG_RETURN_POINTER(result);
}

Datum
gbt_bytea_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *o = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *n = (GISTENTRY *) PG_GETARG_POINTER(1);
	float	   *result = (float *) PG_GETARG_POINTER(2);

	PG_RETURN_POINTER(gbt_var_penalty(result, o, n, PG_GET_COLLATION(),
									  &tinfo, fcinfo->flinfo));
}

static int
gbt_bytea_ssup_cmp(Datum x, Datum y, SortSupport ssup)
{
	GBT_VARKEY *key1 = PG_DETOAST_DATUM(x);
	GBT_VARKEY *key2 = PG_DETOAST_DATUM(y);

	GBT_VARKEY_R xkey = gbt_var_key_readable(key1);
	GBT_VARKEY_R ykey = gbt_var_key_readable(key2);
	Datum		result;

	/* for leaf items we expect lower == upper, so only compare lower */
	result = DirectFunctionCall2(byteacmp,
								 PointerGetDatum(xkey.lower),
								 PointerGetDatum(ykey.lower));

	GBT_FREE_IF_COPY(key1, x);
	GBT_FREE_IF_COPY(key2, y);

	return DatumGetInt32(result);
}

Datum
gbt_bytea_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	ssup->comparator = gbt_bytea_ssup_cmp;
	ssup->ssup_extra = NULL;

	PG_RETURN_VOID();
}
