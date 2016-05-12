#include "n_xbee.h"
#include <xbee/device.h>
#include <xbee/atcmd.h>
#include <xbee/atmode.h>
#include <linux/kthread.h>

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

#define ENSURE_MODULE_RET(RET) \
  if (!try_module_get(THIS_MODULE)) \
    return RET;

/* == Buffer stuff == */
struct xbee_data_buffer* n_xbee_alloc_buffer(int size) {
  struct xbee_data_buffer* buf = kmalloc(sizeof(xbee_data_buffer), GFP_KERNEL);
  buf->size = size;
  buf->buffer = kmalloc(sizeof(unsigned char) * size, GFP_KERNEL);
  buf->pos = 0;
  return buf;
}

void n_xbee_free_buffer(struct xbee_data_buffer* buf) {
  if (buf->buffer)
    kfree(buf->buffer);
  kfree(buf);
}

/* = Serial Bridge Stuff = */
void n_xbee_init_bridge_ll(void) {
  n_xbee_serial_bridges = NULL;
}

// Frees the xbee_dev_t
void n_xbee_free_xbee_dev(xbee_dev_t* dev) {
  kfree(dev);
}

// Just free, don't remove properly.
// Also frees all netdevs etc.
void n_xbee_free_netdev(xbee_serial_bridge* n);
void n_xbee_free_bridge(xbee_serial_bridge* n) {
  if (!n) return;
  // grab read and write locks
  spin_lock(&n->read_lock);
  spin_lock(&n->write_lock);
  if (n->netdevInitialized)
    n_xbee_free_netdev(n);
  if (n->name)
    kfree(n->name);
  if (n->xbee_dev)
    n_xbee_free_xbee_dev(n->xbee_dev);
  if (n->netdevName)
    kfree(n->netdevName);
  if (n->recvbuf)
    n_xbee_free_buffer(n->recvbuf);
  // DO NOT FREE pend_dev
  if (n->pend_dev) {
    n->pend_dev->cancel = 1;
    n->pend_dev->noFreeBridge = 1;
  }
  // maybe unnecessary, do it anyway
  spin_unlock(&n->read_lock);
  spin_unlock(&n->write_lock);
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
xbee_serial_bridge* n_xbee_find_bridge_byname(const char* name) {
  xbee_serial_bridge* n = n_xbee_serial_bridges;
  while (n) {
    if (strcmp(n->name, name) == 0)
      return n;
    n = n->next;
  }
  return NULL;
}

// Find by tty
xbee_serial_bridge* n_xbee_find_bridge_bytty(struct tty_struct* tty) {
  xbee_serial_bridge* n = n_xbee_serial_bridges;
  while (n) {
    if (n->tty == tty)
      return n;
    n = n->next;
  }
  return NULL;
}

/* = XBEE Controls */

// Prepare to transmit by waking up the device, etc
void n_xbee_prepare_xmit(struct tty_struct* tty) {
  tty->flags |= (1 << TTY_DO_WRITE_WAKEUP);
}

// Checks the tty to see if there is really an xbee
// on the other end, and if so, it's communicating right.
int n_xbee_check_tty(xbee_serial_bridge* bridge) {
  int err, mode, iterations;
  xbee_dev_t* xbee = bridge->xbee_dev;
  printk(KERN_INFO "Putting board into AT mode to check values...\n");
  if ((err = xbee_atmode_enter(xbee)) != 0) {
    printk(KERN_ALERT "Unable to put board into AT mode, error: %d\n", err);
    return err;
  }
  iterations = 0;
  while (1) {
    if (iterations >= 500) {
      printk(KERN_ALERT "Timeout waiting for AT mode, assuming invalid.\n");
      return -ETIMEDOUT;
    }
    mode = xbee_atmode_tick(xbee);

    if (mode == XBEE_MODE_COMMAND) {
      printk(KERN_INFO "Successfully entered AT mode...\n");
      break;
    }
    else if (mode == XBEE_MODE_IDLE) {
      printk(KERN_ALERT "Never entered AT mode (in idle mode), assuming failure.\n");
      return -ETIMEDOUT;
    }

    // sleep 5 milliseconds
    msleep(5);
    iterations ++;
  }

  // exit AT mode
  printk(KERN_INFO "Exiting AT mode...\n");
  if ((err = xbee_atmode_exit(xbee)) != 0) {
    printk(KERN_ALERT "Unable to exit AT mode, error: %d\n", err);
    return err;
  }

  iterations = 0;
  // we have to wait around 2 seconds here for the xbee driver to exit AT mode
  while (1) {
    if (iterations > 410) {
      printk(KERN_ALERT "Timeout waiting for AT mode exit, assuming invalid.\n");
      return -ETIMEDOUT;
    }

    mode = xbee_atmode_tick(xbee);

    if (mode == XBEE_MODE_IDLE) {
      printk(KERN_INFO "Successfully exited AT mode.\n");
      break;
    }

    msleep(5);
    iterations++;
  }

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

static int n_xbee_netdev_ioctl(struct net_device* dev, struct ifreq* rq, int cmd) {
  // Default is not implemented
  return -ENOSYS;
}

static const struct net_device_ops n_xbee_netdev_ops = {
  .ndo_init = n_xbee_netdev_init_late,
  .ndo_open = n_xbee_netdev_open,
  .ndo_stop = n_xbee_netdev_release,
  .ndo_change_mtu = n_xbee_netdev_change_mtu,
  .ndo_do_ioctl = n_xbee_netdev_ioctl
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
  bridge = n_xbee_find_bridge_byname((const char*) tty->name);
  if (!bridge) {
    printk(KERN_ALERT "TTY %s not found/previously initialized...\n", tty->name);
    return;
  }
  n_xbee_remove_bridge(bridge);
  n_xbee_free_bridge(bridge);
}

// forward declaration
extern struct tty_ldisc_ops n_xbee_ldisc;

// if return anything but zero, call n_xbee_remove_bridge and n_xbee_free_bridge if NOT noFreeBridge
int n_xbee_resolve_pending_dev(xbee_pending_dev* dev) {
  if (dev->cancel)
    return -1;

  if (n_xbee_check_tty(dev->bridge) != 0) {
    printk(KERN_ALERT "Couldn't contact xbee on %s, make sure it's a valid xbee and the baud is correct.\n", dev->bridge->tty->name);
    return -ENODEV;
  }

  if (dev->cancel)
    return -1;

  if (n_xbee_init_netdev(dev->bridge) != 0) {
    printk(KERN_ALERT "%s n_xbee_init_netdev indicated failure, aborting.\n", dev->bridge->tty->name);
    return -ENODEV;
  }
  dev->bridge->pend_dev = NULL;
  return 0;
}

int n_xbee_resolve_pending_dev_thread(void* data) {
  xbee_pending_dev* dev = (xbee_pending_dev*) data;
  if (n_xbee_resolve_pending_dev(dev) != 0) {
    if (!dev->noFreeBridge) {
      n_xbee_remove_bridge(dev->bridge);
      n_xbee_free_bridge(dev->bridge);
    }
  }
  kfree(dev);
  return 0;
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
  bridge = n_xbee_find_bridge_byname((const char*) tty->name);
  if (bridge) {
    printk(KERN_ALERT "TTY %s was previously attached, shutting it down first...\n", tty->name);
    n_xbee_remove_bridge(bridge);
    n_xbee_free_bridge(bridge);
  }

  // remove the tty
  rttyname = strncmp("tty", tty->name, 3) == 0 ? (tty->name + 3) : tty->name;
  nlen = strlen(rttyname);

  bridge = (xbee_serial_bridge*)kmalloc(sizeof(xbee_serial_bridge), GFP_KERNEL);
  spin_lock_init(&bridge->write_lock);
  spin_lock_init(&bridge->read_lock);
  bridge->tty = tty;
  bridge->name = (char*)kmalloc(sizeof(char) * (strlen(tty->name) + 1), GFP_KERNEL);
  bridge->name[strlen(tty->name)] = '\0';
  for (i = 0; i < strlen(tty->name); i++)
    bridge->name[i] = tty->name[i];
  bridge->next = 0;
  bridge->netdevInitialized = 0;
  bridge->netdev = 0;
  bridge->recvbuf = n_xbee_alloc_buffer(N_XBEE_BUFFER_SIZE);
  bridge->tty->receive_room = bridge->recvbuf->size;

  // we have our own xbee device init here, the other doesn't work
  bridge->xbee_dev = (xbee_dev_t*) kmalloc(sizeof(xbee_dev_t), GFP_KERNEL);
  memset(bridge->xbee_dev, 0, sizeof(xbee_dev_t));
  bridge->xbee_dev->serport.baudrate = tty_get_baud_rate(tty);
  bridge->xbee_dev->serport.tty = tty;
  bridge->xbee_dev->serport.ldisc_ops = &n_xbee_ldisc;
  bridge->xbee_dev->guard_time = 1000;
  bridge->xbee_dev->escape_char = '+';
  bridge->xbee_dev->idle_timeout = 100;
  // Don't use flow control TODO
  bridge->xbee_dev->flags &= ~XBEE_DEV_FLAG_USE_FLOWCONTROL;

  ndevnlen = strlen(XBEE_NETDEV_PREFIX) + nlen;
  bridge->netdevName = (char*)kmalloc(sizeof(char) * (ndevnlen + 1), GFP_KERNEL);
  bridge->netdevName[ndevnlen] = '\0';
  strcpy(bridge->netdevName, XBEE_NETDEV_PREFIX);
  strcpy(bridge->netdevName + strlen(XBEE_NETDEV_PREFIX), rttyname);


  // verify the xbee exists and load its information
  // NOTE: moved, we cannot do this here. let's init the netdev elsewhere too.
  // instead, let's register a pending dev
  bridge->pend_dev = kmalloc(sizeof(xbee_pending_dev), GFP_KERNEL);
  bridge->pend_dev->bridge = bridge;
  bridge->pend_dev->cancel = 0;
  bridge->pend_dev->noFreeBridge = 0;

  n_xbee_insert_bridge(bridge);
  kthread_run(n_xbee_resolve_pending_dev_thread, (void*)bridge->pend_dev, "xbee_penddev");
  return 0;
}

static ssize_t n_xbee_chars_in_buffer(struct tty_struct* tty) {
  struct xbee_serial_bridge* bridge;
  ENSURE_MODULE;
  bridge = n_xbee_find_bridge_bytty(tty);
  if (!bridge)
    return -ENODEV;
  return bridge->recvbuf->pos;
}

// locks read_lock
static void n_xbee_flush_buffer(struct tty_struct* tty) {
  struct xbee_serial_bridge* bridge;
  ENSURE_MODULE_NORET;

  // Get the pointer to the bridge
  bridge = n_xbee_find_bridge_bytty(tty);
  if (!bridge)
    return;
  // Acquire read lock
  spin_lock(&bridge->read_lock);
  printk(KERN_INFO "%s flushing buffer by kernel request.\n", tty->name);
  bridge->recvbuf->pos = 0;
  spin_unlock(&bridge->read_lock);
}

// Userspace requests a read from a tty
static ssize_t n_xbee_read(struct tty_struct* tty, struct file* file, unsigned char __user *buf, size_t nr) {
  struct xbee_serial_bridge* bridge;
  int nleft;
  int i;
  int ntread;
  int cpres;

  ENSURE_MODULE_RET(-1);
  bridge = n_xbee_find_bridge_bytty(tty);
  if (!bridge)
    return -ENODEV;

  if (!bridge->recvbuf->pos)
    return 0;
    // return -EAGAIN;

  // acquire read lock
  spin_lock(&bridge->read_lock);

  // read as much as we can
  ntread = bridge->recvbuf->pos;
  if (nr < ntread)
    ntread = nr;

  // Potential optimization: don't always shift back here.
  // copy out the data

  // if file == NULL we are in kernel space memory
  if (!file)
    memcpy(buf, bridge->recvbuf->buffer, ntread);
  else
    if ((cpres = copy_to_user(buf, bridge->recvbuf->buffer, ntread)) != 0)
      printk(KERN_ALERT "in %s, copy_to_user returned %d.\n", __FUNCTION__, cpres);

  // nleft = amount of bytes left in the buffer
  nleft = bridge->recvbuf->pos - ntread;
  if (nleft <= 0)
    bridge->recvbuf->pos = 0;
  else {
    //index of first byte we need to store is pos
    for (i = 0; i < nleft; i++)
      bridge->recvbuf->buffer[i] = bridge->recvbuf->buffer[i + bridge->recvbuf->pos];
    bridge->recvbuf->pos = nleft;
  }

  spin_unlock(&bridge->read_lock);
  return ntread;
}

static ssize_t n_xbee_write(struct tty_struct* tty, struct file* file, const unsigned char* buf, size_t nr) {
  struct xbee_serial_bridge* bridge;
  int result;

#ifdef N_XBEE_VERBOSE
  int anyNonAscii, i;
  printk(KERN_INFO "%s: to %s, size %d\n", __FUNCTION__, tty->name, (int)nr);
#endif

  ENSURE_MODULE;
  bridge = n_xbee_find_bridge_bytty(tty);
  if (!bridge)
    return -ENODEV;

  // acquire write lock
  spin_lock(&bridge->write_lock);
  tty->flags |= (1 << TTY_DO_WRITE_WAKEUP);
  result = tty->driver->ops->write(tty, buf, nr);
  spin_unlock(&bridge->write_lock);
#ifdef N_XBEE_VERBOSE
  printk(KERN_INFO "%s: to %s, writing result %d\n", __FUNCTION__, tty->name, result);
  {
    anyNonAscii = 0;
    for (i = 0; i < nr; i++) {
      if (buf[i] > 255) {
        anyNonAscii = 1;
        break;
      }
    }
    if (!anyNonAscii)
      printk(KERN_INFO "%s: ascii output: %.*s\n", __FUNCTION__, (int)nr, buf);
  }
#endif

  return result;
}

static int n_xbee_serial_ioctl_chars_in_buffer(struct tty_struct* tty, struct file* file, unsigned int cmd, unsigned long arg) {
  int cpres;
  int blen = (int) n_xbee_chars_in_buffer(tty);
  if (blen < 0)
    return blen;
  if (arg) {
    int* mot = (int*) arg;
    if (!file)
      *mot = (int)blen;
    else
      if ((cpres = copy_to_user(mot, &blen, sizeof(int))) != 0)
        printk(KERN_ALERT "in %s, copy_to_user returned %d.\n", __FUNCTION__, cpres);
  }
  return 0;
}

static int n_xbee_serial_ioctl(struct tty_struct* tty, struct file* file, unsigned int cmd, unsigned long arg) {
  // handle read chars in buffer
  if (cmd ==
#ifdef FIONREAD
      FIONREAD
#else
      TIOCINQ
#endif
     ) {
    return n_xbee_serial_ioctl_chars_in_buffer(tty, file, cmd, arg);
  }

  // Default is not implemented
  return -ENOSYS;
}

//theoretically here we could get in an infite loop of dropping the entire buffer
static void n_xbee_receive_buf(struct tty_struct* tty, const unsigned char* cp, char* fp, int count) {
  struct xbee_serial_bridge* bridge;
  struct xbee_data_buffer* dbuf;
  int finlen;

#ifdef N_XBEE_VERBOSE
  int anyNonAscii, i;
  printk(KERN_INFO "n_xbee_receive_buf from %s with size %d\n", tty->name, count);
  {
    anyNonAscii = 0;
    for (i = 0; i < count; i++) {
      if (cp[i] > 255) {
        anyNonAscii = 1;
        break;
      }
    }
    if (!anyNonAscii)
      printk(KERN_INFO "%s: ascii input: %.*s\n", __FUNCTION__, (int)count, cp);
  }
#endif

  ENSURE_MODULE_NORET;

  bridge = n_xbee_find_bridge_bytty(tty);
  if (!bridge)
    return;
  dbuf = bridge->recvbuf;

  spin_lock(&bridge->read_lock);

  if (count > dbuf->size) {
    printk(KERN_ALERT "%s reading %d bytes which is more than the entire receive buffer size %d\n", tty->name, count, dbuf->size);
    count = dbuf->size;
  }

  finlen = dbuf->pos + count;
  if (finlen > dbuf->size) {
    printk(KERN_ALERT "%s reading %d bytes would overflow recvbuf with %d/%d bytes, dropping existing buffer\n", tty->name, count, dbuf->pos, dbuf->size);
    dbuf->pos = 0;
    finlen = count;
  }
  memcpy((dbuf->buffer + dbuf->pos), cp, count);
  dbuf->pos += count;

#ifdef N_XBEE_VERBOSE
  printk(KERN_INFO "%s: receive buffer size: %d\n", __FUNCTION__, dbuf->pos);
#endif

  spin_unlock(&bridge->read_lock);
}

static void n_xbee_write_wakeup(struct tty_struct* tty) {
  tty->flags &= ~(1 << TTY_DO_WRITE_WAKEUP);
  //maybe wakeup netdev queue here?
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
  .read  = n_xbee_read,
  .write = n_xbee_write,
  .flush_buffer = n_xbee_flush_buffer,
  .chars_in_buffer = n_xbee_chars_in_buffer,
  .ioctl = n_xbee_serial_ioctl,
  .receive_buf = n_xbee_receive_buf,
  .write_wakeup = n_xbee_write_wakeup
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
