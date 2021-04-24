/* PCI PIIX4 IDE */
#define KL_LOG KL_DEV
#include <sys/klog.h>
#include <sys/mimiker.h>
#include <sys/pci.h>
#include <sys/interrupt.h>
#include <sys/klog.h>
#include <sys/timer.h>
#include <sys/spinlock.h>
#include <sys/devclass.h>
#include <sys/malloc.h>
#include <dev/atareg.h>

#define SECTOR_SIZE 512

typedef struct ide_chs {
  int cylinder;
  int head;
  int sector;
} ide_chs_t;

typedef struct ide_device {
  uint8_t identification_space[512];
#define ATA_IS_MODEL 54
#define ATA_IS_SIZE 120
  uint8_t model[41];
  uint32_t size;
} ide_device_t;

typedef struct ide_channel_resources {
  resource_t *base;
  resource_t *ctrl;
  resource_t *bmide;
  unsigned char nIEN;
} ide_channel_resources_t;

typedef struct ide_state {
  ide_channel_resources_t ide_channels[2];
  ide_device_t ide_devices[4];
} ide_state_t;

static void ide_write(ide_state_t *ide, unsigned char channel,
                      unsigned char reg, unsigned char data) {
  if (reg < 0x08)
    bus_write_1(ide->ide_channels[channel].base, reg - 0x00, data);
  else if (reg < 0x10)
    bus_write_1(ide->ide_channels[channel].ctrl, reg - 0x08, data);
  else
    bus_write_1(ide->ide_channels[channel].bmide, reg + channel * 0x08 - 0x10,
                data);
}

static void ide_write2(ide_state_t *ide, unsigned char channel,
                       unsigned char reg, unsigned short data) {
  if (reg < 0x08)
    bus_write_2(ide->ide_channels[channel].base, reg - 0x00, data);
  else if (reg < 0x10)
    bus_write_2(ide->ide_channels[channel].ctrl, reg - 0x08, data);
  else
    bus_write_2(ide->ide_channels[channel].bmide, reg + channel * 0x08 - 0x10,
                data);
}

static unsigned char ide_read(ide_state_t *ide, unsigned char channel,
                              unsigned char reg) {
  unsigned char result;
  if (reg < 0x08)
    result = bus_read_1(ide->ide_channels[channel].base, reg - 0x00);
  else if (reg < 0x10)
    result = bus_read_1(ide->ide_channels[channel].ctrl, reg - 0x08);
  else
    result =
      bus_read_1(ide->ide_channels[channel].bmide, reg + channel * 0x08 - 0x10);
  return result;
}

static unsigned short ide_read2(ide_state_t *ide, unsigned char channel,
                                unsigned char reg) {
  unsigned short result;
  if (reg < 0x08)
    result = bus_read_2(ide->ide_channels[channel].base, reg - 0x00);
  else if (reg < 0x10)
    result = bus_read_2(ide->ide_channels[channel].ctrl, reg - 0x08);
  else
    result =
      bus_read_2(ide->ide_channels[channel].bmide, reg + channel * 0x08 - 0x10);
  return result;
}

/* It should wait an order of n milliseconds */
static void active_wait(int n) {
  for (volatile int i = 0; i < 1000000 * n; i++)
    continue;
}

static void ide_wait_busy(ide_state_t *ide, unsigned char channel) {
  while (ide_read(ide, channel, ATA_REG_STATUS) & WDCS_BSY)
    ;
}

static void ide_wait_ready(ide_state_t *ide, unsigned char channel) {
  while (!(ide_read(ide, channel, ATA_REG_STATUS) & WDCS_DRDY))
    ;
}

static ide_chs_t ide_lba_to_chs(uint32_t logical_address) {
  ide_chs_t chs;

  chs.sector = (logical_address % 63) + 1;
  chs.cylinder = (logical_address + 1 - chs.sector) / (16 * 63);
  chs.head = (logical_address + 1 - chs.sector) % (16 * 63) / (63);

  return chs;
}

static void ide_drive_access(ide_state_t *ide, uint8_t drive, uint16_t *buffer,
                             uint32_t logical_address, uint8_t sector_count,
                             uint8_t action) {
  if (sector_count == 0)
    return;

  uint8_t channel;
  uint8_t slave;
  ide_chs_t chs;

  channel = drive / 2;
  slave = drive % 2;
  chs = ide_lba_to_chs(logical_address);

  ide_wait_busy(ide, channel);

  ide_write(ide, channel, ATA_REG_HDDEVSEL, 0xA0 | (slave << 4) | chs.head);
  ide_write(ide, channel, ATA_REG_SECCOUNT0, sector_count);
  ide_write(ide, channel, ATA_REG_LBA0, chs.sector);
  ide_write(ide, channel, ATA_REG_LBA1, (chs.cylinder >> 0) & 0xFF);
  ide_write(ide, channel, ATA_REG_LBA2, (chs.cylinder >> 8) & 0xFF);

  ide_write(ide, channel, ATA_REG_COMMAND, action);

  for (int i = 0; i < sector_count; i++) {

    ide_wait_busy(ide, channel);

    ide_wait_ready(ide, channel);

    for (int j = 0; j < 256; j++)
      if (action == WDCC_READ)
        buffer[j] = ide_read2(ide, channel, ATA_REG_DATA);
      else
        ide_write2(ide, channel, ATA_REG_DATA, buffer[j]);
    buffer += 256;
  }
}

static void __unused ide_drive_read(ide_state_t *ide, uint8_t drive,
                                    void *buffer, uint32_t logical_address,
                                    int nbytes) {

  int full_sectors = nbytes / 512;
  int full_sectors_nbytes = full_sectors * 512;
  ide_drive_access(ide, drive, buffer, logical_address, full_sectors,
                   WDCC_READ);

  int bytes_left = nbytes - full_sectors_nbytes;

  if (bytes_left) {
    uint8_t *last_sector = kmalloc(M_TEMP, 512, M_ZERO);
    ide_drive_access(ide, drive, (uint16_t *)last_sector,
                     logical_address + full_sectors_nbytes, 1, WDCC_READ);

    for (int i = 0; i < bytes_left; i++)
      ((uint8_t *)buffer)[full_sectors_nbytes + i] = last_sector[i];

    kfree(M_TEMP, last_sector);
  }
}

static void __unused ide_drive_write(ide_state_t *ide, uint8_t drive,
                                     void *buffer, uint32_t logical_address,
                                     int nbytes) {

  int full_sectors = nbytes / 512;
  int full_sectors_nbytes = full_sectors * 512;
  ide_drive_access(ide, drive, buffer, logical_address, full_sectors,
                   WDCC_WRITE);

  int bytes_left = nbytes - full_sectors_nbytes;

  if (bytes_left) {
    uint8_t *last_sector = kmalloc(M_TEMP, 512, M_ZERO);
    ide_drive_access(ide, drive, (uint16_t *)last_sector,
                     logical_address + full_sectors_nbytes, 1, WDCC_READ);

    for (int i = 0; i < bytes_left; i++)
      last_sector[i] = ((uint8_t *)buffer)[full_sectors_nbytes + i];

    ide_drive_access(ide, drive, (uint16_t *)last_sector,
                     logical_address + full_sectors_nbytes, 1, WDCC_WRITE);

    kfree(M_TEMP, last_sector);
  }
}

static void ide_take_resources(device_t *dev) {
  ide_state_t *ide = dev->state;

  ide->ide_channels[0].base = device_take_ioports(dev, 0, RF_ACTIVE);
  assert(ide->ide_channels[0].base != NULL);

  ide->ide_channels[0].ctrl = device_take_ioports(dev, 1, RF_ACTIVE);
  assert(ide->ide_channels[0].ctrl != NULL);

  ide->ide_channels[0].bmide = device_take_ioports(dev, 4, RF_ACTIVE);
  assert(ide->ide_channels[0].bmide != NULL);

  ide->ide_channels[0].nIEN = ATA_DISABLE_INTR;

  ide->ide_channels[1].base = device_take_ioports(dev, 2, RF_ACTIVE);
  assert(ide->ide_channels[1].base != NULL);

  ide->ide_channels[1].ctrl = device_take_ioports(dev, 3, RF_ACTIVE);
  assert(ide->ide_channels[1].ctrl != NULL);

  /* bmide for both channels is the same resource */
  ide->ide_channels[1].bmide = ide->ide_channels[0].bmide;

  ide->ide_channels[1].nIEN = ATA_DISABLE_INTR;
}

static void ide_drive_read_size(ide_state_t *ide, int drive) {
  uint32_t *is = (uint32_t *)ide->ide_devices[drive].identification_space;

  ide->ide_devices[drive].size = 512 * is[ATA_IS_SIZE / sizeof(uint32_t)];
}

static void ide_drive_read_model(ide_state_t *ide, int drive) {
  uint8_t *is = ide->ide_devices[drive].identification_space;
  uint8_t *model = ide->ide_devices[drive].model;

  /* big-endian little-endian conversion */
  for (int i = 0; i < 40; i += 2) {
    model[i] = is[ATA_IS_MODEL + i + 1];
    model[i + 1] = is[ATA_IS_MODEL + i];
  }
  model[40] = 0;
}

static void ide_drive_read_is(ide_state_t *ide, int drive) {
  ide_drive_read_size(ide, drive);
  ide_drive_read_model(ide, drive);
}

static void ide_drive_identify(ide_state_t *ide, int channel, int slave) {

  /* Select the drive */
  ide_write(ide, channel, ATA_REG_HDDEVSEL, 0xA0 | (slave << 4));
  active_wait(1);

  ide_write(ide, channel, ATA_REG_COMMAND, WDCC_IDENTIFY);
  active_wait(1);

  while (1) {
    uint8_t status = ide_read(ide, channel, ATA_REG_STATUS);

    if ((status & WDCS_ERR) || (status == 0))
      return;

    if (!(status & WDCS_BSY) && (status & WDCS_DRQ))
      break;
  }

  int drive = channel * 2 + slave;
  uint16_t *is = (uint16_t *)ide->ide_devices[drive].identification_space;

  for (int i = 0; i < 256; i++) {
    is[i] = ide_read2(ide, channel, ATA_REG_DATA);
  }

  ide_drive_read_is(ide, drive);

  if (ide->ide_devices[drive].size) {
    klog("Found %s of size %d bytes on channel %d , position %d",
         ide->ide_devices[drive].model, ide->ide_devices[drive].size, channel,
         slave);
  }
}

static int ide_attach(device_t *dev) {
  ide_state_t *ide = dev->state;

  ide_take_resources(dev);

  /* We have to zero the control registers first */
  ide_write(ide, 0, ATA_REG_CONTROL, 0);
  ide_write(ide, 1, ATA_REG_CONTROL, 0);

  /* For now we don't use interrupts */
  ide_write(ide, 0, ATA_REG_CONTROL, ATA_DISABLE_INTR);
  ide_write(ide, 1, ATA_REG_CONTROL, ATA_DISABLE_INTR);

  for (int channel = 0; channel < 2; channel++)
    for (int slave = 0; slave < 2; slave++)
      ide_drive_identify(ide, channel, slave);

  return 0;
}

static int ide_probe(device_t *dev) {
  pci_device_t *pcid = pci_device_of(dev);

  if (!pci_device_match(pcid, 0x8086, 0x7111))
    return 0;

  return 1;
}

/* clang-format off */
static driver_t ide_driver = {
  .desc = "IDE driver",
  .size = sizeof(ide_state_t),
  .attach = ide_attach,
  .probe = ide_probe
};
/* clang-format on */

DEVCLASS_ENTRY(pci, ide_driver);
