#ifndef _PB_PROTOCOL_H
#define _PB_PROTOCOL_H

#include <stdint.h>
#include <stdio.h>

#include <list/list.h>
#include <types/types.h>

#define PB_SOCKET_PATH "/tmp/petitboot.ui"

#define PB_PROTOCOL_MAX_PAYLOAD_SIZE (64 * 1024)

enum pb_protocol_action {
	PB_PROTOCOL_ACTION_DEVICE_ADD		= 0x1,
	PB_PROTOCOL_ACTION_BOOT_OPTION_ADD	= 0x2,
	PB_PROTOCOL_ACTION_DEVICE_REMOVE	= 0x3,
/*	PB_PROTOCOL_ACTION_BOOT_OPTION_REMOVE	= 0x4, */
	PB_PROTOCOL_ACTION_BOOT			= 0x5,
	PB_PROTOCOL_ACTION_STATUS		= 0x6,
	PB_PROTOCOL_ACTION_CANCEL_DEFAULT	= 0x7,
	PB_PROTOCOL_ACTION_SYSTEM_INFO		= 0x8,
	PB_PROTOCOL_ACTION_CONFIG		= 0x9,
	PB_PROTOCOL_ACTION_REINIT		= 0xa,
	PB_PROTOCOL_ACTION_ADD_URL		= 0xb,
	PB_PROTOCOL_ACTION_PLUGIN_OPTION_ADD	= 0xc,
	PB_PROTOCOL_ACTION_PLUGINS_REMOVE	= 0xd,
	PB_PROTOCOL_ACTION_PLUGIN_INSTALL	= 0xe,
	PB_PROTOCOL_ACTION_TEMP_AUTOBOOT	= 0xf,
	PB_PROTOCOL_ACTION_AUTHENTICATE		= 0x10,
};

struct pb_protocol_message {
	uint32_t action;
	uint32_t payload_len;
	char     payload[];
};

enum auth_msg_type {
	AUTH_MSG_REQUEST,
	AUTH_MSG_RESPONSE,
	AUTH_MSG_SET,
};

struct auth_message {
	enum auth_msg_type op;
	union {
		bool	authenticated;
		char	*password;
		struct {
			char	*password;
			char	*new_password;
		} set_password;
	};
};

void pb_protocol_dump_device(const struct device *dev, const char *text,
	FILE *stream);
int pb_protocol_device_len(const struct device *dev);
int pb_protocol_boot_option_len(const struct boot_option *opt);
int pb_protocol_boot_len(const struct boot_command *boot);
int pb_protocol_boot_status_len(const struct status *status);
int pb_protocol_system_info_len(const struct system_info *sysinfo);
int pb_protocol_config_len(const struct config *config);
int pb_protocol_url_len(const char *url);
int pb_protocol_command_len(const struct command *command);
int pb_protocol_plugin_option_len(const struct plugin_option *opt);
int pb_protocol_temp_autoboot_len(const struct autoboot_option *opt);
int pb_protocol_authenticate_len(struct auth_message *msg);
int pb_protocol_device_cmp(const struct device *a, const struct device *b);

int pb_protocol_boot_option_cmp(const struct boot_option *a,
	const struct boot_option *b);

int pb_protocol_serialise_string(char *pos, const char *str);
char *pb_protocol_deserialise_string(void *ctx,
		const struct pb_protocol_message *message);

int pb_protocol_serialise_device(const struct device *dev,
		char *buf, int buf_len);
int pb_protocol_serialise_boot_option(const struct boot_option *opt,
		char *buf, int buf_len);
int pb_protocol_serialise_boot_command(const struct boot_command *boot,
		char *buf, int buf_len);
int pb_protocol_serialise_boot_status(const struct status *status,
		char *buf, int buf_len);
int pb_protocol_serialise_system_info(const struct system_info *sysinfo,
		char *buf, int buf_len);
int pb_protocol_serialise_config(const struct config *config,
		char *buf, int buf_len);
int pb_protocol_serialise_url(const char *url, char *buf, int buf_len);
int pb_protocol_serialise_command(char *buf,
		const struct command *command);
int pb_protocol_serialise_plugin_option(const struct plugin_option *opt,
		char *buf, int buf_len);
int pb_protocol_serialise_temp_autoboot(const struct autoboot_option *opt,
		char *buf, int buf_len);
int pb_protocol_serialise_authenticate(struct auth_message *msg,
		char *buf, int buf_len);

int pb_protocol_write_message(int fd, struct pb_protocol_message *message);

struct pb_protocol_message *pb_protocol_create_message(void *ctx,
		enum pb_protocol_action action, int payload_len);

struct pb_protocol_message *pb_protocol_read_message(void *ctx, int fd);

int pb_protocol_deserialise_device(struct device *dev,
		const struct pb_protocol_message *message);

int pb_protocol_deserialise_boot_option(struct boot_option *opt,
		const struct pb_protocol_message *message);

int pb_protocol_deserialise_boot_command(struct boot_command *cmd,
		const struct pb_protocol_message *message);

int pb_protocol_deserialise_boot_status(struct status *status,
		const struct pb_protocol_message *message);

int pb_protocol_deserialise_system_info(struct system_info *sysinfo,
		const struct pb_protocol_message *message);

int pb_protocol_deserialise_config(struct config *config,
		const struct pb_protocol_message *message);

int pb_protocol_deserialise_command(void *ctx, const char **pos,
		unsigned int *len, struct command *command);

int pb_protocol_deserialise_plugin_option(struct plugin_option *opt,
		const struct pb_protocol_message *message);

int pb_protocol_deserialise_temp_autoboot(struct autoboot_option *opt,
		const struct pb_protocol_message *message);

int pb_protocol_deserialise_authenticate(struct auth_message *msg,
		const struct pb_protocol_message *message);
#endif /* _PB_PROTOCOL_H */
