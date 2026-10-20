/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rpmsg_user.h: endpoint helpers shared by the bench tools.
 *
 * Opens the rpmsg control device, creates an endpoint with
 * RPMSG_CREATE_EPT_IOCTL and opens the /dev/rpmsgN node the kernel made for
 * it. N is not fixed (other endpoints or the announced channel may already
 * hold rpmsg0..), so the node is found through sysfs by matching the
 * endpoint's name/src/dst attributes.
 *
 * Env: RPMSG_CTRL_DEV  control device (default /dev/rpmsg_ctrl0)
 *
 * Reconstructed 2026-10 from how the bench tools use it; the original
 * header was never committed.
 */
#ifndef RPMSG_USER_H
#define RPMSG_USER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#if defined(__has_include) && __has_include(<linux/rpmsg.h>)
#include <linux/rpmsg.h>
#else
#include <linux/types.h>
struct rpmsg_endpoint_info {
	char name[32];
	__u32 src;
	__u32 dst;
};
#define RPMSG_CREATE_EPT_IOCTL	_IOW(0xb5, 0x1, struct rpmsg_endpoint_info)
#define RPMSG_DESTROY_EPT_IOCTL	_IO(0xb5, 0x2)
#endif

/* firmware: rpmsg_ns_announce("rpmsg-sg2002-c906l-channel"), LOCAL_EPT_ADDR 30 */
#define RPMSG_DEFAULT_EPT_NAME	"rpmsg-sg2002-c906l-channel"
#define RPMSG_DEFAULT_SRC_ADDR	0x1e
#define RPMSG_DEFAULT_DST_ADDR	0x1e

#ifndef RPMSG_SYSFS_CLASS
#define RPMSG_SYSFS_CLASS	"/sys/class/rpmsg"
#endif
#define RPMSG_EPT_WAIT_MS	1000

static inline void rpmsg_init_endpoint_info(struct rpmsg_endpoint_info *info,
					    const char *name,
					    unsigned int src, unsigned int dst)
{
	memset(info, 0, sizeof(*info));
	strncpy(info->name, name, sizeof(info->name) - 1);
	info->src = src;
	info->dst = dst;
}

/* read one sysfs attribute of /sys/class/rpmsg/<dev>; strips the newline */
static inline int rpmsg_sysfs_read(const char *dev, const char *attr,
				   char *buf, size_t len)
{
	char path[512];	/* d_name (<= 255) + class dir + attr */
	ssize_t n;
	int fd;

	snprintf(path, sizeof(path), RPMSG_SYSFS_CLASS "/%s/%s", dev, attr);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, len - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	buf[strcspn(buf, "\n")] = '\0';
	return 0;
}

/*
 * Find the endpoint device for @info: a /sys/class/rpmsg/rpmsgN whose
 * name, src and dst all match. Highest N wins if several do.
 */
static inline int rpmsg_find_eptdev(const struct rpmsg_endpoint_info *info,
				    char *node, size_t len)
{
	struct dirent *de;
	DIR *dir;
	long best = -1;

	dir = opendir(RPMSG_SYSFS_CLASS);
	if (!dir)
		return -1;
	while ((de = readdir(dir)) != NULL) {
		char name[64], src[32], dst[32], *end;
		long idx;

		if (strncmp(de->d_name, "rpmsg", 5) != 0)
			continue;
		idx = strtol(de->d_name + 5, &end, 10);
		if (end == de->d_name + 5 || *end != '\0')
			continue;	/* rpmsg_ctrlN, virtioX.* ... */
		if (rpmsg_sysfs_read(de->d_name, "name", name, sizeof(name)) ||
		    rpmsg_sysfs_read(de->d_name, "src", src, sizeof(src)) ||
		    rpmsg_sysfs_read(de->d_name, "dst", dst, sizeof(dst)))
			continue;
		/* attributes are printed with %d: RPMSG_ADDR_ANY reads "-1" */
		if (strcmp(name, info->name) != 0 ||
		    (unsigned int)strtoul(src, NULL, 0) != info->src ||
		    (unsigned int)strtoul(dst, NULL, 0) != info->dst)
			continue;
		if (idx > best)
			best = idx;
	}
	closedir(dir);
	if (best < 0)
		return -1;
	snprintf(node, len, "/dev/rpmsg%ld", best);
	return 0;
}

/*
 * Create the endpoint and open its device node. On success *fd is the
 * control device and *fd_ept the endpoint; both are -1 on failure.
 */
static inline int rpmsg_open_endpoint(const struct rpmsg_endpoint_info *info,
				      int *fd, int *fd_ept)
{
	const char *ctrl = getenv("RPMSG_CTRL_DEV");
	char node[64];
	int waited;

	*fd = -1;
	*fd_ept = -1;
	if (!ctrl || !*ctrl)
		ctrl = "/dev/rpmsg_ctrl0";

	*fd = open(ctrl, O_RDWR);
	if (*fd < 0) {
		fprintf(stderr, "rpmsg: open %s: %s\n", ctrl, strerror(errno));
		return -1;
	}
	if (ioctl(*fd, RPMSG_CREATE_EPT_IOCTL, info) < 0) {
		fprintf(stderr, "rpmsg: create endpoint %s (src 0x%x dst 0x%x): %s\n",
			info->name, info->src, info->dst, strerror(errno));
		goto fail;
	}

	/* devtmpfs normally has the node by now; allow a short delay */
	for (waited = 0; waited <= RPMSG_EPT_WAIT_MS; waited += 10) {
		if (rpmsg_find_eptdev(info, node, sizeof(node)) == 0) {
			*fd_ept = open(node, O_RDWR);
			if (*fd_ept >= 0)
				break;
		}
		usleep(10 * 1000);
	}
	if (*fd_ept < 0) {
		fprintf(stderr, "rpmsg: no usable endpoint node for %s under "
			RPMSG_SYSFS_CLASS "\n", info->name);
		goto fail;
	}

	fprintf(stderr, "rpmsg: using %s and %s\n", ctrl, node);
	return 0;

fail:
	close(*fd);
	*fd = -1;
	return -1;
}

static inline void rpmsg_close_endpoint(int fd, int fd_ept)
{
	if (fd_ept >= 0) {
		ioctl(fd_ept, RPMSG_DESTROY_EPT_IOCTL);
		close(fd_ept);
	}
	if (fd >= 0)
		close(fd);
}

#endif /* RPMSG_USER_H */
