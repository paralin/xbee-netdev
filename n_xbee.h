#pragma once
#ifndef _N_XBEE_H
#define _N_XBEE_H

#define N_XBEE_LISC 17
#define XBEE_NETDEV_PREFIX "xbee"

#include <linux/module.h>    // included for all kernel modules
#include <linux/kernel.h>    // included for KERN_INFO
#include <linux/init.h>      // included for __init and __exit macros
#include <linux/netdevice.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/tty.h>

/*
 * One bridge is created per registered xbee.
 */
typedef struct xbee_serial_bridge {
  char* name;
  char* netdevName;
  // boolean if netdev inited
  int netdevInitialized;
  struct net_device* netdev;
  struct xbee_serial_bridge* next;
} xbee_serial_bridge;
struct xbee_serial_bridge* n_xbee_serial_bridges;

typedef struct xbee_netdev_priv {
  xbee_serial_bridge* bridge;
} xbee_netdev_priv;

// kernel module functions not in header file
#endif
