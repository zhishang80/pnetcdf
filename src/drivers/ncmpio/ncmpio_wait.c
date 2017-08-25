/*
 *  Copyright (C) 2003, Northwestern University and Argonne National Laboratory
 *  See COPYRIGHT notice in top-level directory.
 */
/* $Id$ */

/*
 * This file implements the corresponding APIs defined in src/dispatchers/file.c
 *
 * ncmpi_wait()     : dispatcher->wait()
 * ncmpi_wait_all() : dispatcher->wait()
 * ncmpi_cancel()   : dispatcher->cancel()
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h> /* memset() */
#include <assert.h>

#include <mpi.h>

#include <pnc_debug.h>
#include <common.h>
#include "ncmpio_NC.h"


/* buffer layers:

        User Level              buf     (user defined buffer of MPI_Datatype)
        MPI Datatype Level      cbuf    (contiguous buffer of ptype)
        NetCDF XDR Level        xbuf    (XDR I/O buffer)
*/

/* Prototypes for functions used only in this file */
static int wait_getput(NC *ncp, int num_reqs, NC_req *reqs, int rw_flag,
                       int coll_indep, MPI_Offset newnumrecs);

static int mgetput(NC *ncp, int num_reqs, NC_req *reqs, int rw_flag,
                   int coll_indep);

/*----< ncmpio_getput_zero_req() >--------------------------------------------*/
/* This function is called when this process has zero-length I/O request and
 * must participate all the MPI collective calls involved in the collective
 * APIs and wait_all(), which include setting fileview, collective read/write,
 * another setting fileview.
 *
 * This function is collective.
 */
int
ncmpio_getput_zero_req(NC *ncp, int reqMode)
{
    int err, mpireturn, status=NC_NOERR;
    MPI_Status mpistatus;
    MPI_File fh;

    /* do nothing if this came from an independent API */
    if (fIsSet(reqMode, NC_REQ_INDEP)) return NC_NOERR;

    fh = ncp->collective_fh;

    TRACE_IO(MPI_File_set_view)(fh, 0, MPI_BYTE, MPI_BYTE, "native",
                                MPI_INFO_NULL);

    if (fIsSet(reqMode, NC_REQ_RD)) {
        TRACE_IO(MPI_File_read_all)(fh, NULL, 0, MPI_BYTE, &mpistatus);
        if (mpireturn != MPI_SUCCESS) {
            err = ncmpii_error_mpi2nc(mpireturn, "MPI_File_read_all");
            err = (err == NC_EFILE) ? NC_EREAD : err;
            DEBUG_ASSIGN_ERROR(status, err)
        }
    } else { /* write request */
        TRACE_IO(MPI_File_write_all)(fh, NULL, 0, MPI_BYTE, &mpistatus);
        if (mpireturn != MPI_SUCCESS) {
            err = ncmpii_error_mpi2nc(mpireturn, "MPI_File_write_all");
            err = (err == NC_EFILE) ? NC_EWRITE : err;
            DEBUG_ASSIGN_ERROR(status, err)
        }
    }

    /* No longer need to reset the file view, as the root's fileview includes
     * the whole file header.
     TRACE_IO(MPI_File_set_view)(fh, 0, MPI_BYTE, MPI_BYTE, "native",
                                 MPI_INFO_NULL);
     */

    return status;
}

/*----< abuf_coalesce() >----------------------------------------------------*/
/* this function should be called after all bput requests have been served */
static int
abuf_coalesce(NC *ncp)
{
    int i;

    i = ncp->abuf->tail - 1;
    /* tail is always pointing to the last (empty) entry of occupy_table[] */

    /* coalesce the freed entries backwardly from the tail */
    while (i >= 0) {
        if (ncp->abuf->occupy_table[i].is_used == 0) {
            ncp->abuf->size_used -= ncp->abuf->occupy_table[i].req_size;
            i--;
        }
        else break;
    }
    ncp->abuf->tail = i + 1;
    /* This may not be ideal, as we stop at the last one that is yet to be
     * freed. There may be some freed entries before this yet-to-be-freed
     * one, but we would like to keep the available space as contiguous as
     * possible. Maybe some smart approach can be considered here.
     */

    return NC_NOERR;
}

#define FREE_REQUEST(req) {                                                   \
    if (req.num_recs > 0) {                                                   \
        if (req.abuf_index >= 0)                                              \
            ncp->abuf->occupy_table[req.abuf_index].is_used = 0; /* free */   \
        else if (req.xbuf != NULL && req.xbuf != req.buf)                     \
            NCI_Free(req.xbuf);                                               \
    }                                                                         \
    req.xbuf = NULL;                                                          \
    NCI_Free(req.start);                                                      \
}

/*----< ncmpio_cancel() >-----------------------------------------------------*/
/* argument num_req can be NC_REQ_ALL, NC_GET_REQ_ALL, NC_PUT_REQ_ALL, or
 * non-negative value */
int
ncmpio_cancel(void *ncdp,
              int   num_req,
              int  *req_ids,  /* [num_req]: IN/OUT */
              int  *statuses) /* [num_req] can be NULL (ignore status) */
{
    int i, j, first_non_null_get, first_non_null_put, status=NC_NOERR;
    NC *ncp=(NC*)ncdp;

    if (num_req == 0) return NC_NOERR;

    /* 1.7.0 and after nonblocking APIs can be called in define mode.
    if (NC_indef(ncp)) DEBUG_RETURN_ERROR(NC_EINDEFINE)
    */

    if (num_req == NC_GET_REQ_ALL || num_req == NC_REQ_ALL) {
        /* cancel all pending read requests, ignore req_ids and statuses */
        for (i=0; i<ncp->numGetReqs; i++) {
            if (ncp->get_list[i].num_recs > 0) {
                if (ncp->get_list[i].imaptype != MPI_DATATYPE_NULL)
                    MPI_Type_free(&ncp->get_list[i].imaptype);
                if (!ncp->get_list[i].buftype_is_contig)
                    MPI_Type_free(&ncp->get_list[i].buftype);
                if (ncp->get_list[i].tmpBuf != NULL)
                    MPI_Type_free(&ncp->get_list[i].buftype);
            }
            FREE_REQUEST(ncp->get_list[i])
        }
        NCI_Free(ncp->get_list);
        ncp->get_list = NULL;
        ncp->numGetReqs = 0;
    }

    if (num_req == NC_PUT_REQ_ALL || num_req == NC_REQ_ALL) {
        /* cancel all pending write requests, ignore req_ids and statuses */
        NC_req *put_list = ncp->put_list;
        for (i=0; i<ncp->numPutReqs; i++) {
            if (put_list[i].num_recs != 0 && put_list[i].need_swap_back_buf)
                /* if user buffer is in-place byte-swapped, swap it back */
                ncmpii_in_swapn(put_list[i].buf,
                                put_list[i].bnelems * put_list[i].num_recs,
                                put_list[i].varp->xsz);

            if (put_list[i].tmpBuf != NULL && put_list[i].abuf_index == -1)
                NCI_Free(put_list[i].tmpBuf);

            FREE_REQUEST(put_list[i])
        }
        NCI_Free(put_list);
        ncp->put_list = NULL;
        ncp->numPutReqs = 0;
        if (ncp->abuf != NULL) { /* clear out the attached buffer usage */
            ncp->abuf->tail = 0;
            ncp->abuf->size_used = 0;
        }
    }
    if (num_req < 0) return NC_NOERR;

    /* check each request ID from the read/write request list */
    first_non_null_get = 0;
    first_non_null_put = 0;
    for (i=0; i<num_req; i++) {
        if (statuses != NULL) statuses[i] = NC_NOERR;

        if (req_ids[i] == NC_REQ_NULL) continue;

        if (req_ids[i] & 1) {
            /* check if this is a read request (id is an odd number) */
            NC_req *get_list = ncp->get_list;
            int last_index=-1;
            for (j=first_non_null_get; j<ncp->numGetReqs; j++) {
                if (get_list[j].id == NC_REQ_NULL) continue;
                /* there may be more than one node with the same ID */
                if (get_list[j].id == req_ids[i]) { /* found it */
                    if (last_index < 0) last_index = j; /* keep first index */
                    if (get_list[j].num_recs > 0) {
                        if (get_list[j].imaptype != MPI_DATATYPE_NULL)
                            MPI_Type_free(&get_list[j].imaptype);
                        if (!get_list[j].buftype_is_contig)
                            MPI_Type_free(&get_list[j].buftype);
                        if (get_list[j].tmpBuf != NULL) {
                            NCI_Free(get_list[j].tmpBuf);
                            MPI_Type_free(&get_list[j].buftype);
                        }
                    }
                    FREE_REQUEST(get_list[j])
                    get_list[j].id = NC_REQ_NULL; /* marked as freed */
                }
                else if (last_index >= 0)
                    break; /* done with all requests of this ID */
            }
            if (last_index >= 0) { /* found in read list */
                /* using first_non_null_get only makes sense when the request
                 * IDs in get_list[] are monotonically nondecreasing, which is
                 * the case in PnetCDF
                 */
                if (last_index == first_non_null_get) first_non_null_get = j;
                req_ids[i] = NC_REQ_NULL;
                continue; /* loop i, go to next request ID */
            }
            /* else means req_ids[i] is not found in get_list[] */
        }
        else {
            /* check if this is a write request (id is an even number) */
            NC_req *put_list = ncp->put_list;
            int last_index=-1;
            for (j=first_non_null_put; j<ncp->numPutReqs; j++) {
                if (put_list[j].id == NC_REQ_NULL) continue;
                /* there may be more than one node with the same ID */
                if (put_list[j].id == req_ids[i]) {
                    if (last_index < 0) {
                        last_index = j;
                        /* put_list[j].num_recs should be >= 1 */
                        if (put_list[j].need_swap_back_buf)
                            /* if user buffer is in-place byte-swapped, swap it back */
                            ncmpii_in_swapn(put_list[j].buf,
                                   put_list[j].bnelems * put_list[j].num_recs,
                                   put_list[j].varp->xsz);

                        if (put_list[j].tmpBuf != NULL &&
                            put_list[j].abuf_index == -1)
                            NCI_Free(put_list[j].tmpBuf);
                    }
                    FREE_REQUEST(put_list[j])
                    put_list[j].id = NC_REQ_NULL; /* marked as freed */
                }
                else if (last_index >= 0)
                    break; /* done with all requests of this ID */
            }
            if (last_index >= 0) { /* found in write list */
                /* using first_non_null_put only makes sense when the request
                 * IDs in put_list[] are monotonically nondecreasing, which is
                 * the case in PnetCDF
                 */
                if (last_index == first_non_null_put) first_non_null_put = j;
                req_ids[i] = NC_REQ_NULL;
                continue; /* loop i, go to next request ID */
            }
            /* else means req_ids[i] is not found in put_list[] */
        }
        /* no such request ID, if the program reached here */
        if (statuses != NULL) DEBUG_ASSIGN_ERROR(statuses[i], NC_EINVAL_REQUEST)
        /* retain the first error status */
        if (status == NC_NOERR) DEBUG_ASSIGN_ERROR(status, NC_EINVAL_REQUEST)
    }
    if (ncp->abuf != NULL) abuf_coalesce(ncp);

    /* coalesce get_list */
    for (i=0,j=first_non_null_get; j<ncp->numGetReqs; j++) {
        for (; ncp->get_list[j].id==NC_REQ_NULL && j<ncp->numGetReqs; j++);
        if (j == ncp->numGetReqs) break;
        if (i < j) ncp->get_list[i] = ncp->get_list[j];
        i++;
    }
    ncp->numGetReqs = i;
    if (ncp->numGetReqs == 0) {
        NCI_Free(ncp->get_list);
        ncp->get_list = NULL;
    }

    /* coalesce put_list */
    for (i=0,j=first_non_null_put; j<ncp->numPutReqs; j++) {
        for (; ncp->put_list[j].id==NC_REQ_NULL && j<ncp->numPutReqs; j++);
        if (j == ncp->numPutReqs) break;
        if (i < j) ncp->put_list[i] = ncp->put_list[j];
        i++;
    }
    ncp->numPutReqs = i;
    if (ncp->numPutReqs == 0) {
        NCI_Free(ncp->put_list);
        ncp->put_list = NULL;
    }

    return status;
}

#ifndef ENABLE_REQ_AGGREGATION
/*----< extract_reqs() >-----------------------------------------------------*/
/* based on the request type, construct an array of unique request IDs.
 * Input value of *num_reqs is NC_REQ_ALL, NC_GET_REQ_ALL, or NC_PUT_REQ_ALL
 * The output of *num_reqs is the number of requests.
 */
static void
extract_reqs(NC   *ncp,
             int  *num_reqs, /* IN/OUT */
             int **req_ids)  /* OUT */
{
    int i, req_type, prev_id;

    req_type = *num_reqs;

    /* first loop finds the number of unique request IDs
     * some requests may share the same ID, they were requests to record
     * variables or called from iput/iget/bput varn APIs.
     */
    *num_reqs = 0;
    prev_id = -1;
    if (req_type == NC_GET_REQ_ALL || req_type == NC_REQ_ALL) {
        for (i=0; i<ncp->numGetReqs; i++) {
            if (ncp->get_list[i].id != prev_id) {
                prev_id = ncp->get_list[i].id;
                (*num_reqs)++;
            }
        }
    }
    prev_id = -1;
    if (req_type == NC_PUT_REQ_ALL || req_type == NC_REQ_ALL) {
        for (i=0; i<ncp->numPutReqs; i++) {
            if (ncp->put_list[i].id != prev_id) {
                prev_id = ncp->put_list[i].id;
                (*num_reqs)++;
            }
        }
    }

    /* allocate ID array */
    (*req_ids) = (int*) NCI_Malloc(*num_reqs * SIZEOF_INT);

    /* second loop fills the request IDs */
    *num_reqs = 0;
    prev_id = -1;
    if (req_type == NC_GET_REQ_ALL || req_type == NC_REQ_ALL) {
        for (i=0; i<ncp->numGetReqs; i++) {
            if (ncp->get_list[i].id != prev_id) {
                prev_id = ncp->get_list[i].id;
                (*req_ids)[(*num_reqs)++] = prev_id;
            }
        }
    }
    prev_id = -1;
    if (req_type == NC_PUT_REQ_ALL || req_type == NC_REQ_ALL) {
        for (i=0; i<ncp->numPutReqs; i++) {
            if (ncp->put_list[i].id != prev_id) {
                prev_id = ncp->put_list[i].id;
                (*req_ids)[(*num_reqs)++] = prev_id;
            }
        }
    }
}
#endif

/*----< concatenate_datatypes() >--------------------------------------------*/
static int
concatenate_datatypes(int           num,
                      int          *blocklens,     /* IN: [num] */
                      MPI_Offset   *displacements, /* IN: [num] */
                      MPI_Datatype *dtypes,        /* IN: [num] */
                      MPI_Datatype *datatype)      /* OUT: */
{
#if SIZEOF_MPI_AINT != SIZEOF_MPI_OFFSET
    int i;
#endif
    int mpireturn, status=NC_NOERR;
    MPI_Aint *addrs;

    *datatype = MPI_BYTE;

    if (num <= 0) return NC_NOERR;

    /* on most 32 bit systems, MPI_Aint and MPI_Offset are different sizes.
     * Possible that on those platforms some of the beginning offsets of
     * these variables in the dataset won't fit into the aint used by
     * MPI_Type_create_struct.  Minor optimization: we don't need to do any
     * of this if MPI_Aint and MPI_Offset are the same size  */

    /* at the configure time, size of MPI_Offset and MPI_Aint are checked */
#if SIZEOF_MPI_AINT == SIZEOF_MPI_OFFSET
    addrs = (MPI_Aint*) displacements; /* cast ok: types same size */
#else
    /* if (sizeof(MPI_Offset) != sizeof(MPI_Aint)) */
    addrs = (MPI_Aint *) NCI_Malloc((size_t)num * SIZEOF_MPI_AINT);
    for (i=0; i<num; i++) {
        addrs[i] = displacements[i];
        if (displacements[i] != addrs[i]) {
            NCI_Free(addrs);
            DEBUG_RETURN_ERROR(NC_EAINT_TOO_SMALL)
        }
    }
#endif

#ifdef HAVE_MPI_TYPE_CREATE_STRUCT
    mpireturn = MPI_Type_create_struct(num, blocklens, addrs, dtypes, datatype);
#else
    mpireturn = MPI_Type_struct(num, blocklens, addrs, dtypes, datatype);
#endif
    if (mpireturn != MPI_SUCCESS)
        status = ncmpii_error_mpi2nc(mpireturn, "MPI_Type_create_struct");
    else
        MPI_Type_commit(datatype);

#if SIZEOF_MPI_AINT != SIZEOF_MPI_OFFSET
    NCI_Free(addrs);
#endif

    return status;
}

/*----< construct_filetypes() >----------------------------------------------*/
/* concatenate the requests into a single MPI derived filetype */
static int
construct_filetypes(NC           *ncp,
                    int           num_reqs,
                    NC_req       *reqs,      /* [num_reqs] */
                    int           rw_flag,
                    MPI_Datatype *filetype)  /* OUT */
{
    int i, j, err, status=NC_NOERR, *blocklens;
    MPI_Datatype *ftypes;
    MPI_Offset *displacements;

    if (num_reqs <= 0) { /* for participating collective call */
        *filetype = MPI_BYTE;
        return NC_NOERR;;
    }

    /* hereinafter, num_reqs > 0 */
    blocklens     = (int*)          NCI_Malloc((size_t)num_reqs * SIZEOF_INT);
    displacements = (MPI_Offset*)   NCI_Malloc((size_t)num_reqs * SIZEOF_MPI_OFFSET);
    ftypes        = (MPI_Datatype*) NCI_Malloc((size_t)num_reqs * sizeof(MPI_Datatype));

    /* create a filetype for each request */
    int last_contig_req = -1; /* index of the last contiguous request */
    j = 0;                    /* index of last valid ftypes */
    for (i=0; i<num_reqs; i++, j++) {
        int is_filetype_contig;
        ftypes[j] = MPI_BYTE; /* in case the call below failed */
        err = ncmpio_filetype_create_vars(ncp,
                                          reqs[i].varp,
                                          reqs[i].start,
                                          reqs[i].count,
                                          reqs[i].stride,
                                          rw_flag,
                                          &blocklens[j],
                                          &displacements[j], /* to offset 0 */
                                          &ftypes[j],
                                          &is_filetype_contig);
        if (err != NC_NOERR) {
            reqs[i].bnelems = 0; /* make this request no effect */
            if (reqs[i].status != NULL && *reqs[i].status == NC_NOERR)
                *reqs[i].status = err;
            if (status == NC_NOERR) status = err; /* report the first error */
            continue;
        }

        if (is_filetype_contig) {
            if (last_contig_req >= 0 &&
                displacements[j] - displacements[last_contig_req] ==
                blocklens[last_contig_req]) {
                blocklens[last_contig_req] += blocklens[j];
                j--;
            }
            else last_contig_req = j;
        }
        else last_contig_req = -1;
    }
    /* j is the new num_reqs */
    num_reqs = j;

    if (status != NC_NOERR) {
        /* even if error occurs, we still must participate the collective
           call to MPI_File_set_view() */
        *filetype = MPI_BYTE;
    }
    else if (num_reqs == 1 && displacements[0] == 0) {
        MPI_Type_dup(ftypes[0], filetype);
    }
    else { /* if (num_reqs > 1 || (num_reqs == 1 && displacements[0] > 0)) */
        /* all ftypes[] created fine, now concatenate all ftypes[] */
        err = concatenate_datatypes(num_reqs, blocklens, displacements, ftypes,
                                    filetype);
        if (err != NC_NOERR) *filetype = MPI_BYTE;
        if (status == NC_NOERR) status = err; /* report the first error */
    }

    for (i=0; i<num_reqs; i++) {
        if (ftypes[i] != MPI_BYTE)
            MPI_Type_free(&ftypes[i]);
    }
    NCI_Free(ftypes);
    NCI_Free(displacements);
    NCI_Free(blocklens);

    return status;
}

/*----< construct_buffertypes() >--------------------------------------------*/
/* the input requests, reqs[], are non-interleaving requests */
static int
construct_buffertypes(int           num_reqs,
                      NC_req       *reqs,         /* [num_reqs] */
                      MPI_Datatype *buffer_type)  /* OUT */
{
    int i, j, status=NC_NOERR, mpireturn;

    *buffer_type = MPI_BYTE;
    if (num_reqs == 0) return NC_NOERR;

    /* create the I/O buffer derived data type */
    int *blocklengths = (int*) NCI_Malloc((size_t)num_reqs * SIZEOF_INT);
    MPI_Aint *disps = (MPI_Aint*) NCI_Malloc((size_t)num_reqs*SIZEOF_MPI_AINT);
    MPI_Aint a0, ai;

    /* process only valid requests */
    for (i=0, j=0; i<num_reqs; i++) {
        /* check int overflow */
        MPI_Offset int8 = reqs[i].bnelems * reqs[i].varp->xsz;
        blocklengths[j] = (int)int8;
        if (int8 != blocklengths[j]) { /* skip this request */
            DEBUG_ASSIGN_ERROR(status, NC_EINTOVERFLOW)
            continue;
        }
#ifdef HAVE_MPI_GET_ADDRESS
        MPI_Get_address(reqs[i].xbuf, &ai);
#else
        MPI_Address(reqs[i].xbuf, &ai);
#endif
        if (j == 0) a0 = ai;
        disps[j] = ai - a0;
        j++;
    }
    /* update num_reqs to number of valid requests */
    num_reqs = j;

    if (num_reqs > 0) {
        /* concatenate buffer addresses into a single buffer type */
#ifdef HAVE_MPI_TYPE_CREATE_HINDEXED
        mpireturn = MPI_Type_create_hindexed(num_reqs, blocklengths, disps,
                                             MPI_BYTE, buffer_type);
#else
        mpireturn = MPI_Type_hindexed(num_reqs, blocklengths, disps, MPI_BYTE,
                                      buffer_type);
#endif
        if (mpireturn != MPI_SUCCESS) {
            int err = ncmpii_error_mpi2nc(mpireturn,"MPI_Type_create_hindexed");
            /* return the first encountered error if there is any */
            if (status == NC_NOERR) status = err;
        }
        else
            MPI_Type_commit(buffer_type);
    }
    NCI_Free(disps);
    NCI_Free(blocklengths);

    return status;
}

/*----< req_commit() >-------------------------------------------------------*/
/* The buffer management flow is described below. The wait side starts from
   the I/O step, i.e. step 5

   for put_varm:
     1. pack buf to lbuf based on buftype
     2. create imap_type based on imap
     3. pack lbuf to cbuf based on imap_type
     4. type convert and byte swap cbuf to xbuf
     5. write from xbuf
     6. byte swap the buf to its original, if it is swapped
     7. free up temp buffers, cbuf and xbuf if they were allocated separately

   for get_varm:
     1. allocate lbuf
     2. create imap_type based on imap
     3. allocate cbuf
     4. allocate xbuf
     5. read to xbuf
     6. type convert and byte swap xbuf to cbuf
     7. unpack cbuf to lbuf based on imap_type
     8. unpack lbuf to buf based on buftype
     9. free up temp buffers, cbuf and xbuf if they were allocated separately
 */
static int
req_commit(NC  *ncp,
           int  num_reqs,   /* number of requests */
           int *req_ids,    /* [num_reqs] */
           int *statuses,   /* [num_reqs] */
           int  coll_indep) /* NC_REQ_COLL or NC_REQ_INDEP */
{
    int i, j, err=NC_NOERR, status=NC_NOERR;
    int do_read, do_write, num_w_reqs=0, num_r_reqs=0;
    int first_non_null_get, first_non_null_put;
    MPI_Offset newnumrecs=0;
    NC_req *put_list=NULL, *get_list=NULL;

    num_r_reqs = 0;
    num_w_reqs = 0;
    if (num_reqs == NC_GET_REQ_ALL || num_reqs == NC_REQ_ALL) {
        /* in this case, arguments req_ids[] and statuses[] are ignored */
        num_r_reqs      = ncp->numGetReqs;
        get_list        = ncp->get_list;
        ncp->numGetReqs = 0;
        ncp->get_list   = NULL;
    }
    if (num_reqs == NC_PUT_REQ_ALL || num_reqs == NC_REQ_ALL) {
        /* in this case, arguments req_ids[] and statuses[] are ignored */
        num_w_reqs      = ncp->numPutReqs;
        put_list        = ncp->put_list;
        ncp->numPutReqs = 0;
        ncp->put_list   = NULL;
    }

    /* extract the matched requests from the pending queues (get_list and
     * put_list containing all nonblocking requests posted by far) into two
     * separate arrays to be used for read and write separately. In the
     * meantime, coalesce the pending request lists.
     */
    if (num_reqs > 0) { /* assume the max number of requests */
        put_list = (NC_req*) NCI_Malloc((size_t)ncp->numPutReqs*sizeof(NC_req));
        get_list = (NC_req*) NCI_Malloc((size_t)ncp->numGetReqs*sizeof(NC_req));
    }

    /* check each request ID from the read/write request list */
    first_non_null_get = 0;
    first_non_null_put = 0;
    for (i=0; i<num_reqs; i++) {
        /* initialize the request's status */
        if (statuses != NULL) statuses[i] = NC_NOERR;

        if (req_ids[i] == NC_REQ_NULL) continue; /* skip NULL request */

        if (req_ids[i] & 1) {
            /* check if this is a read request (id is an odd number)*/
            int last_index=-1;
            for (j=first_non_null_get; j<ncp->numGetReqs; j++) {
                if (ncp->get_list[j].id == NC_REQ_NULL) continue;
                /* there may be more than one node with the same ID */
                if (ncp->get_list[j].id == req_ids[i]) { /* found it */
                    if (last_index < 0) last_index = j; /* keep first index */
                    get_list[num_r_reqs] = ncp->get_list[j];
                    get_list[num_r_reqs].status = (statuses == NULL) ? NULL :
                                                  statuses + i;
                    num_r_reqs++;
                    ncp->get_list[j].id = NC_REQ_NULL; /* marked as freed */
                }
                else if (last_index >= 0)
                    break; /* done with all requests of this ID */
            }
            if (last_index >= 0) { /* found in read list */
                /* using first_non_null_get only makes sense when the request
                 * IDs in get_list[] are monotonically nondecreasing, which is
                 * the case in PnetCDF
                 */
                if (last_index == first_non_null_get) first_non_null_get = j;
                req_ids[i] = NC_REQ_NULL;
                continue; /* loop i, go to next request ID */
            }
        }
        else { /* check if this is a write request (id is an even number) */
            int last_index=-1;
            for (j=first_non_null_put; j<ncp->numPutReqs; j++) {
                if (ncp->put_list[j].id == NC_REQ_NULL) continue;
                /* there may be more than one node with the same ID */
                if (ncp->put_list[j].id == req_ids[i]) {
                    if (last_index < 0) last_index = j;
                    put_list[num_w_reqs] = ncp->put_list[j];
                    put_list[num_w_reqs].status = (statuses == NULL) ? NULL :
                                                  statuses + i;
                    num_w_reqs++;
                    ncp->put_list[j].id = NC_REQ_NULL; /* marked as freed */
                }
                else if (last_index >= 0)
                    break; /* done with all requests of this ID */
            }
            if (last_index >= 0) { /* found in write list */
                /* using first_non_null_put only makes sense when the request
                 * IDs in put_list[] are monotonically nondecreasing, which is
                 * the case in PnetCDF
                 */
                if (last_index == first_non_null_put) first_non_null_put = j;
                req_ids[i] = NC_REQ_NULL;
                continue; /* loop i, go to next request ID */
            }
        }
        /* no such request ID, if the program reached here */
        if (statuses != NULL) DEBUG_ASSIGN_ERROR(statuses[i], NC_EINVAL_REQUEST)
        /* retain the first error status */
        if (status == NC_NOERR) DEBUG_ASSIGN_ERROR(status, NC_EINVAL_REQUEST)
    }

    if (num_reqs > 0) { /* not NC_REQ_ALL, NC_GET_REQ_ALL, or NC_PUT_REQ_ALL */
        /* coalesce get_list */
        for (i=0,j=first_non_null_get; j<ncp->numGetReqs; j++) {
            for (; ncp->get_list[j].id==NC_REQ_NULL && j<ncp->numGetReqs; j++);
            if (j == ncp->numGetReqs) break;
            if (i < j) ncp->get_list[i] = ncp->get_list[j];
            i++;
        }
        ncp->numGetReqs = i;
        if (ncp->numGetReqs == 0) {
            NCI_Free(ncp->get_list);
            ncp->get_list = NULL;
        }

        /* coalesce put_list */
        for (i=0,j=first_non_null_put; j<ncp->numPutReqs; j++) {
            for (; ncp->put_list[j].id==NC_REQ_NULL && j<ncp->numPutReqs; j++);
            if (j == ncp->numPutReqs) break;
            if (i < j) ncp->put_list[i] = ncp->put_list[j];
            i++;
        }
        ncp->numPutReqs = i;
        if (ncp->numPutReqs == 0) { /* free put_list */
            NCI_Free(ncp->put_list);
            ncp->put_list = NULL;
        }

        if (num_w_reqs == 0) {
            NCI_Free(put_list);
            put_list = NULL;
        }
        if (num_r_reqs == 0) {
            NCI_Free(get_list);
            get_list = NULL;
        }
    }

    /* calculate new number of records:
     * Need to update the number of records if new records have been created.
     * For nonblocking APIs, there is no way for a process to know whether
     * others write to a record variable or not. Hence, we must sync the
     * number of records for write request.
     * Because netCDF allows only one unlimited dimension, find the
     * maximum number of records from all nonblocking write requests
     */
    newnumrecs = ncp->numrecs;
    for (i=0; i<num_w_reqs; i++) {
        if (!IS_RECVAR(put_list[i].varp)) continue; /* not a record var */
        if (put_list[i].bnelems == 0) continue; /* 0-len or invalid request */
        newnumrecs = MAX(newnumrecs, put_list[i].start[0] +
                                     put_list[i].count[0]);
    }

    if (coll_indep == NC_REQ_COLL) {
        int mpireturn;
        MPI_Offset io_req[4], do_io[4];  /* [0]: read [1]: write [2]: error */
        io_req[0] = num_r_reqs;
        io_req[1] = num_w_reqs;
        io_req[2] = -err;   /* all NC errors are negative */
        io_req[3] = newnumrecs;
        TRACE_COMM(MPI_Allreduce)(io_req, do_io, 4, MPI_OFFSET, MPI_MAX,
                                  ncp->comm);
        if (mpireturn != MPI_SUCCESS)
 	    return ncmpii_error_mpi2nc(mpireturn, "MPI_Allreduce"); 

        /* if error occurs, return the API collectively */
        if (do_io[2] != -NC_NOERR) return err;

        /* if at least one process has a non-zero request, all processes must
         * participate the collective read/write */
        do_read    = (do_io[0] > 0);
        do_write   = (do_io[1] > 0);
        newnumrecs = do_io[3];
    }
    else {
        if (err != NC_NOERR) return err;
        do_read  = (num_r_reqs > 0);
        do_write = (num_w_reqs > 0);
    }

    /* carry out writes and reads separately (writes first) */
    if (do_write > 0)
        err = wait_getput(ncp, num_w_reqs, put_list, NC_REQ_WR, coll_indep,
                          newnumrecs);

    if (do_read > 0)
        err = wait_getput(ncp, num_r_reqs, get_list, NC_REQ_RD, coll_indep,
                          newnumrecs);

    /* retain the first error status */
    if (status == NC_NOERR) status = err;

    /* post-IO data processing: In write case, we may need to byte-swap user
     * write buf if it is used as the write buffer in MPI write call and the
     * target machine is little Endian. For read case, we may need to
     * unpack/byte-swap/type-convert a temp buffer to the user read buf
     */

    for (i=0; i<num_w_reqs; i++) {
        /* must byte-swap the user buffer back to its original Endianness
         * only when the buffer itself has been byte-swapped before,
         * i.e. NOT buftype_is_contig && NOT ncmpii_need_convert() &&
         * ncmpii_need_swap()
	 * For requests that write to record variables for more than one
	 * record, only the request containing the lead record does this (it
	 * does swap for the entire request)
         */
        if (put_list[i].num_recs > 0 && put_list[i].need_swap_back_buf)
            ncmpii_in_swapn(put_list[i].buf,
                            put_list[i].bnelems * put_list[i].num_recs,
                            put_list[i].varp->xsz);
    }
    for (i=0; i<num_w_reqs; i++) {
        /* free space allocated for the request objects
         * tmpBuf is used only by nonblocking varn APIs. During the posting of
         * a nonblocking varn request, the temporary buffer, if allocated, can
         * be divided into several sub-buffers, each used in a separate
         * requests. If a request's tmpBuf is set to non-NULL, indicating it
         * should be freed. Because put_list[] may be sorted based on
         * offset_start, non-NULL tmpBuf can be in any put_list[], this loop
         * cannot be combined with the above one. We need to go through
         * put_list[] to check each one for non-NULL tmpBuf.
         */
        if (put_list[i].tmpBuf != NULL && put_list[i].abuf_index == -1)
            NCI_Free(put_list[i].tmpBuf);

        FREE_REQUEST(put_list[i])
    }
    if (num_w_reqs > 0) {
        /* once the bput requests are served, we reclaim the space and try
         * coalesce the freed space for the attached buffer */
        if (ncp->abuf != NULL) abuf_coalesce(ncp);
        NCI_Free(put_list);
    }

    for (i=0; i<num_r_reqs; i++) {
        int need_convert, need_swap;
        /* non-lead record requests skip type-conversion/byte-swap/unpack */
        if (get_list[i].num_recs == 0) continue;

        /* now, xbuf contains the data read from the file.
         * It may need to be type-converted + byte-swapped to cbuf
         */
        need_convert = ncmpii_need_convert(ncp->format, get_list[i].varp->xtype,
                                           get_list[i].ptype);
        need_swap    = ncmpii_need_swap(get_list[i].varp->xtype,
                                        get_list[i].ptype);

        err = ncmpio_unpack_xbuf(ncp->format, get_list[i].varp,
                                 get_list[i].bufcount,
                                 get_list[i].buftype,
                                 get_list[i].buftype_is_contig,
                                 get_list[i].bnelems * get_list[i].num_recs,
                                 get_list[i].ptype,
                                 get_list[i].imaptype,
                                 need_convert, need_swap, get_list[i].buf,
                                 get_list[i].xbuf);
        if (get_list[i].status != NULL && *get_list[i].status == NC_NOERR)
            *get_list[i].status = err;
        if (status == NC_NOERR) status = err;

        if (!get_list[i].buftype_is_contig)
            MPI_Type_free(&get_list[i].buftype);
#if 0
        void *cbuf, *lbuf;
        int el_size, position;
        MPI_Offset insize, bnelems;
        NC_var *varp;

        varp = get_list[i].varp;
        MPI_Type_size(get_list[i].ptype, &el_size);
        bnelems = get_list[i].bnelems * get_list[i].num_recs;
        insize = bnelems * el_size;
        if (insize != (int)insize && status == NC_NOERR)
            DEBUG_ASSIGN_ERROR(status, NC_EINTOVERFLOW)

        if (ncmpii_need_convert(ncp->format, varp->xtype, get_list[i].ptype)) {
            /* need type conversion from the external type to user buffer
               type */
            if (get_list[i].imaptype != MPI_DATATYPE_NULL ||
                !get_list[i].buftype_is_contig)
                cbuf = NCI_Malloc((size_t)insize);
            else
                cbuf = get_list[i].buf;

            /* type convert + byte swap from xbuf to cbuf */
            DATATYPE_GET_CONVERT(ncp->format, varp->xtype, get_list[i].xbuf,
                                 cbuf, bnelems, get_list[i].ptype, err)

            /* keep the first error */
            if (get_list[i].status != NULL && *get_list[i].status == NC_NOERR)
                *get_list[i].status = err;
            if (status == NC_NOERR) status = err;
        } else {
            if (ncmpii_need_swap(varp->xtype, get_list[i].ptype))
                ncmpii_in_swapn(get_list[i].xbuf, bnelems, varp->xsz);
            cbuf = get_list[i].xbuf;
        }

        if (get_list[i].imaptype != MPI_DATATYPE_NULL) {
            /* handle the case for a true get_varm */
            if (get_list[i].buftype_is_contig)
                lbuf = get_list[i].buf;
            else
                lbuf = NCI_Malloc((size_t)insize);

            /* unpack cbuf to lbuf based on imaptype */
            position = 0;
            MPI_Unpack(cbuf, (int)insize, &position, lbuf, 1,
                       get_list[i].imaptype, MPI_COMM_SELF);
            MPI_Type_free(&get_list[i].imaptype);

            /* cbuf is no longer needed
             * for a true varm call, cbuf cannot be == get_list[i].buf */
            if (cbuf != get_list[i].xbuf) NCI_Free(cbuf);
            cbuf = NULL;
        } else { /* get_vars */
            lbuf = cbuf;
        }

        if (!get_list[i].buftype_is_contig) {
            /* unpack lbuf to buf based on buftype */
            position = 0;
            if (get_list[i].bufcount != (int)get_list[i].bufcount &&
                status == NC_NOERR)
                DEBUG_ASSIGN_ERROR(status, NC_EINTOVERFLOW)
            MPI_Unpack(lbuf, (int)insize, &position, get_list[i].buf,
                       (int)get_list[i].bufcount, get_list[i].buftype,
                       MPI_COMM_SELF);
            MPI_Type_free(&get_list[i].buftype);
        }
        /* lbuf is no longer needed */
        if (lbuf != get_list[i].buf && lbuf != get_list[i].xbuf)
            NCI_Free(lbuf);
#endif
    }

    for (i=0; i<num_r_reqs; i++) {
        /* free space allocated for the request objects
         * tmpBuf is used only by nonblocking varn APIs. During the posting of
         * a nonblocking varn request, the temporary buffer, if allocated, can
         * be divided into several sub-buffers, each used in a separate
         * requests. If a request's tmpBuf is set to non-NULL, indicating it
         * should be freed. Because get_list[] may be sorted based on
         * offset_start, non-NULL tmpBuf can be in any get_list[], this loop
         * cannot be combined with the above one. We need to go through
         * get_list[] to check each one for  non-NULL tmpBuf.
         */
        if (get_list[i].tmpBuf != NULL) {
            int position=0, bufsize;
            MPI_Offset insize;

            /* unpack tmpBuf to userBuf and free tmpBuf
             * Note this unpack must wait for all above unpacks are done
             * because get_list[i].buf may be part of get_list[i].userBuf
             */
            MPI_Type_size(get_list[i].buftype, &bufsize);
            insize = get_list[i].bufcount * bufsize;
            if (insize != (int)insize && status == NC_NOERR)
                DEBUG_ASSIGN_ERROR(status, NC_EINTOVERFLOW)

            MPI_Unpack(get_list[i].tmpBuf, (int)insize, &position,
                       get_list[i].userBuf, (int)get_list[i].bufcount,
                       get_list[i].buftype, MPI_COMM_SELF);
            NCI_Free(get_list[i].tmpBuf);
            MPI_Type_free(&get_list[i].buftype);
        }
        FREE_REQUEST(get_list[i])
    }
    if (num_r_reqs > 0) NCI_Free(get_list);

    return status;
}

/*----< ncmpio_wait() >-------------------------------------------------------*/
int
ncmpio_wait(void *ncdp,
            int   num_reqs,
            int  *req_ids,   /* [num_reqs]: IN/OUT */
            int  *statuses,  /* [num_reqs] */
            int   reqMode)   /* only check if NC_REQ_COLL or NC_REQ_INDEP */
{
    NC *ncp = (NC*)ncdp;
    int coll_indep;

    if (NC_indef(ncp)) /* wait must be called in data mode */
        DEBUG_RETURN_ERROR(NC_EINDEFINE)

    coll_indep = (fIsSet(reqMode, NC_REQ_INDEP)) ? NC_REQ_INDEP : NC_REQ_COLL;

#ifdef ENABLE_REQ_AGGREGATION
    /* check collective or independent mode */
    if (coll_indep == NC_REQ_INDEP && !NC_indep(ncp))
        DEBUG_RETURN_ERROR(NC_ENOTINDEP)
    else if (coll_indep == NC_REQ_COLL && NC_indep(ncp))
        DEBUG_RETURN_ERROR(NC_EINDEP)

    if (coll_indep == NC_REQ_INDEP && num_reqs == 0) return NC_NOERR;

    return req_commit(ncp, num_reqs, req_ids, statuses, coll_indep);
#else
    /* If request aggregation is disabled, we call an independent wait() for
     * each request
     */
    int i, status=NC_NOERR, err, *reqids=NULL;

    if (coll_indep == NC_REQ_INDEP) {
        /* This is called from ncmpi_wait(), which is an independent call
	 * Argument num_reqs can be NC_REQ_ALL which means to flush all pending
	 * nonblocking requests. In this case, arguments req_ids and statuses
	 * will be ignored.
	 * Argument num_reqs must either be NC_REQ_ALL, NC_GET_REQ_ALL,
	 * NC_PUT_REQ_ALL, or a non-negative value.
	 * Argument statuses can be NULL, meaning the caller only cares about
	 * the error code returned by this call, but not the statuses of
	 * individual nonblocking requests.
         */
        if (num_reqs == 0) return NC_NOERR;

	/* This is called from ncmpi_wait which must be called in independent
	 * data mode, illegal in collective mode.
         */
        if (!NC_indep(ncp)) DEBUG_RETURN_ERROR(NC_ENOTINDEP);

        if (coll_indep == NC_REQ_INDEP && num_reqs == 0) return NC_NOERR;
    }
    else {
        /* This is called from ncmpi_wait_all(), which is a collective call
         * Argument num_reqs can be NC_REQ_ALL which means to flush all pending
	 * nonblocking requests. In this case, arguments req_ids and statuses
	 * will be ignored.
	 * Argument num_reqs must either be NC_REQ_ALL, NC_GET_REQ_ALL,
	 * NC_PUT_REQ_ALL, or a non-negative value.
	 * Argument statuses can be NULL, meaning the caller only cares about
	 * the error code returned by this call, but not the statuses of
	 * individual nonblocking requests.
         */
        /* the following line CANNOT be added, because ncmpi_wait_all() is a
         * collective call, all processes must participate some MPI collective
         * operations used later on.
         */
        /* if (num_reqs == 0) return NC_NOERR; */

        /* This is called from ncmpi_wait_all which must be called in
         * collective data mode, illegal in indep mode. This also
         * ensures the program will returns back to collective mode.
         */
        if (NC_indep(ncp)) DEBUG_RETURN_ERROR(NC_EINDEP);

        /* must enter independent mode, as num_reqs may be different among
           processes */
        err = ncmpio_begin_indep_data(ncp);
        if (status == NC_NOERR) status = err;
    }

    if (num_reqs <= NC_REQ_ALL) { /* flush all pending requests */
        /* in this case, arguments req_ids[] and statuses[] are ignored.
         * construct request ID array, reqids */
        extract_reqs(ncp, &num_reqs, &reqids);
    }

    for (i=0; i<num_reqs; i++) { /* serve one request at a time */
        if (reqids == NULL)
            err = req_commit(ncp, 1, &req_ids[i],
                  (statuses == NULL) ? NULL : &statuses[i], NC_REQ_INDEP);
        else
            err = req_commit(ncp, 1, &reqids[i], NULL, NC_REQ_INDEP);
        if (status == NC_NOERR) status = err;
    }
    if (reqids != NULL) NCI_Free(reqids);

    if (coll_indep == NC_REQ_COLL) {
        /* return to collective data mode */
        err = ncmpio_end_indep_data(ncp);
        if (status == NC_NOERR) status = err;
    }

    return status; /* return the first error encountered, if there is any */
#endif
}

/* C struct for breaking down a request to a list of offset-length segments */
typedef struct {
    MPI_Offset off;      /* starting file offset of the request */
    MPI_Offset len;      /* requested length in bytes starting from off */
    MPI_Aint   buf_addr; /* distance of this request's I/O buffer to the first
                            request to be merged */
} off_len;

/*----< off_compare() >-------------------------------------------------------*/
/* used for sorting the offsets of the off_len array */
static int
off_compare(const void *a, const void *b)
{
    if (((off_len*)a)->off > ((off_len*)b)->off) return  1;
    if (((off_len*)a)->off < ((off_len*)b)->off) return -1;
    return 0;
}

/*----< vars_flatten() >------------------------------------------------------*/
/* flatten a subarray request into a list of offset-length pairs */
static MPI_Offset
vars_flatten(int          ndim,    /* number of dimensions */
             int          el_size, /* array element size */
             MPI_Offset  *dimlen,  /* [ndim] dimension lengths */
             MPI_Offset   offset,  /* starting file offset of variable */
             MPI_Aint     buf_addr,/* starting buffer address */
             MPI_Offset  *start,   /* [ndim] starts of subarray */
             MPI_Offset  *count,   /* [ndim] counts of subarray */
             MPI_Offset  *stride,  /* [ndim] strides of subarray */
             MPI_Offset  *nseg,    /* OUT: number of segments */
             off_len     *seg)     /* OUT: array of segments */
{
    int i, j, to_free_stride=0;
    MPI_Offset seg_len, nstride, array_len, off, subarray_len;
    off_len *ptr=seg, *seg0;

    *nseg = 0;
    if (ndim < 0) return *nseg;

    if (ndim == 0) {  /* scalar record variable */
        *nseg = 1;
        seg->off      = offset;
        seg->len      = el_size;
        seg->buf_addr = buf_addr;
        return *nseg;
    }

    if (stride == NULL) { /* equivalent to {1, 1, ..., 1} */
        stride = (MPI_Offset*) NCI_Malloc((size_t)ndim * SIZEOF_MPI_OFFSET);
        for (i=0; i<ndim; i++) stride[i] = 1;
        to_free_stride = 1;
    }

    /* TODO: check if all stride[] >= 1
       Q: Is it legal if any stride[] <= 0 ? */

    /* calculate the number of offset-length pairs */
    *nseg = (stride[ndim-1] == 1) ? 1 : count[ndim-1];
    for (i=0; i<ndim-1; i++)
        *nseg *= count[i];
    if (*nseg == 0) {  /* not reachable, an error if count[] == 0 */
        if (to_free_stride) NCI_Free(stride);
        return *nseg;
    }

    /* the length of all segments are of the same size */
    seg_len  = (stride[ndim-1] == 1) ? count[ndim-1] : 1;
    seg_len *= el_size;
    nstride  = (stride[ndim-1] == 1) ? 1 : count[ndim-1];

    /* set the offset-length pairs for the lowest dimension */
    off = offset + start[ndim-1] * el_size;
    for (i=0; i<nstride; i++) {
        ptr->off       = off;
        ptr->len       = seg_len;
        ptr->buf_addr  = buf_addr;
        buf_addr      += seg_len;
        off           += stride[ndim-1] * el_size;
        ptr++;
    }
    ndim--;

    subarray_len = nstride;
    array_len = 1;
    /* for higher dimensions */
    while (ndim > 0) {
        /* array_len is global array size from lowest up to ndim */
        array_len *= dimlen[ndim];

        /* off is the global array offset for this dimension, ndim-1 */
        off = start[ndim-1] * array_len * el_size;

        /* update all offsets from lowest up to dimension ndim-1 */
        seg0 = seg;
        for (j=0; j<subarray_len; j++) {
            seg0->off += off;
            seg0++;
        }

        /* update each plan subarray of dimension ndim-1 */
        off = array_len * stride[ndim-1] * el_size;
        for (i=1; i<count[ndim-1]; i++) {
            seg0 = seg;
            for (j=0; j<subarray_len; j++) {
                ptr->off       = seg0->off + off;
                ptr->len       = seg_len;
                ptr->buf_addr  = buf_addr;
                buf_addr      += seg_len;
                ptr++;
                seg0++;
            }
            off += array_len * stride[ndim-1] * el_size;
        }
        ndim--;  /* move to next higher dimension */
        subarray_len *= count[ndim];
    }
    if (to_free_stride) NCI_Free(stride);

    return *nseg;
}

/*----< merge_requests() >---------------------------------------------------*/
static int
merge_requests(NC          *ncp,
               int          num_reqs,
               NC_req      *reqs,    /* [num_reqs] */
               void       **buf,     /* OUT: 1st I/O buf addr */
               MPI_Offset  *nsegs,   /* OUT: no. off-len pairs */
               off_len    **segs)    /* OUT: [*nsegs] */
{
    int i, j, status=NC_NOERR, ndims, is_recvar;
    MPI_Offset  nseg, *start, *count, *shape, *stride;
    MPI_Aint addr, buf_addr;

    *nsegs = 0;    /* total number of offset-length pairs */
    *segs  = NULL; /* array of offset-length pairs */

    /* note invalid requests have been removed in wait_getput() */
    *buf = reqs[0].xbuf; /* I/O buffer of first request */

    /* buf_addr is the buffer address of the first request */
#ifdef HAVE_MPI_GET_ADDRESS
    MPI_Get_address(reqs[0].xbuf, &buf_addr);
#else
    MPI_Address(reqs[0].xbuf, &buf_addr);
#endif

    /* Count the number off-len pairs from reqs[], so we can malloc a
     * contiguous memory space for storing off-len pairs
     */
    for (i=0; i<num_reqs; i++) {
        is_recvar = IS_RECVAR(reqs[i].varp);

        /* for record variable, each reqs[] is within a record */
        ndims  = (is_recvar) ? reqs[i].varp->ndims - 1 : reqs[i].varp->ndims;
        count  = (is_recvar) ? reqs[i].count + 1       : reqs[i].count;
        stride = NULL;
        if (reqs[i].stride != NULL)
            stride = (is_recvar) ? reqs[i].stride + 1 : reqs[i].stride;

        if (ndims < 0) continue;
        if (ndims == 0) {  /* 1D record variable */
            (*nsegs)++;
            continue;
        }
        nseg = 1;
        if (stride != NULL && stride[ndims-1] > 1)
            nseg = count[ndims-1];  /* count of last dimension */
        for (j=0; j<ndims-1; j++)
            nseg *= count[j];  /* all count[] except the last dimension */

        *nsegs += nseg;
    }

    /* now we can allocate a contiguous memory space for the off-len pairs */
    off_len *seg_ptr = (off_len*) NCI_Malloc((size_t)(*nsegs) * sizeof(off_len));
    *segs = seg_ptr;

    /* now re-run the loop to fill in the off-len pairs */
    for (i=0; i<num_reqs; i++) {
        /* buf_addr is the buffer address of the first valid request */
#ifdef HAVE_MPI_GET_ADDRESS
        MPI_Get_address(reqs[i].xbuf, &addr);
#else
        MPI_Address(reqs[i].xbuf, &addr);
#endif
        addr -= buf_addr,  /* distance to the buf of first req */

        is_recvar = IS_RECVAR(reqs[i].varp);

        /* for record variable, each reqs[] is within a record */
        ndims  = (is_recvar) ? reqs[i].varp->ndims  - 1 : reqs[i].varp->ndims;
        start  = (is_recvar) ? reqs[i].start  + 1       : reqs[i].start;
        count  = (is_recvar) ? reqs[i].count  + 1       : reqs[i].count;
        shape  = (is_recvar) ? reqs[i].varp->shape  + 1 : reqs[i].varp->shape;
        stride = NULL;
        if (reqs[i].stride != NULL)
            stride = (is_recvar) ? reqs[i].stride + 1 : reqs[i].stride;

        /* find the starting file offset for this record */
        MPI_Offset var_begin = reqs[i].varp->begin;
        if (is_recvar) var_begin += reqs[i].start[0] * ncp->recsize;

        /* flatten each request to a list of offset-length pairs */
        vars_flatten(ndims, reqs[i].varp->xsz, shape, var_begin,
                     addr, start, count, stride,
                     &nseg,    /* OUT: number of offset-length pairs */
                     seg_ptr); /* OUT: array of offset-length pairs */
        seg_ptr += nseg; /* append the list to the end of segs array */
    }

    /* check if (*segs)[].off are in an increasing order */
    for (i=1; i<*nsegs; i++) {
        if ((*segs)[i-1].off > (*segs)[i].off)
            break;
    }
    if (i < *nsegs) /* not in an increasing order */
        /* sort the off-len array, segs[], in an increasing order */
        qsort(*segs, (size_t)(*nsegs), sizeof(off_len), off_compare);

    /* merge the overlapped requests, skip the overlapped regions for those
     * requests with higher j indices (i.e. requests with lower j indices
     * win the writes to the overlapped regions)
     */
    for (i=0, j=1; j<*nsegs; j++) {
        if ((*segs)[i].off + (*segs)[i].len >= (*segs)[j].off + (*segs)[j].len)
            /* segment i completely covers segment j, skip j */
            continue;

        MPI_Offset gap = (*segs)[i].off + (*segs)[i].len - (*segs)[j].off;
        if (gap >= 0) { /* segments i and j overlaps */
            if ((*segs)[i].buf_addr + (*segs)[i].len ==
                (*segs)[j].buf_addr + gap) {
                /* buffers i and j are contiguous, merge j to i */
                (*segs)[i].len += (*segs)[j].len - gap;
            }
            else { /* buffers are not contiguous, reduce j's len */
                (*segs)[i+1].off      = (*segs)[j].off + gap;
                (*segs)[i+1].len      = (*segs)[j].len - gap;
                (*segs)[i+1].buf_addr = (*segs)[j].buf_addr + gap;
                i++;
            }
        }
        else { /* i and j do not overlap */
            i++;
            if (i < j) (*segs)[i] = (*segs)[j];
        }
    }

    /* update number of segments, now all off-len pairs are not overlapped */
    *nsegs = i+1;

    return status;
}

/*----< type_create_off_len() >----------------------------------------------*/
static int
type_create_off_len(MPI_Offset    nsegs,    /* no. off-len pairs */
                    off_len      *segs,     /* [nsegs] off-en pairs */
                    MPI_Datatype *filetype,
                    MPI_Datatype *buf_type)
{
    int i, j, mpireturn, *blocklengths;
    MPI_Aint   *displacements;
    MPI_Offset  next_off, next_len;

    assert(nsegs > 0);

    /* create the file view MPI derived data type by concatenating the sorted
       offset-length pairs */

    /* For filetype, the segs[].off can be further coalesced. For example,
     * when writing a consecutive columns of a 2D array, even though the I/O
     * buffer addresses may not be able to coalesced, the file offsets on
     * the same row can be coalesced. Thus, first calculate the length of
     * coalesced off-len pairs (the memory space needed for malloc)
     */
    next_off = segs[0].off;
    next_len = segs[0].len;
    for (j=0,i=1; i<nsegs; i++) {
        if (next_off + next_len == segs[i].off) /* j and i are contiguous */
            next_len += segs[i].len;
        else {
            j++;
            next_off = segs[i].off;
            next_len = segs[i].len;
        }
    }
    /* j+1 is the coalesced length */
    blocklengths  = (int*)      NCI_Malloc((size_t)(j+1) * SIZEOF_INT);
    displacements = (MPI_Aint*) NCI_Malloc((size_t)(j+1) * SIZEOF_MPI_AINT);

    /* coalesce segs[].off and len to dispalcements[] and blocklengths[] */
    if (segs[0].len != (int)segs[0].len) DEBUG_RETURN_ERROR(NC_EINTOVERFLOW)
    displacements[0] =      segs[0].off;
    blocklengths[0]  = (int)segs[0].len;
    for (j=0,i=1; i<nsegs; i++) {
        if (segs[i].len != (int)segs[i].len) DEBUG_RETURN_ERROR(NC_EINTOVERFLOW)
        if (displacements[j] + blocklengths[j] == segs[i].off)
            /* j and i are contiguous */
            blocklengths[j] += (int)segs[i].len;
            /* TODO: take care of 4-byte int overflow problem */
        else {
            j++;
            displacements[j] =      segs[i].off;
            blocklengths[j]  = (int)segs[i].len;
        }
    }
    /* j+1 is the coalesced length */

#ifdef HAVE_MPI_TYPE_CREATE_HINDEXED
    mpireturn = MPI_Type_create_hindexed(j+1, blocklengths, displacements,
                                         MPI_BYTE, filetype);
#else
    mpireturn = MPI_Type_hindexed(j+1, blocklengths, displacements, MPI_BYTE,
                                  filetype);
#endif
    if (mpireturn != MPI_SUCCESS) {
        *filetype = MPI_BYTE;
        *buf_type = MPI_BYTE;
        NCI_Free(displacements);
        NCI_Free(blocklengths);
        return ncmpii_error_mpi2nc(mpireturn, "MPI_Type_create_hindexed");
    }
    MPI_Type_commit(filetype);
    NCI_Free(displacements);
    NCI_Free(blocklengths);

    /* create the I/O buffer derived data type from the I/O buffer's
       offset-length pairs */

    /* Although it is unlikely buffers can be coalesced, it will not harm to
       check it out */
    next_off = segs[0].buf_addr;
    next_len = segs[0].len;
    for (j=0,i=1; i<nsegs; i++) {
        if (next_off + next_len == segs[i].buf_addr)
            /* j and i are contiguous */
            next_len += segs[i].len;
        else {
            j++;
            next_off = segs[i].buf_addr;
            next_len = segs[i].len;
        }
    }
    /* j+1 is the coalesced length */
    blocklengths  = (int*)      NCI_Malloc((size_t)(j+1) * SIZEOF_INT);
    displacements = (MPI_Aint*) NCI_Malloc((size_t)(j+1) * SIZEOF_MPI_AINT);

    /* coalesce segs[].off and len to dispalcements[] and blocklengths[] */
    if (segs[0].len != (int)segs[0].len) {
        NCI_Free(displacements);
        NCI_Free(blocklengths);
        DEBUG_RETURN_ERROR(NC_EINTOVERFLOW)
    }
    displacements[0] =      segs[0].buf_addr;
    blocklengths[0]  = (int)segs[0].len;
    for (j=0,i=1; i<nsegs; i++) {
        if (segs[i].len != (int)segs[i].len) {
            NCI_Free(displacements);
            DEBUG_RETURN_ERROR(NC_EINTOVERFLOW)
        }
        if (displacements[j] + blocklengths[j] == segs[i].buf_addr)
            /* j and i are contiguous */
            blocklengths[j] += (int)segs[i].len;
        else {
            j++;
            displacements[j] =      segs[i].buf_addr;
            blocklengths[j]  = (int)segs[i].len;
        }
    }
    /* j+1 is the coalesced length */
#ifdef HAVE_MPI_TYPE_CREATE_HINDEXED
    mpireturn = MPI_Type_create_hindexed(j+1, blocklengths, displacements,
                                         MPI_BYTE, buf_type);
#else
    mpireturn = MPI_Type_hindexed(j+1, blocklengths, displacements, MPI_BYTE,
                                  buf_type);
#endif
    if (mpireturn != MPI_SUCCESS) {
        if (*filetype != MPI_BYTE) MPI_Type_free(filetype);
        *filetype = MPI_BYTE;
        *buf_type = MPI_BYTE;
        NCI_Free(displacements);
        NCI_Free(blocklengths);
        return ncmpii_error_mpi2nc(mpireturn, "MPI_Type_create_hindexed");
    }
    MPI_Type_commit(buf_type);
    NCI_Free(displacements);
    NCI_Free(blocklengths);

    return NC_NOERR;
}

/*----< req_compare() >------------------------------------------------------*/
/* used to sort the the string file offsets of reqs[] */
static int
req_compare(const void *a, const void *b)
{
    if (((NC_req*)a)->offset_start > ((NC_req*)b)->offset_start) return (1);
    if (((NC_req*)a)->offset_start < ((NC_req*)b)->offset_start) return (-1);
    return (0);
}

/*----< req_aggregation() >--------------------------------------------------*/
/* aggregate multiple read/write (non-contiguous) requests and call MPI-IO
 */
static int
req_aggregation(NC     *ncp,
                int     num_reqs,    /* # requests */
                NC_req *reqs,        /* sorted requests */
                int     rw_flag,     /* NC_REQ_WR or NC_REQ_RD */
                int     coll_indep,  /* NC_REQ_COLL or NC_REQ_INDEP */
                int     interleaved) /* interleaved in reqs[] */
{
    int i, type, err, status=NC_NOERR, ngroups, mpireturn, buf_len;
    int *group_index, *group_type, buf_type_size=0;
    int *f_blocklengths, *b_blocklengths;
    void *buf; /* point to starting buffer, used by MPI-IO call */
    MPI_Aint      b_begin, b_addr, *f_disps, *b_disps;
    MPI_Datatype  filetype, buf_type, *ftypes, *btypes;
    MPI_File fh;
    MPI_Status mpistatus;

    if (num_reqs == 0) { /* only NC_REQ_COLL can reach here for 0 request */
        assert(coll_indep == NC_REQ_COLL);
        /* simply participate the collective call */
        return ncmpio_getput_zero_req(ncp, rw_flag);
    }
    if (! interleaved) {
        /* concatenate all filetypes into a single one and do I/O */
        return mgetput(ncp, num_reqs, reqs, rw_flag, coll_indep);
    }
    /* now some request's aggregate access region is interleaved with other's */

    /* divide the requests into groups.
     * Two types of groups: one contains requests that all are not interleaved
     * and the other contains requests that any 2 consecutive requests are
     * interleaved. All requests will be aggregated into one and carried out
     * by a single MPI-IO call.
     * This approach is because MPI collective I/O requires each process's
     * fileview must contain only monotonic non-decreasing file offsets. Thus
     * if the nonblocking requests interleave with each other (although not
     * overlap), then we cannot simply concatenate the filetypes of individual
     * requests. This approach flattens the requests of "interleaved" groups
     * into offset-length pairs, sorts, and merges them into an aggregated
     * filetype. Similar for building an aggregated I/O buffer type.
     */

    /* first calculate the number of groups, so group_index and group_type can
       be malloc-ed. Group type: 0 for non-interleaved group and 1 for
       interleaved group.
     */
    ngroups = 1;
    type    = (reqs[0].offset_end > reqs[1].offset_start) ? 1 : 0;
    for (i=1; i<num_reqs-1; i++) {
        if (type == 0 && reqs[i].offset_end > reqs[i+1].offset_start) {
            ngroups++;
            type = 1;
        }
        else if (type == 1 && reqs[i].offset_end <= reqs[i+1].offset_start) {
            type = 0;
            if (i+2 < num_reqs && reqs[i+1].offset_end > reqs[i+2].offset_start)
                type = 1; /* next group is also interleaved */
            ngroups++;
        }
    }

    group_index = (int*) NCI_Malloc((size_t)(ngroups+1) * SIZEOF_INT);
    group_type  = (int*) NCI_Malloc((size_t)(ngroups+1) * SIZEOF_INT);

    /* calculate the starting index of each group and determine group type */
    ngroups        = 1;
    type           = (reqs[0].offset_end > reqs[1].offset_start) ? 1 : 0;
    group_index[0] = 0;
    group_type[0]  = type;
    for (i=1; i<num_reqs-1; i++) {
        if (type == 0 &&
            reqs[i].offset_end > reqs[i+1].offset_start) {
            /* reqs[i] starts an interleaved group */
            group_index[ngroups] = i;
            type = 1;
            group_type[ngroups] = type;
            ngroups++;
        }
        else if (type == 1 &&
                 reqs[i].offset_end <= reqs[i+1].offset_start) {
            /* the interleaved group ends with reqs[i] */
            group_index[ngroups] = i+1;
            type = 0;
            if (i+2 < num_reqs && reqs[i+1].offset_end > reqs[i+2].offset_start)
                type = 1; /* next group is also interleaved */
            group_type[ngroups] = type;
            ngroups++;
        }
    }
    group_index[ngroups] = num_reqs; /* to indicate end of groups */

    /* for each group, construct one filetype by concatenating if the group
     * is non-interleaved and by flatten/sort/merge if the group is
     * interleaved. At the end, all ngroups filetypes are concatenated into
     * a single filetype. Similar for constructing buffer types.
     * Then use one collective I/O to commit.
     */

    ftypes = (MPI_Datatype*) NCI_Malloc((size_t)ngroups*2*sizeof(MPI_Datatype));
    btypes = ftypes + ngroups;
    f_blocklengths = (int*) NCI_Malloc((size_t)ngroups*2*SIZEOF_INT);
    b_blocklengths = f_blocklengths + ngroups;
    f_disps = (MPI_Aint*) NCI_Malloc((size_t)ngroups*2*SIZEOF_MPI_AINT);
    b_disps = f_disps + ngroups;

    buf = reqs[0].xbuf; /* the buffer of 1st request */
    b_disps[0] = 0;     /* relative to address of 1st buf */
#ifdef HAVE_MPI_GET_ADDRESS
    MPI_Get_address(buf, &b_begin);
#else
    MPI_Address(buf, &b_begin);
#endif

    /* for each group, build a filetype and a buffer type in ftypes[i] and
       btypes[i] */
    for (i=0; i<ngroups; i++) {
        NC_req *g_reqs = reqs + group_index[i];
        int     g_num_reqs = group_index[i+1] - group_index[i];
        f_disps[i] = 0;  /* file displacements always to the file offset 0 */

        if (group_type[i] == 0) {
            /* This group contains no interleaved filetypes, so we can
             * simply concatenate filetypes of this group into a single one
             */
            err = construct_filetypes(ncp, g_num_reqs, g_reqs, rw_flag,
                                      &ftypes[i]);
            if (status == NC_NOERR) status = err;
            if (err != NC_NOERR) { /* skip this group */
                ftypes[i] = btypes[i] = MPI_BYTE;
                f_blocklengths[i] = 0;
                continue;
            }
            f_blocklengths[i] = 1;

            /* concatenate buffer types of this group into a single one */
            err = construct_buffertypes(g_num_reqs, g_reqs, &btypes[i]);
            if (status == NC_NOERR) status = err;
            if (err != NC_NOERR) { /* skip this group */
                ftypes[i] = btypes[i] = MPI_BYTE;
                b_blocklengths[i] = 0;
                f_blocklengths[i] = 0;
                continue;
            }
        }
        else { /* this group is interleaved */
            /* flatten the interleaved requests in this group, so interleaved
             * requests can be sorted and merged into a monotonically
             * non-decreasing filetype. For example, multiple nonblocking
             * requests each accessing a single column of a 2D array, that each
             * produces a filetype interleaving with others'.
             *
             * The pitfall of this flattening is the additional memory
             * requirement, as it will have to break down each request into a
             * list of offset-length pairs, and merge all lists into a sorted
             * list based on their offsets into an increasing order.
             *
             * Be warned! The additional memory requirement for this merging can
             * be more than the I/O data itself. For example, each nonblocking
             * request access a single column of a 2D array of 4-byte integer
             * type. Each off-len pair represents only a 4-byte integer, but the
             * off-len pair itself takes 24 bytes. Additional memory is also
             * required for MPI arguments of displacements and blocklengths.
             */
            MPI_Offset  nsegs=0;   /* number of merged offset-length pairs */
            off_len    *segs=NULL; /* array of the offset-length pairs */
            void       *merged_buf;

            /* merge all requests into sorted offset-length pairs */
            err = merge_requests(ncp, g_num_reqs, g_reqs, &merged_buf, &nsegs,
                                 &segs);
            if (status == NC_NOERR) status = err;
            if (err != NC_NOERR) { /* skip this group */
                ftypes[i] = btypes[i] = MPI_BYTE;
                b_blocklengths[i] = 0;
                f_blocklengths[i] = 0;
                if (segs != NULL) NCI_Free(segs);
                continue;
            }
            assert(nsegs > 0);

            /* sges[] will be used to construct fileview and buffer type */
            err = type_create_off_len(nsegs, segs, &ftypes[i], &btypes[i]);
            /* preserve the previous error if there is any */
            if (status == NC_NOERR) status = err;
            NCI_Free(segs);
            if (err != NC_NOERR) { /* skip this group */
                ftypes[i] = btypes[i] = MPI_BYTE;
                b_blocklengths[i] = 0;
                f_blocklengths[i] = 0;
                continue;
            }
            f_blocklengths[i] = 1;
        }

        if (i > 0) {
            /* get the buffer address of the first request in this group */
#ifdef HAVE_MPI_GET_ADDRESS
            MPI_Get_address(g_reqs[0].xbuf, &b_addr);
#else
            MPI_Address(g_reqs[0].xbuf, &b_addr);
#endif
            b_disps[i] = b_addr - b_begin; /* to 1st buffer of 1st group*/
        }
        b_blocklengths[i] = 1;
    }
    NCI_Free(group_index);
    NCI_Free(group_type);

    buf_len=1;

    if (ngroups == 1) {
        /* use ftypes[0] and btypes[0] directly */
        filetype = ftypes[0];
        buf_type = btypes[0];
    }
    else {
        /* concatenate all ftypes[] to filetype */
#ifdef HAVE_MPI_TYPE_CREATE_STRUCT
        mpireturn = MPI_Type_create_struct(ngroups, f_blocklengths, f_disps,
                                           ftypes, &filetype);
#else
        mpireturn = MPI_Type_struct(ngroups, f_blocklengths, f_disps, ftypes,
                                    &filetype);
#endif
        if (mpireturn != MPI_SUCCESS) {
            err = ncmpii_error_mpi2nc(mpireturn, "MPI_Type_create_struct");
            /* return the first encountered error if there is any */
            if (status == NC_NOERR) status = err;

            buf_len  = 0; /* skip this request */
            filetype = MPI_BYTE;
        }
        else
            MPI_Type_commit(&filetype);

        for (i=0; i<ngroups; i++) {
            if (ftypes[i] != MPI_BYTE) MPI_Type_free(&ftypes[i]);
        }

        /* concatenate all btypes[] to buf_type */
#ifdef HAVE_MPI_TYPE_CREATE_STRUCT
        mpireturn = MPI_Type_create_struct(ngroups, b_blocklengths, b_disps,
                                           btypes, &buf_type);
#else
        mpireturn = MPI_Type_struct(ngroups, b_blocklengths, b_disps, btypes,
                                    &buf_type);
#endif
        if (mpireturn != MPI_SUCCESS) {
            err = ncmpii_error_mpi2nc(mpireturn, "MPI_Type_create_struct");
            /* return the first encountered error if there is any */
            if (status == NC_NOERR) status = err;

            buf_len  = 0; /* skip this request */
            buf_type = MPI_BYTE;
        }
        else
            MPI_Type_commit(&buf_type);

        for (i=0; i<ngroups; i++) {
            if (btypes[i] != MPI_BYTE) MPI_Type_free(&btypes[i]);
        }
    }

    MPI_Type_size(buf_type, &buf_type_size);

    if (coll_indep == NC_REQ_COLL)
        fh = ncp->collective_fh;
    else
        fh = ncp->independent_fh;

    /* set the file view */
    MPI_Offset offset=0;
    err = ncmpio_file_set_view(ncp, fh, &offset, filetype);
    if (err != NC_NOERR) {
        buf_len = 0; /* skip this request */
        if (status == NC_NOERR) status = err;
    }

#ifdef _USE_MPI_GET_COUNT
        /* explicitly initialize mpistatus object to 0, see comments below */
        memset(&mpistatus, 0, sizeof(MPI_Status));
#endif

    if (rw_flag == NC_REQ_RD) {
        if (coll_indep == NC_REQ_COLL) {
            TRACE_IO(MPI_File_read_at_all)(fh, offset, buf, buf_len, buf_type,
                                           &mpistatus);
            if (mpireturn != MPI_SUCCESS) {
                err = ncmpii_error_mpi2nc(mpireturn, "MPI_File_read_at_all");
                /* return the first encountered error if there is any */
                if (status == NC_NOERR) {
                    err = (err == NC_EFILE) ? NC_EREAD : err;
                    DEBUG_ASSIGN_ERROR(status, err)
                }
            }
        } else {
            TRACE_IO(MPI_File_read_at)(fh, offset, buf, buf_len, buf_type,
                                       &mpistatus);
            if (mpireturn != MPI_SUCCESS) {
                err = ncmpii_error_mpi2nc(mpireturn, "MPI_File_read_at");
                /* return the first encountered error if there is any */
                if (status == NC_NOERR) {
                    err = (err == NC_EFILE) ? NC_EREAD : err;
                    DEBUG_ASSIGN_ERROR(status, err)
                }
            }
        }
        if (mpireturn == MPI_SUCCESS) {
            /* update the number of bytes read since file open */
#ifdef _USE_MPI_GET_COUNT
            int get_size;
            MPI_Get_count(&mpistatus, MPI_BYTE, &get_size);
            ncp->get_size += get_size;
#else
            ncp->get_size += buf_len * buf_type_size;
#endif
        }
    } else { /* NC_REQ_WR */
        if (coll_indep == NC_REQ_COLL) {
            TRACE_IO(MPI_File_write_at_all)(fh, offset, buf, buf_len, buf_type,
                                            &mpistatus);
            if (mpireturn != MPI_SUCCESS) {
                err = ncmpii_error_mpi2nc(mpireturn, "MPI_File_write_at_all");
                /* return the first encountered error if there is any */
                if (status == NC_NOERR) {
                    err = (err == NC_EFILE) ? NC_EWRITE : err;
                    DEBUG_ASSIGN_ERROR(status, err)
                }
            }
        } else {
            TRACE_IO(MPI_File_write_at)(fh, offset, buf, buf_len, buf_type,
                                        &mpistatus);
            if (mpireturn != MPI_SUCCESS) {
                err = ncmpii_error_mpi2nc(mpireturn, "MPI_File_write_at");
                /* return the first encountered error if there is any */
                if (status == NC_NOERR) {
                    err = (err == NC_EFILE) ? NC_EWRITE : err;
                    DEBUG_ASSIGN_ERROR(status, err)
                }
            }
        }
        if (mpireturn == MPI_SUCCESS) {
#ifdef _USE_MPI_GET_COUNT
            int put_size;
            MPI_Get_count(&mpistatus, MPI_BYTE, &put_size);
            ncp->put_size += put_size;
#else
            ncp->put_size += buf_len * buf_type_size;
#endif
        }
    }

    if (filetype != MPI_BYTE) MPI_Type_free(&filetype);
    if (buf_type != MPI_BYTE) MPI_Type_free(&buf_type);

    /* No longer need to reset the file view, as the root's fileview includes
     * the whole file header.
     TRACE_IO(MPI_File_set_view)(fh, 0, MPI_BYTE, MPI_BYTE, "native",
                                 MPI_INFO_NULL);
     */

    NCI_Free(ftypes);
    NCI_Free(f_blocklengths);
    NCI_Free(f_disps);

    return status;
}

/*----< wait_getput() >------------------------------------------------------*/
static int
wait_getput(NC         *ncp,
            int         num_reqs,   /* # requests */
            NC_req     *reqs,       /* array of requests */
            int         rw_flag,    /* NC_REQ_WR or NC_REQ_RD */
            int         coll_indep, /* NC_REQ_COLL or NC_REQ_INDEP */
            MPI_Offset  newnumrecs) /* new number of records */
{
    int i, err, status=NC_NOERR, access_interleaved=0;

    /* move the offset calculation from posting API calls (pack_request) to
     * wait call, such that posting a nonblocking request can be made in
     * define mode
     */  
    for (i=0; i<num_reqs; i++) {
        /* get the starting file offset for this request */
        ncmpio_last_offset(ncp, reqs[i].varp, reqs[i].start, NULL, NULL,
                           rw_flag, &reqs[i].offset_start);

        /* get the ending file offset for this request */
        ncmpio_last_offset(ncp, reqs[i].varp, reqs[i].start, reqs[i].count,
                           reqs[i].stride, rw_flag, &reqs[i].offset_end);
        reqs[i].offset_end += reqs[i].varp->xsz - 1;
    }

    /* check if reqs[].offset_start are in an increasing order */
    for (i=1; i<num_reqs; i++) {
        if (reqs[i-1].offset_start > reqs[i].offset_start) {
            break;
        }
    }
    if (i < num_reqs) /* a non-increasing order is found */
        /* sort reqs[] based on reqs[].offset_start */
        qsort(reqs, (size_t)num_reqs, sizeof(NC_req), req_compare);

    /* check for any interleaved requests */
    for (i=1; i<num_reqs; i++) {
        if (reqs[i-1].offset_end > reqs[i].offset_start) {
            access_interleaved = 1;
            break;
        }
    }

    /* aggregate requests and carry out the I/O */
    err = req_aggregation(ncp, num_reqs, reqs, rw_flag, coll_indep,
                          access_interleaved);
    if (status == NC_NOERR) status = err;

    /* Update the number of records if new records have been created.
     * For nonblocking APIs, there is no way for a process to know whether
     * others write to a record variable or not. Note newnumrecs has been
     * sync-ed and always >= ncp->numrecs.
     */
    if (rw_flag == NC_REQ_WR) {
        if (coll_indep == NC_REQ_COLL) {
            if (newnumrecs > ncp->numrecs) {
                /* update new record number in file. Note newnumrecs is already
		 * sync-ed among all processes and in collective mode
		 * ncp->numrecs is always sync-ed in memory among processes,
		 * thus no need another MPI_Allreduce to sync it. */
                err = ncmpio_write_numrecs(ncp, newnumrecs);
                if (status == NC_NOERR) status = err;
                /* retain the first error if there is any */
                if (ncp->numrecs < newnumrecs) ncp->numrecs = newnumrecs;
            }
        }
        else { /* NC_REQ_INDEP */
            if (ncp->numrecs < newnumrecs) {
                ncp->numrecs = newnumrecs;
                set_NC_ndirty(ncp);
                /* delay numrecs sync until end_indep, redef or close */
            }
        }

        if (NC_doFsync(ncp)) { /* NC_SHARE is set */
            int mpireturn;
            if (coll_indep == NC_REQ_INDEP) {
                TRACE_IO(MPI_File_sync)(ncp->independent_fh);
                if (mpireturn != MPI_SUCCESS) {
                    err = ncmpii_error_mpi2nc(mpireturn, "MPI_File_sync"); 
                    if (status == NC_NOERR) status = err;
                }
            }
            else {
                TRACE_IO(MPI_File_sync)(ncp->collective_fh);
                if (mpireturn != MPI_SUCCESS) {
                    err = ncmpii_error_mpi2nc(mpireturn, "MPI_File_sync"); 
                    if (status == NC_NOERR) status = err;
                }
                TRACE_COMM(MPI_Barrier)(ncp->comm);
            }
        }
    }

    return status;
}

/*----< mgetput() >----------------------------------------------------------*/
/* Before reaching to this subroutine, all the filetypes in the request array
 * are sorted in a non-decreasing order and not interleaved. This subroutine
 * concatenates the filetypes into a single fileview and calls MPI-IO function.
 * This subroutine also concatenates user buffertypes into a single derived
 * data type to be used in the MPI read/write function call.
 */
static int
mgetput(NC     *ncp,
        int     num_reqs,
        NC_req *reqs,        /* [num_reqs] */
        int     rw_flag,     /* NC_REQ_WR or NC_REQ_RD */
        int     coll_indep)  /* NC_REQ_COLL or NC_REQ_INDEP */
{
    int i, j, len=0, status=NC_NOERR, mpireturn, err, buf_type_size=0;
    void *buf=NULL;
    MPI_Status mpistatus;
    MPI_Datatype filetype, buf_type=MPI_BYTE;
    MPI_File fh;
    MPI_Offset offset=0;

    if (coll_indep == NC_REQ_COLL)
        fh = ncp->collective_fh;
    else
        fh = ncp->independent_fh;

    /* construct a MPI file type by concatenating fileviews of all requests */
    status = construct_filetypes(ncp,num_reqs, reqs, rw_flag, &filetype);
    if (status != NC_NOERR) { /* if failed, skip this request */
        if (coll_indep == NC_REQ_INDEP) return status;

        /* For collective I/O, we still need to participate the successive
           collective calls: setview/read/write */
        num_reqs = 0;
        filetype = MPI_BYTE;
    }

    /* set the MPI-IO fileview */
    offset=0;
    err = ncmpio_file_set_view(ncp, fh, &offset, filetype);
    if (err != NC_NOERR) {
        if (coll_indep == NC_REQ_INDEP) return status;
        num_reqs = 0; /* skip this request */
        if (status == NC_NOERR) status = err;
    }

    if (filetype != MPI_BYTE) MPI_Type_free(&filetype);

    /* now construct buffer datatype */
    if (num_reqs == 0) {
        /* num_reqs == 0, simply participate the collective call */
        buf = NULL;
        len = 0;
    }
    else if (num_reqs == 1) {
        MPI_Offset int8 = reqs[0].bnelems * reqs[0].varp->xsz;
        len = (int)int8;
        if (int8 != len) {
            if (status == NC_NOERR) DEBUG_ASSIGN_ERROR(status, NC_EINTOVERFLOW)
            len = 0; /* skip this request */
        }
        buf = reqs[0].xbuf;
    }
    else if (num_reqs > 1) { /* create the I/O buffer derived data type */
        int *blocklengths, last_contig_req;
        MPI_Aint *disps, a0=0, ai, a_last_contig;

        blocklengths = (int*) NCI_Malloc((size_t)num_reqs * SIZEOF_INT);
        disps = (MPI_Aint*) NCI_Malloc((size_t)num_reqs * SIZEOF_MPI_AINT);

        last_contig_req = 0; /* index of the last contiguous request */
        buf = NULL;
        /* process only valid requests */
        for (i=0, j=0; i<num_reqs; i++) {
            /* check int overflow */
            MPI_Offset int8 = reqs[i].bnelems * reqs[i].varp->xsz;
            blocklengths[j] = (int)int8;
            if (int8 != blocklengths[j]) { /* int overflow */
                if (status == NC_NOERR) /* keep the 1st encountered error */
                    DEBUG_ASSIGN_ERROR(status, NC_EINTOVERFLOW)
                blocklengths[j] = 0;
                continue; /* skip this request */
            }
#ifdef HAVE_MPI_GET_ADDRESS
            MPI_Get_address(reqs[i].xbuf, &ai);
#else
            MPI_Address(reqs[i].xbuf, &ai);
#endif
            if (j == 0) { /* first valid request */
                a_last_contig = a0 = ai;
                buf = reqs[i].xbuf;
            }
            disps[j] = ai - a0;

            if (ai - a_last_contig == blocklengths[last_contig_req])
                /* user buffer of request j is contiguous from j-1
                 * we coalesce j to j-1 */
                blocklengths[last_contig_req] += blocklengths[j];
            else if (j > 0) {
                /* not contiguous from request last_contig_req */
                last_contig_req++;
                a_last_contig = ai;
                disps[last_contig_req] = ai - a0;
                blocklengths[last_contig_req] = blocklengths[i];
            }
            j++;
        }

        /* last_contig_req is the index of last contiguous request */
        if (last_contig_req == 0) {
            /* user buffers can be concatenated into a contiguous buffer */
            buf_type = MPI_BYTE;
            len = blocklengths[0];
        }
        else {
            /* after possible concatenating the user buffers, the true number
             * of non-contiguous buffers is last_contig_req+1 */
            num_reqs = last_contig_req+1;

            /* concatenate buffer addresses into a single buffer type */
#ifdef HAVE_MPI_TYPE_CREATE_HINDEXED
            mpireturn = MPI_Type_create_hindexed(num_reqs, blocklengths, disps,
                                                 MPI_BYTE, &buf_type);
#else
            mpireturn = MPI_Type_hindexed(num_reqs, blocklengths, disps,
                                          MPI_BYTE, &buf_type);
#endif
            if (mpireturn != MPI_SUCCESS) {
                err = ncmpii_error_mpi2nc(mpireturn,"MPI_Type_create_hindexed");
                /* return the first encountered error if there is any */
                if (status == NC_NOERR) status = err;
            }
            else {
                mpireturn = MPI_Type_commit(&buf_type);
                if (mpireturn != MPI_SUCCESS) {
                    err = ncmpii_error_mpi2nc(mpireturn,"MPI_Type_commit");
                    /* return the first encountered error if there is any */
                    if (status == NC_NOERR) status = err;
                }
            }

            len = 1;
        }
        NCI_Free(disps);
        NCI_Free(blocklengths);
    }
    /* if (buf_type == MPI_BYTE) then the whole buf is contiguous */

    MPI_Type_size(buf_type, &buf_type_size);

#ifdef _USE_MPI_GET_COUNT
        /* explicitly initialize mpistatus object to 0, see comments below */
        memset(&mpistatus, 0, sizeof(MPI_Status));
#endif

    if (rw_flag == NC_REQ_RD) {
        if (coll_indep == NC_REQ_COLL) {
            TRACE_IO(MPI_File_read_at_all)(fh, offset, buf, len, buf_type,
                                           &mpistatus);
            if (mpireturn != MPI_SUCCESS) {
                err = ncmpii_error_mpi2nc(mpireturn, "MPI_File_read_at_all");
                /* return the first encountered error if there is any */
                if (status == NC_NOERR) {
                    err = (err == NC_EFILE) ? NC_EREAD : err;
                    DEBUG_ASSIGN_ERROR(status, err)
                }
            }
        } else {
            TRACE_IO(MPI_File_read_at)(fh, offset, buf, len, buf_type,
                                       &mpistatus);
            if (mpireturn != MPI_SUCCESS) {
                err = ncmpii_error_mpi2nc(mpireturn, "MPI_File_read_at");
                /* return the first encountered error if there is any */
                if (status == NC_NOERR) {
                    err = (err == NC_EFILE) ? NC_EREAD : err;
                    DEBUG_ASSIGN_ERROR(status, err)
                }
            }
        }
        if (mpireturn == MPI_SUCCESS) {
            /* update the number of bytes read since file open */
#ifdef _USE_MPI_GET_COUNT
            int get_size;
            MPI_Get_count(&mpistatus, MPI_BYTE, &get_size);
            ncp->get_size += get_size;
#else
            ncp->get_size += len * buf_type_size;
#endif
        }
    } else { /* NC_REQ_WR */
        if (coll_indep == NC_REQ_COLL) {
            TRACE_IO(MPI_File_write_at_all)(fh, offset, buf, len, buf_type,
                                            &mpistatus);
            if (mpireturn != MPI_SUCCESS) {
                err = ncmpii_error_mpi2nc(mpireturn, "MPI_File_write_at_all");
                /* return the first encountered error if there is any */
                if (status == NC_NOERR) {
                    err = (err == NC_EFILE) ? NC_EWRITE : err;
                    DEBUG_ASSIGN_ERROR(status, err)
                }
            }
        } else {
            TRACE_IO(MPI_File_write_at)(fh, offset, buf, len, buf_type,
                                        &mpistatus);
            if (mpireturn != MPI_SUCCESS) {
                err = ncmpii_error_mpi2nc(mpireturn, "MPI_File_write_at");
                /* return the first encountered error if there is any */
                if (status == NC_NOERR) {
                    err = (err == NC_EFILE) ? NC_EWRITE : err;
                    DEBUG_ASSIGN_ERROR(status, err)
                }
            }
        }
        if (mpireturn == MPI_SUCCESS) {
            /* update the number of bytes written since file open */
#ifdef _USE_MPI_GET_COUNT
            int put_size;
            MPI_Get_count(&mpistatus, MPI_BYTE, &put_size);
            ncp->put_size += put_size;
#else
            ncp->put_size += len * buf_type_size;
#endif
        }
    }

    if (buf_type != MPI_BYTE) /* free user buffer type */
        mpireturn = MPI_Type_free(&buf_type);

    /* No longer need to reset the file view, as the root's fileview includes
     * the whole file header.
     TRACE_IO(MPI_File_set_view)(fh, 0, MPI_BYTE, MPI_BYTE, "native",
                                 MPI_INFO_NULL);
     */

    return status;
}
