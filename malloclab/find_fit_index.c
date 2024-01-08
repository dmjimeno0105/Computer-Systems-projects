#include <stdio.h>
#include <stdlib.h>

#define MIN_BLOCK_SIZE_WORDS 8

static int find_fit_index(size_t size);

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <number>\n", argv[0]);
        return 1; // Exit with an error status
    }

    // Convert the command line string to an integer
    size_t input = (size_t)atoi(argv[1]);

    printf("actual out: %d\n", find_fit_index(input));

    return 0; // Exit with a success status
}

static int find_fit_index(size_t words)
{
    if (words < MIN_BLOCK_SIZE_WORDS)
    {
        return -1;
    }
    else if (words < 16)
    {
        return 0;
    }
    else if (words < 32)
    {
        return 1;
    }
    else if (words < 64)
    {
        return 2;
    }
    else if (words < 128)
    {
        return 3;
    }
    else
    {
        return 4;
    }
}