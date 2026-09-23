#include "lmux2_runtime.h"
#include "supervisor.h"

#include <string.h>

int main(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++)
        if (strcmp(argv[i], "--supervisor") == 0)
            return lmux_supervisor_main(argc, argv);
    return lmux2_runtime_main(argc, argv);
}
