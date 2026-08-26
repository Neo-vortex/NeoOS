#include <unistd.h>
#include <string.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    const char msg[] = "child running, exiting with code 42\n";
    write(msg, strlen(msg));
    return 42;
}
