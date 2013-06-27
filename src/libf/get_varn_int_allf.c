/* -*- Mode: C; c-basic-offset:4 ; -*- */
/*  
 *  (C) 2003 by Argonne National Laboratory and Northwestern University.
 *      See COPYRIGHT in top-level directory.
 *
 * This file is automatically generated by ./buildiface -infile=../lib/pnetcdf.h -deffile=defs
 * DO NOT EDIT
 */
#include "mpinetcdf_impl.h"


#ifdef F77_NAME_UPPER
#define nfmpi_get_varn_int_all_ NFMPI_GET_VARN_INT_ALL
#elif defined(F77_NAME_LOWER_2USCORE)
#define nfmpi_get_varn_int_all_ nfmpi_get_varn_int_all__
#elif !defined(F77_NAME_LOWER_USCORE)
#define nfmpi_get_varn_int_all_ nfmpi_get_varn_int_all
/* Else leave name alone */
#endif


/* Prototypes for the Fortran interfaces */
#include "mpifnetcdf.h"
FORTRAN_API int FORT_CALL nfmpi_get_varn_int_all_ ( int *v1, int *v2, int *v3, MPI_Fint *v4 ){
    int ierr;
    int l2 = *v2 - 1;
    ierr = ncmpi_get_varn_int_all( *v1, l2, *v3, v4 );
    return ierr;
}
