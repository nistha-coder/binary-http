/*
 * pathres.h - turn an untrusted request path into a file under the root.
 * The rules implemented here are the normative "path rules" of SPEC.md.
 */
#ifndef PATHRES_H
#define PATHRES_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    PATH_OK = 0,        /* `out` holds the absolute path of a regular file     */
    PATH_BAD,           /* syntactically invalid or traversal attempt  -> 400  */
    PATH_NOT_FOUND,     /* no such regular file inside the root        -> 404  */
    PATH_ERROR          /* could not be examined (permissions, I/O)    -> 500  */
} PathStatus;

/*
 * root_real : the root directory after realpath() (computed once at startup)
 * path      : request path bytes (NOT NUL-terminated), `len` bytes
 * out       : receives the resolved absolute file path (NUL-terminated)
 */
PathStatus resolve_path(const char *root_real, const uint8_t *path, size_t len,
                        char *out, size_t out_size);

#endif
