/* -*- mode: C -*-  */
/* 
   IGraph library.
   Copyright (C) 2009  Gabor Csardi <csardi@rmki.kfki.hu>
   MTA RMKI, Konkoly-Thege Miklos st. 29-33, Budapest 1121, Hungary
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc.,  51 Franklin Street, Fifth Floor, Boston, MA 
   02110-1301 USA

*/

#ifndef IGRAPH_SPMATRIX_H
#define IGRAPH_SPMATRIX_H

#undef __BEGIN_DECLS
#undef __END_DECLS
#ifdef __cplusplus
# define __BEGIN_DECLS extern "C" {
# define __END_DECLS }
#else
# define __BEGIN_DECLS /* empty */
# define __END_DECLS /* empty */
#endif

#include "igraph_vector.h"

__BEGIN_DECLS

/* -------------------------------------------------- */
/* Sparse matrix                                      */
/* -------------------------------------------------- */

/** 
 * \section about_igraph_spmatrix_t_objects About \type igraph_spmatrix_t objects
 * 
 * <para>The \type igraph_spmatrix_t type stores a sparse matrix with the
 * assumption that the number of nonzero elements in the matrix scales
 * linearly with the row or column count of the matrix (so most of the
 * elements are zero). Of course it can store an arbitrary real matrix,
 * but if most of the elements are nonzero, one should use \type igraph_matrix_t
 * instead.</para>
 *
 * <para>The elements are stored in column compressed format, so the elements
 * in the same column are stored adjacent in the computer's memory. The storage
 * requirement for a sparse matrix is O(n) where n is the number of nonzero
 * elements. Actually it can be a bit larger, see the documentation of
 * the vector type for an explanation.</para>
 */
typedef struct s_spmatrix {
  igraph_vector_t ridx, cidx, data;
  long int nrow, ncol;
} igraph_spmatrix_t;

#define IGRAPH_SPMATRIX_INIT_FINALLY(m, nr, nc) \
  do { IGRAPH_CHECK(igraph_spmatrix_init(m, nr, nc)); \
  IGRAPH_FINALLY(igraph_spmatrix_destroy, m); } while (0)

int igraph_spmatrix_init(igraph_spmatrix_t *m, long int nrow, long int ncol);
void igraph_spmatrix_destroy(igraph_spmatrix_t *m);
int igraph_spmatrix_resize(igraph_spmatrix_t *m, long int nrow, long int ncol);
igraph_real_t igraph_spmatrix_e(const igraph_spmatrix_t *m, long int row, long int col);
int igraph_spmatrix_set(igraph_spmatrix_t *m, long int row, long int col,
                        igraph_real_t value);
int igraph_spmatrix_add_e(igraph_spmatrix_t *m, long int row, long int col,
                        igraph_real_t value);
int igraph_spmatrix_add_col_values(igraph_spmatrix_t *m, long int to, long int from);
long int igraph_spmatrix_count_nonzero(const igraph_spmatrix_t *m);
long int igraph_spmatrix_size(const igraph_spmatrix_t *m);
long int igraph_spmatrix_nrow(const igraph_spmatrix_t *m);
long int igraph_spmatrix_ncol(const igraph_spmatrix_t *m);
int igraph_spmatrix_copy_to(const igraph_spmatrix_t *m, igraph_real_t *to);
int igraph_spmatrix_null(igraph_spmatrix_t *m);
int igraph_spmatrix_add_cols(igraph_spmatrix_t *m, long int n);
int igraph_spmatrix_add_rows(igraph_spmatrix_t *m, long int n);
int igraph_spmatrix_clear_col(igraph_spmatrix_t *m, long int col);
int igraph_spmatrix_clear_row(igraph_spmatrix_t *m, long int row);
int igraph_spmatrix_copy(igraph_spmatrix_t *to, const igraph_spmatrix_t *from);
igraph_real_t igraph_spmatrix_max_nonzero(const igraph_spmatrix_t *m,
    igraph_real_t *ridx, igraph_real_t *cidx);
igraph_real_t igraph_spmatrix_max(const igraph_spmatrix_t *m,
    igraph_real_t *ridx, igraph_real_t *cidx);
void igraph_spmatrix_scale(igraph_spmatrix_t *m, igraph_real_t by);
int igraph_spmatrix_colsums(const igraph_spmatrix_t *m, igraph_vector_t *res);

int igraph_i_spmatrix_get_col_nonzero_indices(const igraph_spmatrix_t *m,
    igraph_vector_t *res, long int col);
int igraph_i_spmatrix_clear_row_fast(igraph_spmatrix_t *m, long int row);
int igraph_i_spmatrix_cleanup(igraph_spmatrix_t *m);

__END_DECLS

#endif