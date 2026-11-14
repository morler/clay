#include "clay/prompt.h"
#include "clay/term.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    clay_term_init();
    char *line = clay_prompt_line(NULL);
    if (!line) return 1;
    if (argc > 1) {
        FILE *file = fopen(argv[1], "wb");
        if (!file) return 2;
        fputs(line, file);
        fclose(file);
    }
    printf("\nRESULT:%s\n", line);
    free(line);
    return 0;
}
