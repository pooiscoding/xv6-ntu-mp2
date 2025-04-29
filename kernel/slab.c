#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "slab.h"
#include "list.h"
#include "debug.h"

/*
 * strings definition
 */

#define SMALL_SLAB "[MYOUT] the slab cannot contain even one object due to your wrong implementation.\n"
// #define FREE_TO_CACHE "[MYOUT] the object is freed to cache\n"
// #define FREE_TO_SLAB "[MYOUT] the object is freed to normal slab\n"

// Helpers
#define ptrsizeof(ptr) sizeof( *((typeof(ptr)) 0) )
#define INIT_JUNK ((void *) 0x0000000000000000)

// -------------------------------------------------------------------------------------------------
// [CHECK] general slab

#define __gs_object_number(gs) \
    ( ( PGSIZE - ((uint) ptrsizeof(gs) ) ) / ((gs)->object_size) )

#define __gs_fully_constructed(gs) \
    ((gs)->used_objects & 1)

#define __gs_set_fully_constructed(gs) \
    ((gs)->used_objects) |= 1

#define __gs_get_used_objects(gs) \
    ((gs)->used_objects >> 1)

#define __gs_inc_used_objects(gs) \
    (gs)->used_objects += 2

#define __gs_dec_used_objects(gs) \
    (gs)->used_objects -= 2

#define __gs_isfree(gs) \
    (__gs_get_used_objects(gs) == 0)

#define __gs_isfull(gs) \
    ((gs)->freelist_offset == 0)

/*
 * This macro ensures the freelist pointer correctly points to the next available object.
 * It is designed for delayed construction, avoiding list structure initialization during allocation.
 *
 * If the pointer is uninitialized and construction is incomplete, it updates the pointer to the next object.
 * If the next object is the last one, it sets the pointer to NULL and marks the page as fully constructed,
 * ensuring no further construction is needed.
 */
#define __gs_check_freelist_link(ns_gs, ns_fl)                                                    \
    do {                                                                                          \
        if (ns_fl->next == ((struct run *) INIT_JUNK) && !__gs_fully_constructed(ns_gs)) {        \
            ns_fl->next = (struct run *) ((char *) ns_fl + ns_gs->object_size);                   \
            if (((char *) ns_fl->next + (ns_gs->object_size << 1)) > ((char *) ns_gs + PGSIZE)) { \
                __gs_set_fully_constructed(ns_gs);                                                \
                ns_fl->next->next = NULL;                                                         \
            }                                                                                     \
        }                                                                                         \
    } while (0)

#define __gs_get_freelist(gs) ({                                          \
    typeof(gs) __gsg = (gs);                                              \
    struct run *__ret;                                                    \
    if (__gsg->freelist_offset == 0) {                                    \
        __ret = NULL;                                                     \
    } else {                                                              \
        __ret = (struct run *) ((char *) __gsg + __gsg->freelist_offset); \
        __gs_check_freelist_link(__gsg, __ret);                           \
    }                                                                     \
    __ret;                                                                \
})

#define __gs_set_freelist(gs, ptr)                                                  \
    do {                                                                            \
        typeof(gs) __gss = (gs);                                                    \
        void *__ptr = (void *) (ptr);                                               \
        if (__ptr == NULL) __gss->freelist_offset = 0;                              \
        else __gss->freelist_offset = (uint16) ((uint64) __ptr) - ((uint64) __gss); \
    } while (0)

#define __gs_setup_member(gs, _object_size)                       \
    do {                                                          \
        typeof(gs) __gs = (gs);                                   \
        __gs_set_freelist(__gs, ((char *) __gs + ptrsizeof(gs))); \
        __gs->used_objects = 0;                                   \
        __gs->object_size = (_object_size);                       \
        INIT_LIST_HEAD(&__gs->slab_list);                         \
    } while (0)

#define __gs_container(obj) \
    ((void *) ((uint64) obj & ~((uint64) PGSIZE - 1)))

#define __gs_print_slab_info(gs, pfp) ({                               \
    typeof(gs) __gsp = (gs);                                           \
    char *obj_ptr;                                                     \
    uint obj_num;                                                      \
    obj_ptr = (char *) __gsp + ptrsizeof(__gsp);                       \
    obj_num = __gs_object_number(__gsp);                               \
    for (uint i = 0; i < obj_num; ++i) {                               \
        __gs_check_freelist_link(__gsp, ((struct run *) obj_ptr));     \
        debug(                                                         \
            "[SLAB]    [ idx %u ] { addr: %p, as_ptr: %p, as_obj: { ", \
            i,                                                         \
            obj_ptr,                                                   \
            ((struct run *) obj_ptr)->next                             \
        );                                                             \
        pfp((void *) obj_ptr);                                         \
        debug(" } }\n");                                               \
        obj_ptr += __gsp->object_size;                                 \
    }                                                                  \
})



// -------------------------------------------------------------------------------------------------
// general slab
#define __gs_alloc(gs) ({                                         \
    void *__obja;                                                 \
    typeof(gs) __gsa = (gs);                                      \
    if (__gs_isfull(__gsa)) {                                     \
        __obja = NULL;                                            \
    } else {                                                      \
        __obja = (void *) __gs_get_freelist(__gsa);               \
        __gs_set_freelist(__gsa, __gs_get_freelist(__gsa)->next); \
        __gs_inc_used_objects(__gsa);                             \
    }                                                             \
    __obja;                                                       \
})

#define __gs_free(gs, obj)                                        \
    do {                                                          \
        void *__objf = (obj);                                     \
        typeof(gs) __gsf = (gs);                                  \
        ((struct run *) __objf)->next = __gs_get_freelist(__gsf); \
        __gs_set_freelist(__gsf, (struct run *) __objf);          \
        __gs_dec_used_objects(__gsf);                             \
    } while (0)

// -------------------------------------------------------------------------------------------------
// [CHECK] slab
#define __s_alloc_slab_space() ({                \
    struct slab *__s = (struct slab *) kalloc(); \
    memset((void *) __s, 0, PGSIZE);             \
    __s;                                         \
})

#define __s_free_slab_space(s) \
    kfree((void *) (s))

// -------------------------------------------------------------------------------------------------
// slab
static inline struct slab* __s_create(struct kmem_cache *cache)
{
    struct slab *s;
    // allocate a new slab from pm
    s = __s_alloc_slab_space();
    // setup the slab member
    __gs_setup_member(s, cache->object_size);

    return s;
}

#define __s_destroy(s) \
    __s_free_slab_space(s)

// -------------------------------------------------------------------------------------------------
// [CHECK] slab list
#define __sl_container(sl) \
    container_of((sl), struct slab, slab_list)

#define __sl_next_slab(sl) \
    __sl_container((sl)->next)

static inline void __sl_destroy(struct list_head *sl)
{
    struct list_head *node, *safe;
    list_for_each_safe(node, safe, sl)
        __s_destroy(__sl_container(node));
}

// -------------------------------------------------------------------------------------------------
// [CHECK] kmem_cache
#define __kc_alloc_cache_space() ({                          \
    struct kmem_cache *__c = (struct kmem_cache *) kalloc(); \
    memset((void *) __c, 0, PGSIZE);                         \
    __c;                                                     \
})

#define __kc_free_cache_space(c) \
    kfree((void *) (c));

#define __kc_setup_member(c, name_ptr)                     \
    do {                                                   \
        struct kmem_cache *__c = (c);                      \
        strncpy(__c->name, (name_ptr), sizeof(__c->name)); \
        __c->name[sizeof(__c->name) - 1] = '\0';           \
        initlock(&__c->lock, __c->name);                   \
        INIT_LIST_HEAD(&__c->slab_list);                   \
        __c->avail_slabs = 0;                              \
    } while (0)

#define __kc_free_member(c) \
    __sl_destroy(&(c)->slab_list)

#define __kc_track_slab(c, s)                       \
    do {                                            \
        struct kmem_cache *__c = (c);               \
        list_add(&(s)->slab_list, &__c->slab_list); \
        ++(__c->avail_slabs);                       \
    } while (0)

#define __kc_untrack_slab(c, s)       \
    do {                              \
        list_del(&(s)->slab_list);    \
        --((c)->avail_slabs);         \
    } while (0)

#define __kc_overuse_memory(c) \
    ((c)->avail_slabs > MP2_MIN_AVAIL_SLAB)

#define __kc_isavail(c) \
    ((c)->avail_slabs)

#define __kc_choose_slab(c) \
    __sl_next_slab(&(c)->slab_list)

#define __kc_update_slab_before_alloc(c, s)

#define __kc_update_slab_after_alloc(c, s) \
    do {                                   \
        struct slab *__s = (s);            \
        if (__gs_isfull(__s))              \
            __kc_untrack_slab((c), __s);   \
    } while (0)

#define __kc_update_slab_before_free(c, s) \
    do {                                   \
        struct slab *__s = (s);            \
        if (__gs_isfull(__s))              \
            __kc_track_slab((c), __s);     \
    } while (0)

    
static inline void __kc_update_slab_after_free(struct kmem_cache *c, struct slab *s)
{
    if (__gs_isfree(s) && __kc_overuse_memory(c)) {
// OUTPUT
        debug(
            "[SLAB] slab %p (%s) is freed due to save memory\n",
            s,
            c->name
        );
// OUTPUT
        __kc_untrack_slab(c, s);
        __s_destroy(s);
    }
}

#define __kc_print_incache_slab_info(c, pfp)                    \
    do {                                                        \
        struct kmem_cache *__c = (c);                           \
        debug("[SLAB]  [ cache slabs ]\n");                     \
        debug(                                                  \
            "[SLAB]   [ slab %p ] { freelist: %p, nxt: %p }\n", \
            __c,                                                \
            __gs_get_freelist(__c),                             \
            (void *) 0                                          \
        );                                                      \
        __gs_print_slab_info(__c, pfp);                         \
    } while (0)

static inline void __kc_print_normal_slabs_info(struct kmem_cache *c, void (*pfp)(void *))
{
    struct slab *__s;
    struct list_head *__node;
    char is_partial_in = 0;
    list_for_each(__node, &c->slab_list) {
        __s = __sl_container(__node);
        if (__gs_isfree(__s)) continue;
        if (!is_partial_in) {
            is_partial_in = 1;
            debug("[SLAB]  [ partial slabs ]\n");
        }
        debug(
            "[SLAB]    [ slab %p ] { freelist: %p, nxt: %p }\n",
            __s,
            __gs_get_freelist(__s),
            __sl_next_slab(&__s->slab_list)
        );
        __gs_print_slab_info(__s, pfp);
    }
}

// -------------------------------------------------------------------------------------------------
// main api
void print_kmem_cache(struct kmem_cache *cache, void (*slab_obj_printer)(void *))
{
  // TODO: Implement print_kmem_cache
  // printf("[SLAB] TODO: print_kmem_cache is not yet implemented \n");
    //struct list_head *node;
    acquire(&cache->lock);

// OUTPUT
    debug(
        "[SLAB] kmem_cache { name: %s, object_size: %u, at: %p, in_cache_obj: %u }\n",
        cache->name,
        cache->object_size,
        cache,
        __gs_object_number(cache)
    );
// OUTPUT

    __kc_print_incache_slab_info(cache, slab_obj_printer);
    __kc_print_normal_slabs_info(cache, slab_obj_printer);

// OUTPUT
    debug("[SLAB] print_kmem_cache end\n");
// OUTPUT

    release(&cache->lock);
}


struct kmem_cache *kmem_cache_create(char *name, uint object_size)
{
    // TODO: Implement kmem_cache_create
  
    struct kmem_cache *cache;
    cache = __kc_alloc_cache_space();

    __kc_setup_member(cache, name);
    __gs_setup_member(cache, object_size);

// OUTPUT
    debug(
        "[SLAB] New kmem_cache (name: %s, object size: %u bytes, at: %p, max objects per slab: %u, support in cache obj: %u) is created\n",
        cache->name,
        cache->object_size,
        cache,
        __gs_object_number(&((struct slab){.object_size = cache->object_size})),
        __gs_object_number(cache)
    );
// OUTPUT

    return cache;
}

void kmem_cache_destroy(struct kmem_cache *cache)
{
    // TODO: Implement kmem_cache_destroy (will not be tested)

    acquire(&cache->lock);

    __kc_free_member(cache);
    __kc_free_cache_space(cache);   

    // the cache page was released, the access would cause page fault
    // no need to release the lock
}

void *kmem_cache_alloc(struct kmem_cache *cache)
{
    // TODO: Implement kmem_cache_alloc

    struct slab *cur;
    void *obj;

    acquire(&cache->lock); // acquire the lock before modification

// OUTPUT
    debug("[SLAB] Alloc request on cache %s\n", cache->name);
// OUTPUT

    if (!__gs_isfull(cache)) {
        // allocate from cache
        cur = (struct slab *) cache;
        obj = __gs_alloc(cache);
    } else {
        // allocate from normal slab
        if (__kc_isavail(cache)) {
            cur = __kc_choose_slab(cache);
        } else {
            cur = __s_create(cache);
            __kc_track_slab(cache, cur);
// OUTPUT
            debug("[SLAB] A new slab %p (%s) is allocated\n", cur, cache->name); 
// OUTPUT
        }

        __kc_update_slab_before_alloc(cache, cur);
        obj = __gs_alloc(cur);
        __kc_update_slab_after_alloc(cache, cur);
    }

// OUTPUT
    debug(
        "[SLAB] Object %p in slab %p (%s) is allocated and initialized\n",
        obj,
        cur,
        cache->name
    );
// OUTPUT
    
    release(&cache->lock); // release the lock before return
  
    return obj;
}

void kmem_cache_free(struct kmem_cache *cache, void *obj)
{
    
    // TODO: Implement kmem_cache_free

    struct slab *cur;

    acquire(&cache->lock); // acquire the lock before modification
    
    // ... (modify kmem_cache)
    cur = (struct slab *) __gs_container(obj);

// OUTPUT
    debug(
        "[SLAB] Free %p in slab %p (%s)\n",
        obj,
        cur,
        cache->name
    );
// OUTPUT

    if ((struct kmem_cache *) cur == cache) {
        // free from cache
        // debug(FREE_TO_CACHE);

        __gs_free(cache, obj);

    } else {
        // free from normal slab
        // debug(FREE_TO_SLAB);

        __kc_update_slab_before_free(cache, cur);
        __gs_free(cur, obj);
        __kc_update_slab_after_free(cache, cur);
    }

// OUTPUT
    debug("[SLAB] End of free\n");
// OUTPUT

    release(&cache->lock); // release the lock before return
}
