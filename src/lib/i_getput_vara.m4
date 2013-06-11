dnl Process this m4 file to produce 'C' language file.
dnl
dnl If you see this line, you can ignore the next one.
/* Do not edit this file. It is produced from the corresponding .m4 source */
dnl
/*
 *  Copyright (C) 2003, Northwestern University and Argonne National Laboratory
 *  See COPYRIGHT notice in top-level directory.
 */
/* $Id$ */

#if HAVE_CONFIG_H
# include <ncconfig.h>
#endif

#include "nc.h"
#include "ncx.h"
#include <mpi.h>
#include <stdio.h>
#include <unistd.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#include <assert.h>

#include "ncmpidtype.h"
#include "macro.h"


/* ftype is the variable's nc_type defined in file, eg. int64
 * btype is the I/O buffer's C data type, eg. long long
 * buftype is I/O bufer's MPI data type, eg. MPI_UNSIGNED_LONG_LONG
 * apitype is data type appeared in the API names, eg. ncmpi_get_vara_longlong
 */

/*----< ncmpi_iput_vara() >---------------------------------------------------*/
int
ncmpi_iput_vara(int               ncid,
                int               varid,
                const MPI_Offset *start,
                const MPI_Offset *count,
                const void       *buf,
                MPI_Offset        bufcount,
                MPI_Datatype      buftype,
                int              *reqid)
{
    int     status;
    NC     *ncp;
    NC_var *varp=NULL;

    *reqid = NC_REQ_NULL;
    SANITY_CHECK(ncid, ncp, varp, WRITE_REQ, INDEP_COLL_IO, status)

    status = NCcoordck(ncp, varp, start);
    if (status != NC_NOERR) return status;
    status = NCedgeck(ncp, varp, start, count);
    if (status != NC_NOERR) return status;

    return ncmpii_igetput_varm(ncp, varp, start, count, NULL, NULL,
                               (void*)buf, bufcount, buftype, reqid,
                               WRITE_REQ, 0);
}

dnl
dnl IPUT_VARA_TYPE(ncid, varid, start, count, op, reqid)
dnl
define(`IPUT_VARA_TYPE',dnl
`dnl
/*----< ncmpi_iput_vara_$1() >------------------------------------------------*/
int
ncmpi_iput_vara_$1(int               ncid,
                   int               varid,
                   const MPI_Offset  start[],
                   const MPI_Offset  count[],
                   const $2         *op,
                   int              *reqid)
{
    int         status;
    NC         *ncp;
    NC_var     *varp=NULL;
    MPI_Offset  nelems;

    *reqid = NC_REQ_NULL;
    SANITY_CHECK(ncid, ncp, varp, WRITE_REQ, INDEP_COLL_IO, status)

    status = NCcoordck(ncp, varp, start);
    if (status != NC_NOERR) return status;
    status = NCedgeck(ncp, varp, start, count);
    if (status != NC_NOERR) return status;
    GET_NUM_ELEMENTS(nelems)

    return ncmpii_igetput_varm(ncp, varp, start, count, NULL, NULL,
                               (void*)op, nelems, $3, reqid,
                               WRITE_REQ, 0);
}
')dnl

IPUT_VARA_TYPE(text,      char,               MPI_CHAR)
IPUT_VARA_TYPE(schar,     schar,              MPI_BYTE)
IPUT_VARA_TYPE(uchar,     uchar,              MPI_UNSIGNED_CHAR)
IPUT_VARA_TYPE(short,     short,              MPI_SHORT)
IPUT_VARA_TYPE(ushort,    ushort,             MPI_UNSIGNED_SHORT)
IPUT_VARA_TYPE(int,       int,                MPI_INT)
IPUT_VARA_TYPE(uint,      uint,               MPI_UNSIGNED)
IPUT_VARA_TYPE(long,      long,               MPI_LONG)
IPUT_VARA_TYPE(float,     float,              MPI_FLOAT)
IPUT_VARA_TYPE(double,    double,             MPI_DOUBLE)
IPUT_VARA_TYPE(longlong,  long long,          MPI_LONG_LONG_INT)
IPUT_VARA_TYPE(ulonglong, unsigned long long, MPI_UNSIGNED_LONG_LONG)
dnl IPUT_VARA_TYPE(string, char*,             MPI_CHAR)
dnl string is not yet supported

/*----< ncmpi_iget_vara() >---------------------------------------------------*/
int
ncmpi_iget_vara(int               ncid,
                int               varid,
                const MPI_Offset *start,
                const MPI_Offset *count,
                void             *buf,
                MPI_Offset        bufcount,
                MPI_Datatype      buftype,
                int              *reqid)
{
    int     status;
    NC     *ncp;
    NC_var *varp=NULL;

    *reqid = NC_REQ_NULL;
    SANITY_CHECK(ncid, ncp, varp, READ_REQ, INDEP_COLL_IO, status)

    status = NCcoordck(ncp, varp, start);
    if (status != NC_NOERR) return status;
    status = NCedgeck(ncp, varp, start, count);
    if (status != NC_NOERR) return status;
    if (IS_RECVAR(varp) &&
        start[0] + count[0] > NC_get_numrecs(ncp)) return NC_EEDGE;

    return ncmpii_igetput_varm(ncp, varp, start, count, NULL, NULL, buf,
                               bufcount, buftype, reqid, READ_REQ, 0);
}

dnl
dnl IGET_VARA_TYPE(ncid, varid, start, count, ip, reqid)
dnl
define(`IGET_VARA_TYPE',dnl
`dnl
/*----< ncmpi_iget_vara_$1() >------------------------------------------------*/
int
ncmpi_iget_vara_$1(int               ncid,
                   int               varid,
                   const MPI_Offset  start[],
                   const MPI_Offset  count[],
                   $2               *ip,
                   int              *reqid)
{
    int         status;
    NC         *ncp;
    NC_var     *varp=NULL;
    MPI_Offset  nelems;

    *reqid = NC_REQ_NULL;
    SANITY_CHECK(ncid, ncp, varp, READ_REQ, INDEP_COLL_IO, status)

    status = NCcoordck(ncp, varp, start);
    if (status != NC_NOERR) return status;
    status = NCedgeck(ncp, varp, start, count);
    if (status != NC_NOERR) return status;
    if (IS_RECVAR(varp) &&
        start[0] + count[0] > NC_get_numrecs(ncp)) return NC_EEDGE;
    GET_NUM_ELEMENTS(nelems)

    return ncmpii_igetput_varm(ncp, varp, start, count, NULL, NULL,
                               ip, nelems, $3, reqid, READ_REQ, 0);
}
')dnl

IGET_VARA_TYPE(text,      char,               MPI_CHAR)
IGET_VARA_TYPE(schar,     schar,              MPI_BYTE)
IGET_VARA_TYPE(uchar,     uchar,              MPI_UNSIGNED_CHAR)
IGET_VARA_TYPE(short,     short,              MPI_SHORT)
IGET_VARA_TYPE(ushort,    ushort,             MPI_UNSIGNED_SHORT)
IGET_VARA_TYPE(int,       int,                MPI_INT)
IGET_VARA_TYPE(uint,      uint,               MPI_UNSIGNED)
IGET_VARA_TYPE(long,      long,               MPI_LONG)
IGET_VARA_TYPE(float,     float,              MPI_FLOAT)
IGET_VARA_TYPE(double,    double,             MPI_DOUBLE)
IGET_VARA_TYPE(longlong,  long long,          MPI_LONG_LONG_INT)
IGET_VARA_TYPE(ulonglong, unsigned long long, MPI_UNSIGNED_LONG_LONG)
dnl IGET_VARA_TYPE(string, char*,             MPI_CHAR)
dnl string is not yet supported

