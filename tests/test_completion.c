#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include "completion.h"
#include "shell.h"
#include "var.h"
#include "alias.h"
#include "memalloc.h"

int main() {
    arena_init(&parse_arena);
    memset(&sh, 0, sizeof(sh));
    var_init();
    alias_init();

    /* Test path completion prefix */
    {
        struct completion_result *cr = completion_get("ls src/ma", 9);
        assert(cr != NULL);
        assert(cr->count >= 1);
        int found = 0;
        for (size_t i = 0; i < cr->count; i++) {
            if (strcmp(cr->matches[i], "src/main.c") == 0) found = 1;
        }
        assert(found);
        completion_free(cr);
    }

    /* Test command completion (builtins) */
    {
        struct completion_result *cr = completion_get("exi", 3);
        assert(cr != NULL);
        assert(cr->count >= 1);
        int found = 0;
        for (size_t i = 0; i < cr->count; i++) {
            if (strcmp(cr->matches[i], "exit") == 0) found = 1;
        }
        assert(found);
        completion_free(cr);
    }

    printf("All completion tests passed!\n");
    return 0;
}
