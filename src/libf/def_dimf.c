/* -*- Mode: C; c-basic-offset:4 ; -*- */
/*  
 *  (C) 2001 by Argonne National Laboratory.
 *      See COPYRIGHT in top-level directory.
 *
 * This file is automatically generated by buildiface -infile=../lib/pnetcdf.h -deffile=defs
 * DO NOT EDIT
 */
#include "mpinetcdf_impl.h"


#ifdef F77_NAME_UPPER
#define nfmpi_def_dim_ NFMPI_DEF_DIM
#elif defined(F77_NAME_LOWER_2USCORE)
#define nfmpi_def_dim_ nfmpi_def_dim__
#elif !defined(F77_NAME_LOWER_USCORE)
#define nfmpi_def_dim_ nfmpi_def_dim
/* Else leave name alone */
#endif


/* Prototypes for the Fortran interfaces */
#include "mpifnetcdf.h"
FORTRAN_API void FORT_CALL nfmpi_def_dim_ ( int *v1, char *v2 FORT_MIXED_LEN(d2), int *v3, MPI_Fint *v4, MPI_Fint *ierr FORT_END_LEN(d2) ){
    char *p2;
    size_t l3 = (size_t)*v3;

    {char *p = v2 + d2 - 1;
     int  li;
        while (*p == ' ' && p > v2) p--;
        p++;
        p2 = (char *)malloc( p-v2 + 1 );
        for (li=0; li<(p-v2); li++) { p2[li] = v2[li]; }
        p2[li] = 0; 
    }
    *ierr = ncmpi_def_dim( *v1, p2, l3, v4 );
    free( p2 );

    *v4 = *v4 + 1;
}
