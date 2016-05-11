#pragma once
#ifndef _N_XBEE_H
#define _N_XBEE_H

#include <linux/module.h>    // included for all kernel modules
#include <linux/kernel.h>    // included for KERN_INFO
#include <linux/init.h>      // included for __init and __exit macros
#include <linux/netdevice.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/tty.h>

#define N_XBEE_LISC 17

// Actual MTU post-fragmentation
#define N_XBEE_DATA_MTU 72
#define N_XBEE_MAXFRAME (N_XBEE_DATA_MTU*2 + 16)

#define XBEE_NETDEV_PREFIX "xbee"
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
  struct tty_struct* tty;
  spinlock_t write_lock;
} xbee_serial_bridge;
struct xbee_serial_bridge* n_xbee_serial_bridges;

// Private storage for xbee netdev
typedef struct xbee_netdev_priv {
  xbee_serial_bridge* bridge;
} xbee_netdev_priv;

// kernel module functions not in header file
#endif
