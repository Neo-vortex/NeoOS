#include <unistd.h>
#include <string.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    const char msg[] = "spin test program running\n";
    write(msg, strlen(msg));
    return 0;
}
