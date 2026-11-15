#include "box/print.h"
#include "box/file.h"
#include "box/system.h"
#include "box/error.h"

/*
 * escheat — let go for good what a book can never give back: every
 * rotten range of FILES its Tally names, the files in it declared lost and the
 * blocks held for them freed once the next Tally names none of it, and every
 * DIGESTS leaf that reads in no copy laid anew of the data. Destructive and
 * final: save what can be saved first (volume-read
 * on another machine reads a rotten range's blocks unchecked). Only a program
 * wearing system or god may ask.
 */
int main(void)
{
    EscheatAnswer said = { 0 };
    int           rc   = escheat(&said);

    if (rc == -ERR_OBJECT_NOT_FOUND) {
        println("escheat: nothing to let go — no rotten range of FILES, no DIGESTS leaf known rotten, no tail "
                "that can never be laid");
        exit(0);
        return 0;
    }
    if (rc < 0) {
        printf("escheat: refused (%d)\n", rc);
        exit(1);
        return 1;
    }
    printf("escheat: %lu rotten range(s) let go — %lu file(s) declared lost, %lu block(s) freed, %lu DIGESTS "
           "range(s) laid anew of the data, %lu tail(s) lost, once the next Tally is written\n",
           (unsigned long)said.ranges, (unsigned long)said.files, (unsigned long)said.blocks,
           (unsigned long)said.digest_ranges, (unsigned long)said.tails);
    exit(0);
    return 0;
}
