#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d_main.h"
#include "m_argv.h"

#define DOOM_ARGUMENT_LIMIT 16
#define DOOM_COMMAND_LINE_SIZE 256

static char command_line[DOOM_COMMAND_LINE_SIZE];
static char *command_arguments[DOOM_ARGUMENT_LIMIT];

int main(int argc, char **argv)
{
    (void)argc;
    myargc = 1;
    command_arguments[0] = (char *)"doom";

    if (argv[1] && argv[1][0])
    {
        size_t length = strlen(argv[1]);
        if (length >= sizeof(command_line))
        {
            fputs("doom: command line is too long\n", stderr);
            return 2;
        }
        memcpy(command_line, argv[1], length + 1);

        char *cursor = command_line;
        while (*cursor)
        {
            while (*cursor == ' ' || *cursor == '\t')
                cursor++;
            if (!*cursor)
                break;
            if (myargc >= DOOM_ARGUMENT_LIMIT - 1)
            {
                fputs("doom: too many command-line arguments\n", stderr);
                return 2;
            }

            char quote = 0;
            if (*cursor == '"' || *cursor == '\'')
                quote = *cursor++;
            command_arguments[myargc++] = cursor;

            while (*cursor &&
                   (quote ? *cursor != quote
                          : (*cursor != ' ' && *cursor != '\t')))
                cursor++;
            if (quote && *cursor != quote)
            {
                fputs("doom: unmatched quote in command line\n", stderr);
                return 2;
            }
            if (*cursor)
                *cursor++ = '\0';
        }
    }

    command_arguments[myargc] = NULL;
    myargv = command_arguments;
    D_DoomMain();
    return 0;
}
