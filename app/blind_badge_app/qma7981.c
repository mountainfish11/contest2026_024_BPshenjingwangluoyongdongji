/****************************************************************************
 * Contest 2026 team 024 - QMA7981 3-axis accelerometer driver
 *
 * ESP32-S3-EYE board: QMA7981 on I2C bus 0, address 0x12.
 * Data registers are 14-bit two's-complement values.  Each LSB register
 * stores data bits 5:0 in bits 7:2 and status bits in bits 1:0.
 *
 * Uses the NuttX I2C character device (/dev/i2c0) via I2CIOC_TRANSFER.
 ****************************************************************************/

#include "qma7981.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <nuttx/i2c/i2c_master.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define QMA7981_I2C_FREQ   400000

/****************************************************************************
 * Private Functions - I2C Helpers (via /dev/i2cN ioctl)
 ****************************************************************************/

static int qma7981_i2c_xfer(struct qma7981_dev *dev,
                            struct i2c_msg_s *msgs, int count)
{
  struct i2c_transfer_s xfer;

  xfer.msgv = msgs;
  xfer.msgc = count;
  return ioctl(dev->fd, I2CIOC_TRANSFER, (unsigned long)&xfer);
}

static int qma7981_writereg8(struct qma7981_dev *dev,
                             uint8_t reg, uint8_t val)
{
  struct i2c_msg_s msg;
  uint8_t buf[2];
  int ret;

  buf[0] = reg;
  buf[1] = val;

  msg.frequency = QMA7981_I2C_FREQ;
  msg.addr      = dev->addr;
  msg.flags     = 0;
  msg.buffer    = buf;
  msg.length    = 2;

  ret = qma7981_i2c_xfer(dev, &msg, 1);
  return ret < 0 ? ret : 0;
}

static int qma7981_readreg8(struct qma7981_dev *dev,
                            uint8_t reg, uint8_t *val)
{
  struct i2c_msg_s msg[2];
  int ret;

  msg[0].frequency = QMA7981_I2C_FREQ;
  msg[0].addr      = dev->addr;
  msg[0].flags     = I2C_M_NOSTOP;
  msg[0].buffer    = &reg;
  msg[0].length    = 1;

  msg[1].frequency = QMA7981_I2C_FREQ;
  msg[1].addr      = dev->addr;
  msg[1].flags     = I2C_M_READ;
  msg[1].buffer    = val;
  msg[1].length    = 1;

  ret = qma7981_i2c_xfer(dev, msg, 2);
  return ret < 0 ? ret : 0;
}

static int qma7981_readregs(struct qma7981_dev *dev,
                            uint8_t reg, uint8_t *buf, uint8_t len)
{
  struct i2c_msg_s msg[2];
  int ret;

  msg[0].frequency = QMA7981_I2C_FREQ;
  msg[0].addr      = dev->addr;
  msg[0].flags     = I2C_M_NOSTOP;
  msg[0].buffer    = &reg;
  msg[0].length    = 1;

  msg[1].frequency = QMA7981_I2C_FREQ;
  msg[1].addr      = dev->addr;
  msg[1].flags     = I2C_M_READ;
  msg[1].buffer    = buf;
  msg[1].length    = len;

  ret = qma7981_i2c_xfer(dev, msg, 2);
  return ret < 0 ? ret : 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int qma7981_init(struct qma7981_dev *dev, const char *i2c_path)
{
  uint8_t chip_id;
  int ret;

  if (dev == NULL || i2c_path == NULL)
    {
      return -EINVAL;
    }

  memset(dev, 0, sizeof(*dev));
  dev->addr = QMA7981_I2C_ADDR;

  dev->fd = open(i2c_path, O_RDWR);
  if (dev->fd < 0)
    {
      ret = -errno;
      printf("[QMA7981] error: cannot open %s (%d)\n", i2c_path, ret);
      return ret;
    }

  /* Verify chip ID */

  ret = qma7981_readreg8(dev, QMA7981_REG_CHIP_ID, &chip_id);
  if (ret < 0)
    {
      printf("[QMA7981] error: I2C read chip ID failed (%d)\n", ret);
      goto err_close;
    }

  if (chip_id != QMA7981_CHIP_ID_LEGACY &&
      (chip_id & QMA7981_CHIP_ID_MASK) != QMA7981_CHIP_ID_VALUE)
    {
      printf("[QMA7981] error: unexpected chip ID 0x%02x "
             "(expected 0x90 or 0xEx)\n", chip_id);
      ret = -ENODEV;
      goto err_close;
    }

  dev->chip_id = chip_id;

  /* Load the factory trim image.  QMA6100P-compatible 0x90 parts require
   * active mode while NVM_LOAD is asserted.  This also recovers cleanly if
   * the sensor was reset independently of the MCU.
   */

  ret = qma7981_writereg8(dev, QMA7981_REG_MODE, QMA7981_MODE_ACTIVE);
  if (ret < 0)
    {
      goto err_close;
    }

  usleep(5000);

  ret = qma7981_writereg8(dev, QMA7981_REG_NVM_CTRL, QMA7981_NVM_LOAD);
  if (ret < 0)
    {
      goto err_close;
    }

  usleep(5000);

  /* Standby before configuring */

  ret = qma7981_writereg8(dev, QMA7981_REG_MODE, QMA7981_MODE_STANDBY);
  if (ret < 0)
    {
      goto err_close;
    }

  /* ±8g preserves impact peaks while retaining 0.977 mg/LSB resolution. */

  ret = qma7981_writereg8(dev, QMA7981_REG_RANGE, QMA7981_RANGE_8G);
  if (ret < 0)
    {
      goto err_close;
    }

  dev->range       = QMA7981_RANGE_8G;
  dev->sensitivity = (float)QMA7981_SENS_8G;

  /* Keep the reserved high bits at their documented values. */

  ret = qma7981_writereg8(dev, QMA7981_REG_BW,
                          QMA7981_BW_MCLK_7695);
  if (ret < 0)
    {
      goto err_close;
    }

  /* Enter active mode */

  ret = qma7981_writereg8(dev, QMA7981_REG_MODE, QMA7981_MODE_ACTIVE);
  if (ret < 0)
    {
      goto err_close;
    }

  usleep(5000);

  printf("[QMA7981] initialized: chip_id=0x%02x range=±8g "
         "dev=%s\n", chip_id, i2c_path);
  return 0;

err_close:
  close(dev->fd);
  dev->fd = -1;
  return ret;
}

void qma7981_close(struct qma7981_dev *dev)
{
  if (dev != NULL && dev->fd >= 0)
    {
      close(dev->fd);
      dev->fd = -1;
    }
}

int qma7981_read_sample(struct qma7981_dev *dev, struct qma7981_sample *out)
{
  uint8_t buf[6];
  int16_t raw_x;
  int16_t raw_y;
  int16_t raw_z;
  int ret;

  if (dev == NULL || out == NULL || dev->fd < 0)
    {
      return -EINVAL;
    }

  /* Read 6 bytes: XL XH YL YH ZL ZH */

  ret = qma7981_readregs(dev, QMA7981_REG_X_LSB, buf, 6);
  if (ret < 0)
    {
      return ret;
    }

  /* Reconstruct the signed 14-bit value.  The two status bits in each LSB
   * byte must not be interpreted as acceleration data.
   */

  raw_x = (int16_t)((buf[1] << 8) | (buf[0] & 0xfc));
  raw_y = (int16_t)((buf[3] << 8) | (buf[2] & 0xfc));
  raw_z = (int16_t)((buf[5] << 8) | (buf[4] & 0xfc));
  raw_x >>= 2;
  raw_y >>= 2;
  raw_z >>= 2;

  out->x = (float)raw_x / dev->sensitivity;
  out->y = (float)raw_y / dev->sensitivity;
  out->z = (float)raw_z / dev->sensitivity;
  out->magnitude = sqrtf(out->x * out->x +
                         out->y * out->y +
                         out->z * out->z);
  return 0;
}

int qma7981_set_standby(struct qma7981_dev *dev)
{
  return qma7981_writereg8(dev, QMA7981_REG_MODE, QMA7981_MODE_STANDBY);
}

int qma7981_set_active(struct qma7981_dev *dev)
{
  return qma7981_writereg8(dev, QMA7981_REG_MODE, QMA7981_MODE_ACTIVE);
}
