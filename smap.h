#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct smap smap_t;

smap_t* smap_create(size_t bucket_count);
void    smap_free(smap_t* m);

// set/replace value for key; returns previous value (or NULL)
void*   smap_set(smap_t* m, const char* key, void* value);

// get value by key; returns NULL if not found
void*   smap_get(smap_t* m, const char* key);

typedef void (*smap_iter_cb)(const char* key, void* value, void* ud);
void    smap_iterate(smap_t* m, smap_iter_cb cb, void* ud);


