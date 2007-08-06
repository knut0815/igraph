/* -*- mode: C -*-  */
/* 
   IGraph library.
   Copyright (C) 2007  Gabor Csardi <csardi@rmki.kfki.hu>
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
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 
   02110-1301 USA

*/

#include "igraph.h"
#include "memory.h"

/* #define IGRAPH_FASTCOMM_DEBUG */

#ifdef _MSC_VER
/* MSVC does not support variadic macros */
void debug(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
#ifdef IGRAPH_FASTCOMM_DEBUG
    vfprintf(stderr, fmt, args);
#endif
    va_end(args);
}
#else
#  ifdef IGRAPH_FASTCOMM_DEBUG
#    define debug(...) fprintf(stderr, __VA_ARGS__)
#  else
#    define debug(...) 
#  endif
#endif

/*
 * Implementation of the community structure algorithm originally published
 * by Clauset et al in:
 *
 * A. Clauset, M.E.J. Newman and C. Moore, "Finding community structure in
 * very large networks.". Phys. Rev. E 70, 066111 (2004).
 *
 * The data structures being used are slightly different and they are described
 * most closely in:
 *
 * K. Wakita, T. Tsurumi, "Finding community structure in mega-scale social
 * networks.". arXiv:cs/0702048v1.
 *
 * We maintain a vector of communities, each of which containing a list of
 * pointers to their neighboring communities along with the increase in the
 * modularity score that could be achieved by joining the two communities.
 * Each community has a pointer to one of its neighbors - the one which would
 * result in the highest increase in modularity after a join. The local
 * (community-level) maximums are also stored in an indexed max-heap. The
 * max-heap itself stores its elements in an array which satisfies the heap
 * property, but to allow us to access any of the elements in the array based
 * on the community index (and not based on the array index - which depends on
 * the element's actual position in the heap), we also maintain an index
 * vector in the heap: the ith element of the index vector contains the
 * position of community i in the array of the max-heap. When we perform
 * sifting operations on the heap to restore the heap property, we also maintain
 * the index vector.
 */

/* Structure storing a pair of communities along with their dQ values */
typedef struct s_igraph_i_fastgreedy_commpair {
  long int first;       /* first member of the community pair */
  long int second;      /* second member of the community pair */
  igraph_real_t *dq;    /* pointer to a member of the dq vector storing the */
                        /* increase in modularity achieved when joining */
  struct s_igraph_i_fastgreedy_commpair *opposite;
} igraph_i_fastgreedy_commpair;

/* Structure storing a community */
typedef struct {
  igraph_integer_t id;      /* Identifier of the community (for merges matrix) */
  igraph_integer_t size;    /* Size of the community */
  igraph_vector_ptr_t neis; /* references to neighboring communities */
  igraph_i_fastgreedy_commpair* maxdq; /* community pair with maximal dq */
} igraph_i_fastgreedy_community;

/* Global community list structure */
typedef struct {
  long int no_of_communities, n;  /* number of communities, number of vertices */
  igraph_i_fastgreedy_community* e;     /* list of communities */
  igraph_i_fastgreedy_community** heap; /* heap of communities */
  igraph_integer_t *heapindex; /* heap index to speed up lookup by community idx */
} igraph_i_fastgreedy_community_list;

/* Scans the community neighborhood list for the new maximal dq value.
 * Returns 1 if the maximum is different from the previous one,
 * 0 otherwise. */
int igraph_i_fastgreedy_community_rescan_max(
  igraph_i_fastgreedy_community* comm) {
  long int i, n;
  igraph_i_fastgreedy_commpair *p, *oldmax;

  n = igraph_vector_ptr_size(&comm->neis);
  if (n==0) {
    comm->maxdq = 0;
    return 1;
  }
  
  oldmax = comm->maxdq;
  comm->maxdq = (igraph_i_fastgreedy_commpair*)VECTOR(comm->neis)[0];
  for (i=1; i<n; i++) {
	p = (igraph_i_fastgreedy_commpair*)VECTOR(comm->neis)[i];
	if (*p->dq > *comm->maxdq->dq) comm->maxdq=p;
  }
  if (oldmax == comm->maxdq) return 0;
  return 1;
}

/* Destroys the global community list object */
void igraph_i_fastgreedy_community_list_destroy(
  igraph_i_fastgreedy_community_list* list) {
  long int i;
  for (i=0; i<list->n; i++) {
	igraph_vector_ptr_destroy(&list->e[i].neis);
  }
  free(list->e);
  if (list->heapindex != 0) free(list->heapindex);
  if (list->heap != 0) free(list->heap);
}

/* Community list heap maintenance: sift down */
void igraph_i_fastgreedy_community_list_sift_down(
  igraph_i_fastgreedy_community_list* list, long int idx) {
  long int root, child, c1, c2;
  igraph_i_fastgreedy_community* dummy;
  igraph_integer_t dummy2;

  root = idx;
  while (root*2+1 < list->no_of_communities) {
    child = root*2+1;
	if (child+1 < list->no_of_communities &&
		*list->heap[child]->maxdq->dq < *list->heap[child+1]->maxdq->dq)
	  child++;
	if (*list->heap[root]->maxdq->dq < *list->heap[child]->maxdq->dq) {
	  c1 = list->heap[root]->maxdq->first;
	  c2 = list->heap[child]->maxdq->first;

	  dummy = list->heap[root];
	  list->heap[root] = list->heap[child];
	  list->heap[child] = dummy;
	  
	  dummy2 = list->heapindex[c1];
	  list->heapindex[c1] = list->heapindex[c2];
	  list->heapindex[c2] = dummy2;

	  root = child;
	} else break;
  }
}

/* Community list heap maintenance: sift up */
void igraph_i_fastgreedy_community_list_sift_up(
  igraph_i_fastgreedy_community_list* list, long int idx) {
  long int root, parent, c1, c2;
  igraph_i_fastgreedy_community* dummy;
  igraph_integer_t dummy2;

  root = idx;
  while (root>0) {
    parent = (root-1)/2;
	if (*list->heap[parent]->maxdq->dq < *list->heap[root]->maxdq->dq) {
	  c1 = list->heap[root]->maxdq->first;
	  c2 = list->heap[parent]->maxdq->first;
	  
	  dummy = list->heap[parent];
	  list->heap[parent] = list->heap[root];
	  list->heap[root] = dummy;
	  
	  dummy2 = list->heapindex[c1];
	  list->heapindex[c1] = list->heapindex[c2];
	  list->heapindex[c2] = dummy2;

	  root = parent;
	} else break;
  }
}

/* Builds the community heap for the first time */
void igraph_i_fastgreedy_community_list_build_heap(
  igraph_i_fastgreedy_community_list* list) {
  long int i;
  for (i=list->no_of_communities/2-1; i>=0; i--)
	igraph_i_fastgreedy_community_list_sift_down(list, i);
}

/* Finds the element belonging to a given community in the heap and return its
 * index in the heap array */
#define igraph_i_fastgreedy_community_list_find_in_heap(list, idx) (list)->heapindex[idx]

/* Dumps the heap - for debugging purposes */
void igraph_i_fastgreedy_community_list_dump_heap(
  igraph_i_fastgreedy_community_list* list) {
  long int i;
  debug("Heap:\n");
  for (i=0; i<list->no_of_communities; i++) {
	debug("(%ld, %p, %p)", i, list->heap[i],
	  list->heap[i]->maxdq);
    if (list->heap[i]->maxdq)
	  debug(" (%ld, %ld, %.7f)", list->heap[i]->maxdq->first,
	    list->heap[i]->maxdq->second, *list->heap[i]->maxdq->dq);
	debug("\n");
  }
  debug("Heap index:\n");
  for (i=0; i<list->no_of_communities; i++) debug("%ld ", (long)list->heapindex[i]);
  debug("\nEND\n");
}

/* Checks if the community heap satisfies the heap property.
 * Only useful for debugging. */
void igraph_i_fastgreedy_community_list_check_heap(
  igraph_i_fastgreedy_community_list* list) {
  long int i;
  for (i=0; i<list->no_of_communities/2; i++) {
	if ((2*i+1<list->no_of_communities && *list->heap[i]->maxdq->dq < *list->heap[2*i+1]->maxdq->dq) ||
		(2*i+2<list->no_of_communities && *list->heap[i]->maxdq->dq < *list->heap[2*i+2]->maxdq->dq)) {
	  IGRAPH_WARNING("Heap property violated");
	  debug("Position: %ld, %ld and %ld\n", i, 2*i+1, 2*i+2);
	  igraph_i_fastgreedy_community_list_dump_heap(list);
	}
  }
}

/* Removes a given element from the heap */
void igraph_i_fastgreedy_community_list_remove(
  igraph_i_fastgreedy_community_list* list, long int idx) {
  igraph_real_t old;
  long int commidx;
  
  /* First adjust the index */
  commidx=list->heap[list->no_of_communities-1]->maxdq->first;
  list->heapindex[commidx] = idx;
  commidx=list->heap[idx]->maxdq->first;
  list->heapindex[commidx] = -1;

  /* Now remove the element */
  old=*list->heap[idx]->maxdq->dq;
  list->heap[idx] = list->heap[list->no_of_communities-1];
  list->no_of_communities--;
  
  /* Recover heap property */
  if (old > *list->heap[idx]->maxdq->dq)
	igraph_i_fastgreedy_community_list_sift_down(list, idx);
  else
	igraph_i_fastgreedy_community_list_sift_up(list, idx);
}

/* Removes a given element from the heap when there are no more neighbors
 * for it (comm->maxdq is NULL) */
void igraph_i_fastgreedy_community_list_remove2(
  igraph_i_fastgreedy_community_list* list, long int idx, long int comm) {
  long int i;
 
  if (idx == list->no_of_communities-1) {
    /* We removed the rightmost element on the bottom level, no problem,
     * there's nothing to be done */
    return;
  }

  /* First adjust the index */
  i=list->heap[list->no_of_communities-1]->maxdq->first;
  list->heapindex[i] = idx;
  list->heapindex[comm] = -1;

  /* Now remove the element */
  list->heap[idx] = list->heap[list->no_of_communities-1];
  list->no_of_communities--;
  
  /* Recover heap property */
  for (i=list->no_of_communities/2-1; i>=0; i--)
	igraph_i_fastgreedy_community_list_sift_down(list, i);
}

/* Removes the pair belonging to community k from the neighborhood list
 * of community c (that is, clist[c]) and recalculates maxdq */
void igraph_i_fastgreedy_community_remove_nei(
  igraph_i_fastgreedy_community_list* list, long int c, long int k) {
  long int i, n;
  igraph_bool_t rescan=0;
  igraph_i_fastgreedy_commpair *p;
  igraph_i_fastgreedy_community *comm;
  igraph_real_t olddq;

  comm=&list->e[c];
  n=igraph_vector_ptr_size(&comm->neis);
  for (i=0; i<n; i++) {
	p=(igraph_i_fastgreedy_commpair*)VECTOR(comm->neis)[i];
    if (p->second == k) {
	  /* Check current maxdq */
	  if (comm->maxdq == p) rescan=1;
	  break;
	}
  }
  if (i<n) {
	olddq = *comm->maxdq->dq;
	igraph_vector_ptr_remove(&comm->neis, i);
	if (rescan) {
	  igraph_i_fastgreedy_community_rescan_max(comm);
      i=igraph_i_fastgreedy_community_list_find_in_heap(list, c);
	  if (comm->maxdq) {
        if (*comm->maxdq->dq > olddq)
		  igraph_i_fastgreedy_community_list_sift_up(list, i);
        else
		  igraph_i_fastgreedy_community_list_sift_down(list, i);
      } else {
        /* no more neighbors for this community. we should remove this
         * community from the heap and restore the heap property */
        debug("REMOVING (NO MORE NEIS): %ld\n", i);
        igraph_i_fastgreedy_community_list_remove2(list, i, c);
      }
	}
  }
}

/* Updates the dq value of community pair p in the community with index p->first
 * of the community list clist to newdq and restores the heap property
 * in community c if necessary. Returns 1 if the maximum in the row had
 * to be updated, zero otherwise */
int igraph_i_fastgreedy_community_update_dq(
  igraph_i_fastgreedy_community_list* list,
  igraph_i_fastgreedy_commpair* p, igraph_real_t newdq) {
  long int i,j,n,to,from;
  igraph_real_t olddq;
  igraph_i_fastgreedy_community *comm_to, *comm_from;
  to=p->first; from=p->second;
  comm_to=&list->e[to];
  comm_from=&list->e[from];
  if (comm_to->maxdq == p && newdq >= *p->dq) {
	/* If we are adjusting the current maximum and it is increased, we don't
	 * have to re-scan for the new maximum */
	*p->dq = newdq;
	/* The maximum was increased, so perform a sift-up in the heap */
	i = igraph_i_fastgreedy_community_list_find_in_heap(list, to);
	igraph_i_fastgreedy_community_list_sift_up(list, i);
	/* Let's check the opposite side. If the pair was not the maximal in
	 * the opposite side (the other community list)... */
	if (comm_from->maxdq != p->opposite) {
	  if (*comm_from->maxdq->dq < newdq) {
	    /* ...and it will become the maximal, we need to adjust and sift up */
		comm_from->maxdq = p->opposite;
	    j = igraph_i_fastgreedy_community_list_find_in_heap(list, from);
	    igraph_i_fastgreedy_community_list_sift_up(list, j);
	  } else {
		/* The pair was not the maximal in the opposite side and it will
		 * NOT become the maximal, there's nothing to do there */
	  }
	} else {
	  /* The pair was maximal in the opposite side, so we need to sift it up
	   * with the new value */
	  j = igraph_i_fastgreedy_community_list_find_in_heap(list, from);
	  igraph_i_fastgreedy_community_list_sift_up(list, j);
	}
	return 1;
  } else if (comm_to->maxdq != p && (newdq <= *comm_to->maxdq->dq)) {
	/* If we are modifying an item which is not the current maximum, and the
	 * new value is less than the current maximum, we don't
	 * have to re-scan for the new maximum */
	olddq = *p->dq;
	*p->dq = newdq;
	/* However, if the item was the maximum on the opposite side, we'd better
	 * re-scan it */
	if (comm_from->maxdq == p->opposite) {
	  if (olddq>newdq) {
		/* Decreased the maximum on the other side, we have to re-scan for the
		 * new maximum */
		igraph_i_fastgreedy_community_rescan_max(comm_from);
  	    j = igraph_i_fastgreedy_community_list_find_in_heap(list, from);
	    igraph_i_fastgreedy_community_list_sift_down(list, j);
	  } else {
		/* Increased the maximum on the other side, we don't have to re-scan
		 * but we might have to sift up */
		j = igraph_i_fastgreedy_community_list_find_in_heap(list, from);
		igraph_i_fastgreedy_community_list_sift_up(list, j);
	  }
	}
	return 0;
  } else {
	/* We got here in two cases:
     (1) the pair we are modifying right now is the maximum in the given
	     community and we are decreasing it
	 (2) the pair we are modifying right now is NOT the maximum in the
	     given community, but we increase it so much that it will become
		 the new maximum
     */
    *p->dq = newdq;
    if (comm_to->maxdq != p) {
	  /* case (2) */
	  comm_to->maxdq = p;
	  /* The maximum was increased, so perform a sift-up in the heap */
	  i = igraph_i_fastgreedy_community_list_find_in_heap(list, to);
	  igraph_i_fastgreedy_community_list_sift_up(list, i);
	  /* Opposite side. Chances are that the new value became the maximum
	   * in the opposite side, but check it first */
	  if (comm_from->maxdq != p->opposite) {
		if (*comm_from->maxdq->dq < newdq) {
		  /* Yes, it will become the new maximum */
		  comm_from->maxdq = p->opposite;
  	      j = igraph_i_fastgreedy_community_list_find_in_heap(list, from);
	      igraph_i_fastgreedy_community_list_sift_up(list, j);
		} else {
		  /* No, nothing to do there */
		}
	  } else {
	    /* Already increased the maximum on the opposite side, so sift it up */
  	    j = igraph_i_fastgreedy_community_list_find_in_heap(list, from);
	    igraph_i_fastgreedy_community_list_sift_up(list, j);
	  }
    } else {
	  /* case (1) */
	  /* This is the worst, we have to re-scan the whole community to find
	   * the new maximum and update the global maximum as well if necessary */
      n = igraph_vector_ptr_size(&comm_to->neis);
      igraph_i_fastgreedy_community_rescan_max(comm_to);
	  /* The maximum was decreased, so perform a sift-down in the heap */
	  i = igraph_i_fastgreedy_community_list_find_in_heap(list, to);
	  igraph_i_fastgreedy_community_list_sift_down(list, i);
  	  if (comm_from->maxdq != p->opposite) {
		/* The one that we decreased on the opposite side is not the
		 * maximal one. Nothing to do. */
	  } else {
		/* We decreased the maximal on the opposite side as well. Re-scan
		 * and sift down */
		igraph_i_fastgreedy_community_rescan_max(comm_from);
	    j = igraph_i_fastgreedy_community_list_find_in_heap(list, from);
	    igraph_i_fastgreedy_community_list_sift_down(list, j);
      }
    }
  }
  return 1;
}

/* Auxiliary function to sort a community pair list with respect to the
 * `second` field */
int igraph_i_fastgreedy_commpair_cmp(const void* p1, const void* p2) {
  igraph_i_fastgreedy_commpair *cp1, *cp2;
  cp1=*(igraph_i_fastgreedy_commpair**)p1;
  cp2=*(igraph_i_fastgreedy_commpair**)p2;
  return cp1->second - cp2->second;
}

/**
 * \function igraph_community_fastgreedy
 * \brief Finding community structure by greedy optimization of modularity
 * 
 * This function implements the fast greedy modularity optimization
 * algorithm for finding community structure, see 
 * A Clauset, MEJ Newman, C Moore: Finding community structure in very
 * large networks, http://www.arxiv.org/abs/cond-mat/0408187 for the
 * details.
 *
 * </para><para>
 * Some improvements proposed in K Wakita, T Tsurumi: Finding community
 * structure in mega-scale social networks,
 * http://www.arxiv.org/abs/cs.CY/0702048v1 have also been implemented.
 *
 * \param graph The input graph.
 * \param merges Pointer to an initialized matrix or NULL, the result of the
 *    computation is stored here. The matrix has two columns and each
 *    merge corresponds to one merge, the ids of the two merged
 *    components are stored. The component ids are numbered from zero and 
 *    the first \c n components are the individual vertices, \c n is
 *    the number of vertices in the graph. Component \c n is created
 *    in the first merge, component \c n+1 in the second merge, etc.
 *    The matrix will be resized as needed. If this argument is NULL
 *    then it is ignored completely.
 * \param modularity Pointer to an initialized matrix or NULL pointer,
 *    in the former case the modularity scores along the stages of the
 *    computation are recorded here. The vector will be resized as
 *    needed.
 * \return Error code.
 *
 * \sa \ref igraph_community_walktrap(), \ref
 * igraph_community_edge_betweenness() for other community detection
 * algorithms, \ref igraph_community_to_membership() to convert the
 * dendrogram to a membership vector.
 *
 * Time complexity: O(|E||V|log|V|) in the worst case,
 * O(|E|+|V|log^2|V|) typically, |V| is the number of vertices, |E| is
 * the number of edges.
 */
int igraph_community_fastgreedy(const igraph_t *graph,
  igraph_matrix_t *merges, igraph_vector_t *modularity) {
  long int no_of_edges, no_of_nodes, no_of_joins, total_joins;
  long int i, j, k, n, m, from, to, dummy;
  igraph_integer_t ffrom, fto;
  igraph_eit_t edgeit;
  igraph_i_fastgreedy_commpair *pairs, *p1, *p2;
  igraph_i_fastgreedy_community_list communities;
  igraph_vector_t a;
  igraph_real_t q, maxq, *dq;

  /*long int join_order[] = { 16,5, 5,6, 6,0, 4,0, 10,0, 26,29, 29,33, 23,33, 27,33, 25,24, 24,31, 12,3, 21,1, 30,8, 8,32, 9,2, 17,1, 11,0, 7,3, 3,2, 13,2, 1,2, 28,31, 31,33, 22,32, 18,32, 20,32, 32,33, 15,33, 14,33, 0,19, 19,2, -1,-1 };*/
  /*long int join_order[] = { 43,42, 42,41, 44,41, 41,36, 35,36, 37,36, 36,29, 38,29, 34,29, 39,29, 33,29, 40,29, 32,29, 14,29, 30,29, 31,29, 6,18, 18,4, 23,4, 21,4, 19,4, 27,4, 20,4, 22,4, 26,4, 25,4, 24,4, 17,4, 0,13, 13,2, 1,2, 11,2, 8,2, 5,2, 3,2, 10,2, 9,2, 7,2, 2,28, 28,15, 12,15, 29,16, 4,15, -1,-1 };*/

  no_of_nodes = igraph_vcount(graph);
  no_of_edges = igraph_ecount(graph);
 
  if (igraph_is_directed(graph)) {
	IGRAPH_ERROR("fast greedy community detection works for undirected graphs only", IGRAPH_UNIMPLEMENTED);
  }
  
  total_joins=no_of_nodes-1;

  if (merges != 0) {
	IGRAPH_CHECK(igraph_matrix_resize(merges, total_joins, 2));
    IGRAPH_CHECK(igraph_matrix_null(merges));
  }
  if (modularity != 0) {
	IGRAPH_CHECK(igraph_vector_resize(modularity, total_joins+1));
  }

  /* Create degree vector */
  debug("Calculating degrees\n");
  IGRAPH_VECTOR_INIT_FINALLY(&a, no_of_nodes);
  IGRAPH_CHECK(igraph_degree(graph, &a, igraph_vss_all(), IGRAPH_ALL, 0));

  /* Create list of communities */
  debug("Creating community list\n");
  communities.n = no_of_nodes;
  communities.no_of_communities = no_of_nodes;
  communities.e = (igraph_i_fastgreedy_community*)calloc(no_of_nodes, sizeof(igraph_i_fastgreedy_community));
  if (communities.e == 0) {
	IGRAPH_ERROR("can't run fast greedy community detection", IGRAPH_ENOMEM);
  }
  IGRAPH_FINALLY(free, communities.e);
  communities.heap = (igraph_i_fastgreedy_community**)calloc(no_of_nodes, sizeof(igraph_i_fastgreedy_community*));
  if (communities.heap == 0) {
	IGRAPH_ERROR("can't run fast greedy community detection", IGRAPH_ENOMEM);
  }
  IGRAPH_FINALLY(free, communities.heap);
  communities.heapindex = (igraph_integer_t*)calloc(no_of_nodes, sizeof(igraph_integer_t));
  if (communities.heapindex == 0) {
	IGRAPH_ERROR("can't run fast greedy community detection", IGRAPH_ENOMEM);
  }
  IGRAPH_FINALLY_CLEAN(2);
  IGRAPH_FINALLY(igraph_i_fastgreedy_community_list_destroy, &communities);
  for (i=0; i<no_of_nodes; i++) {
    igraph_vector_ptr_init(&communities.e[i].neis, 0);
    communities.e[i].id = i;
    communities.e[i].size = 1;
  }

  /* Create list of community pairs from edges */
  debug("Allocating dq vector\n");
  dq = (igraph_real_t*)calloc(no_of_edges, sizeof(igraph_real_t));
  if (dq == 0) {
	IGRAPH_ERROR("can't run fast greedy community detection", IGRAPH_ENOMEM);
  }
  IGRAPH_FINALLY(free, dq);
  debug("Creating community pair list\n");
  IGRAPH_CHECK(igraph_eit_create(graph, igraph_ess_all(0), &edgeit));
  IGRAPH_FINALLY(igraph_eit_destroy, &edgeit);
  pairs = (igraph_i_fastgreedy_commpair*)calloc(2*no_of_edges, sizeof(igraph_i_fastgreedy_commpair));
  if (pairs == 0) {
	IGRAPH_ERROR("can't run fast greedy community detection", IGRAPH_ENOMEM);
  }
  IGRAPH_FINALLY(free, pairs);
  i=j=0;
  while (!IGRAPH_EIT_END(edgeit)) {
    igraph_edge(graph, IGRAPH_EIT_GET(edgeit), &ffrom, &fto);
    
	/* Create the pairs themselves */
	from = (long int)ffrom; to = (long int)fto;
	if (from == to) {
	  IGRAPH_ERROR("loop edge detected, simplify the graph before starting community detection", IGRAPH_EINVAL);
	}

	if (from>to) {
	  dummy=from; from=to; to=dummy;
	}
	dq[j]=2*(1.0/(no_of_edges*2.0) - VECTOR(a)[from]*VECTOR(a)[to]/(4.0*no_of_edges*no_of_edges));
	pairs[i].first = from;
	pairs[i].second = to;
    pairs[i].dq = &dq[j];
	pairs[i].opposite = &pairs[i+1];
	pairs[i+1].first = to;
	pairs[i+1].second = from;
	pairs[i+1].dq = pairs[i].dq;
	pairs[i+1].opposite = &pairs[i];
	/* Link the pair to the communities */
	igraph_vector_ptr_push_back(&communities.e[from].neis, &pairs[i]);
	igraph_vector_ptr_push_back(&communities.e[to].neis, &pairs[i+1]);
	/* Update maximums */
	if (communities.e[from].maxdq==0 || *communities.e[from].maxdq->dq < *pairs[i].dq)
	  communities.e[from].maxdq = &pairs[i];
	if (communities.e[to].maxdq==0 || *communities.e[to].maxdq->dq < *pairs[i+1].dq)
	  communities.e[to].maxdq = &pairs[i+1];

    /* Iterate */
	i+=2; j++;
    IGRAPH_EIT_NEXT(edgeit);
  }
  igraph_eit_destroy(&edgeit);
  IGRAPH_FINALLY_CLEAN(1);

  /* Sorting community neighbor lists by community IDs */
  debug("Sorting community neighbor lists\n");
  for (i=0, j=0; i<no_of_nodes; i++) {
	igraph_vector_ptr_sort(&communities.e[i].neis, igraph_i_fastgreedy_commpair_cmp);
    /* Isolated vertices won't be stored in the heap (to avoid maxdq == 0) */
    if (VECTOR(a)[i] > 0) {
	  communities.heap[j] = &communities.e[i];
      communities.heapindex[i] = j;
      j++;
    } else {
      communities.heapindex[i] = -1;
    }
  }
  communities.no_of_communities = j;

  /* Calculate proper vector a (see paper) and initial modularity */
  q=0;
  igraph_vector_multiply(&a, 1.0/(2.0*no_of_edges));
  for (i=0; i<no_of_nodes; i++)
	q -= VECTOR(a)[i]*VECTOR(a)[i];
  maxq=q;

  /* Initializing community heap */
  debug("Initializing community heap\n");
  igraph_i_fastgreedy_community_list_build_heap(&communities);

  debug("Initial modularity: %.4f\n", q);

  /* Let's rock ;) */
  no_of_joins=0;
  while (no_of_joins<total_joins) {
    IGRAPH_ALLOW_INTERRUPTION();
	igraph_progress("fast greedy community detection", no_of_joins*100.0/total_joins, 0);
    
	/* Store the modularity */
	if (modularity) VECTOR(*modularity)[no_of_joins] = q;
    
	/* Some debug info if needed */
	/* igraph_i_fastgreedy_community_list_check_heap(&communities); */
#ifdef DEBUG
	debug("===========================================\n");
	for (i=0; i<communities.n; i++) {
	  if (communities.e[i].maxdq == 0) {
	    debug("Community #%ld: PASSIVE\n", i);
	    continue;
	  }
      debug("Community #%ld\n ", i);
	  for (j=0; j<igraph_vector_ptr_size(&communities.e[i].neis); j++) {
	    p1=(igraph_i_fastgreedy_commpair*)VECTOR(communities.e[i].neis)[j];
	    debug(" (%ld,%ld,%.4f)", p1->first, p1->second, *p1->dq);
	  }
	  p1=communities.e[i].maxdq;
	  debug("\n  Maxdq: (%ld,%ld,%.4f)\n", p1->first, p1->second, *p1->dq);
    }
	debug("Global maxdq is: (%ld,%ld,%.4f)\n", communities.heap[0]->maxdq->first,
	    communities.heap[0]->maxdq->second, *communities.heap[0]->maxdq->dq);
    for (i=0; i<communities.no_of_communities; i++)
	  debug("(%ld,%ld,%.4f) ", communities.heap[i]->maxdq->first, communities.heap[i]->maxdq->second, *communities.heap[0]->maxdq->dq);
	debug("\n");
#endif
	if (communities.heap[0] == 0) break; /* no more communities */
	if (communities.heap[0]->maxdq == 0) break; /* there are only isolated comms */
    to=communities.heap[0]->maxdq->second;
	from=communities.heap[0]->maxdq->first;

	debug("Q[%ld] = %.7f\tdQ = %.7f\t |H| = %ld\n",
	  no_of_joins, q, *communities.heap[0]->maxdq->dq, no_of_nodes-no_of_joins-1);

	/* DEBUG */
	/* from=join_order[no_of_joins*2]; to=join_order[no_of_joins*2+1];
	if (to == -1) break;
    for (i=0; i<igraph_vector_ptr_size(&communities.e[to].neis); i++) {
      p1=(igraph_i_fastgreedy_commpair*)VECTOR(communities.e[to].neis)[i];
	  if (p1->second == from) communities.maxdq = p1;
	} */

	n = igraph_vector_ptr_size(&communities.e[to].neis);
	m = igraph_vector_ptr_size(&communities.e[from].neis);
	/*if (n>m) {
	  dummy=n; n=m; m=dummy;
	  dummy=to; to=from; from=dummy;
	}*/
	debug("  joining: %ld <- %ld\n", to, from);
    q += *communities.heap[0]->maxdq->dq; 
	
	/* Merge the second community into the first */
	i = j = 0;
	while (i<n && j<m) {
	  p1 = (igraph_i_fastgreedy_commpair*)VECTOR(communities.e[to].neis)[i];
	  p2 = (igraph_i_fastgreedy_commpair*)VECTOR(communities.e[from].neis)[j];
	  debug("Pairs: %ld-%ld and %ld-%ld\n", p1->first, p1->second,
		  p2->first, p2->second);
	  if (p1->second < p2->second) {
		/* Considering p1 from now on */
		debug("    Considering: %ld-%ld\n", p1->first, p1->second);
	    if (p1->second == from) {
		  debug("    WILL REMOVE: %ld-%ld\n", to, from);
	    } else {
		  /* chain, case 1 */
		  debug("    CHAIN(1): %ld-%ld %ld, now=%.7f, adding=%.7f, newdq(%ld,%ld)=%.7f\n",
		    to, p1->second, from, *p1->dq, -2*VECTOR(a)[from]*VECTOR(a)[p1->second], p1->first, p1->second, *p1->dq-2*VECTOR(a)[from]*VECTOR(a)[p1->second]);
		  igraph_i_fastgreedy_community_update_dq(&communities, p1, *p1->dq - 2*VECTOR(a)[from]*VECTOR(a)[p1->second]);
		}
		i++;
	  } else if (p1->second == p2->second) {
	    /* p1->first, p1->second and p2->first form a triangle */
		debug("    Considering: %ld-%ld and %ld-%ld\n", p1->first, p1->second,
		  p2->first, p2->second);
		/* Update dq value */
		debug("    TRIANGLE: %ld-%ld-%ld, now=%.7f, adding=%.7f, newdq(%ld,%ld)=%.7f\n",
		  to, p1->second, from, *p1->dq, *p2->dq, p1->first, p1->second, *p1->dq+*p2->dq);
		igraph_i_fastgreedy_community_update_dq(&communities, p1, *p1->dq + *p2->dq);
        igraph_i_fastgreedy_community_remove_nei(&communities, p1->second, from);
		i++;
		j++;
	  } else {
		debug("    Considering: %ld-%ld\n", p2->first, p2->second);
		if (p2->second == to) {
		  debug("    WILL REMOVE: %ld-%ld\n", p2->second, p2->first);
		} else {
		  /* chain, case 2 */
		  debug("    CHAIN(2): %ld %ld-%ld, newdq(%ld,%ld)=%.7f\n",
		    to, p2->second, from, to, p2->second, *p2->dq-2*VECTOR(a)[to]*VECTOR(a)[p2->second]);
		  p2->opposite->second=to;
	      /* need to re-sort community nei list `p2->second` */
	      /* TODO: quicksort is O(n*logn), although we could do a deletion and
	       * insertion which can be done in O(logn) if deletion is O(1) */
	      debug("    Re-sorting community %ld\n", p2->second);
	      igraph_vector_ptr_sort(&communities.e[p2->second].neis, igraph_i_fastgreedy_commpair_cmp);
		  /* link from.neis[j] to the current place in to.neis if
		   * from.neis[j] != to */
		  p2->first=to;
		  IGRAPH_CHECK(igraph_vector_ptr_insert(&communities.e[to].neis,i,p2));
		  n++; i++;
		  if (*p2->dq > *communities.e[to].maxdq->dq) {
		    communities.e[to].maxdq = p2;
            k=igraph_i_fastgreedy_community_list_find_in_heap(&communities, to);
		    igraph_i_fastgreedy_community_list_sift_up(&communities, k);
		  }
		  igraph_i_fastgreedy_community_update_dq(&communities, p2, *p2->dq - 2*VECTOR(a)[to]*VECTOR(a)[p2->second]);
		}
		j++;
	  }
	}

	while (i<n) {
	  p1 = (igraph_i_fastgreedy_commpair*)VECTOR(communities.e[to].neis)[i];
	  if (p1->second == from) {
	    debug("    WILL REMOVE: %ld-%ld\n", p1->first, from);
	  } else {
	    /* chain, case 1 */
	    debug("    CHAIN(1): %ld-%ld %ld, now=%.7f, adding=%.7f, newdq(%ld,%ld)=%.7f\n",
	      to, p1->second, from, *p1->dq, -2*VECTOR(a)[from]*VECTOR(a)[p1->second], p1->first, p1->second, *p1->dq-2*VECTOR(a)[from]*VECTOR(a)[p1->second]);
	    igraph_i_fastgreedy_community_update_dq(&communities, p1, *p1->dq - 2*VECTOR(a)[from]*VECTOR(a)[p1->second]);
	  }
	  i++;
	}
	while (j<m) {
	  p2 = (igraph_i_fastgreedy_commpair*)VECTOR(communities.e[from].neis)[j];
      if (to == p2->second) { j++; continue; }
	  /* chain, case 2 */
	  debug("    CHAIN(2): %ld %ld-%ld, newdq(%ld,%ld)=%.7f\n",
	    to, p2->second, from, p1->first, p2->second, *p2->dq-2*VECTOR(a)[to]*VECTOR(a)[p2->second]);
	  p2->opposite->second=to;
	  /* need to re-sort community nei list `p2->second` */
	  /* TODO: quicksort is O(n*logn), although we could do a deletion and
	   * insertion which can be done in O(logn) if deletion is O(1) */
	  debug("    Re-sorting community %ld\n", p2->second);
	  igraph_vector_ptr_sort(&communities.e[p2->second].neis, igraph_i_fastgreedy_commpair_cmp);
	  /* link from.neis[j] to the current place in to.neis if
	   * from.neis[j] != to */
	  p2->first=to;
	  IGRAPH_CHECK(igraph_vector_ptr_push_back(&communities.e[to].neis,p2));
	  if (*p2->dq > *communities.e[to].maxdq->dq) {
	    communities.e[to].maxdq = p2;
        k=igraph_i_fastgreedy_community_list_find_in_heap(&communities, to);
		igraph_i_fastgreedy_community_list_sift_up(&communities, k);
	  }
	  igraph_i_fastgreedy_community_update_dq(&communities, p2, *p2->dq-2*VECTOR(a)[to]*VECTOR(a)[p2->second]);
	  j++;
	}

	/* Now, remove community `from` from the neighbors of community `to` */
	if (communities.no_of_communities > 2) {
	  debug("    REMOVING: %ld-%ld\n", to, from);
	  igraph_i_fastgreedy_community_remove_nei(&communities, to, from);
	  i=igraph_i_fastgreedy_community_list_find_in_heap(&communities, from);
	  igraph_i_fastgreedy_community_list_remove(&communities, i);
    }
	communities.e[from].maxdq=0;

    /* Update community sizes */
    communities.e[to].size += communities.e[from].size;
    communities.e[from].size = 0;

	/* record what has been merged */
	igraph_vector_ptr_clear(&communities.e[from].neis);
	if (merges) {
	  MATRIX(*merges, no_of_joins, 0) = communities.e[to].id;
	  MATRIX(*merges, no_of_joins, 1) = communities.e[from].id;
	  communities.e[to].id = no_of_nodes+no_of_joins;
    }

	/* Update vector a */
	VECTOR(a)[to] += VECTOR(a)[from];
	VECTOR(a)[from] = 0.0;
	
	no_of_joins++;
  }
  /* TODO: continue merging when some isolated communities remained. Always
   * joining the communities with the least number of nodes results in the
   * smallest decrease in modularity every step. Now we're simply deleting
   * the excess rows from the merge matrix */
  if (no_of_joins < total_joins) {
    long int *ivec;
    ivec=Calloc(igraph_matrix_nrow(merges), long int);
    if (ivec == 0)
      IGRAPH_ERROR("can't run fast greedy community detection", IGRAPH_ENOMEM);
    IGRAPH_FINALLY(free, ivec);
    for (i=0; i<no_of_joins; i++) ivec[i] = i+1;
    igraph_matrix_permdelete_rows(merges, ivec, total_joins-no_of_joins);
    free(ivec);
    IGRAPH_FINALLY_CLEAN(1);
  }
  igraph_progress("fast greedy community detection", 100.0, 0);

  if (modularity) {
	VECTOR(*modularity)[no_of_joins] = q;
	igraph_vector_resize(modularity, no_of_joins+1);
  }

  debug("Freeing memory\n");
  free(pairs);
  free(dq);
  igraph_i_fastgreedy_community_list_destroy(&communities);
  igraph_vector_destroy(&a);
  IGRAPH_FINALLY_CLEAN(4);
  return 0;
}

#ifdef IGRAPH_FASTCOMM_DEBUG
#undef IGRAPH_FASTCOMM_DEBUG
#endif


