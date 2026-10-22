#include "bsp_obd_dsp/exio/TCA9554PWR.h"
#include "esp_log.h"

static const char *TAG = "TCA9554PWR";

/* TCA9554 sits on the main I2C bus (NUM_0, new API) */
static i2c_master_dev_handle_t get_dev(void)
{
    static i2c_master_dev_handle_t s_dev;
    static bool s_initialized = false;
    if (!s_initialized) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_7,
            .device_address = TCA9554_ADDRESS,
            .scl_speed_hz = I2C_MASTER_FREQ_HZ,
        };
        i2c_master_bus_add_device(I2C_GetBusHandle(), &dev_cfg, &s_dev);
        s_initialized = true;
    }
    return s_dev;
}

/*****************************************************  Operation register REG   ****************************************************/
static uint8_t Read_REG(uint8_t REG)                                // Read the value of the TCA9554PWR register REG
{
    uint8_t bitsStatus = 0;
    i2c_master_transmit_receive(get_dev(), &REG, 1, &bitsStatus, 1,
                                (int)I2C_MASTER_TIMEOUT_MS);
    return bitsStatus;
}
static void Write_REG(uint8_t REG,uint8_t Data)                    // Write Data to the REG register of the TCA9554PWR
{
    uint8_t buf[2] = { REG, Data };
    i2c_master_transmit(get_dev(), buf, 2, (int)I2C_MASTER_TIMEOUT_MS);
}

/********************************************************** Set EXIO mode **********************************************************/
static void Mode_EXIOS(uint8_t PinState)                        // Set the mode of the 7 pins from the TCA9554PWR with PinState
{
    Write_REG(TCA9554_CONFIG_REG,PinState);
}

/********************************************************** Read EXIO status **********************************************************/

/********************************************************** Set the EXIO output status **********************************************************/
void Set_EXIO(uint8_t Pin,bool State)                  // Sets the level state of the Pin without affecting the other pins(PIN：1~8)
{
    uint8_t Data = 0;
    uint8_t bitsStatus = Read_REG(TCA9554_OUTPUT_REG);
    if(Pin < 9 && Pin > 0){
        if(State)
            Data = (0x01 << (Pin-1)) | bitsStatus;
        else
            Data = (~(0x01 << (Pin-1)) & bitsStatus);
        Write_REG(TCA9554_OUTPUT_REG,Data);
    }
    else
        ESP_LOGW(TAG, "Parameter error, please enter the correct parameter!");

}
/********************************************************* TCA9554PWR Initializes the device ***********************************************************/
esp_err_t EXIO_Init(void)
{
    Mode_EXIOS(0x00);
    return ESP_OK;
}
