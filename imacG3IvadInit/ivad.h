/**
 * @file ivad.h
 * @brief IVAD board register definitions and configuration values
 *
 * Contains:
 * - I2C register addresses
 * - Setting offsets and enumeration
 * - Default, minimum, and maximum values for all 21 IVAD settings
 *
 * IVAD Settings (0x00-0x12):
 * - Color: CONTRAST, RGB_DRIVE, RGB_CUTOFF (6 settings)
 * - Geometry: HORIZONTAL_POS, HEIGHT, VERTICAL_POS (3 settings)
 * - Correction: S_CORRECTION, KEYSTONE, PINCUSHION, WIDTH (4 settings)
 * - Balance: PARALLELOGRAM, PINCUSHION_BALANCE (2 settings)
 * - Other: RESERVED6, BRIGHTNESS, ROTATION (3 settings)
 *
 * @see imacG3IvadInit.h for EEPROM configuration
 */

// =============================================================================
// I2C REGISTER ADDRESSES
// =============================================================================

// IVAD property register (primary configuration register)
// All settings are written to this address
byte IVAD_REGISTER_PROPERTY = 0x46;

// IVAD unknown/unlock register
// Used during initialization for calibration data access
byte IVAD_REGISTER_UNKNOWN = 0x53;

// =============================================================================
// IVAD SETTING OFFSET ENUMERATION
// =============================================================================

/**
 * @brief Register offsets for IVAD settings
 *
 * These offsets map to I2C register addresses when writing to IVAD.
 * The enum values also serve as array indices for configuration arrays.
 *
 * Value Range: 0x00 to 0x12 (19 settings total)
 *
 * Register Mapping:
 * | Offset  | Register | Description              |
 * |---------|----------|--------------------------|
 * | 0x00    | CONTRAST | Video contrast level     |
 * | 0x01-0x03 | RGB_DRIVE | RGB drive levels      |
 * | 0x04-0x06 | RGB_CUTOFF | RGB cutoff levels    |
 * | 0x07    | HORIZONTAL_POS | Horizontal position|
 * | 0x08    | HEIGHT | Vertical size              |
 * | 0x09    | VERTICAL_POS | Vertical position    |
 * | 0x0A    | S_CORRECTION | S-curve correction   |
 * | 0x0B    | KEYSTONE | Keystone correction      |
 * | 0x0C    | PINCUSHION | Pincushion distortion  |
 * | 0x0D    | WIDTH | Horizontal size             |
 * | 0x0E    | PINCUSHION_BALANCE | Balance        |
 * | 0x0F    | PARALLELOGRAM | Parallelogram       |
 * | 0x10    | BRIGHTNESS_DRIVE | Unused?          |
 * | 0x11    | BRIGHTNESS | Brightness level       |
 * | 0x12    | ROTATION | Screen rotation          |
 *
 * @note BRIGHTNESS_DRIVE (0x10) has no defined min/max bounds
 */

enum IVAD_REGISTER_OFFSET : const byte
{
  // IVAD register offset - enum value - min/max range
  // =============================================
  IVAD_SETTING_CONTRAST,           // 0x00    0xB5 - 0xFF
  IVAD_SETTING_RED_DRIVE,          // 0x01    0x00 - 0xFF
  IVAD_SETTING_GREEN_DRIVE,        // 0x02    0x00 - 0xFF
  IVAD_SETTING_BLUE_DRIVE,         // 0x03    0x00 - 0xFF
  IVAD_SETTING_RED_CUTOFF,         // 0x04    0x00 - 0xFF
  IVAD_SETTING_GREEN_CUTOFF,       // 0x05    0x00 - 0xFF
  IVAD_SETTING_BLUE_CUTOFF,        // 0x06    0x00 - 0xFF
  IVAD_SETTING_HORIZONTAL_POS,     // 0x07    0x80 - 0xFF
  IVAD_SETTING_HEIGHT,             // 0x08    0x80 - 0xFF
  IVAD_SETTING_VERTICAL_POS,       // 0x09    0x00 - 0x7F
  IVAD_SETTING_S_CORRECTION,       // 0x0A    0x80 - 0xFF
  IVAD_SETTING_KEYSTONE,           // 0x0B    0x80 - 0xFF
  IVAD_SETTING_PINCUSHION,         // 0x0C    0x80 - 0xFF
  IVAD_SETTING_WIDTH,              // 0x0D    0x00 - 0x7F
  IVAD_SETTING_PINCUSHION_BALANCE, // 0x0E    0x80 - 0xFF
  IVAD_SETTING_PARALLELOGRAM,      // 0x0F    0x80 - 0xFF
  IVAD_SETTING_BRIGHTNESS_DRIVE,   // 0x10    No min/max defined
  IVAD_SETTING_BRIGHTNESS,         // 0x11    0x00 - 0x0A (only 11 valid values)
  IVAD_SETTING_ROTATION,           // 0x12    0x00 - 0x7F
  IVAD_SETTING_END                 // Sentinel - not a real setting
};

/**
 * @brief Default configuration values for all IVAD settings
 *
 * These values are calibrated for a typical iMac G3 CRT display.
 * Loaded on first firmware flash or after settings_reset_default().
 *
 * Array Order (21 bytes total):
 * 0: CONTRAST  = 0xFE
 * 1: RED_DRIVE   = 0x93
 * 2: GREEN_DRIVE = 0x93
 * 3: BLUE_DRIVE  = 0x8F
 * 4: RED_CUTOFF  = 0x80
 * 5: GREEN_CUTOFF = 0xB0
 * 6: BLUE_CUTOFF = 0x78
 * 7: HORIZONTAL_POS = 0xB0
 * 8: HEIGHT     = 0xF0
 * 9: VERTICAL_POS = 0x4D
 * 10: S_CORRECTION = 0x9A
 * 11: KEYSTONE  = 0x9B
 * 12: PINCUSHION = 0xCB
 * 13: WIDTH     = 0x19
 * 14: PINCUSHION_BALANCE = 0x7B
 * 15: PARALLELOGRAM = 0xC6
 * 16: RESERVED6 = 0x7B
 * 17: BRIGHTNESS = 0x0A
 * 18: ROTATION  = 0x42
 *
 * @see VIDEO_CONFIG_MIN for minimum valid values
 * @see VIDEO_CONFIG_MAX for maximum valid values
 * @see ivad_change_setting() for bounds checking
 */
const byte VIDEO_CONFIG_DEFAULT[] =
    {
        0xFE, // CONTRAST
        0x93, // RED_DRIVE
        0x93, // GREEN_DRIVE
        0x8F, // BLUE_DRIVE
        0x80, // RED_CUTOFF
        0xB0, // GREEN_CUTOFF
        0x78, // BLUE_CUTOFF
        0xB0, // HORIZONTAL_POS
        0xF0, // HEIGHT
        0x4D, // VERTICAL_POS
        0x9A, // S_CORRECTION
        0x9B, // KEYSTONE
        0xCB, // PINCUSHION
        0x19, // WIDTH
        0x7B, // PINCUSHION_BALANCE
        0xC6, // PARALLELOGRAM
        0x7B, // RESERVED6
        0x0A, // BRIGHTNESS
        0x42, // ROTATION
};

/**
 * @brief Minimum valid values for IVAD settings
 *
 * Values below these will be clamped by ivad_change_setting().
 * Some values are 0x00 (no constraint), others have meaningful minima.
 *
 * Notable Constraints:
 * - HORIZONTAL_POS, HEIGHT, S_CORRECTION, KEYSTONE, PINCUSHION,
 *   PARALLELOGRAM, PINCUSHION_BALANCE: minimum 0x80 (128)
 * - WIDTH, VERTICAL_POS, ROTATION: maximum 0x7F (127)
 * - CONTRAST: minimum 0xB5 (181)
 *
 * @see VIDEO_CONFIG_DEFAULT for default values
 * @see VIDEO_CONFIG_MAX for upper bounds
 */
const byte VIDEO_CONFIG_MIN[] =
    {
        0xB5, // CONTRAST
        0x00, // RED_DRIVE
        0x00, // GREEN_DRIVE
        0x00, // BLUE_DRIVE
        0x00, // RED_RED_CUTOFF
        0x00, // GREEN_RED_CUTOFF
        0x00, // BLUE_RED_CUTOFF
        0x80, // HORIZONTAL_POS
        0x80, // HEIGHT
        0x00, // VERTICAL_POS
        0x80, // S_CORRECTION
        0x80, // KEYSTONE
        0x80, // PINCUSHION
        0x00, // WIDTH
        0x80, // PINCUSHION_BALANCE
        0x80, // PARALLELOGRAM
        0x7B, // RESERVED6
        0x00, // BRIGHTNESS
        0x00  // ROTATION
};

/**
 * @brief Maximum valid values for IVAD settings
 *
 * Values above these will be clamped by ivad_change_setting().
 * Most are 0xFF (255), but some have lower limits:
 *
 * Maximum Constraints:
 * - VERTICAL_POS, WIDTH, ROTATION: maximum 0x7F (127)
 * - BRIGHTNESS: maximum 0x0A (10)
 * - RESERVED6: fixed at 0x7B (123) - cannot be changed
 *
 * @see VIDEO_CONFIG_DEFAULT for default values
 * @see VIDEO_CONFIG_MIN for lower bounds
 */
const byte VIDEO_CONFIG_MAX[] =
    {
        0xFF, // CONTRAST
        0xFF, // RED_DRIVE
        0xFF, // GREEN_DRIVE
        0xFF, // BLUE_DRIVE
        0xFF, // RED_RED_CUTOFF
        0xFF, // GREEN_RED_CUTOFF
        0xFF, // BLUE_RED_CUTOFF
        0xFF, // HORIZONTAL_POS
        0xFF, // HEIGHT
        0x7F, // VERTICAL_POS
        0xFF, // S_CORRECTION
        0xFF, // KEYSTONE
        0xFF, // PINCUSHION
        0x7F, // WIDTH
        0xFF, // PINCUSHION_BALANCE
        0xFF, // PARALLELOGRAM
        0x7B, // RESERVED6
        0x0A, // BRIGHTNESS
        0x7F  // ROTATION
};
