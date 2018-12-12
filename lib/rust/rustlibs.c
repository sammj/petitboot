/*
 *  Copyright (C) 2018 IBM Corporation
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 */

#include <log/log.h>
#include <talloc/talloc.h>

#include "rustlibs.h"

enum field_type {
	FIELD_STR,
	FIELD_I64,
	FIELD_F64,
};

/* Internal FFI Structs */
struct ArgumentRaw {
	char		*name;
	enum field_type type;
	int64_t		arg_i64;
	double		arg_f64;
	char		*arg_str;
};

struct CommandRaw {
	char			*platform;
	char			*name;
	char			*cmd;
	char			*args_fmt;
	struct ArgumentRaw	*args;
	size_t			n_args;
	char			*help;
};

struct CommandArray {
	struct CommandRaw	*commands;
	size_t			len;
};

/* Provided by librustlibs */
struct CommandArray *parse_json(const char *filename);
void free_command_array(struct CommandArray *commands);

int parse_command_file(void *ctx, const char *path, struct command **commands)
{
	struct CommandArray *raw_commands;
	struct CommandRaw *tmp;
	struct command *new;
	struct argument *arg;
	unsigned int i, j, len;

	raw_commands = parse_json(path);
	if (!raw_commands) {
		pb_log("Failed to parse machine command\n");
		return 0;
	}

	len = raw_commands->len;
	new = talloc_zero_array(ctx, struct command, len);
	for (i = 0; i < len; i++) {
		tmp = &raw_commands->commands[i];
		new[i].platform = talloc_strdup(new, tmp->platform);
		new[i].name = talloc_strdup(new, tmp->name);
		new[i].cmd = talloc_strdup(new, tmp->cmd);
		new[i].args_fmt = talloc_strdup(new, tmp->args_fmt);

		new[i].n_args = tmp->n_args;
		new[i].args = talloc_zero_array(new, struct argument,
				new[i].n_args);
		for (j = 0; j < tmp->n_args; j++) {
			arg = &new[i].args[j];
			arg->name = talloc_strdup(new, tmp->args[j].name);
			arg->type = tmp->args[j].type;
			switch (tmp->args[j].type) {
			case FIELD_STR:
				arg->arg_str = talloc_strdup(new,
						tmp->args[j].arg_str);
				break;
			case FIELD_I64:
				arg->arg_i64 = tmp->args[j].arg_i64;
				break;
			case FIELD_F64:
				arg->arg_f64 = tmp->args[j].arg_f64;
				break;
			default:
				pb_log_fn("Unknown field type %d for arg '%s'\n",
						tmp->args[j].type, tmp->args[j].name);
				break;
			}
		}

		new[i].help = talloc_strdup(new, tmp->help);
	}

	free_command_array(raw_commands);

	*commands = new;
	return len;
}
