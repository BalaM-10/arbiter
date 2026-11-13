/*
 * Cross-language contract test (FR-C3, FR-C4). Runs the corpus written by the
 * host compiler (`arbiter contract corpus`, host/arbiter/corpus.py):
 *   - each "ok" table must load with arb_contract_load() and, printed field by
 *     field, must equal the compiler's own dump of the source contract;
 *   - each "reject" table must be refused with exactly the expected code.
 * Built for the host (pytest) and for QNX (scripts/run-tests.sh).
 *
 *   test_contract_file <corpus-dir>/manifest.txt
 *
 * Test code: uses stdio file I/O, which trusted code never does.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arb_contract.h"

#define MAX_FILE  4096
#define MAX_LINE  512
#define MAX_DIR   1024
#define MAX_NAME  256
#define MAX_PATH  (MAX_DIR + MAX_NAME + 2)

/* dir + "/" + name into out; 0 on success, -1 if it would not fit. */
static int join_path(char *out, size_t cap, const char *dir, const char *name)
{
    const int n = snprintf(out, cap, "%s/%s", dir, name);

    if (n < 0 || (size_t)n >= cap) {
        printf("  path too long: %s/%s\n", dir, name);
        return -1;
    }
    return 0;
}

static int read_file(const char *path, unsigned char *buf, size_t cap, size_t *len)
{
    FILE *f = fopen(path, "rb");
    size_t n;

    if (f == NULL) {
        printf("  cannot open %s\n", path);
        return -1;
    }
    n = fread(buf, 1, cap, f);
    if (ferror(f) || (n == cap && fgetc(f) != EOF)) {
        printf("  cannot read %s (or larger than %zu bytes)\n", path, cap);
        fclose(f);
        return -1;
    }
    fclose(f);
    *len = n;
    return 0;
}

/* Same format as host/arbiter/table.py Table.dump(). */
static int dump_table(const arb_contract_table_t *t, char *out, size_t cap)
{
    size_t pos = 0;
    int n;

#define EMIT(...)                                                        \
    do {                                                                 \
        n = snprintf(out + pos, cap - pos, __VA_ARGS__);                 \
        if (n < 0 || (size_t)n >= cap - pos) { return -1; }              \
        pos += (size_t)n;                                                \
    } while (0)

    EMIT("magic=0x%08x\n", (unsigned)t->magic);
    EMIT("version=%u\n", (unsigned)t->version);
    EMIT("table_size=%u\n", (unsigned)t->table_size);
    EMIT("source_sha256=");
    for (int i = 0; i < 32; i++) {
        EMIT("%02x", (unsigned)t->source_sha256[i]);
    }
    EMIT("\n");
    EMIT("joint_min_urad=%d,%d\n", (int)t->joint_min_urad[0], (int)t->joint_min_urad[1]);
    EMIT("joint_max_urad=%d,%d\n", (int)t->joint_max_urad[0], (int)t->joint_max_urad[1]);
    EMIT("vmax_urad_s=%u\n", (unsigned)t->vmax_urad_s);
    EMIT("silence_timeout_ms=%u\n", (unsigned)t->silence_timeout_ms);
    EMIT("rate_per_s=%u\n", (unsigned)t->rate_per_s);
    EMIT("rate_burst=%u\n", (unsigned)t->rate_burst);
    EMIT("violation_threshold=%u\n", (unsigned)t->violation_threshold);
    EMIT("violation_window_ms=%u\n", (unsigned)t->violation_window_ms);
    EMIT("max_duration_ms=%u\n", (unsigned)t->max_duration_ms);
    EMIT("zone_count=%u\n", (unsigned)t->zone_count);
    for (int z = 0; z < ARB_MAX_ZONES; z++) {
        EMIT("zone%d=%d,%d,%d,%d\n", z, (int)t->zones[z].x_min_um, (int)t->zones[z].y_min_um,
             (int)t->zones[z].x_max_um, (int)t->zones[z].y_max_um);
    }
    EMIT("crc32=0x%08x\n", (unsigned)t->crc32);
#undef EMIT
    return 0;
}

static int fr_c3_compiled_table_matches_source_dump(const char *dir, const char *bin, const char *dump)
{
    unsigned char data[MAX_FILE];
    unsigned char expect[MAX_FILE + 1];
    char got[MAX_FILE];
    char path[MAX_PATH];
    size_t len, elen;
    arb_contract_table_t t;
    arb_load_result_t r;

    if (join_path(path, sizeof path, dir, bin) != 0 || read_file(path, data, sizeof data, &len) != 0) {
        return 0;
    }
    r = arb_contract_load(data, len, &t);
    if (r != ARB_LOAD_OK) {
        printf("  %s: load failed with code %d\n", bin, (int)r);
        return 0;
    }
    if (join_path(path, sizeof path, dir, dump) != 0 || read_file(path, expect, sizeof expect - 1, &elen) != 0) {
        return 0;
    }
    expect[elen] = '\0';
    if (dump_table(&t, got, sizeof got) != 0) {
        printf("  dump buffer too small\n");
        return 0;
    }
    if (strcmp(got, (const char *)expect) != 0) {
        printf("  %s: loader view differs from the compiler's dump\n--- loader\n%s--- compiler\n%s",
               bin, got, (const char *)expect);
        return 0;
    }
    return 1;
}

static int fr_c4_corrupted_compiled_table_rejected(const char *dir, const char *bin, int expected)
{
    unsigned char data[MAX_FILE];
    char path[MAX_PATH];
    size_t len;
    arb_contract_table_t t;
    arb_load_result_t r;

    if (join_path(path, sizeof path, dir, bin) != 0 || read_file(path, data, sizeof data, &len) != 0) {
        return 0;
    }
    r = arb_contract_load(data, len, &t);
    if ((int)r != expected) {
        printf("  %s: expected code %d, got %d\n", bin, expected, (int)r);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    char dir[MAX_DIR];
    char line[MAX_LINE];
    char *slash;
    FILE *m;
    int total = 0, passed = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc != 2) {
        printf("usage: %s <corpus-dir>/manifest.txt\n", argv[0]);
        return 2;
    }
    snprintf(dir, sizeof dir, "%s", argv[1]);
    slash = strrchr(dir, '/');
    if (slash != NULL) {
        *slash = '\0';
    } else {
        snprintf(dir, sizeof dir, ".");
    }
    m = fopen(argv[1], "r");
    if (m == NULL) {
        printf("FAIL cannot open manifest %s\n", argv[1]);
        return 1;
    }
    while (fgets(line, sizeof line, m) != NULL) {
        char kind[16], a[256], b[256];
        int ok;

        if (sscanf(line, "%15s %255s %255s", kind, a, b) != 3) {
            continue;
        }
        total++;
        if (strcmp(kind, "ok") == 0) {
            ok = fr_c3_compiled_table_matches_source_dump(dir, a, b);
            printf("%s fr_c3_compiled_table_matches_source_dump %s\n", ok ? "PASS" : "FAIL", a);
        } else if (strcmp(kind, "reject") == 0) {
            ok = fr_c4_corrupted_compiled_table_rejected(dir, a, atoi(b));
            printf("%s fr_c4_corrupted_compiled_table_rejected %s (code %s)\n", ok ? "PASS" : "FAIL", a, b);
        } else {
            ok = 0;
            printf("FAIL unknown manifest entry: %s", line);
        }
        passed += ok;
    }
    fclose(m);
    printf("RESULT %s test_contract_file %d/%d\n",
           (passed == total && total > 0) ? "PASS" : "FAIL", passed, total);
    return (passed == total && total > 0) ? 0 : 1;
}
