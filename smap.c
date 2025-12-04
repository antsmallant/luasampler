#include "smap.h"
#include "profile.h" // for pmalloc/pfree
#include <string.h>

typedef struct smap_entry {
    uint64_t hash;
    char*    key;
    void*    value;
    struct smap_entry* next;
} smap_entry_t;

struct smap {
    size_t bucket_count;
    smap_entry_t** buckets;
};

static inline uint64_t hash64_str(const char* s) {
    const uint64_t FNV_OFFSET = 1469598103934665603ULL;
    const uint64_t FNV_PRIME  = 1099511628211ULL;
    uint64_t h = FNV_OFFSET;
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
        h ^= (uint64_t)(*p);
        h *= FNV_PRIME;
    }
    return h;
}

static inline char* pstrdup_local(const char* s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char* d = (char*)pmalloc(n + 1);
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

static inline size_t smap_index(struct smap* m, uint64_t h) { return (size_t)(h & (m->bucket_count - 1)); }

smap_t* smap_create(size_t bucket_count) {
    if (bucket_count == 0) bucket_count = 1024;
    size_t b = 1;
    while (b < bucket_count) b <<= 1;
    smap_t* m = (smap_t*)pmalloc(sizeof(*m));
    m->bucket_count = b;
    m->buckets = (smap_entry_t**)pmalloc(sizeof(smap_entry_t*) * b);
    memset(m->buckets, 0, sizeof(smap_entry_t*) * b);
    return m;
}

void smap_free(smap_t* m) {
    if (!m) return;
    for (size_t i = 0; i < m->bucket_count; i++) {
        smap_entry_t* e = m->buckets[i];
        while (e) {
            smap_entry_t* next = e->next;
            if (e->key) pfree(e->key);
            pfree(e);
            e = next;
        }
    }
    pfree(m->buckets);
    pfree(m);
}

void* smap_set(smap_t* m, const char* key, void* value) {
    if (!m || !key) return NULL;
    uint64_t h = hash64_str(key);
    size_t bi = smap_index(m, h);
    smap_entry_t* e = m->buckets[bi];
    while (e) {
        if (e->hash == h && strcmp(e->key, key) == 0) {
            void* old = e->value;
            e->value = value;
            return old;
        }
        e = e->next;
    }
    smap_entry_t* ne = (smap_entry_t*)pmalloc(sizeof(*ne));
    ne->hash = h;
    ne->key = pstrdup_local(key);
    ne->value = value;
    ne->next = m->buckets[bi];
    m->buckets[bi] = ne;
    return NULL;
}

void* smap_get(smap_t* m, const char* key) {
    if (!m || !key) return NULL;
    uint64_t h = hash64_str(key);
    size_t bi = smap_index(m, h);
    smap_entry_t* e = m->buckets[bi];
    while (e) {
        if (e->hash == h && strcmp(e->key, key) == 0) return e->value;
        e = e->next;
    }
    return NULL;
}

void smap_iterate(smap_t* m, smap_iter_cb cb, void* ud) {
    if (!m || !cb) return;
    for (size_t i = 0; i < m->bucket_count; i++) {
        for (smap_entry_t* e = m->buckets[i]; e; e = e->next) {
            cb(e->key, e->value, ud);
        }
    }
}


