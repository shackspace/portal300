#ifndef PORTAL300_MLX90393_H
#define PORTAL300_MLX90393_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define MLX90393_DEFAULT_ADDR (0x0C) /* Can also be 0x18, depending on IC */

#define MLX90393_AXIS_ALL      (0x0E) /**< X+Y+Z axis bits for commands. */
#define MLX90393_CONF1         (0x00) /**< Gain */
#define MLX90393_CONF2         (0x01) /**< Burst, comm mode */
#define MLX90393_CONF3         (0x02) /**< Oversampling, filter, res. */
#define MLX90393_CONF4         (0x03) /**< Sensitivty drift. */
#define MLX90393_GAIN_SHIFT    (4)    /**< Left-shift for gain bits. */
#define MLX90393_HALL_CONF     (0x0C) /**< Hall plate spinning rate adj. */
#define MLX90393_STATUS_OK     (0x00) /**< OK value for status response. */
#define MLX90393_STATUS_SMMODE (0x08) /**< SM Mode status response. */
#define MLX90393_STATUS_RESET  (0x01) /**< Reset value for status response. */
#define MLX90393_STATUS_ERROR  (0xFF) /**< OK value for status response. */
#define MLX90393_STATUS_MASK   (0xFC) /**< Mask for status OK checks. */

/** Gain settings for CONF1 register. */
enum mlx90393_gain
{
  MLX90393_GAIN_5X = (0x00),
  MLX90393_GAIN_4X,
  MLX90393_GAIN_3X,
  MLX90393_GAIN_2_5X,
  MLX90393_GAIN_2X,
  MLX90393_GAIN_1_67X,
  MLX90393_GAIN_1_33X,
  MLX90393_GAIN_1X
};

/** Resolution settings for CONF3 register. */
enum mlx90393_resolution
{
  MLX90393_RES_16,
  MLX90393_RES_17,
  MLX90393_RES_18,
  MLX90393_RES_19,
};

/** Axis designator. */
enum mlx90393_axis
{
  MLX90393_X,
  MLX90393_Y,
  MLX90393_Z
};

/** Digital filter settings for CONF3 register. */
enum mlx90393_filter
{
  MLX90393_FILTER_0,
  MLX90393_FILTER_1,
  MLX90393_FILTER_2,
  MLX90393_FILTER_3,
  MLX90393_FILTER_4,
  MLX90393_FILTER_5,
  MLX90393_FILTER_6,
  MLX90393_FILTER_7,
};

/** Oversampling settings for CONF3 register. */
enum mlx90393_oversampling
{
  MLX90393_OSR_0,
  MLX90393_OSR_1,
  MLX90393_OSR_2,
  MLX90393_OSR_3,
};

struct MLX90393
{
  uint8_t                    device_address;
  enum mlx90393_gain         gain;
  enum mlx90393_resolution   res_x, res_y, res_z;
  enum mlx90393_filter       dig_filt;
  enum mlx90393_oversampling osr;

  int32_t sensorID;
  int     cspin;
};

bool mlx90393_init(struct MLX90393 * mlx, uint8_t i2c_addr);

bool mlx90393_reset(struct MLX90393 * mlx);
bool mlx90393_exitMode(struct MLX90393 * mlx);

bool mlx90393_readMeasurement(struct MLX90393 * mlx, float * x, float * y, float * z);
bool mlx90393_startSingleMeasurement(struct MLX90393 * mlx);

bool               mlx90393_setGain(struct MLX90393 * mlx, enum mlx90393_gain gain);
enum mlx90393_gain mlx90393_getGain(struct MLX90393 * mlx);

bool                     mlx90393_setResolution(struct MLX90393 * mlx, enum mlx90393_axis, enum mlx90393_resolution resolution);
enum mlx90393_resolution mlx90393_getResolution(struct MLX90393 * mlx, enum mlx90393_axis);

bool                 mlx90393_setFilter(struct MLX90393 * mlx, enum mlx90393_filter filter);
enum mlx90393_filter mlx90393_getFilter(struct MLX90393 * mlx);

bool                       mlx90393_setOversampling(struct MLX90393 * mlx, enum mlx90393_oversampling oversampling);
enum mlx90393_oversampling mlx90393_getOversampling(struct MLX90393 * mlx);

bool mlx90393_setTrigInt(struct MLX90393 * mlx, bool state);
bool mlx90393_readData(struct MLX90393 * mlx, float * x, float * y, float * z);

#endif