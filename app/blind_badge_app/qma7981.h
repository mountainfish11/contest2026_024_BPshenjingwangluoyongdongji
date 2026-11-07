/****************************************************************************
 * Contest 2026 team 024 - QMA7981 3-axis accelerometer driver
 *
 * QMA7981 is the on-board accelerometer of ESP32-S3-EYE.
 * I2C address: 0x12 (7-bit).  ESP32-S3-EYE production boards have
 * been observed with chip ID 0x90; newer revisions use the 0xEx family.
 *
 * Uses the NuttX I2C character device (/dev/i2c0) via ioctl.
 ****************************************************************************/

#ifndef __BLIND_BADGE_QMA7981_H
#define __BLIND_BADGE_QMA7981_H

#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define QMA7981_I2C_ADDR      0x12
#define QMA7981_CHIP_ID_LEGACY 0x90
#define QMA7981_CHIP_ID_MASK  0xf0
#define QMA7981_CHIP_ID_VALUE 0xe0

/* Register map */

#define QMA7981_REG_CHIP_ID   0x00
#define QMA7981_REG_X_LSB     0x01
#define QMA7981_REG_X_MSB     0x02
#define QMA7981_REG_Y_LSB     0x03
#define QMA7981_REG_Y_MSB     0x04
#define QMA7981_REG_Z_LSB     0x05
#define QMA7981_REG_Z_MSB     0x06
#define QMA7981_REG_RANGE     0x0f
#define QMA7981_REG_BW        0x10
#define QMA7981_REG_MODE      0x11
#define QMA7981_REG_NVM_CTRL  0x33

/* Range settings for REG_RANGE */

#define QMA7981_RANGE_2G      0x01
#define QMA7981_RANGE_4G      0x02
#define QMA7981_RANGE_8G      0x04
#define QMA7981_RANGE_16G     0x08

/* Bandwidth settings for REG_BW */

#define QMA7981_BW_MCLK_7695  0xe0

/* Mode settings for REG_MODE */

#define QMA7981_MODE_STANDBY  0x00
#define QMA7981_MODE_ACTIVE   0x80
#define QMA7981_NVM_LOAD      0x08

/* Full-scale sensitivity (LSB per g) */

#define QMA7981_SENS_2G       4096
#define QMA7981_SENS_4G       2048
#define QMA7981_SENS_8G       1024
#define QMA7981_SENS_16G      512

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct qma7981_sample
{
  float x;  /* Acceleration in g */
  float y;
  float z;
  float magnitude;  /* sqrt(x²+y²+z²) in g */
};

struct qma7981_dev
{
  int fd;             /* /dev/i2cN file descriptor */
  uint8_t addr;
  uint8_t chip_id;
  uint8_t range;      /* QMA7981_RANGE_xx */
  float sensitivity;  /* LSB per g for current range */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/**
 * Open I2C device node, verify QMA7981 chip ID, set ±8g range,
 * configure the default output bandwidth, and enter active mode.
 *
 * @param dev       Device handle (filled on success).
 * @param i2c_path  e.g. "/dev/i2c0".
 * @return 0 on success, negative errno on failure.
 */
int qma7981_init(struct qma7981_dev *dev, const char *i2c_path);

/**
 * Close the I2C device node.
 */
void qma7981_close(struct qma7981_dev *dev);

/**
 * Read one acceleration sample.
 *
 * @param dev  Device handle from qma7981_init.
 * @param out  Sample output (x/y/z in g, magnitude in g).
 * @return 0 on success, negative errno on failure.
 */
int qma7981_read_sample(struct qma7981_dev *dev, struct qma7981_sample *out);

/**
 * Put sensor into standby mode (low power).
 */
int qma7981_set_standby(struct qma7981_dev *dev);

/**
 * Put sensor into active (measurement) mode.
 */
int qma7981_set_active(struct qma7981_dev *dev);

#endif /* __BLIND_BADGE_QMA7981_H */
