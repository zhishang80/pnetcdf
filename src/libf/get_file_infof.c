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
#define nfmpi_get_file_info_ NFMPI_GET_FILE_INFO
#elif defined(F77_NAME_LOWER_2USCORE)
#define nfmpi_get_file_info_ nfmpi_get_file_info__
#elif !defined(F77_NAME_LOWER_USCORE)
#define nfmpi_get_file_info_ nfmpi_get_file_info
/* Else leave name alone */
#endif


/* Prototypes for the Fortran interfaces */
#include "mpifnetcdf.h"
FORTRAN_API int FORT_CALL nfmpi_get_file_info_ ( MPI_Fint *v1, MPI_Fint *v2 ){
    int ierr;
    MPI_Info info;

    ierr = ncmpi_get_file_info( *v1, &info );
    *v2 = MPI_Info_c2f(info);
    return ierr;
}
