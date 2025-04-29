#pragma once

#include "spinlock.h"
#include "types.h"
#include "param.h"
#include "list.h"

struct run {
  struct run *next;
};

/*
 * struct slab memory layout
 *
 * bytes - field
 *    16 - slab_list
 *     2 - used_objects    ---+---(align)
 *     2 - freelist_offset ---|
 *     4 - object_size     ---|
 * +) --------------------------
 *    24 - total
 *
 * slab struct bonus: 24 / 8 = 3 <= 3
 *
 */

/**
 * struct slab - Represents a slab in the slab allocator.
 * @freelist: Linked list of free objects.
 */
struct slab
{
  struct list_head slab_list;
  uint16 used_objects;
  uint16 freelist_offset;
  uint object_size;
};

/*
 * struct kmem_cache memory layout
 *
 * bytes - field
 *    16 - name
 *     4 - lock.locked     ---+---(align)---+---(lock)
 *     2 - used_objects    ---|             |
 *     2 - freelist_offset ---|             |
 *     8 - lock.name       -----------------|
 *     8 - lock.cpu        -----------------|
 *    16 - slab_list
 *     4 - object_size     ---+---(align)
 *     4 - avail_slabs     ---|
 * +) --------------------------
 *    64 - total
 *
 * in-cache slab size: 4032 bytes
 * # of file structure in cache: 4032 // 504 = 8 :)
 *
 */

/**
 * struct kmem_cache - Represents a cache of slabs.
 * @name: Cache name (e.g., "file").
 * @object_size: Size of a single object.
 * @lock: Lock for cache management.
 */
struct kmem_cache
{
  char name[MP2_CACHE_MAX_NAME];
  union {
    struct {
      uint _;
      uint16 used_objects;
      uint16 freelist_offset;
    };
    struct spinlock lock;
  };

  struct list_head slab_list;  // Partially allocated slabs
  uint object_size;     // Size of a single object
  uint avail_slabs;
};

/**
 * kmem_cache_create - Create a new slab cache.
 * @name: The name of the cache.
 * @object_size: The size of each object in the cache.
 *
 * Return: A pointer to the new cache.
 */
struct kmem_cache *kmem_cache_create(char *name, uint object_size);

/**
 * kmem_cache_destroy - Destroy a slab cache.
 * @cache: The cache to be destroyed.
 */
void kmem_cache_destroy(struct kmem_cache *cache);

/**
 * kmem_cache_alloc - Allocate an object from a slab cache.
 * @cache: The cache to allocate from.
 *
 * Return: A pointer to the allocated object.
 */
void *kmem_cache_alloc(struct kmem_cache *cache);

/**
 * kmem_cache_free - Free an object back to its slab cache.
 * @cache: The cache to free to.
 * @obj: The object to free.
 */
void kmem_cache_free(struct kmem_cache *cache, void *obj);

/**
 * print_kmem_cache - Print the details of a kmem_cache.
 * @cache: The cache to print.
 * @print_fn: Function to print each object in the cache. If NULL (0) is given, will skip object printing part.
 */
void print_kmem_cache(struct kmem_cache *cache, void (*print_fn)(void *));
