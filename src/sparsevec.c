#include "postgres.h"

#include <limits.h>
#include <math.h>

#include "fmgr.h"
#include "libpq/pqformat.h"
#include "sparsevec.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "vector.h"

#if PG_VERSION_NUM >= 120000
#include "common/shortest_dec.h"
#include "utils/float.h"
#else
#include <float.h>
#include "utils/builtins.h"
#endif

/*
 * Ensure same dimensions
 */
static inline void
CheckDims(SparseVector * a, SparseVector * b)
{
	if (a->dim != b->dim)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("different sparsevec dimensions %d and %d", a->dim, b->dim)));
}

/*
 * Ensure expected dimensions
 */
static inline void
CheckExpectedDim(int32 typmod, int dim)
{
	if (typmod != -1 && typmod != dim)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("expected %d dimensions, not %d", typmod, dim)));
}

/*
 * Ensure valid dimensions
 */
static inline void
CheckDim(int dim)
{
	if (dim < 1)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("sparsevec must have at least 1 dimension")));

	if (dim > SPARSEVEC_MAX_DIM)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("sparsevec cannot have more than %d dimensions", SPARSEVEC_MAX_DIM)));
}

/*
 * Ensure valid nnz
 */
static inline void
CheckNnz(int nnz, int dim)
{
	if (nnz < 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("sparsevec must have at least one element")));

	if (nnz > dim)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("sparsevec cannot have more elements than dimensions")));
}

/*
 * Ensure valid index
 */
static inline void
CheckIndex(int32 *indices, int i, int dim)
{
	int32		index = indices[i];

	if (index < 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("index must not be negative")));

	if (index >= dim)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("index must be less than dimensions")));

	if (i > 0)
	{
		if (index < indices[i - 1])
			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("indexes must be in ascending order")));

		if (index == indices[i - 1])
			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("indexes must not contain duplicates")));
	}
}

/*
 * Ensure finite element
 */
static inline void
CheckElement(float value)
{
	if (isnan(value))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("NaN not allowed in sparsevec")));

	if (isinf(value))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("infinite value not allowed in sparsevec")));
}

/*
 * Allocate and initialize a new sparse vector
 */
SparseVector *
InitSparseVector(int dim, int nnz)
{
	SparseVector *result;
	int			size;

	size = SPARSEVEC_SIZE(nnz);
	result = (SparseVector *) palloc0(size);
	SET_VARSIZE(result, size);
	result->dim = dim;
	result->nnz = nnz;

	return result;
}

/*
 * Check for whitespace, since array_isspace() is static
 */
static inline bool
sparsevec_isspace(char ch)
{
	if (ch == ' ' ||
		ch == '\t' ||
		ch == '\n' ||
		ch == '\r' ||
		ch == '\v' ||
		ch == '\f')
		return true;
	return false;
}

/*
 * Convert textual representation to internal representation
 */
PGDLLEXPORT PG_FUNCTION_INFO_V1(sparsevec_in);
Datum
sparsevec_in(PG_FUNCTION_ARGS)
{
	char	   *lit = PG_GETARG_CSTRING(0);
	int32		typmod = PG_GETARG_INT32(2);
	int			dim;
	char	   *pt;
	char	   *stringEnd;
	SparseVector *result;
	float	   *rvalues;
	char	   *litcopy = pstrdup(lit);
	char	   *str = litcopy;
	int32	   *indices;
	float	   *values;
	int			maxNnz;
	int			nnz = 0;

	maxNnz = 1;
	pt = str;
	while (*pt != '\0')
	{
		if (*pt == ',')
			maxNnz++;

		pt++;
	}

	indices = palloc(maxNnz * sizeof(int32));
	values = palloc(maxNnz * sizeof(float));

	while (sparsevec_isspace(*str))
		str++;

	if (*str != '{')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("malformed sparsevec literal: \"%s\"", lit),
				 errdetail("Vector contents must start with \"{\".")));

	str++;
	pt = strtok(str, ",");
	stringEnd = pt;

	while (pt != NULL && *stringEnd != '}')
	{
		long		index;
		float		value;

		/* TODO Better error */
		if (nnz == maxNnz)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("ran out of buffer: \"%s\"", lit)));

		while (sparsevec_isspace(*pt))
			pt++;

		/* Check for empty string like float4in */
		if (*pt == '\0')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type sparsevec: \"%s\"", lit)));

		/* Use similar logic as int2vectorin */
		errno = 0;
		index = strtol(pt, &stringEnd, 10);

		if (stringEnd == pt)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type sparsevec: \"%s\"", lit)));

		if (errno == ERANGE || index < 0 || index > INT_MAX)
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("index \"%ld\" is out of range for type sparsevec", index)));

		if (stringEnd == pt)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type sparsevec: \"%s\"", lit)));

		while (sparsevec_isspace(*stringEnd))
			stringEnd++;

		if (*stringEnd != ':')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type sparsevec: \"%s\"", lit)));

		stringEnd++;

		while (sparsevec_isspace(*stringEnd))
			stringEnd++;

		errno = 0;
		pt = stringEnd;
		value = strtof(pt, &stringEnd);

		if (stringEnd == pt)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type sparsevec: \"%s\"", lit)));

		/* Check for range error like float4in */
		if (errno == ERANGE && (value == 0 || isinf(value)))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("\"%s\" is out of range for type sparsevec", pt)));

		/* TODO Decide whether to store zero values */
		if (value != 0)
		{
			indices[nnz] = index;
			values[nnz] = value;
			nnz++;
		}

		if (*stringEnd != '\0' && *stringEnd != '}')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type sparsevec: \"%s\"", lit)));

		pt = strtok(NULL, ",");
	}

	if (stringEnd == NULL || *stringEnd != '}')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("malformed sparsevec literal: \"%s\"", lit),
				 errdetail("Unexpected end of input.")));

	stringEnd++;

	if (*stringEnd != '/')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("malformed sparsevec literal: \"%s\"", lit),
				 errdetail("Unexpected end of input.")));

	stringEnd++;

	/* Use similar logic as int2vectorin */
	errno = 0;
	pt = stringEnd;
	dim = strtol(pt, &stringEnd, 10);

	if (stringEnd == pt)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type sparsevec: \"%s\"", lit)));

	/* Only whitespace is allowed after the closing brace */
	while (sparsevec_isspace(*stringEnd))
		stringEnd++;

	if (*stringEnd != '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("malformed sparsevec literal: \"%s\"", lit),
				 errdetail("Junk after closing.")));

	pfree(litcopy);

	CheckDim(dim);
	CheckExpectedDim(typmod, dim);

	result = InitSparseVector(dim, nnz);
	rvalues = SPARSEVEC_VALUES(result);
	for (int i = 0; i < nnz; i++)
	{
		result->indices[i] = indices[i];
		rvalues[i] = values[i];

		CheckIndex(result->indices, i, dim);
		CheckElement(rvalues[i]);
	}

	PG_RETURN_POINTER(result);
}

#define AppendChar(ptr, c) (*(ptr)++ = (c))
#define AppendFloat(ptr, f) ((ptr) += float_to_shortest_decimal_bufn((f), (ptr)))

#if PG_VERSION_NUM >= 140000
#define AppendInt(ptr, i) ((ptr) += pg_ltoa((i), (ptr)))
#else
#define AppendInt(ptr, i) \
	do { \
		pg_ltoa(i, ptr); \
		while (*ptr != '\0') \
			ptr++; \
	} while (0)
#endif

/*
 * Convert internal representation to textual representation
 */
PGDLLEXPORT PG_FUNCTION_INFO_V1(sparsevec_out);
Datum
sparsevec_out(PG_FUNCTION_ARGS)
{
	SparseVector *sparsevec = PG_GETARG_SPARSEVEC_P(0);
	float	   *values = SPARSEVEC_VALUES(sparsevec);
	char	   *buf;
	char	   *ptr;

	/*
	 * Need:
	 *
	 * nnz * 10 bytes for index (positive integer)
	 *
	 * nnz bytes for :
	 *
	 * nnz * (FLOAT_SHORTEST_DECIMAL_LEN - 1) bytes for
	 * float_to_shortest_decimal_bufn
	 *
	 * nnz - 1 bytes for ,
	 *
	 * 10 bytes for dimensions
	 *
	 * 4 bytes for {, }, /, and \0
	 */
	buf = (char *) palloc((11 + FLOAT_SHORTEST_DECIMAL_LEN) * sparsevec->nnz + 13);
	ptr = buf;

	AppendChar(ptr, '{');

	for (int i = 0; i < sparsevec->nnz; i++)
	{
		if (i > 0)
			AppendChar(ptr, ',');

		AppendInt(ptr, sparsevec->indices[i]);
		AppendChar(ptr, ':');
		AppendFloat(ptr, values[i]);
	}

	AppendChar(ptr, '}');
	AppendChar(ptr, '/');
	AppendInt(ptr, sparsevec->dim);
	*ptr = '\0';

	PG_FREE_IF_COPY(sparsevec, 0);
	PG_RETURN_CSTRING(buf);
}

/*
 * Convert type modifier
 */
PGDLLEXPORT PG_FUNCTION_INFO_V1(sparsevec_typmod_in);
Datum
sparsevec_typmod_in(PG_FUNCTION_ARGS)
{
	ArrayType  *ta = PG_GETARG_ARRAYTYPE_P(0);
	int32	   *tl;
	int			n;

	tl = ArrayGetIntegerTypmods(ta, &n);

	if (n != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid type modifier")));

	if (*tl < 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dimensions for type sparsevec must be at least 1")));

	if (*tl > SPARSEVEC_MAX_DIM)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("dimensions for type sparsevec cannot exceed %d", SPARSEVEC_MAX_DIM)));

	PG_RETURN_INT32(*tl);
}

/*
 * Convert external binary representation to internal representation
 */
PGDLLEXPORT PG_FUNCTION_INFO_V1(sparsevec_recv);
Datum
sparsevec_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	int32		typmod = PG_GETARG_INT32(2);
	SparseVector *result;
	int32		dim;
	int32		nnz;
	int32		unused;
	float	   *values;

	dim = pq_getmsgint(buf, sizeof(int32));
	nnz = pq_getmsgint(buf, sizeof(int32));
	unused = pq_getmsgint(buf, sizeof(int32));

	CheckDim(dim);
	CheckNnz(nnz, dim);
	CheckExpectedDim(typmod, dim);

	if (unused != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("expected unused to be 0, not %d", unused)));

	result = InitSparseVector(dim, nnz);
	values = SPARSEVEC_VALUES(result);

	for (int i = 0; i < nnz; i++)
	{
		result->indices[i] = pq_getmsgint(buf, sizeof(int32));
		CheckIndex(result->indices, i, dim);
	}

	for (int i = 0; i < nnz; i++)
	{
		values[i] = pq_getmsgfloat4(buf);
		CheckElement(values[i]);
	}

	PG_RETURN_POINTER(result);
}

/*
 * Convert internal representation to the external binary representation
 */
PGDLLEXPORT PG_FUNCTION_INFO_V1(sparsevec_send);
Datum
sparsevec_send(PG_FUNCTION_ARGS)
{
	SparseVector *svec = PG_GETARG_SPARSEVEC_P(0);
	float	   *values = SPARSEVEC_VALUES(svec);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint(&buf, svec->dim, sizeof(int32));
	pq_sendint(&buf, svec->nnz, sizeof(int32));
	pq_sendint(&buf, svec->unused, sizeof(int32));
	for (int i = 0; i < svec->nnz; i++)
		pq_sendint(&buf, svec->indices[i], sizeof(int32));
	for (int i = 0; i < svec->nnz; i++)
		pq_sendfloat4(&buf, values[i]);

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * Convert sparse vector to sparse vector
 * This is needed to check the type modifier
 */
PGDLLEXPORT PG_FUNCTION_INFO_V1(sparsevec);
Datum
sparsevec(PG_FUNCTION_ARGS)
{
	SparseVector *svec = PG_GETARG_SPARSEVEC_P(0);
	int32		typmod = PG_GETARG_INT32(1);

	CheckExpectedDim(typmod, svec->dim);

	PG_RETURN_POINTER(svec);
}

/*
 * Convert dense vector to sparse vector
 */
PGDLLEXPORT PG_FUNCTION_INFO_V1(vector_to_sparsevec);
Datum
vector_to_sparsevec(PG_FUNCTION_ARGS)
{
	Vector	   *vec = PG_GETARG_VECTOR_P(0);
	int32		typmod = PG_GETARG_INT32(1);
	SparseVector *result;
	int			dim = vec->dim;
	int			nnz = 0;
	float	   *values;
	int			j = 0;

	CheckDim(dim);
	CheckExpectedDim(typmod, dim);

	for (int i = 0; i < dim; i++)
	{
		if (vec->x[i] != 0)
			nnz++;
	}

	result = InitSparseVector(dim, nnz);
	values = SPARSEVEC_VALUES(result);
	for (int i = 0; i < dim; i++)
	{
		if (vec->x[i] != 0)
		{
			/* Safety check */
			if (j == nnz)
				elog(ERROR, "safety check failed");

			result->indices[j] = i;
			values[j] = vec->x[i];
			j++;
		}
	}

	PG_RETURN_POINTER(result);
}

/*
 *  Get the L2 squared distance between sparse vectors
 */
static double
l2_distance_squared_internal(SparseVector * a, SparseVector * b)
{
	float	   *ax = SPARSEVEC_VALUES(a);
	float	   *bx = SPARSEVEC_VALUES(b);
	double		distance = 0.0;
	int			bpos = 0;

	for (int i = 0; i < a->nnz; i++)
	{
		int			ai = a->indices[i];
		int			bi = -1;

		for (int j = bpos; j < b->nnz; j++)
		{
			bi = b->indices[j];

			if (ai == bi)
			{
				double		diff = ax[i] - bx[j];

				distance += diff * diff;
			}
			else if (ai > bi)
				distance += bx[j] * bx[j];

			/* Update start for next iteration */
			if (ai >= bi)
				bpos = j + 1;

			/* Found or passed it */
			if (bi >= ai)
				break;
		}

		if (ai != bi)
			distance += ax[i] * ax[i];
	}

	for (int j = bpos; j < b->nnz; j++)
		distance += bx[j] * bx[j];

	return distance;
}

/*
 * Get the L2 distance between sparse vectors
 */
PGDLLEXPORT PG_FUNCTION_INFO_V1(sparsevec_l2_distance);
Datum
sparsevec_l2_distance(PG_FUNCTION_ARGS)
{
	SparseVector *a = PG_GETARG_SPARSEVEC_P(0);
	SparseVector *b = PG_GETARG_SPARSEVEC_P(1);

	CheckDims(a, b);

	PG_RETURN_FLOAT8(sqrt(l2_distance_squared_internal(a, b)));
}

/*
 * Get the L2 squared distance between sparse vectors
 * This saves a sqrt calculation
 */
PGDLLEXPORT PG_FUNCTION_INFO_V1(sparsevec_l2_squared_distance);
Datum
sparsevec_l2_squared_distance(PG_FUNCTION_ARGS)
{
	SparseVector *a = PG_GETARG_SPARSEVEC_P(0);
	SparseVector *b = PG_GETARG_SPARSEVEC_P(1);

	CheckDims(a, b);

	PG_RETURN_FLOAT8(l2_distance_squared_internal(a, b));
}

/*
 * Get the inner product of two sparse vectors
 */
static double
inner_product_internal(SparseVector * a, SparseVector * b)
{
	float	   *ax = SPARSEVEC_VALUES(a);
	float	   *bx = SPARSEVEC_VALUES(b);
	double		distance = 0.0;
	int			bpos = 0;

	for (int i = 0; i < a->nnz; i++)
	{
		int			ai = a->indices[i];

		for (int j = bpos; j < b->nnz; j++)
		{
			int			bi = b->indices[j];

			/* Only update when the same index */
			if (ai == bi)
				distance += ax[i] * bx[j];

			/* Update start for next iteration */
			if (ai >= bi)
				bpos = j + 1;

			/* Found or passed it */
			if (bi >= ai)
				break;
		}
	}

	return distance;
}

/*
 * Get the inner product of two sparse vectors
 */
PGDLLEXPORT PG_FUNCTION_INFO_V1(sparsevec_inner_product);
Datum
sparsevec_inner_product(PG_FUNCTION_ARGS)
{
	SparseVector *a = PG_GETARG_SPARSEVEC_P(0);
	SparseVector *b = PG_GETARG_SPARSEVEC_P(1);

	CheckDims(a, b);

	PG_RETURN_FLOAT8(inner_product_internal(a, b));
}

/*
 * Get the negative inner product of two sparse vectors
 */
PGDLLEXPORT PG_FUNCTION_INFO_V1(sparsevec_negative_inner_product);
Datum
sparsevec_negative_inner_product(PG_FUNCTION_ARGS)
{
	SparseVector *a = PG_GETARG_SPARSEVEC_P(0);
	SparseVector *b = PG_GETARG_SPARSEVEC_P(1);

	CheckDims(a, b);

	PG_RETURN_FLOAT8(-inner_product_internal(a, b));
}

/*
 * Get the cosine distance between two sparse vectors
 */
PGDLLEXPORT PG_FUNCTION_INFO_V1(sparsevec_cosine_distance);
Datum
sparsevec_cosine_distance(PG_FUNCTION_ARGS)
{
	SparseVector *a = PG_GETARG_SPARSEVEC_P(0);
	SparseVector *b = PG_GETARG_SPARSEVEC_P(1);
	float	   *ax = SPARSEVEC_VALUES(a);
	float	   *bx = SPARSEVEC_VALUES(b);
	float		norma = 0.0;
	float		normb = 0.0;
	double		similarity;

	CheckDims(a, b);

	similarity = inner_product_internal(a, b);

	/* Auto-vectorized */
	for (int i = 0; i < a->nnz; i++)
		norma += ax[i] * ax[i];

	/* Auto-vectorized */
	for (int i = 0; i < b->nnz; i++)
		normb += bx[i] * bx[i];

	/* Use sqrt(a * b) over sqrt(a) * sqrt(b) */
	similarity /= sqrt((double) norma * (double) normb);

#ifdef _MSC_VER
	/* /fp:fast may not propagate NaN */
	if (isnan(similarity))
		PG_RETURN_FLOAT8(NAN);
#endif

	/* Keep in range */
	if (similarity > 1)
		similarity = 1.0;
	else if (similarity < -1)
		similarity = -1.0;

	PG_RETURN_FLOAT8(1.0 - similarity);
}

/*
 * Get the L2 norm of a sparse vector
 */
PGDLLEXPORT PG_FUNCTION_INFO_V1(sparsevec_norm);
Datum
sparsevec_norm(PG_FUNCTION_ARGS)
{
	SparseVector *a = PG_GETARG_SPARSEVEC_P(0);
	float	   *ax = SPARSEVEC_VALUES(a);
	double		norm = 0.0;

	/* Auto-vectorized */
	for (int i = 0; i < a->nnz; i++)
		norm += (double) ax[i] * (double) ax[i];

	PG_RETURN_FLOAT8(sqrt(norm));
}
