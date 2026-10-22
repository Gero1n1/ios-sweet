/* Inert installer test double: logs arguments and never opens a USB device. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *scenario = getenv("SWEET_TEST_SCENARIO");
    const char *log_path = getenv("SWEET_TEST_LOG");
    FILE *log;
    int i, product_queries = 0;
    char line[4096];
    if (!scenario || !log_path) return 98;
    log = fopen(log_path, "a");
    if (!log) return 98;
    for (i = 1; i < argc; ++i) fprintf(log, "%s%s", i == 1 ? "" : "\t", argv[i]);
    fputc('\n', log); fclose(log);
    if (argc == 2 && strcmp(argv[1], "devices") == 0) {
        if (strcmp(scenario, "none") != 0) puts("MOCKSWEET001\tfastboot");
        if (strcmp(scenario, "multiple") == 0) puts("MOCKSWEET002\tfastboot");
        return 0;
    }
    /* Any target operation without an explicit serial is a test failure. */
    if (argc < 4 || strcmp(argv[1], "-s") || strcmp(argv[2], "MOCKSWEET001")) return 97;
    i = 3;
    if (strcmp(argv[i], "-S") == 0) i += 2;
    if (i >= argc) return 96;
    if (strcmp(argv[i], "getvar") == 0 && i + 1 < argc) {
        const char *name = argv[i + 1];
        if (strcmp(name, "product") == 0) {
            log = fopen(log_path, "r");
            if (!log) return 98;
            while (fgets(line, sizeof(line), log))
                if (strstr(line, "\tgetvar\tproduct")) ++product_queries;
            fclose(log);
            fprintf(stderr, "(bootloader) product: %s\n",
                !strcmp(scenario, "wrongproduct") ||
                (!strcmp(scenario, "changed") && product_queries > 1) ? "other" : "sweet");
            return !strcmp(scenario, "queryfail") ? 2 : 0;
        }
        if (!strcmp(name, "unlocked")) {
            fprintf(stderr, "unlocked: %s\n", !strcmp(scenario, "locked") ? "no" : "yes");
            return 0;
        }
        if (!strcmp(name, "is-userspace")) {
            fprintf(stderr, "is-userspace: %s\n", !strcmp(scenario, "fastbootd") ? "yes" : "no");
            return 0;
        }
        return 95;
    }
    if (!strcmp(argv[i], "flash")) return !strcmp(scenario, "flashfail") ? 3 : 0;
    if (!strcmp(argv[i], "boot")) return !strcmp(scenario, "bootfail") ? 4 : 0;
    return 94;
}
