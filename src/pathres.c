#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "pathres.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "protocol.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Is `resolved` the root itself or somewhere beneath it? Compares whole path
 * components, so "/srv/www2" is NOT inside "/srv/www". */
static int inside_root(const char *root, const char *resolved)
{
    size_t rl = strlen(root);

    if (rl == 1 && root[0] == '/')
        return 1;                               /* root is "/" */
    if (strncmp(resolved, root, rl) != 0)
        return 0;
    return resolved[rl] == '\0' || resolved[rl] == '/';
}

static PathStatus errno_status(void)
{
    switch (errno) {
    case ENOENT:
    case ENOTDIR:
    case ELOOP:
    case ENAMETOOLONG:
        return PATH_NOT_FOUND;
    default:
        return PATH_ERROR;                      /* EACCES, EIO, ... */
    }
}

/* realpath + containment check + stat. */
static PathStatus canonical(const char *root, const char *candidate,
                            char resolved[PATH_MAX], struct stat *st)
{
    if (realpath(candidate, resolved) == NULL)
        return errno_status();
    if (!inside_root(root, resolved))
        return PATH_NOT_FOUND;                  /* symlink escape: pretend absent */
    if (stat(resolved, st) != 0)
        return errno_status();
    return PATH_OK;
}

PathStatus resolve_path(const char *root_real, const uint8_t *path, size_t len,
                        char *out, size_t out_size)
{
    char rel[MAX_PATH_LENGTH + 2];
    char candidate[PATH_MAX + 32];
    char resolved[PATH_MAX];
    struct stat st;
    size_t i, rel_len = 0, root_len = strlen(root_real);
    int trailing_slash;
    PathStatus ps;

    /* Rule 1-2: must be non-empty, begin with '/', fit the limit. */
    if (path == NULL || len == 0 || len > MAX_PATH_LENGTH || path[0] != '/')
        return PATH_BAD;

    /* Rule 3: no control bytes (this includes NUL) and no backslash. */
    for (i = 0; i < len; i++) {
        uint8_t c = path[i];
        if (c < 0x20 || c == 0x7f || c == '\\')
            return PATH_BAD;
    }

    /* Rule 4: split on '/', collapse repeated slashes, drop ".", reject "..".
     * '%' is an ordinary byte: there is NO percent-decoding in BHTP. */
    trailing_slash = path[len - 1] == '/';
    i = 0;
    while (i < len) {
        size_t start, seg_len;

        while (i < len && path[i] == '/')
            i++;
        start = i;
        while (i < len && path[i] != '/')
            i++;
        seg_len = i - start;
        if (seg_len == 0)
            break;
        if (seg_len == 1 && path[start] == '.')
            continue;
        if (seg_len == 2 && path[start] == '.' && path[start + 1] == '.')
            return PATH_BAD;
        rel[rel_len++] = '/';
        memcpy(rel + rel_len, path + start, seg_len);
        rel_len += seg_len;
    }
    rel[rel_len] = '\0';

    /* Rule 5: join under the root, canonicalise, verify containment. */
    if (root_len + rel_len >= PATH_MAX)
        return PATH_NOT_FOUND;
    memcpy(candidate, root_real, root_len);
    memcpy(candidate + root_len, rel, rel_len + 1);
    if (root_len == 1 && root_real[0] == '/' && rel_len > 0)   /* avoid "//x" */
        memmove(candidate, candidate + 1, rel_len + 1);

    ps = canonical(root_real, candidate, resolved, &st);
    if (ps != PATH_OK)
        return ps;

    /* Rule 6: a directory means its index.html. */
    if (S_ISDIR(st.st_mode)) {
        size_t rl = strlen(resolved);
        if (rl + sizeof "/index.html" >= PATH_MAX)
            return PATH_NOT_FOUND;
        memcpy(candidate, resolved, rl);
        memcpy(candidate + rl, "/index.html", sizeof "/index.html");
        ps = canonical(root_real, candidate, resolved, &st);
        if (ps != PATH_OK)
            return ps;
    } else if (trailing_slash) {
        return PATH_NOT_FOUND;                  /* "/file.txt/" is not a file */
    }

    /* Rule 7: regular files only (no devices, FIFOs, sockets). */
    if (!S_ISREG(st.st_mode))
        return PATH_NOT_FOUND;

    if (strlen(resolved) + 1 > out_size)
        return PATH_NOT_FOUND;
    strcpy(out, resolved);
    return PATH_OK;
}
