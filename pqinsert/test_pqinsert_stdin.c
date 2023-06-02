/**
 * Copyright 2023, University Corporation for Atmospheric Research
 * See the file COPYRIGHT in the top-level source-directory for copying and
 * redistribution conditions.
 *
 * @author Michael Klatt
 * @file   Test execution of the `pqinsert` command with input from STDIN
 */
#include <errno.h>
#include <ftw.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>


#define MAXLEN 1024


static const char* tmpdir = "/tmp/test_pqinsert_stdin";  // TODO: use mkdtemp()
static const char* product = "TEST PRODUCT";

static char queue[MAXLEN];


/**
 * Set up the test fixture.
 */
void setup()
{
    snprintf(queue, MAXLEN, "%s/ldm.pq", tmpdir);
    const int mode = S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH;  // 0775
    if (mkdir(tmpdir, mode) != 0) {
        perror("ERROR setup() failed: mkdir");
        exit(errno);
    }
    char command[MAXLEN];
    snprintf(command, MAXLEN, "../pqcreate/pqcreate -f -s 10k -S 10 -q %s", queue);
    int status = system(command);
    if (status != EXIT_SUCCESS) {
        fprintf(stderr, "ERROR setup() failed: pqcreate failed: %d", status);
        exit(EXIT_FAILURE);
    }
}


/**
 * Callback function to remove a path.
 *
 * @param path to remove
 * @return 0 on success
 */
int ftw_remove(const char *path,
               __attribute__((unused)) const struct stat *sb,
               __attribute__((unused)) int typeflag,
               __attribute__((unused)) struct FTW *ftwbuf)
{
    if (remove(path) != 0) {
        perror("ERROR ftw_remove() failed: remove");
        return 1;  // abort traversal
    }
    return 0;
}


/**
 * Tear down the test fixture.
 */
void teardown()
{
    // Visit every item in the directory tree (including the root) and remove
    // it. The FTW_DEPTH option enables postorder traversal so that each
    // directory is visited after all of its entries have already been removed.
    const int flags = FTW_DEPTH | FTW_MOUNT | FTW_PHYS;
    if (nftw(tmpdir, ftw_remove, 256, flags) != 0) {
        perror("WARN teardown() failed");
    }
}


/**
 * Test `pqinsert` execution.
 *
 * @return true on success
 */
bool test_pqinsert() {
    // For now, product needs to be written to a temporary file.
    char tmp_path[MAXLEN];
    snprintf(tmp_path, MAXLEN, "%s/test.tmp", tmpdir);
    FILE* tmp_file = fopen(tmp_path, "wb");
    if (!tmp_file) {
        perror("test_pqinsert: cannot write tmp file");
        exit(errno);
    }
    fwrite(product, sizeof(char), strlen(product), tmp_file);
    fclose(tmp_file);

    // Act as if `pqinsert` reads from STDIN, although for now it needs a
    // physical file for input.
    char command[MAXLEN];
    snprintf(command, MAXLEN, "./pqinsert -q %s -p test %s", queue, tmp_path);
    FILE* stream = popen(command, "w");
    if (!stream) {
        perror("test_pqinsert: could not execute pqinsert");
        exit(errno);
    }
    fwrite(product, sizeof(char), strlen(product), stream);  // no effect yet
    return pclose(stream) == EXIT_SUCCESS;
}


/**
 * Test that product exists in the queue.
 */
bool test_queue()
{
    char command[MAXLEN];
    snprintf(command, MAXLEN, "../pqcat/pqcat -q %s", queue);
    FILE* stream = popen(command, "r");
    if (!stream) {
        perror("test_queue: could not execute pqcat");
    }
    char buffer[MAXLEN];
    fread(buffer, sizeof(char), MAXLEN, stream);
    return strncmp(buffer, product, strlen(product)) == 0;
}


/**
 * Execute tests.
 *
 * @return      EXIT_SUCCESS if all tests were successful
 */
int main()
{
    atexit(teardown);
    setup();
    int failed = 0;
    if (!test_pqinsert()) {
        ++failed;
        fprintf(stderr, "FAIL test_pqinsert\n");
    }
    if (!test_queue()) {
        ++failed;
        fprintf(stderr, "FAIL test_queue\n");
    }
    return failed;
}
