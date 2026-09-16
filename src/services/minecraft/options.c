#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "sxcl/options.h"

#include "sxcl/fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct sxcl_option_entry {
    char *key;
    char *value; /* bare 行为 "" */
    int bare;    /* 1 = 原文没有冒号 */
} sxcl_option_entry;

struct sxcl_options {
    sxcl_option_entry *entries;
    size_t count;
    size_t capacity;
};

static char *dup_range(const char *s, size_t n)
{
    char *p = (char *)malloc(n + 1);
    if (!p) {
        return NULL;
    }
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static char *dup_str(const char *s)
{
    return dup_range(s ? s : "", s ? strlen(s) : 0);
}

/* 可移植读行(MSVC 没有 getline):返回读到的字节数(不含结尾 NUL),EOF 且无数据返回 -1。 */
static long read_line(FILE *fh, char **buf, size_t *cap)
{
    if (!*buf || *cap == 0) {
        *cap = 256;
        *buf = (char *)malloc(*cap);
        if (!*buf) {
            return -1;
        }
    }
    size_t n = 0;
    for (;;) {
        if (!fgets(*buf + n, (int)(*cap - n), fh)) {
            return n > 0 ? (long)n : -1;
        }
        n += strlen(*buf + n);
        if (n > 0 && (*buf)[n - 1] == '\n') {
            return (long)n;
        }
        if (n + 1 < *cap) {
            return (long)n; /* 文件末尾没有换行 */
        }
        const size_t ncap = *cap * 2;
        char *grown = (char *)realloc(*buf, ncap);
        if (!grown) {
            return -1;
        }
        *buf = grown;
        *cap = ncap;
    }
}

static int push(sxcl_options *o, char *key, char *value, int bare)
{
    if (o->count == o->capacity) {
        const size_t next = o->capacity ? o->capacity * 2 : 64;
        sxcl_option_entry *grown = (sxcl_option_entry *)realloc(o->entries, next * sizeof(*grown));
        if (!grown) {
            return -1;
        }
        o->entries = grown;
        o->capacity = next;
    }
    o->entries[o->count].key = key;
    o->entries[o->count].value = value;
    o->entries[o->count].bare = bare;
    ++o->count;
    return 0;
}

sxcl_options *sxcl_options_load(const char *path)
{
    if (!path) {
        return NULL;
    }
    sxcl_options *o = (sxcl_options *)calloc(1, sizeof(*o));
    if (!o) {
        return NULL;
    }
    FILE *fh = fopen(path, "rb");
    if (!fh) {
        return o; /* 不存在 / 打不开 = 空表 */
    }
    char *line = NULL;
    size_t cap = 0;
    long n = 0;
    while ((n = read_line(fh, &line, &cap)) >= 0) {
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        if (n == 0) {
            continue; /* 空行不保留:MC 自己的 options.txt 也没有空行 */
        }
        const char *colon = strchr(line, ':');
        char *key = NULL;
        char *value = NULL;
        int bare = 0;
        if (colon) {
            key = dup_range(line, (size_t)(colon - line));
            value = dup_str(colon + 1);
        } else {
            key = dup_str(line);
            value = dup_str("");
            bare = 1;
        }
        if (!key || !value || push(o, key, value, bare) != 0) {
            free(key);
            free(value);
        }
    }
    free(line);
    fclose(fh);
    return o;
}

int sxcl_options_save(const sxcl_options *options, const char *path)
{
    if (!options || !path) {
        return -1;
    }
    const size_t len = strlen(path);
    char *tmp = (char *)malloc(len + 5);
    if (!tmp) {
        return -1;
    }
    memcpy(tmp, path, len);
    memcpy(tmp + len, ".tmp", 5);

    int rc = -1;
    if (sxcl_fs_mkdirs_for_file(path) == 0) {
        FILE *fh = fopen(tmp, "wb");
        if (fh) {
            rc = 0;
            for (size_t i = 0; i < options->count; ++i) {
                const sxcl_option_entry *e = &options->entries[i];
                if (e->bare) {
                    if (fprintf(fh, "%s\n", e->key) < 0) {
                        rc = -1;
                    }
                } else if (fprintf(fh, "%s:%s\n", e->key, e->value) < 0) {
                    rc = -1;
                }
                if (rc != 0) {
                    break;
                }
            }
            if (fclose(fh) != 0) {
                rc = -1;
            }
            if (rc == 0 && sxcl_fs_rename_replace(tmp, path) != 0) {
                rc = -1;
            }
        }
    }
    if (rc != 0) {
        sxcl_fs_remove(tmp);
    }
    free(tmp);
    return rc;
}

static sxcl_option_entry *find(sxcl_options *o, const char *key)
{
    if (!o || !key) {
        return NULL;
    }
    for (size_t i = 0; i < o->count; ++i) {
        if (strcmp(o->entries[i].key, key) == 0) {
            return &o->entries[i];
        }
    }
    return NULL;
}

const char *sxcl_options_get(const sxcl_options *options, const char *key)
{
    for (size_t i = 0; options && i < options->count; ++i) {
        if (strcmp(options->entries[i].key, key) == 0) {
            return options->entries[i].value;
        }
    }
    return NULL;
}

int sxcl_options_set(sxcl_options *options, const char *key, const char *value)
{
    if (!options || !key || !value) {
        return -1;
    }
    sxcl_option_entry *e = find(options, key);
    if (e) {
        char *nv = dup_str(value);
        if (!nv) {
            return -1;
        }
        free(e->value);
        e->value = nv;
        e->bare = 0;
        return 0;
    }
    char *k = dup_str(key);
    char *v = dup_str(value);
    if (!k || !v) {
        free(k);
        free(v);
        return -1;
    }
    if (push(options, k, v, 0) != 0) {
        free(k);
        free(v);
        return -1;
    }
    return 0;
}

int sxcl_options_remove(sxcl_options *options, const char *key)
{
    if (!options || !key) {
        return -1;
    }
    for (size_t i = 0; i < options->count; ++i) {
        if (strcmp(options->entries[i].key, key) == 0) {
            free(options->entries[i].key);
            free(options->entries[i].value);
            memmove(&options->entries[i], &options->entries[i + 1],
                    (options->count - i - 1) * sizeof(sxcl_option_entry));
            --options->count;
            return 0;
        }
    }
    return 0; /* 不存在不算错误 */
}

size_t sxcl_options_count(const sxcl_options *options)
{
    return options ? options->count : 0;
}

const char *sxcl_options_key_at(const sxcl_options *options, size_t index)
{
    if (!options || index >= options->count) {
        return NULL;
    }
    return options->entries[index].key;
}

const char *sxcl_options_value_at(const sxcl_options *options, size_t index)
{
    if (!options || index >= options->count) {
        return NULL;
    }
    return options->entries[index].value;
}

void sxcl_options_free(sxcl_options *options)
{
    if (!options) {
        return;
    }
    for (size_t i = 0; i < options->count; ++i) {
        free(options->entries[i].key);
        free(options->entries[i].value);
    }
    free(options->entries);
    free(options);
}
