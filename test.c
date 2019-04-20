#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <malloc.h>
#include <stdint.h>
#include <stdbool.h>

void *thmalloc(size_t size);

void pdword(void *p) {
    for (int byte = 0; byte < 8; byte++) {
        printf("%02x", ((char*)p)[byte]);
        if (byte == 3) {
            printf(" ");
        }
    }
    printf("\n");
}


int main() {
//    void *p = 124;// = malloc(3);

    void *pointers[20];


    printf("Hello!\n");
    printf("getpid() = %d\n", getpid());

  
    void *foo = malloc(4097);
    free(foo);
    
    foo = malloc(2);
    free(foo);

    foo = malloc(3);
    free(foo);


    return 0;

    void *pages[1000];

    size_t _1_MB = 1 * 1024 * 1024;
    size_t _1_GB = 1024 * _1_MB;
 
    size_t MALLOC_SIZE = 10;
    size_t ALLOCATIONS = 10;

    printf("sizeof(void) = %ld\n", sizeof(void));
    printf("sizeof(void*) = %ld\n", sizeof(void*));
    printf("sizeof(int) = %ld\n", sizeof(int));
    
   /* 
    for (int i = 0; i < ALLOCATIONS; i++) {
        p = malloc(MALLOC_SIZE);
        if (p == NULL) {
            perror("malloc failed");
            return -1;
        }
        pages[i] = p;
        printf("Pointer of allocation %d : %p\n", i, p);

        for (int j = 0; j < MALLOC_SIZE; j++) {
            ((char*)pages[i])[j] = 0x1b;               
        }
        
    }
    */

    if (!mallopt(M_CHECK_ACTION, 0x3)) {
        perror("mallopt");
        exit(EXIT_FAILURE);
    }
    void *p;
    for (int i = 0; i < ALLOCATIONS; i++) {
        p = malloc(MALLOC_SIZE);
        printf("Address of p: %p\n", p);
        char *c = (char*)p;
        printf("Address of c: %p\n", c);
        *((char*)p) = 0;
        *((char*)p+1) = 1;
        *((char*)p+2) = 2;
        *((char*)p+3) = 3;

        pdword(p);
        pdword(p + 16);
        free(p);
        free(p);
    }

/*    for (int i = 0; i < ALLOCATIONS; i += 2) {
        if (munmap(p, MALLOC_SIZE) == -1) {
            perror("munmap failed");
        }
    }
*/
    printf("End of main() reached. Sleeping for analysis...\n");
    sleep(4 * 60 * 60);
}
