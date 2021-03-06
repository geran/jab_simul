/* --------------------------------------------------------------------------
 *
 * License
 *
 * The contents of this file are subject to the Jabber Open Source License
 * Version 1.0 (the "JOSL").  You may not copy or use this file, in either
 * source code or executable form, except in compliance with the JOSL. You
 * may obtain a copy of the JOSL at http://www.jabber.org/ or at
 * http://www.opensource.org/.  
 *
 * Software distributed under the JOSL is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied.  See the JOSL
 * for the specific language governing rights and limitations under the
 * JOSL.
 *
 * Copyrights
 * 
 * Portions created by or assigned to Jabber.com, Inc. are 
 * Copyright (c) 1999-2002 Jabber.com, Inc.  All Rights Reserved.  Contact
 * information for Jabber.com, Inc. is available at http://www.jabber.com/.
 *
 * Portions Copyright (c) 1998-1999 Jeremie Miller.
 * 
 * Acknowledgements
 * 
 * Special thanks to the Jabber Open Source Contributors for their
 * suggestions and support of Jabber.
 * 
 * Alternatively, the contents of this file may be used under the terms of the
 * GNU General Public License Version 2 or later (the "GPL"), in which case
 * the provisions of the GPL are applicable instead of those above.  If you
 * wish to allow use of your version of this file only under the terms of the
 * GPL and not to allow others to use your version of this file under the JOSL,
 * indicate your decision by deleting the provisions above and replace them
 * with the notice and other provisions required by the GPL.  If you do not
 * delete the provisions above, a recipient may use your version of this file
 * under either the JOSL or the GPL. 
 * 
 * 
 * --------------------------------------------------------------------------*/


#define FAST_POOL

#include "lib.h"


#ifdef POOL_DEBUG
int pool__total = 0;
int pool__ltotal = 0;
HASHTABLE pool__disturbed = NULL;
void *_pool__malloc(size_t size)
{
    pool__total++;
    return malloc(size);
}
void _pool__free(void *block)
{
    pool__total--;
    free(block);
}
#else
#define _pool__malloc malloc
#define _pool__free free
#endif


/* make an empty pool */
pool _pool_new(char *zone)
{
    pool p;
    while((p = _pool__malloc(sizeof(_pool))) == NULL) sleep(1);
    p->cleanup = NULL;
    p->heap = NULL;
    p->size = 0;

#ifdef POOL_DEBUG
    p->lsize = -1;
    p->zone[0] = '\0';
    strcat(p->zone,zone);
    sprintf(p->name,"%X",p);

    if(pool__disturbed == NULL)
    {
        pool__disturbed = 1; /* reentrancy flag! */
        pool__disturbed = ghash_create(POOL_DEBUG,(KEYHASHFUNC)str_hash_code,(KEYCOMPAREFUNC)j_strcmp);
    }
    if(pool__disturbed != 1)
        ghash_put(pool__disturbed,p->name,p);
#endif

    return p;
}

/* free a heap */
void _pool_heap_free(void *arg)
{
    struct pheap *h = (struct pheap *)arg;

#ifndef FAST_POOL
    _pool__free(h->block);
#endif
    _pool__free(h);
}

/* mem should always be freed last */
void _pool_cleanup_append(pool p, struct pfree *pf)
{
    struct pfree *cur;

    if(p->cleanup == NULL)
    {
        p->cleanup = pf;
        return;
    }

    /* fast forward to end of list */
    for(cur = p->cleanup; cur->next != NULL; cur = cur->next);

    cur->next = pf;
}

/* create a cleanup tracker */
struct pfree *_pool_free(pool p, pool_cleaner f, void *arg)
{
    struct pfree *ret;

    /* make the storage for the tracker */
    while((ret = _pool__malloc(sizeof(struct pfree))) == NULL) sleep(1);
    ret->f = f;
    ret->arg = arg;
    ret->next = NULL;    
    return ret;
}

#ifdef FAST_POOL
struct pfree *_pool_free_fast(struct pfree *ret, pool_cleaner f, void *arg)
{
    ret->f = f;
    ret->arg = arg;
    ret->next = NULL;
    ret->heap = arg;
    return ret;
}
#endif

/* create a heap and make sure it get's cleaned up */
struct pheap *_pool_heap(pool p, int size)
{
    struct pheap *ret;
    struct pfree *clean;


#ifdef FAST_POOL
    while((ret = _pool__malloc(sizeof(struct pheap)+
			       size+
			       sizeof(struct pfree))) == NULL) sleep(1);
    ret->block = (void*)ret + sizeof(struct pheap);
#else
    /* make the return heap */
    while((ret = _pool__malloc(sizeof(struct pheap))) == NULL) sleep(1);
    while((ret->block = _pool__malloc(size)) == NULL) sleep(1);
#endif
    ret->size = size;
    p->size += size;
    ret->used = 0;

    /* append to the cleanup list */
#ifdef FAST_POOL
    clean = _pool_free_fast(ret->block+size, 
			    _pool_heap_free, (void *)ret);
#else
    clean = _pool_free(p, _pool_heap_free, (void *)ret);
#endif
    clean->heap = ret; /* for future use in finding used mem for pstrdup */
    _pool_cleanup_append(p, clean);

    return ret;
}

pool _pool_new_heap(int size, char *zone)
{
    pool p;
#ifdef FAST_POOL
    struct pheap *ret;

    while((p = _pool__malloc(sizeof(_pool)+
			     sizeof(struct pheap)+
			     size)) == NULL) sleep(1);
    p->cleanup = NULL;
    p->size = 0;
    
#ifdef POOL_DEBUG
    p->lsize = -1;
    p->zone[0] = '\0';
    strcat(p->zone,zone);
    sprintf(p->name,"%X",p);
    
    if(pool__disturbed == NULL)
      {
        pool__disturbed = 1; /* reentrancy flag! */
        pool__disturbed = ghash_create(POOL_DEBUG,(KEYHASHFUNC)str_hash_code,(KEYCOMPAREFUNC)j_strcmp);
      }
    if(pool__disturbed != 1)
      ghash_put(pool__disturbed,p->name,p);
#endif
    
    ret = (void *)p + sizeof(_pool);
    ret->block = (void*)ret + sizeof(struct pheap);

    ret->size = size;
    p->size += size;
    ret->used = 0;

    p->heap = ret;
#else
    p = _pool_new(zone);
    p->heap = _pool_heap(p,size);
#endif

    return p;
}

void *pmalloc(pool p, int size)
{
    void *block;

    if(p == NULL)
    {
        fprintf(stderr,"Memory Leak! [pmalloc received NULL pool, unable to track allocation, exiting]\n");
        abort();
    }

    /* if there is no heap for this pool or it's a big request, just raw, I like how we clean this :) */
    if(p->heap == NULL || size > (p->heap->size / 2))
    {
#ifdef FAST_POOL
      while((block = _pool__malloc(size+
				   sizeof(struct pfree))) == NULL) sleep(1);
      p->size += size;
      _pool_cleanup_append(p, _pool_free_fast(block+size, _pool__free, block));
#else
      while((block = _pool__malloc(size)) == NULL) sleep(1);
      p->size += size;
      _pool_cleanup_append(p, _pool_free(p, _pool__free, block));
#endif
      return block;
    }

    /* we have to preserve boundaries, long story :) */
    if(size >= 4)
        while(p->heap->used&7) p->heap->used++;

    /* if we don't fit in the old heap, replace it */
    if(size > (p->heap->size - p->heap->used))
        p->heap = _pool_heap(p, p->heap->size);

    /* the current heap has room */
    block = (char *)p->heap->block + p->heap->used;
    p->heap->used += size;
    return block;
}

void *pmalloc_x(pool p, int size, char c)
{
   void* result = pmalloc(p, size);
   if (result != NULL)
           memset(result, c, size);
   return result;
}  

/* easy safety utility (for creating blank mem for structs, etc) */
void *pmalloco(pool p, int size)
{
    void *block = pmalloc(p, size);
    memset(block, 0, size);
    return block;
}  

/* XXX efficient: move this to const char * and then loop throug the existing heaps to see if src is within a block in this pool */
char *pstrdup(pool p, const char *src)
{
    char *ret;
    int len;

    if(src == NULL)
        return NULL;

    len = strlen(src)+1;
    ret = pmalloc(p,len);
    memcpy(ret,src,len);

    return ret;
}

/* when move above, this one would actually return a new block */
char *pstrdupx(pool p, const char *src)
{
    return pstrdup(p, src);
}

int pool_size(pool p)
{
    if(p == NULL) return 0;

    return p->size;
}

void pool_free(pool p)
{
    struct pfree *cur, *stub;

    if(p == NULL) return;

    cur = p->cleanup;

    while(cur != NULL)
    {
      stub = cur->next;

#ifdef FAST_POOL
      /* if pfree in memory block */
      if (cur->heap) {
	/* free */
	(*cur->f)(cur->arg);
      }
      else {
	(*cur->f)(cur->arg);
	_pool__free(cur);
      }
#else
      (*cur->f)(cur->arg);
      _pool__free(cur);
#endif
      cur = stub;
    }

#ifdef POOL_DEBUG
    ghash_remove(pool__disturbed,p->name);
#endif

    _pool__free(p);

}

/* public cleanup utils, insert in a way that they are run FIFO, before mem frees */
void pool_cleanup(pool p, pool_cleaner f, void *arg)
{
    struct pfree *clean;

    clean = _pool_free(p, f, arg);
    clean->heap = NULL;
    clean->next = p->cleanup;
    p->cleanup = clean;
}

#ifdef POOL_DEBUG
void debug_log(char *zone, const char *msgfmt, ...);
int _pool_stat(void *arg, const void *key, void *data)
{
    pool p = (pool)data;

    if(p->lsize == -1)
        debug_log("leak","%s: %X is a new pool",p->zone,p->name);
    else if(p->size > p->lsize)
        debug_log("leak","%s: %X grew %d",p->zone,p->name, p->size - p->lsize);
    else if((int)arg)
        debug_log("leak","%s: %X exists %d",p->zone,p->name, p->size);
    p->lsize = p->size;
    return 1;
}

void pool_stat(int full)
{
    ghash_walk(pool__disturbed,_pool_stat,(void *)full);
    if(pool__total != pool__ltotal)
        debug_log("leak","%d\ttotal missed mallocs",pool__total);
    pool__ltotal = pool__total;
    return;
}
#else
void pool_stat(int full)
{
    return;
}
#endif
