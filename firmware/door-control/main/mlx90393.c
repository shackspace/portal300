#include "mlx90393.h"

#include <driver/i2c.h>
#include <driver/gpio.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define I2C_DEVICE         I2C_NUM_0
#define I2C_MASTER_SDA_IO  GPIO_NUM_33
#define I2C_MASTER_SCL_IO  GPIO_NUM_32
#define I2C_MASTER_FREQ_HZ 100000
#define I2C_TIMEOUT        (100 / portTICK_PERIOD_MS)

int                       i2c_master_port = 0;
static const i2c_config_t i2c_config      = {
    .mode             = I2C_MODE_MASTER,
    .sda_io_num       = I2C_MASTER_SDA_IO, // select GPIO specific to your project
    .sda_pullup_en    = GPIO_PULLUP_DISABLE,
    .scl_io_num       = I2C_MASTER_SCL_IO, // select GPIO specific to your project
    .scl_pullup_en    = GPIO_PULLUP_DISABLE,
    .master.clk_speed = I2C_MASTER_FREQ_HZ, // select frequency specific to your project
};

/** Register map. */
enum
{
  MLX90393_REG_SB  = (0x10), /**< Start burst mode. */
  MLX90393_REG_SW  = (0x20), /**< Start wakeup on change mode. */
  MLX90393_REG_SM  = (0x30), /**> Start single-meas mode. */
  MLX90393_REG_RM  = (0x40), /**> Read measurement. */
  MLX90393_REG_RR  = (0x50), /**< Read register. */
  MLX90393_REG_WR  = (0x60), /**< Write register. */
  MLX90393_REG_EX  = (0x80), /**> Exit moode. */
  MLX90393_REG_HR  = (0xD0), /**< Memory recall. */
  MLX90393_REG_HS  = (0x70), /**< Memory store. */
  MLX90393_REG_RT  = (0xF0), /**< Reset. */
  MLX90393_REG_NOP = (0x00), /**< NOP. */
};

/** Lookup table to convert raw values to uT based on [HALLCONF][GAIN_SEL][RES].
 */
const float mlx90393_lsb_lookup[2][8][4][2] = {

    /* HALLCONF = 0xC (default) */
    {
        /* GAIN_SEL = 0, 5x gain */
        {{0.751, 1.210}, {1.502, 2.420}, {3.004, 4.840}, {6.009, 9.680}},
        /* GAIN_SEL = 1, 4x gain */
        {{0.601, 0.968}, {1.202, 1.936}, {2.403, 3.872}, {4.840, 7.744}},
        /* GAIN_SEL = 2, 3x gain */
        {{0.451, 0.726}, {0.901, 1.452}, {1.803, 2.904}, {3.605, 5.808}},
        /* GAIN_SEL = 3, 2.5x gain */
        {{0.376, 0.605}, {0.751, 1.210}, {1.502, 2.420}, {3.004, 4.840}},
        /* GAIN_SEL = 4, 2x gain */
        {{0.300, 0.484}, {0.601, 0.968}, {1.202, 1.936}, {2.403, 3.872}},
        /* GAIN_SEL = 5, 1.667x gain */
        {{0.250, 0.403}, {0.501, 0.807}, {1.001, 1.613}, {2.003, 3.227}},
        /* GAIN_SEL = 6, 1.333x gain */
        {{0.200, 0.323}, {0.401, 0.645}, {0.801, 1.291}, {1.602, 2.581}},
        /* GAIN_SEL = 7, 1x gain */
        {{0.150, 0.242}, {0.300, 0.484}, {0.601, 0.968}, {1.202, 1.936}},
    },

    /* HALLCONF = 0x0 */
    {
        /* GAIN_SEL = 0, 5x gain */
        {{0.787, 1.267}, {1.573, 2.534}, {3.146, 5.068}, {6.292, 10.137}},
        /* GAIN_SEL = 1, 4x gain */
        {{0.629, 1.014}, {1.258, 2.027}, {2.517, 4.055}, {5.034, 8.109}},
        /* GAIN_SEL = 2, 3x gain */
        {{0.472, 0.760}, {0.944, 1.521}, {1.888, 3.041}, {3.775, 6.082}},
        /* GAIN_SEL = 3, 2.5x gain */
        {{0.393, 0.634}, {0.787, 1.267}, {1.573, 2.534}, {3.146, 5.068}},
        /* GAIN_SEL = 4, 2x gain */
        {{0.315, 0.507}, {0.629, 1.014}, {1.258, 2.027}, {2.517, 4.055}},
        /* GAIN_SEL = 5, 1.667x gain */
        {{0.262, 0.422}, {0.524, 0.845}, {1.049, 1.689}, {2.097, 3.379}},
        /* GAIN_SEL = 6, 1.333x gain */
        {{0.210, 0.338}, {0.419, 0.676}, {0.839, 1.352}, {1.678, 2.703}},
        /* GAIN_SEL = 7, 1x gain */
        {{0.157, 0.253}, {0.315, 0.507}, {0.629, 1.014}, {1.258, 2.027}},
    }};

/** Lookup table for conversion time based on [DIF_FILT][OSR].
 */
const float mlx90393_tconv[8][4] = {
    /* DIG_FILT = 0 */
    {1.27, 1.84, 3.00, 5.30},
    /* DIG_FILT = 1 */
    {1.46, 2.23, 3.76, 6.84},
    /* DIG_FILT = 2 */
    {1.84, 3.00, 5.30, 9.91},
    /* DIG_FILT = 3 */
    {2.61, 4.53, 8.37, 16.05},
    /* DIG_FILT = 4 */
    {4.15, 7.60, 14.52, 28.34},
    /* DIG_FILT = 5 */
    {7.22, 13.75, 26.80, 52.92},
    /* DIG_FILT = 6 */
    {13.36, 26.04, 51.38, 102.07},
    /* DIF_FILT = 7 */
    {25.65, 50.61, 100.53, 200.37},
};

static inline void delay(uint32_t ms)
{
  vTaskDelay(ms / portTICK_PERIOD_MS);
}

#define DEFAULT_INTERDELAY 10

static bool    mlx90393_readRegister(struct MLX90393 * mlx, uint8_t reg, uint16_t * data);
static bool    mlx90393_writeRegister(struct MLX90393 * mlx, uint8_t reg, uint16_t data);
static uint8_t mlx90393_transceive(struct MLX90393 * mlx, uint8_t * txbuf, uint8_t txlen, uint8_t * rxbuf, uint8_t rxlen, uint8_t interdelay);

bool mlx90393_init(struct MLX90393 * mlx, uint8_t i2c_addr)
{
  *mlx = (struct MLX90393){
      .gain           = 0,
      .res_x          = 0,
      .res_y          = 0,
      .res_z          = 0,
      .dig_filt       = 0,
      .osr            = 0,
      .sensorID       = 90393,
      .cspin          = 0,
      .device_address = i2c_addr,
  };

  ESP_ERROR_CHECK(i2c_driver_install(I2C_DEVICE, I2C_MODE_MASTER, 0, 0, 0));
  ESP_ERROR_CHECK(i2c_param_config(I2C_DEVICE, &i2c_config));

  if (!mlx90393_exitMode(mlx))
    goto _error;

  if (!mlx90393_reset(mlx))
    goto _error;

  /* Set gain and sensor config. */
  if (!mlx90393_setGain(mlx, MLX90393_GAIN_1X)) {
    goto _error;
  }

  /* Set resolution. */
  if (!mlx90393_setResolution(mlx, MLX90393_X, MLX90393_RES_16))
    goto _error;
  if (!mlx90393_setResolution(mlx, MLX90393_Y, MLX90393_RES_16))
    goto _error;
  if (!mlx90393_setResolution(mlx, MLX90393_Z, MLX90393_RES_16))
    goto _error;

  /* Set oversampling. */
  if (!mlx90393_setOversampling(mlx, MLX90393_OSR_3))
    goto _error;

  /* Set digital filtering. */
  if (!mlx90393_setFilter(mlx, MLX90393_FILTER_7))
    goto _error;

  /* set INT pin to output interrupt */
  if (!mlx90393_setTrigInt(mlx, false))
    goto _error;

  return true;

_error:

  return false;
}

/**
 * Perform a mode exit
 * @return True if the operation succeeded, otherwise false.
 */
bool mlx90393_exitMode(struct MLX90393 * mlx)
{
  uint8_t tx[1] = {MLX90393_REG_EX};

  /* Perform the transaction. */
  return (mlx90393_transceive(mlx, tx, sizeof(tx), NULL, 0, 0) == MLX90393_STATUS_OK);
}

/**
 * Perform a soft reset
 * @return True if the operation succeeded, otherwise false.
 */
bool mlx90393_reset(struct MLX90393 * mlx)
{
  uint8_t tx[1] = {MLX90393_REG_RT};

  /* Perform the transaction. */
  if (mlx90393_transceive(mlx, tx, sizeof(tx), NULL, 0, 5) != MLX90393_STATUS_RESET) {
    return false;
  }
  return true;
}

/**
 * Sets the sensor gain to the specified level.
 * @param gain  The gain level to set.
 * @return True if the operation succeeded, otherwise false.
 */
bool mlx90393_setGain(struct MLX90393 * mlx, enum mlx90393_gain gain)
{
  mlx->gain = gain;

  uint16_t data;
  mlx90393_readRegister(mlx, MLX90393_CONF1, &data);

  // mask off gain bits
  data &= ~0x0070;
  // set gain bits
  data |= gain << MLX90393_GAIN_SHIFT;

  return mlx90393_writeRegister(mlx, MLX90393_CONF1, data);
}

/**
 * Gets the current sensor gain.
 *
 * @return An enum containing the current gain level.
 */
enum mlx90393_gain mlx90393_getGain(struct MLX90393 * mlx)
{
  uint16_t data;
  mlx90393_readRegister(mlx, MLX90393_CONF1, &data);

  // mask off gain bits
  data &= 0x0070;

  return (enum mlx90393_gain)(data >> 4);
}

/**
 * Sets the sensor resolution to the specified level.
 * @param axis  The axis to set.
 * @param resolution  The resolution level to set.
 * @return True if the operation succeeded, otherwise false.
 */
bool mlx90393_setResolution(struct MLX90393 *        mlx,
                            enum mlx90393_axis       axis,
                            enum mlx90393_resolution resolution)
{

  uint16_t data;
  mlx90393_readRegister(mlx, MLX90393_CONF3, &data);

  switch (axis) {
  case MLX90393_X:
    mlx->res_x = resolution;
    data &= ~0x0060;
    data |= resolution << 5;
    break;
  case MLX90393_Y:
    mlx->res_y = resolution;
    data &= ~0x0180;
    data |= resolution << 7;
    break;
  case MLX90393_Z:
    mlx->res_z = resolution;
    data &= ~0x0600;
    data |= resolution << 9;
    break;
  }

  return mlx90393_writeRegister(mlx, MLX90393_CONF3, data);
}

/**
 * Gets the current sensor resolution.
 * @param axis  The axis to get.
 * @return An enum containing the current resolution.
 */
enum mlx90393_resolution
mlx90393_getResolution(struct MLX90393 * mlx, enum mlx90393_axis axis)
{
  switch (axis) {
  case MLX90393_X:
    return mlx->res_x;
  case MLX90393_Y:
    return mlx->res_y;
  case MLX90393_Z:
    return mlx->res_z;
  }
  __builtin_unreachable();
}

/**
 * Sets the digital filter.
 * @param filter The digital filter setting.
 * @return True if the operation succeeded, otherwise false.
 */
bool mlx90393_setFilter(struct MLX90393 * mlx, enum mlx90393_filter filter)
{
  mlx->dig_filt = filter;

  uint16_t data;
  mlx90393_readRegister(mlx, MLX90393_CONF3, &data);

  data &= ~0x1C;
  data |= filter << 2;

  return mlx90393_writeRegister(mlx, MLX90393_CONF3, data);
}

/**
 * Gets the current digital filter setting.
 * @return An enum containing the current digital filter setting.
 */
enum mlx90393_filter mlx90393_getFilter(struct MLX90393 * mlx)
{
  return mlx->dig_filt;
}

/**
 * Sets the oversampling.
 * @param oversampling The oversampling value to use.
 * @return True if the operation succeeded, otherwise false.
 */
bool mlx90393_setOversampling(struct MLX90393 * mlx, enum mlx90393_oversampling oversampling)
{
  mlx->osr = oversampling;

  uint16_t data;
  mlx90393_readRegister(mlx, MLX90393_CONF3, &data);

  data &= ~0x03;
  data |= oversampling;

  return mlx90393_writeRegister(mlx, MLX90393_CONF3, data);
}

/**
 * Gets the current oversampling setting.
 * @return An enum containing the current oversampling setting.
 */
enum mlx90393_oversampling mlx90393_getOversampling(struct MLX90393 * mlx)
{
  return mlx->osr;
}

/**
 * Sets the TRIG_INT pin to the specified function.
 *
 * @param state  'true/1' sets the pin to INT, 'false/0' to TRIG.
 *
 * @return True if the operation succeeded, otherwise false.
 */
bool mlx90393_setTrigInt(struct MLX90393 * mlx, bool state)
{
  uint16_t data;
  mlx90393_readRegister(mlx, MLX90393_CONF2, &data);

  // mask off trigint bit
  data &= ~0x8000;

  // set trigint bit if desired
  if (state) {
    /* Set the INT, highest bit */
    data |= 0x8000;
  }

  return mlx90393_writeRegister(mlx, MLX90393_CONF2, data);
}

/**
 * Begin a single measurement on all axes
 *
 * @return True on command success
 */
bool mlx90393_startSingleMeasurement(struct MLX90393 * mlx)
{
  uint8_t tx[1] = {MLX90393_REG_SM | MLX90393_AXIS_ALL};

  /* Set the device to single measurement mode */
  uint8_t stat = mlx90393_transceive(mlx, tx, sizeof(tx), NULL, 0, 0);
  if ((stat == MLX90393_STATUS_OK) || (stat == MLX90393_STATUS_SMMODE)) {
    return true;
  }
  return false;
}

/**
 * Reads data from data register & returns the results.
 *
 * @param x     Pointer to where the 'x' value should be stored.
 * @param y     Pointer to where the 'y' value should be stored.
 * @param z     Pointer to where the 'z' value should be stored.
 *
 * @return True on command success
 */
bool mlx90393_readMeasurement(struct MLX90393 * mlx, float * x, float * y, float * z)
{
  uint8_t tx[1] = {MLX90393_REG_RM | MLX90393_AXIS_ALL};
  uint8_t rx[6] = {0};

  /* Read a single data sample. */
  if (mlx90393_transceive(mlx, tx, sizeof(tx), rx, sizeof(rx), 0) != MLX90393_STATUS_OK) {
    return false;
  }

  int16_t xi, yi, zi;

  /* Convert data to uT and float. */
  xi = (rx[0] << 8) | rx[1];
  yi = (rx[2] << 8) | rx[3];
  zi = (rx[4] << 8) | rx[5];

  if (mlx->res_x == MLX90393_RES_18)
    xi -= 0x8000;
  if (mlx->res_x == MLX90393_RES_19)
    xi -= 0x4000;
  if (mlx->res_y == MLX90393_RES_18)
    yi -= 0x8000;
  if (mlx->res_y == MLX90393_RES_19)
    yi -= 0x4000;
  if (mlx->res_z == MLX90393_RES_18)
    zi -= 0x8000;
  if (mlx->res_z == MLX90393_RES_19)
    zi -= 0x4000;

  *x = (float)xi * mlx90393_lsb_lookup[0][mlx->gain][mlx->res_x][0];
  *y = (float)yi * mlx90393_lsb_lookup[0][mlx->gain][mlx->res_y][0];
  *z = (float)zi * mlx90393_lsb_lookup[0][mlx->gain][mlx->res_z][1];

  return true;
}

/**
 * Performs a single X/Y/Z conversion and returns the results.
 *
 * @param x     Pointer to where the 'x' value should be stored.
 * @param y     Pointer to where the 'y' value should be stored.
 * @param z     Pointer to where the 'z' value should be stored.
 *
 * @return True if the operation succeeded, otherwise false.
 */
bool mlx90393_readData(struct MLX90393 * mlx, float * x, float * y, float * z)
{
  if (!mlx90393_startSingleMeasurement(mlx))
    return false;
  // See MLX90393 Getting Started Guide for fancy formula
  // tconv = f(OSR, DIG_FILT, OSR2, ZYXT)
  // For now, using Table 18 from datasheet
  // Without +10ms delay measurement doesn't always seem to work
  delay(mlx90393_tconv[mlx->dig_filt][mlx->osr] + 10);
  return mlx90393_readMeasurement(mlx, x, y, z);
}

static bool mlx90393_writeRegister(struct MLX90393 * mlx, uint8_t reg, uint16_t data)
{
  uint8_t tx[4] = {
      MLX90393_REG_WR,
      data >> 8,   // high byte
      data & 0xFF, // low byte
      reg << 2,    // the register itself, shift up by 2 bits!
  };

  /* Perform the transaction. */
  return (mlx90393_transceive(mlx, tx, sizeof(tx), NULL, 0, 0) == MLX90393_STATUS_OK);
}

static bool mlx90393_readRegister(struct MLX90393 * mlx, uint8_t reg, uint16_t * data)
{
  uint8_t tx[2] = {
      MLX90393_REG_RR,
      reg << 2, // the register itself, shift up by 2 bits!
  };

  uint8_t rx[2];

  /* Perform the transaction. */
  if (mlx90393_transceive(mlx, tx, sizeof(tx), rx, sizeof(rx), 0) != MLX90393_STATUS_OK) {
    return false;
  }

  *data = ((uint16_t)rx[0] << 8) | rx[1];

  return true;
}

/**
 * Performs a full read/write transaction with the sensor.
 *
 * @param txbuf     Pointer the the buffer containing the data to write.
 * @param txlen     The number of bytes to write.
 * @param rxbuf     Pointer to an appropriately large buffer where data read
 *                  back will be written.
 * @param rxlen     The number of bytes to read back (not including the
 *                  mandatory status byte that is always returned).
 *
 * @return The status byte from the IC.
 */
static uint8_t mlx90393_transceive(struct MLX90393 * mlx, uint8_t * txbuf, uint8_t txlen, uint8_t * rxbuf, uint8_t rxlen, uint8_t interdelay)
{
  uint8_t status = 0;
  uint8_t i;
  uint8_t rxbuf2[rxlen + 2];

  ESP_ERROR_CHECK(i2c_master_write_to_device(I2C_DEVICE, mlx->device_address, txbuf, txlen, I2C_TIMEOUT));

  delay(interdelay);

  ESP_ERROR_CHECK(i2c_master_read_from_device(I2C_DEVICE, mlx->device_address, rxbuf2, rxlen + 1, I2C_TIMEOUT));

  status = rxbuf2[0];
  for (i = 0; i < rxlen; i++) {
    rxbuf[i] = rxbuf2[i + 1];
  }

  /* Mask out bytes available in the status response. */
  return (status >> 2);
}
