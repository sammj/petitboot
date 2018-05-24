
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>
#include <linux/if.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <i18n/i18n.h>

#include <log/log.h>
#include <list/list.h>
#include <file/file.h>
#include <types/types.h>
#include <talloc/talloc.h>
#include <waiter/waiter.h>
#include <process/process.h>
#include <system/system.h>

#include "network.h"
#include "sysinfo.h"
#include "platform.h"
#include "device-handler.h"
#include "paths.h"

#define HWADDR_SIZE	6
#define PIDFILE_BASE	(LOCAL_STATE_DIR "/petitboot/")
#define INITIAL_BUFSIZE	4096

#define for_each_nlmsg(buf, nlmsg, len) \
	for (nlmsg = (struct nlmsghdr *)buf; \
		NLMSG_OK(nlmsg, len) && nlmsg->nlmsg_type != NLMSG_DONE; \
		nlmsg = NLMSG_NEXT(nlmsg, len))

#define for_each_rta(buf, rta, attrlen) \
	for (rta = (struct rtattr *)(buf); RTA_OK(rta, attrlen); \
			rta = RTA_NEXT(rta, attrlen))


struct interface {
	int	ifindex;
	char	name[IFNAMSIZ];
	uint8_t	hwaddr[HWADDR_SIZE];

	enum {
		IFSTATE_NEW,
		IFSTATE_UP_WAITING_LINK,
		IFSTATE_CONFIGURED,
		IFSTATE_IGNORED,
	} state;

	struct list_item list;
	struct process *udhcpc_process;
	struct process *udhcpc6_process;
	struct discover_device *dev;
	bool ready;
};

struct network {
	struct list		interfaces;
	struct device_handler	*handler;
	struct waiter		*waiter;
	int			netlink_sd;
	void			*netlink_buf;
	unsigned int		netlink_buf_size;
	bool			manual_config;
	bool			dry_run;
};

static char *mac_bytes_to_string(void *ctx, uint8_t *addr, int len)
{
	const int l = strlen("xx:");
	char *buf;
	int i;

	if (len <= 0)
		return talloc_strdup(ctx, "");

	buf = talloc_array(ctx, char, (len * l) + 1);

	for (i = 0; i < len; i++)
		sprintf(buf + (l * i), "%02x:", addr[i]);

	*(buf + (l * len) - 1) = '\0';

	return buf;
}

static const struct interface_config *find_config_by_hwaddr(
		uint8_t *hwaddr)
{
	const struct config *config;
	unsigned int i;

	config = config_get();
	if (!config)
		return NULL;

	for (i = 0; i < config->network.n_interfaces; i++) {
		struct interface_config *ifconf = config->network.interfaces[i];

		if (!memcmp(ifconf->hwaddr, hwaddr, HWADDR_SIZE))
			return ifconf;
	}

	return NULL;
}

static struct interface *find_interface_by_ifindex(struct network *network,
		int ifindex)
{
	struct interface *interface;

	list_for_each_entry(&network->interfaces, interface, list)
		if (interface->ifindex == ifindex)
			return interface;

	return NULL;
}

static struct interface *find_interface_by_name(struct network *network,
		const char *name)
{
	struct interface *interface;

	list_for_each_entry(&network->interfaces, interface, list)
		if (!strcmp(interface->name, name))
			return interface;

	return NULL;
}

static struct interface *find_interface_by_uuid(struct network *network,
		const char *uuid)
{
	struct interface *interface;
	char *mac;

	list_for_each_entry(&network->interfaces, interface, list) {
		mac = mac_bytes_to_string(interface, interface->hwaddr,
					sizeof(interface->hwaddr));
		if (!strcmp(mac, uuid)) {
			talloc_free(mac);
			return interface;
		}
		talloc_free(mac);
	}

	return NULL;
}

uint8_t *find_mac_by_name(void *ctx, struct network *network,
		const char *name)
{
	struct interface *interface;

	interface = find_interface_by_name(network, name);
	if (!interface)
		return NULL;

	return talloc_memdup(ctx, &interface->hwaddr,
			     sizeof(uint8_t) * HWADDR_SIZE);
}

static int network_init_netlink(struct network *network)
{
	struct sockaddr_nl addr;
	int rc;

	memset(&addr, 0, sizeof(addr));
	addr.nl_family = AF_NETLINK;
	addr.nl_groups = RTMGRP_LINK;

	network->netlink_sd = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
	if (network->netlink_sd < 0) {
		perror("socket(AF_NETLINK)");
		return -1;
	}

	rc = bind(network->netlink_sd, (struct sockaddr *)&addr, sizeof(addr));
	if (rc) {
		perror("bind(sockaddr_nl)");
		close(network->netlink_sd);
		return -1;
	}

	network->netlink_buf_size = INITIAL_BUFSIZE;
	network->netlink_buf = talloc_array(network, char,
				network->netlink_buf_size);

	return 0;
}

static int network_send_link_query(struct network *network)
{
	int rc;
	struct {
		struct nlmsghdr nlmsg;
		struct rtgenmsg rtmsg;
	} msg;

	memset(&msg, 0, sizeof(msg));

	msg.nlmsg.nlmsg_len = sizeof(msg);
	msg.nlmsg.nlmsg_type = RTM_GETLINK;
	msg.nlmsg.nlmsg_flags = NLM_F_REQUEST | NLM_F_ROOT;
	msg.nlmsg.nlmsg_seq = 0;
	msg.nlmsg.nlmsg_pid = 0;
	msg.rtmsg.rtgen_family = AF_UNSPEC;

	rc = send(network->netlink_sd, &msg, sizeof(msg), MSG_NOSIGNAL);
	if (rc != sizeof(msg))
		return -1;

	return 0;
}

static void create_interface_dev(struct network *network,
		struct interface *interface)
{
	char *uuid = mac_bytes_to_string(interface, interface->hwaddr,
						sizeof(interface->hwaddr));

	interface->dev = discover_device_create(network->handler, uuid,
						interface->name);
	interface->dev->device->type = DEVICE_TYPE_NETWORK;
	device_handler_add_device(network->handler, interface->dev);
	talloc_free(uuid);
}

static void remove_interface(struct network *network,
		struct interface *interface)
{
	if (interface->dev)
		device_handler_remove(network->handler, interface->dev);
	list_remove(&interface->list);
	talloc_free(interface);
}

void network_register_device(struct network *network,
		struct discover_device *dev)
{
	struct interface *iface;

	if (dev->uuid)
		iface = find_interface_by_uuid(network, dev->uuid);
	else
		iface = find_interface_by_name(network, dev->label);
	if (!iface)
		return;

	iface->dev = dev;
	dev->uuid = mac_bytes_to_string(iface->dev, iface->hwaddr,
			sizeof(iface->hwaddr));
}

void network_unregister_device(struct network *network,
		struct discover_device *dev)
{
	struct interface *iface;

	iface = find_interface_by_uuid(network, dev->uuid);
	if (!iface)
		return;

	iface->dev = NULL;
}

static int interface_change(struct interface *interface, bool up)
{
	const char *statestr = up ? "up" : "down";
	int rc;

	if (!up && interface->udhcpc_process) {
		/* we don't care about the callback from here */
		interface->udhcpc_process->exit_cb = NULL;
		interface->udhcpc_process->data = NULL;
		process_stop_async(interface->udhcpc_process);
		process_release(interface->udhcpc_process);
	}
	if (!up && interface->udhcpc6_process) {
		/* we don't care about the callback from here */
		interface->udhcpc6_process->exit_cb = NULL;
		interface->udhcpc6_process->data = NULL;
		process_stop_async(interface->udhcpc6_process);
		process_release(interface->udhcpc6_process);
	}

	if (!up) {
		rc = process_run_simple(interface, pb_system_apps.ip,
				"address", "flush", "dev", interface->name,
				NULL);
		if (rc)
			pb_log("failed to flush addresses from interface %s\n",
				interface->name);
	}

	rc = process_run_simple(interface, pb_system_apps.ip,
			"link", "set", interface->name, statestr, NULL);
	if (rc) {
		pb_log("failed to bring interface %s %s\n", interface->name,
				statestr);
		return -1;
	}
	return 0;
}

static int interface_up(struct interface *interface)
{
	return interface_change(interface, true);
}

static int interface_down(struct interface *interface)
{
	return interface_change(interface, false);
}

static void udhcpc_process_exit(struct process *process)
{
	struct interface *interface = process->data;

	if (process == interface->udhcpc_process) {
		pb_debug("udhcpc client [pid %d] for interface %s exited, rc %d\n",
				process->pid, interface->name, process->exit_status);
		interface->udhcpc_process = NULL;
	} else {
		pb_debug("udhcpc6 client [pid %d] for interface %s exited, rc %d\n",
				process->pid, interface->name, process->exit_status);
		interface->udhcpc6_process = NULL;
	}

	process_release(process);
}

static void configure_interface_dhcp(struct network *network,
		struct interface *interface)
{
	const struct platform *platform;
	char pidfile[256], idv4[10], idv6[10];
	struct process *p_v4, *p_v6;
	int rc;
	const char *argv_ipv4[] = {
		pb_system_apps.udhcpc,
		"-R",
		"-f",
		"-O", "pxeconffile",
		"-O", "pxepathprefix",
		"-p", pidfile,
		"-i", interface->name,
		"-x", idv4, /* [11,12] - dhcp client identifier */
		NULL,
	};

	const char *argv_ipv6[] = {
		pb_system_apps.udhcpc6,
		"-R",
		"-f",
		"-O", "bootfile_url",
		"-O", "bootfile_param",
		"-O", "pxeconffile",
		"-O", "pxepathprefix",
		"-p", pidfile,
		"-i", interface->name,
		"-x", idv6, /* [15,16] - dhcp client identifier */
		NULL,
	};

	device_handler_status_dev_info(network->handler, interface->dev,
			_("Configuring with DHCP"));

	snprintf(pidfile, sizeof(pidfile), "%s/udhcpc-%s.pid",
			PIDFILE_BASE, interface->name);

	platform = platform_get();
	if (platform && platform->dhcp_arch_id != 0xffff) {
		snprintf(idv6, sizeof(idv6), "0x3d:%04x",
				platform->dhcp_arch_id);
		snprintf(idv4, sizeof(idv4), "0x5d:%04x",
				platform->dhcp_arch_id);
	} else {
		argv_ipv4[11] = argv_ipv6[15] =  NULL;
	}

	p_v4 = process_create(interface);
	p_v4->path = pb_system_apps.udhcpc;
	p_v4->argv = argv_ipv4;
	p_v4->exit_cb = udhcpc_process_exit;
	p_v4->data = interface;

	pb_log("Running DHCPv4 client\n");
	rc = process_run_async(p_v4);
	if (rc)
		process_release(p_v4);
	else
		interface->udhcpc_process = p_v4;

	pb_log("Running DHCPv6 client\n");
	p_v6 = process_create(interface);
	p_v6->path = pb_system_apps.udhcpc6;
	p_v6->argv = argv_ipv6;
	p_v6->exit_cb = udhcpc_process_exit;
	p_v6->data = interface;

	rc = process_run_async(p_v6);
	if (rc)
		process_release(p_v6);
	else
		interface->udhcpc6_process = p_v6;

	return;
}

static void configure_interface_static(struct network *network,
		struct interface *interface,
		const struct interface_config *config)
{
	int rc;

	device_handler_status_dev_info(network->handler, interface->dev,
			_("Configuring with static address (ip: %s)"),
			config->static_config.address);

	rc = process_run_simple(interface, pb_system_apps.ip,
			"address", "add", config->static_config.address,
			"dev", interface->name, NULL);


	if (rc) {
		pb_log("failed to add address %s to interface %s\n",
				config->static_config.address,
				interface->name);
		return;
	}

	system_info_set_interface_address(sizeof(interface->hwaddr),
				interface->hwaddr,
				config->static_config.address);

	/* we need the interface up before we can route through it */
	rc = interface_up(interface);
	if (rc)
		return;

	if (config->static_config.gateway)
		rc = process_run_simple(interface, pb_system_apps.ip,
				"route", "add", "default",
				"via", config->static_config.gateway,
				NULL);

	if (rc) {
		pb_log("failed to add default route %s on interface %s\n",
				config->static_config.gateway,
				interface->name);
	}

	if (config->static_config.url) {
		pb_log("config URL %s\n", config->static_config.url);
		device_handler_process_url(network->handler,
				config->static_config.url,
				mac_bytes_to_string(interface->dev,
						interface->hwaddr,
						sizeof(interface->hwaddr)),
				config->static_config.address);
	}

	return;
}

static void configure_interface(struct network *network,
		struct interface *interface, bool up, bool link)
{
	const struct interface_config *config = NULL;

	if (interface->state == IFSTATE_IGNORED)
		return;

	/* old interface? check that we're still up and running */
	if (interface->state == IFSTATE_CONFIGURED) {
		if (!up)
			interface->state = IFSTATE_NEW;
		else if (!link)
			interface->state = IFSTATE_UP_WAITING_LINK;
		else {
			pb_debug("network: skipping configured interface %s\n",
					interface->name);
			return;
		}
	}

	/* always up the lookback, no other handling required */
	if (!strcmp(interface->name, "lo")) {
		if (interface->state == IFSTATE_NEW)
			interface_up(interface);
		interface->state = IFSTATE_CONFIGURED;
		return;
	}

	config = find_config_by_hwaddr(interface->hwaddr);
	if (config && config->ignore) {
		pb_log("network: ignoring interface %s\n", interface->name);
		interface->state = IFSTATE_IGNORED;
		return;
	}

	/* if we're in manual config mode, we need an interface configuration */
	if (network->manual_config && !config) {
		interface->state = IFSTATE_IGNORED;
		pb_log("network: skipping %s: manual config mode, "
				"but no config for this interface\n",
				interface->name);
		return;
	}

	/* new interface? bring up to the point so we can detect a link */
	if (interface->state == IFSTATE_NEW) {
		if (!up) {
			interface_up(interface);
			pb_log("network: bringing up interface %s\n",
					interface->name);
			return;

		} else if (!link) {
			interface->state = IFSTATE_UP_WAITING_LINK;
		}
	}

	/* no link? wait for a notification */
	if (interface->state == IFSTATE_UP_WAITING_LINK && !link)
		return;

	pb_log("network: configuring interface %s\n", interface->name);

	if (!config || config->method == CONFIG_METHOD_DHCP) {
		configure_interface_dhcp(network, interface);

	} else if (config->method == CONFIG_METHOD_STATIC) {
		configure_interface_static(network, interface, config);
		/* Nothing left to do for static interfaces */
		pending_network_jobs_start();
	}

	interface->state = IFSTATE_CONFIGURED;
}

static int network_handle_nlmsg(struct network *network, struct nlmsghdr *nlmsg)
{
	bool have_ifaddr, have_ifname;
	struct interface *interface, *tmp;
	struct ifinfomsg *info;
	struct rtattr *attr;
	unsigned int mtu;
	uint8_t ifaddr[6];
	char ifname[IFNAMSIZ+1];
	int attrlen, type;


	/* we're only interested in NEWLINK messages */
	type = nlmsg->nlmsg_type;
	if (!(type == RTM_NEWLINK || type == RTM_DELLINK))
		return 0;

	info = NLMSG_DATA(nlmsg);

	have_ifaddr = have_ifname = false;
	mtu = 1;

	attrlen = nlmsg->nlmsg_len - sizeof(*info);

	/* extract the interface name and hardware address attributes */
	for_each_rta(info + 1, attr, attrlen) {
		void *data = RTA_DATA(attr);

		switch (attr->rta_type) {
		case IFLA_ADDRESS:
			memcpy(ifaddr, data, sizeof(ifaddr));
			have_ifaddr = true;
			break;

		case IFLA_IFNAME:
			strncpy(ifname, data, IFNAMSIZ);
			have_ifname = true;
			break;

		case IFLA_MTU:
			mtu = *(unsigned int *)data;
			break;
		}
	}

	if (!have_ifaddr || !have_ifname)
		return -1;

	if (type == RTM_DELLINK || mtu == 0) {
		interface = find_interface_by_ifindex(network, info->ifi_index);
		if (!interface)
			return 0;
		pb_log("network: interface %s removed\n", interface->name);
		remove_interface(network, interface);
		return 0;
	}

	/* ignore the default tun device in some environments */
	if (strncmp(ifname, "tun", strlen("tun")) == 0)
		return 0;

	interface = find_interface_by_ifindex(network, info->ifi_index);
	if (!interface) {
		interface = talloc_zero(network, struct interface);
		interface->ifindex = info->ifi_index;
		interface->state = IFSTATE_NEW;
		memcpy(interface->hwaddr, ifaddr, sizeof(interface->hwaddr));
		strncpy(interface->name, ifname, sizeof(interface->name) - 1);

		list_for_each_entry(&network->interfaces, tmp, list)
			if (memcmp(interface->hwaddr, tmp->hwaddr,
				   sizeof(interface->hwaddr)) == 0) {
				pb_log("%s: %s has duplicate MAC address, ignoring\n",
				       __func__, interface->name);
				talloc_free(interface);
				return -1;
			}

		list_add(&network->interfaces, &interface->list);
		create_interface_dev(network, interface);
	}

	/* A repeated RTM_NEWLINK can represent an interface name change */
	if (strncmp(interface->name, ifname, IFNAMSIZ)) {
		pb_debug("ifname update: %s -> %s\n", interface->name, ifname);
		strncpy(interface->name, ifname, sizeof(interface->name) - 1);
		talloc_free(interface->dev->device->id);
		interface->dev->device->id =
			talloc_strdup(interface->dev->device, ifname);
	}

	/* notify the sysinfo code about changes to this interface */
	if (strcmp(interface->name, "lo"))
		system_info_register_interface(
				sizeof(interface->hwaddr),
				interface->hwaddr, interface->name,
				info->ifi_flags & IFF_LOWER_UP);

	if (!interface->dev)
		create_interface_dev(network, interface);

	if (!interface->ready && strncmp(interface->name, "lo", strlen("lo"))) {
		pb_log("%s not marked ready yet\n", interface->name);
		return 0;
	}

	configure_interface(network, interface,
			info->ifi_flags & IFF_UP,
			info->ifi_flags & IFF_LOWER_UP);

	return 0;
}

void network_mark_interface_ready(struct device_handler *handler,
		int ifindex, const char *ifname, uint8_t *mac, int hwsize)
{
	struct network *network = device_handler_get_network(handler);
	struct interface *interface, *tmp = NULL;
	char *macstr;

	if (!network) {
		pb_log("Network not ready - can not mark interface ready\n");
		return;
	}

	if (hwsize != HWADDR_SIZE)
		return;

	if (strncmp(ifname, "lo", strlen("lo")) == 0)
		return;

	interface = find_interface_by_ifindex(network, ifindex);
	if (!interface) {
		pb_debug("Creating ready interface %d - %s\n",
				ifindex, ifname);
		interface = talloc_zero(network, struct interface);
		interface->ifindex = ifindex;
		interface->state = IFSTATE_NEW;
		memcpy(interface->hwaddr, mac, HWADDR_SIZE);
		strncpy(interface->name, ifname, sizeof(interface->name) - 1);

		list_for_each_entry(&network->interfaces, tmp, list)
			if (memcmp(interface->hwaddr, tmp->hwaddr,
				   sizeof(interface->hwaddr)) == 0) {
				pb_log("%s: %s has duplicate MAC address, ignoring\n",
				       __func__, interface->name);
				talloc_free(interface);
				return;
			}

		list_add(&network->interfaces, &interface->list);
		create_interface_dev(network, interface);
	}

	if (interface->ready) {
		pb_log("%s already ready\n", interface->name);
		return;
	}

	if (strncmp(interface->name, ifname, strlen(ifname)) != 0) {
		pb_debug("ifname update from udev: %s -> %s\n", interface->name, ifname);
		strncpy(interface->name, ifname, sizeof(interface->name) - 1);
		talloc_free(interface->dev->device->id);
		interface->dev->device->id =
			talloc_strdup(interface->dev->device, ifname);
	}

	if (memcmp(interface->hwaddr, mac, HWADDR_SIZE) != 0) {
		macstr = mac_bytes_to_string(interface, mac, hwsize);
		pb_log("Warning - new MAC for interface %d does not match: %s\n",
				ifindex, macstr);
		talloc_free(macstr);
	}

	pb_log("Interface %s ready\n", ifname);
	interface->ready = true;
	configure_interface(network, interface, false, false);
}

static int network_netlink_process(void *arg)
{
	struct network *network = arg;
	struct nlmsghdr *nlmsg;
	struct msghdr msg;
	struct iovec iov;
	unsigned int len;
	int rc, flags;

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	flags = MSG_PEEK;

retry:
	iov.iov_len = network->netlink_buf_size;
	iov.iov_base = network->netlink_buf;

	rc = recvmsg(network->netlink_sd, &msg, flags);

	if (rc < 0) {
		perror("netlink recv header");
		return -1;
	}

	len = rc;

	/* if the netlink message was larger than our buffer, realloc
	 * before reading again */
	if (len > network->netlink_buf_size || msg.msg_flags & MSG_TRUNC) {
		network->netlink_buf_size *= 2;
		network->netlink_buf = talloc_realloc(network,
					network->netlink_buf,
					char *,
					network->netlink_buf_size);
		goto retry;
	}

	/* otherwise, we're good to read the entire message without PEEK */
	if (flags == MSG_PEEK) {
		flags = 0;
		goto retry;
	}

	for_each_nlmsg(network->netlink_buf, nlmsg, len)
		network_handle_nlmsg(network, nlmsg);

	return 0;
}

static void network_init_dns(struct network *network)
{
	const struct config *config;
	unsigned int i;
	int rc, len;
	bool modified;
	char *buf;

	if (network->dry_run)
		return;

	config = config_get();
	if (!config || !config->network.n_dns_servers)
		return;

	rc = read_file(network, "/etc/resolv.conf", &buf, &len);

	if (rc) {
		buf = talloc_strdup(network, "");
		len = 0;
	}

	modified = false;

	for (i = 0; i < config->network.n_dns_servers; i++) {
		int dns_conf_len;
		char *dns_conf;

		dns_conf = talloc_asprintf(network, "nameserver %s\n",
				config->network.dns_servers[i]);

		if (strstr(buf, dns_conf)) {
			talloc_free(dns_conf);
			continue;
		}

		dns_conf_len = strlen(dns_conf);
		buf = talloc_realloc(network, buf, char, len + dns_conf_len + 1);
		memcpy(buf + len, dns_conf, dns_conf_len);
		len += dns_conf_len;
		buf[len] = '\0';
		modified = true;

		talloc_free(dns_conf);
	}

	if (modified) {
		rc = replace_file("/etc/resolv.conf", buf, len);
		if (rc)
			pb_log("error replacing resolv.conf: %s\n",
					strerror(errno));
	}

	talloc_free(buf);
}

struct network *network_init(struct device_handler *handler,
		struct waitset *waitset, bool dry_run)
{
	struct network *network;
	int rc;

	network = talloc(handler, struct network);
	list_init(&network->interfaces);
	network->handler = handler;
	network->dry_run = dry_run;
	network->manual_config = config_get()->network.n_interfaces != 0;

	network_init_dns(network);

	rc = network_init_netlink(network);
	if (rc)
		goto err;

	network->waiter = waiter_register_io(waitset, network->netlink_sd,
			WAIT_IN, network_netlink_process, network);

	if (!network->waiter)
		goto err;

	rc = network_send_link_query(network);
	if (rc)
		goto err;

	return network;

err:
	network_shutdown(network);
	return NULL;
}

int network_shutdown(struct network *network)
{
	struct interface *interface;

	if (network->waiter)
		waiter_remove(network->waiter);

	list_for_each_entry(&network->interfaces, interface, list) {
		if (interface->state == IFSTATE_IGNORED)
			continue;
		if (!strcmp(interface->name, "lo"))
			continue;
		interface_down(interface);
	}

	close(network->netlink_sd);
	talloc_free(network);
	return 0;
}
