#include <stdio.h>

void main()
{
    struct boundary_tag
    {
        int inuse : 1;
        int size : 31;
    };

    struct list_elem
    {
        struct list_elem *prev; /* Previous list element. */
        struct list_elem *next; /* Next list element. */
    };

    struct deallocated_block
    {
        struct boundary_tag header; /* Offset 0, at address 12 mod 16 */
        struct list_elem elem;
        char padding[8];
    };

    // size_t size;

    printf("sizeof(struct boundary_tag): %d\n", sizeof(struct boundary_tag));
    printf("sizeof(struct list_elem): %d\n", sizeof(struct list_elem));
    printf("sizeof(struct deallocated_block): %d\n", sizeof(struct deallocated_block));
    printf("sizeof(size_t): %d\n", sizeof(size_t));
}