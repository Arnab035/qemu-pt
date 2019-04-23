/* use a separate file that represents your record and replay mechanisms */

#ifndef EVENTS_H
#define EVENTS_H

#define PCI_DISK_EVENT        1
#define PCI_NETWORK_EVENT     2
#define VMENTRY_EVENT         3

struct replay_block_event {
  int64_t offset;
  unsigned long size;
  void *iov;       // QEMUIOVector *iov
  void *cb;
  void *cb_opaque;
  void *opaque;     // BlockBackend *blk
};

struct replay_network_event {
  void *device;      // PCIDevice *dev
};

#endif
