
#define _GNU_SOURCE

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <types/types.h>
#include <talloc/talloc.h>
#include <pb-protocol/pb-protocol.h>

static int test_command_protocol(void *ctx)
{
	unsigned int len, write_len, read_len;
	struct command *write, *read;
	const char *read_buf;
	char *buf, *pos;

	write = talloc_zero(ctx, struct command);
	if (!write)
		return -1;

	write->platform = talloc_asprintf(ctx, "platform");
	write->name = talloc_asprintf(ctx, "test config");
	write->cmd = talloc_asprintf(ctx, "command");
	write->args_fmt = talloc_asprintf(ctx, "{} {} {}");
	write->n_args = 3;
	write->args = talloc_zero_array(write, struct argument, write->n_args);

	write->args[0].name = talloc_asprintf(write, "arg 0");
	write->args[0].type = ARG_STR;
	write->args[0].arg_str = talloc_asprintf(write, "string arg");

	write->args[1].name = talloc_asprintf(write, "arg 1");
	write->args[1].type = ARG_I64;
	write->args[1].arg_i64 = 5;

	write->args[2].name = talloc_asprintf(write, "arg 2");
	write->args[2].type = ARG_F64;
	write->args[2].arg_f64 = 4.4;

	write->help = talloc_asprintf(write, "good luck");

	len = pb_protocol_command_len(write);
	fprintf(stderr, "write config is %d bytes\n", len);

	buf = talloc_array(ctx, char, len);
	pos = buf;

	write_len = pb_protocol_serialise_command(pos, write);
	if (write_len != len) {
		fprintf(stderr, "Failed to serialise machine config\n");
		fprintf(stderr, "Serialised length does not match expected (%lu vs %d)\n",
				pos - buf, write_len);
		return -1;
	}

	read_buf = talloc_memdup(ctx, buf, write_len);
	read_len = write_len;
	read = talloc_zero(ctx, struct command);
	if (pb_protocol_deserialise_command(ctx, &read_buf, &read_len, read)) {
		fprintf(stderr, "Failed to deserialise machine config\n");
		return -1;
	}

	if (strcmp(write->platform, read->platform)) {
		fprintf(stderr, "platform field does not match: %s vs %s\n",
				write->platform, read->platform);
		return -1;
	}
	if (strcmp(write->name, read->name)) {
		fprintf(stderr, "name field does not match: %s vs %s\n",
				write->name, read->name);
		return -1;
	}
	if (strcmp(write->cmd, read->cmd)) {
		fprintf(stderr, "cmd field does not match: %s vs %s\n",
				write->cmd, read->cmd);
		return -1;
	}
	if (strcmp(write->args_fmt, read->args_fmt)) {
		fprintf(stderr, "args_fmt field does not match: %s vs %s\n",
				write->args_fmt, read->args_fmt);
		return -1;
	}

	if (write->n_args != read->n_args) {
		fprintf(stderr, "n_args mismatch: %u vs %u\n", write->n_args,
				read->n_args);
		return -1;
	}

	if (strcmp(write->args[0].name, read->args[0].name)) {
		fprintf(stderr, "arg 0 name does not match: %s vs %s\n",
				write->args[0].name, read->args[0].name);
		return -1;
	}
	if (write->args[0].type != read->args[0].type) {
		fprintf(stderr, "arg 0 field type does not match: %d vs %d\n",
				write->args[0].type, read->args[0].type);
		return -1;
	}
	if (strcmp(write->args[0].arg_str, read->args[0].arg_str)) {
		fprintf(stderr, "arg 0 arg does not match: %s vs %s\n",
				write->args[0].arg_str, read->args[0].arg_str);
		return -1;
	}

	if (strcmp(write->args[1].name, read->args[1].name)) {
		fprintf(stderr, "arg 1 name does not match: %s vs %s\n",
				write->args[1].name, read->args[1].name);
		return -1;
	}
	if (write->args[1].type != read->args[1].type) {
		fprintf(stderr, "arg 1 field type does not match: %d vs %d\n",
				write->args[1].type, read->args[1].type);
		return -1;
	}
	if (write->args[1].arg_i64 != read->args[1].arg_i64) {
		fprintf(stderr, "arg 1 field arg does not match: %ld vs %ld\n",
				write->args[1].arg_i64, read->args[1].arg_i64);
		return -1;
	}

	if (strcmp(write->args[2].name, read->args[2].name)) {
		fprintf(stderr, "arg 2 name does not match: %s vs %s\n",
				write->args[2].name, read->args[2].name);
		return -1;
	}
	if (write->args[2].type != read->args[2].type) {
		fprintf(stderr, "arg 2 field type does not match: %d vs %d\n",
				write->args[2].type, read->args[2].type);
		return -1;
	}
	if (write->args[2].arg_f64 != read->args[2].arg_f64) {
		fprintf(stderr, "arg 2 field arg does not match: %f vs %f\n",
				write->args[2].arg_f64, read->args[2].arg_f64);
		return -1;
	}

	return 0;
}

int main(void)
{
	void *ctx;
	int rc = 0;

	ctx = talloc_new(NULL);

	rc = test_command_protocol(ctx);
	if (rc)
		printf("FAIL: test_command\n");

	talloc_free(ctx);

	return rc ? EXIT_FAILURE : EXIT_SUCCESS;
}
