#include "n_xbee.h"
#include <xbee/device.h>
#include <xbee/atcmd.h>

// Module init stuff
MODULE_LICENSE("GPL");
MODULE_AUTHOR("paralin");
MODULE_DESCRIPTION("IP over Xbee.");

#define ENSURE_MODULE \
  if (!try_module_get(THIS_MODULE)) \
    return -ENODEV;

#define ENSURE_MODULE_NORET \
  if (!try_module_get(THIS_MODULE)) \
    return;

/* = Serial Bridge Stuff = */
void n_xbee_init_bridge_ll(void) {
  n_xbee_serial_bridges = NULL;
}

// Just free, don't remove properly.
// Also frees all netdevs etc.
void n_xbee_free_netdev(xbee_serial_bridge* n);
void n_xbee_free_bridge(xbee_serial_bridge* n) {
  if (!n) return;
  if (n->netdevInitialized)
    n_xbee_free_netdev(n);
  if (n->name)
    kfree(n->name);
  if (n->netdevName)
    kfree(n->name);
  kfree(n);
}

// Insert a bridge into the linked list
void n_xbee_insert_bridge(xbee_serial_bridge* n) {
  if (!n_xbee_serial_bridges)
    n_xbee_serial_bridges = n;
  else {
    xbee_serial_bridge* nc = n_xbee_serial_bridges;
    while (nc->next)
      nc = nc->next;
    nc->next = n;
  }
}

// Remove a bridge from the list of bridges. Does not free.
void n_xbee_remove_bridge(xbee_serial_bridge* ntd) {
  xbee_serial_bridge* n = n_xbee_serial_bridges;
  xbee_serial_bridge* nl = NULL;
  while (n) {
    if (n == ntd) {
      if (!nl)
        n_xbee_serial_bridges = n->next;
      else
        nl->next = n->next;
      return;
    }
    nl = n;
    n = n->next;
  }
}

// Free all bridges
void n_xbee_free_all_bridges(void) {
  xbee_serial_bridge* n = n_xbee_serial_bridges;
  n_xbee_serial_bridges = NULL;
  while (n) {
    xbee_serial_bridge* ni = n;
    n = n->next;
    n_xbee_free_bridge(ni);
  }
}

// Find by name
xbee_serial_bridge* n_xbee_find_bridge(const char* name) {
  xbee_serial_bridge* n = n_xbee_serial_bridges;
  while (n) {
    if (strcmp(n->name, name) == 0)
      return n;
    n = n->next;
  }
  return NULL;
}

/* = XBEE Controls */

// Prepare to transmit by waking up the device, etc
void n_xbee_prepare_xmit(struct tty_struct* tty) {
  tty->flags |= (1 << TTY_DO_WRITE_WAKEUP);
  // to write, use tty->driver->ops->write(tty, frame, len)
}

// Checks the tty to see if there is really an xbee
// on the other end, and if so, it's communicating right.
int n_xbee_check_tty(struct tty_struct* tty) {
  // TODO: implement this

  return 0;
}

/* = XBEE NetDev = */
static int n_xbee_netdev_open(struct net_device* dev) {
  printk(KERN_INFO "Kernel is opening %s...\n", dev->name);
  netif_start_queue(dev);
  return 0;
}

static int n_xbee_netdev_release(struct net_device* dev) {
  printk(KERN_INFO "Kernel is closing %s...\n", dev->name);
  netif_stop_queue(dev);
  return 0;
}

static int n_xbee_netdev_init_late(struct net_device* dev) {
  // Initialize the fragmentation system
  return 0;
}

static int n_xbee_netdev_change_mtu(struct net_device* dev, int new_mtu) {
  struct xbee_netdev_priv* priv;
  if (new_mtu < N_XBEE_DATA_MTU)
    return -EINVAL;
  if (new_mtu == dev->mtu)
    return 0;
  priv = netdev_priv(dev);

  printk(KERN_INFO "Changing MTU of %s to %u...\n", dev->name, new_mtu);

  // Stop the net queue
  netif_stop_queue(dev);

  // Get the spinlock
  spin_lock(&priv->bridge->write_lock);

  // Stop the net queue (again) just to be sure
  netif_stop_queue(dev);

  // Reset the fragmentation system
  // TODO

  // Set the MTU on the net device
  dev->mtu = new_mtu;

  // Initialize the fragmentation system
  // TODO

  // Start the net queue
  spin_unlock(&priv->bridge->write_lock);
  netif_wake_queue(dev);
  return 0;
}

static const struct net_device_ops n_xbee_netdev_ops = {
  .ndo_init = n_xbee_netdev_init_late,
  .ndo_open = n_xbee_netdev_open,
  .ndo_stop = n_xbee_netdev_release,
  .ndo_change_mtu = n_xbee_netdev_change_mtu,
};

static void n_xbee_netdev_init_early(struct net_device* dev) {
  // struct xbee_netdev_priv* priv = netdev_priv(dev);

  ether_setup(dev);
  dev->netdev_ops = &n_xbee_netdev_ops;
  // dev->flags |= IFF_NOARP;
  dev->mtu    = N_XBEE_DATA_MTU;
  // set priv flags and features and mtu
}

int n_xbee_init_netdev(xbee_serial_bridge* bridge) {
  struct net_device* ndev;
  struct xbee_netdev_priv* priv;
  int err;
  if (!bridge || !bridge->name) return -1;
  if (bridge->netdevInitialized) {
    printk(KERN_ALERT "Net bridge %s already inited!\n", bridge->netdevName);
    return -1;
  }

  printk(KERN_INFO "Initializing net bridge %s as %s...\n", bridge->name, bridge->netdevName);
  ndev = bridge->netdev = alloc_netdev(sizeof(xbee_netdev_priv), bridge->netdevName, NET_NAME_UNKNOWN, n_xbee_netdev_init_early);
  if (!ndev) {
    printk(KERN_ALERT "Failed to init netdev %s...\n", bridge->netdevName);
    return -1;
  }

  bridge->netdevInitialized = 1;
  priv = netdev_priv(ndev);
  memset(priv, 0, sizeof(*priv));
  priv->bridge = bridge;
  spin_lock_init(&priv->bridge->write_lock);

  if ((err = register_netdev(ndev)) != 0) {
    printk(KERN_ALERT "Failed to register netdev %s with error %i...", bridge->netdevName, err);
    bridge->netdevInitialized = 0;
    free_netdev(ndev);
    bridge->netdev = NULL;
    return err;
  }

  return 0;
}

void n_xbee_free_netdev(xbee_serial_bridge* n) {
  struct xbee_netdev_priv* priv;
  if (!n) return;
  if (n->netdevInitialized)
    unregister_netdev(n->netdev);
  n->netdevInitialized = 0;
  if (!n->netdev) return;
  printk(KERN_INFO "Shutting down net bridge %s...\n", n->netdevName);
  priv = netdev_priv(n->netdev);
  priv->bridge = NULL;
  free_netdev(n->netdev);
  n->netdev = NULL;
}

/* = XBEE Detection and Setup  =
 *
 * This driver relies on user-space code to
 * prompt it to attach/scan for an xbee on a serial
 * interface. This is done through a user-space daemon
 * which sets a line discipline on the serial line and then
 * holds it open. When a new serial line opens up this driver
 * is called, which results in a new network device being created
 * to handle the serial line.
 */

// Called when the userspace closes the tty.
static void n_xbee_serial_close(struct tty_struct* tty) {
  xbee_serial_bridge* bridge;
  // Make sure our module is still loaded
  ENSURE_MODULE_NORET;

  printk(KERN_INFO "TTY %s detached.\n", tty->name);
  bridge = n_xbee_find_bridge((const char*) tty->name);
  if (!bridge) {
    printk(KERN_ALERT "TTY %s not found/previously initialized...\n", tty->name);
    return;
  }
  n_xbee_remove_bridge(bridge);
  n_xbee_free_bridge(bridge);
}

/*
 * Called when the user-space daemon attaches to a
 * serial line. We should allocate the network device
 * in this function.
 *
 * We actually need to check if there is a valid xbee on the
 * other end, and if not, bail out with an error.
 */
static int n_xbee_serial_open(struct tty_struct* tty) {
  xbee_serial_bridge* bridge;
  int i;
  int nlen;
  int ndevnlen;
  char* rttyname;

  // Make sure our module is still loaded
  ENSURE_MODULE;

  // Find the existing allocated netdev for this (shouldn't happen)
  // .. or make a new one.
  printk(KERN_INFO "TTY %s attached.\n", tty->name);
  bridge = n_xbee_find_bridge((const char*) tty->name);
  if (bridge) {
    printk(KERN_ALERT "TTY %s was previously attached, shutting it down first...\n", tty->name);
    n_xbee_remove_bridge(bridge);
    n_xbee_free_bridge(bridge);
  }

  // remove the tty
  rttyname = strncmp("tty", tty->name, 3) == 0 ? (tty->name + 3) : tty->name;
  nlen = strlen(rttyname);

  // verify the tty
  if (n_xbee_check_tty(tty) != 0) {
    printk(KERN_ALERT "Couldn't contact xbee on %s, make sure it's a valid xbee and the baud is correct.\n", tty->name);
    return -ENODEV;
  }

  bridge = (xbee_serial_bridge*)kmalloc(sizeof(xbee_serial_bridge), GFP_KERNEL);
  bridge->tty = tty;
  bridge->name = (char*)kmalloc(sizeof(char) * (strlen(tty->name) + 1), GFP_KERNEL);
  bridge->name[strlen(tty->name)] = '\0';
  for (i = 0; i < strlen(tty->name); i++)
    bridge->name[i] = tty->name[i];
  bridge->next = 0;
  bridge->netdevInitialized = 0;
  bridge->netdev = 0;

  ndevnlen = strlen(XBEE_NETDEV_PREFIX) + nlen;
  bridge->netdevName = (char*)kmalloc(sizeof(char) * (ndevnlen + 1), GFP_KERNEL);
  bridge->netdevName[ndevnlen] = '\0';
  strcpy(bridge->netdevName, XBEE_NETDEV_PREFIX);
  strcpy(bridge->netdevName + strlen(XBEE_NETDEV_PREFIX), rttyname);

  n_xbee_insert_bridge(bridge);
  if (n_xbee_init_netdev(bridge) != 0) {
    printk(KERN_ALERT "%s n_xbee_init_netdev indicated failure, aborting.\n", tty->name);
    n_xbee_remove_bridge(bridge);
    n_xbee_free_bridge(bridge);
    return -ENODEV;
  }

  return 0;
}

// Line discipline, allows us to assign ownership
// of a serial device to this driver.
// NOTE: make sure to call ENSURE_MODULE FIRST!!
// this ldisc remains after the module is unloaded.
// this means if the userspace tries to use the ldisc it will segfault
// assuimng the module had been removed.
struct tty_ldisc_ops n_xbee_ldisc = {
  .owner = THIS_MODULE,
  .magic = TTY_LDISC_MAGIC,
  .name  = "n_xbee",
  .open  = n_xbee_serial_open,
  .close = n_xbee_serial_close,
};

static int __init n_xbee_init(void) {
  int result;
  printk(KERN_INFO "xbee-net initializing...\n");

  result = tty_register_ldisc(N_XBEE_LISC, &n_xbee_ldisc);
  if (result) {
    printk(KERN_ALERT "Registering line discipline failed: %d\n", result);
    return result;
  }

  return result;
}

static void __exit n_xbee_cleanup(void) {
  printk(KERN_INFO "xbee-net shutting down...\n");
  n_xbee_free_all_bridges();
  tty_unregister_ldisc(N_XBEE_LISC);
}

module_init(n_xbee_init);
module_exit(n_xbee_cleanup);
