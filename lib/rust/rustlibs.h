#ifndef __RUST_LIBS__
#define __RUST_LIBS__

#include <stddef.h>
#include <stdint.h>

#include <types/types.h>

int parse_command_file(void *ctx, const char *path, struct command **commands);

#endif /* __RUST_LIBS__ */
