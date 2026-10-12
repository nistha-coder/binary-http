/*
 * test_pathres.c - the path-security rules (SPEC section 8) against a real
 * directory tree, including symlink escapes and special files.
 */
#define _XOPEN_SOURCE 700
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pathres.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int failures = 0, checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("  CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

/* base is capped at 200 chars so every path built from it provably fits. */
static char base[208], root[300], outside[300];

static void must_symlink(const char *target, const char *linkpath)
{
    if (symlink(target, linkpath) != 0) {
        perror(linkpath);
        exit(2);
    }
}

static void put(const char *rel, const char *content)
{
    char p[PATH_MAX + 64];
    FILE *f;
    snprintf(p, sizeof p, "%s/%s", root, rel);
    f = fopen(p, "w");
    if (!f) { perror(p); exit(2); }
    fputs(content, f);
    fclose(f);
}

static void make_dir(const char *rel)
{
    char p[PATH_MAX + 64];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    if (mkdir(p, 0755) != 0) { perror(p); exit(2); }
}

static void fixture(void)
{
    char tmpl[] = "/tmp/bhtp-pathres-XXXXXX", p[PATH_MAX + 64], q[PATH_MAX + 64];
    char real[PATH_MAX];
    char *d = mkdtemp(tmpl);
    if (!d) { perror("mkdtemp"); exit(2); }
    if (!realpath(d, real)) { perror("realpath"); exit(2); }
    snprintf(base, sizeof base, "%.200s", real);

    snprintf(root, sizeof root, "%s/site", base);
    snprintf(outside, sizeof outside, "%s/outside", base);
    mkdir(root, 0755);
    mkdir(outside, 0755);
    snprintf(p, sizeof p, "%s/secret.txt", outside);
    { FILE *f = fopen(p, "w"); fputs("TOP-SECRET", f); fclose(f); }

    put("index.html", "root index");
    put("hello.txt", "hello");
    put("a..b.txt", "dots inside a name are fine");
    put("%2e%2e", "a file literally named %2e%2e");
    put("%41.txt", "literal percent name");
    make_dir("sub");
    put("sub/index.html", "sub index");
    put("sub/page.txt", "page");
    make_dir("emptydir");
    make_dir("noindex");

    snprintf(p, sizeof p, "%s/escape", root);               /* symlink -> outside file */
    snprintf(q, sizeof q, "%s/secret.txt", outside);
    must_symlink(q, p);
    snprintf(p, sizeof p, "%s/escapedir", root);            /* symlink -> outside dir  */
    must_symlink(outside, p);
    snprintf(p, sizeof p, "%s/inlink", root);               /* symlink inside root     */
    must_symlink("hello.txt", p);
    snprintf(p, sizeof p, "%s/fifo", root);
    mkfifo(p, 0644);

    /* a sibling whose name starts with the root's name: "site2" */
    snprintf(p, sizeof p, "%s/site2", base);
    mkdir(p, 0755);
    snprintf(q, sizeof q, "%s/site2/x.txt", base);
    { FILE *f = fopen(q, "w"); fputs("sibling", f); fclose(f); }
    snprintf(p, sizeof p, "%s/sibling", root);
    snprintf(q, sizeof q, "%s/site2", base);
    must_symlink(q, p);
}

static PathStatus res(const char *path, char *out)
{
    return resolve_path(root, (const uint8_t *)path, strlen(path), out, PATH_MAX);
}

static void expect(const char *path, PathStatus want, const char *want_file)
{
    char out[PATH_MAX] = "";
    PathStatus got = res(path, out);
    char full[PATH_MAX + 64];

    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL %-34s expected status %d got %d (%s)\n", path, want, got, out);
        return;
    }
    if (want == PATH_OK && want_file != NULL) {
        snprintf(full, sizeof full, "%s/%s", root, want_file);
        checks++;
        if (strcmp(out, full) != 0) {
            failures++;
            printf("  FAIL %-34s resolved to %s, expected %s\n", path, out, full);
        }
    }
}

int main(void)
{
    char out[PATH_MAX], cmd[PATH_MAX + 32];
    char big[5000];

    fixture();

    puts("-- ordinary paths");
    expect("/", PATH_OK, "index.html");
    expect("/index.html", PATH_OK, "index.html");
    expect("/hello.txt", PATH_OK, "hello.txt");
    expect("/sub", PATH_OK, "sub/index.html");
    expect("/sub/", PATH_OK, "sub/index.html");
    expect("/sub/page.txt", PATH_OK, "sub/page.txt");

    puts("-- repeated slashes and dot segments are collapsed");
    expect("//", PATH_OK, "index.html");
    expect("//hello.txt", PATH_OK, "hello.txt");
    expect("/sub//page.txt", PATH_OK, "sub/page.txt");
    expect("/./hello.txt", PATH_OK, "hello.txt");
    expect("/sub/./page.txt", PATH_OK, "sub/page.txt");
    expect("///sub///./page.txt", PATH_OK, "sub/page.txt");

    puts("-- '..' is always rejected, even when it would stay inside the root");
    expect("/..", PATH_BAD, NULL);
    expect("/../", PATH_BAD, NULL);
    expect("/../hello.txt", PATH_BAD, NULL);
    expect("/sub/../hello.txt", PATH_BAD, NULL);
    expect("/../../etc/passwd", PATH_BAD, NULL);
    expect("/sub/../../outside/secret.txt", PATH_BAD, NULL);
    expect("/a/b/c/../../../hello.txt", PATH_BAD, NULL);
    expect("/hello.txt/..", PATH_BAD, NULL);

    puts("-- '..' as part of a longer name is just a name");
    expect("/a..b.txt", PATH_OK, "a..b.txt");
    expect("/...", PATH_NOT_FOUND, NULL);
    expect("/..a", PATH_NOT_FOUND, NULL);

    puts("-- syntax errors");
    expect("", PATH_BAD, NULL);
    expect("index.html", PATH_BAD, NULL);
    expect("hello.txt", PATH_BAD, NULL);
    expect("./hello.txt", PATH_BAD, NULL);
    expect("../hello.txt", PATH_BAD, NULL);
    expect("\\hello.txt", PATH_BAD, NULL);
    expect("/sub\\page.txt", PATH_BAD, NULL);
    expect("/..\\..\\etc\\passwd", PATH_BAD, NULL);
    expect("/a\nb", PATH_BAD, NULL);
    expect("/a\tb", PATH_BAD, NULL);
    expect("/a\x7f", PATH_BAD, NULL);
    CHECK(resolve_path(root, (const uint8_t *)"/a\0b", 4, out, sizeof out) == PATH_BAD);   /* NUL byte */
    CHECK(resolve_path(root, (const uint8_t *)"/hello.txt\0", 11, out, sizeof out) == PATH_BAD);
    CHECK(resolve_path(root, NULL, 0, out, sizeof out) == PATH_BAD);

    puts("-- absolute-looking paths are relative to the root");
    expect("/etc/passwd", PATH_NOT_FOUND, NULL);
    expect("//etc/passwd", PATH_NOT_FOUND, NULL);
    snprintf(cmd, sizeof cmd, "%s/secret.txt", outside);
    expect(cmd, PATH_NOT_FOUND, NULL);                      /* the outside file's real path */

    puts("-- percent-encoding is NOT decoded");
    expect("/%2e%2e", PATH_OK, "%2e%2e");                   /* a real file with that name */
    expect("/%2e%2e/%2e%2e/etc/passwd", PATH_NOT_FOUND, NULL);
    expect("/%2e%2e%2fhello.txt", PATH_NOT_FOUND, NULL);
    expect("/%41.txt", PATH_OK, "%41.txt");
    expect("/A.txt", PATH_NOT_FOUND, NULL);

    puts("-- missing files, directories, special files");
    expect("/nope.txt", PATH_NOT_FOUND, NULL);
    expect("/sub/nope", PATH_NOT_FOUND, NULL);
    expect("/hello.txt/x", PATH_NOT_FOUND, NULL);           /* ENOTDIR */
    expect("/hello.txt/", PATH_NOT_FOUND, NULL);            /* file with trailing slash */
    expect("/emptydir", PATH_NOT_FOUND, NULL);              /* dir without index.html */
    expect("/emptydir/", PATH_NOT_FOUND, NULL);
    expect("/noindex", PATH_NOT_FOUND, NULL);
    expect("/fifo", PATH_NOT_FOUND, NULL);                  /* not a regular file */

    puts("-- symlinks");
    expect("/inlink", PATH_OK, "hello.txt");                /* stays inside: allowed */
    expect("/escape", PATH_NOT_FOUND, NULL);                /* points outside: refused */
    expect("/escapedir", PATH_NOT_FOUND, NULL);
    expect("/escapedir/secret.txt", PATH_NOT_FOUND, NULL);
    expect("/sibling/x.txt", PATH_NOT_FOUND, NULL);         /* /site2 shares a name prefix */

    puts("-- length limits");
    memset(big, 'a', sizeof big);
    big[0] = '/';
    CHECK(resolve_path(root, (const uint8_t *)big, 4097, out, sizeof out) == PATH_BAD);
    CHECK(resolve_path(root, (const uint8_t *)big, 4096, out, sizeof out) == PATH_NOT_FOUND);
    CHECK(resolve_path(root, (const uint8_t *)"/hello.txt", 10, out, 5) == PATH_NOT_FOUND);   /* out too small */

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    if (system(cmd) != 0) { /* best effort cleanup */ }

    printf("\n%d checks, %d failed\n", checks, failures);
    if (failures) {
        printf("UNIT TESTS FAILED (pathres)\n");
        return 1;
    }
    printf("ALL PATH-RESOLVER UNIT TESTS PASSED\n");
    return 0;
}
