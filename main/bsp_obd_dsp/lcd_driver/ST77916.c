/* The selected board profile (hw/hw_board.h) supplies hw_lcd_init_seq[]; define
 * this before the header so only this translation unit compiles the table. */
#define HW_LCD_INIT_SEQ_IMPL
#include "bsp_obd_dsp/lcd_driver/ST77916.h"


#define LCD_OPCODE_READ_CMD         (0x0BULL)

static const char *TAG_LCD = "ST77916";

esp_lcd_panel_handle_t panel_handle = NULL;

/* LVGL flush-complete callback (optional; set via LCD_SetFlushCallback before LCD_Init) */
static bool (*s_flush_cb)(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *) = NULL;
static void *s_flush_ctx = NULL;

void LCD_SetFlushCallback(bool (*cb)(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *), void *ctx)
{
    s_flush_cb = cb;
    s_flush_ctx = ctx;
}


/* The ST77916 panel init table now lives in the per-board hardware header
 * (hw/hw_v1_waveshare.h, hw/hw_v2_new.h, hw/hw_v3_new.h) and is selected for
 * this build by hw_board.h; this file only consumes hw_lcd_init_seq[]. */
void ST77916_Reset(){
#if HW_HAS_TCA9554
  Set_EXIO(TCA9554_EXIO2,false);
  vTaskDelay(pdMS_TO_TICKS(10));
  Set_EXIO(TCA9554_EXIO2,true);
  vTaskDelay(pdMS_TO_TICKS(50));
#else
  /* New boards: LCD reset is a direct GPIO (EXAMPLE_LCD_PIN_NUM_RST=47),
     already handled by esp_lcd_panel_reset() inside QSPI_Init(). */
#endif
}
void LCD_Init() {        
  /* Keep the panel physically dark before its display-on command is sent. */
  Backlight_Init();
  ST77916_Init();
  Touch_Init();
}

int QSPI_Init(void){
  static const spi_bus_config_t host_config = {            
    .data0_io_num = ESP_PANEL_LCD_SPI_IO_DATA0,                    
    .data1_io_num = ESP_PANEL_LCD_SPI_IO_DATA1,                   
    .sclk_io_num = ESP_PANEL_LCD_SPI_IO_SCK,                   
    .data2_io_num = ESP_PANEL_LCD_SPI_IO_DATA2,                    
    .data3_io_num = ESP_PANEL_LCD_SPI_IO_DATA3,                    
    .data4_io_num = -1,                       
    .data5_io_num = -1,                      
    .data6_io_num = -1,                       
    .data7_io_num = -1,                      
    .max_transfer_sz = ESP_PANEL_HOST_SPI_MAX_TRANSFER_SIZE, 
    .flags = SPICOMMON_BUSFLAG_MASTER,       
    .intr_flags = 0,                            
  };
  if(spi_bus_initialize(ESP_PANEL_HOST_SPI_ID_DEFAULT, &host_config, SPI_DMA_CH_AUTO) != ESP_OK){
    ESP_LOGE(TAG_LCD, "The SPI initialization failed.");
    return 0;
  }
  ESP_LOGD(TAG_LCD, "The SPI initialization succeeded.");
  
  esp_lcd_panel_io_spi_config_t io_config ={
    .cs_gpio_num = ESP_PANEL_LCD_SPI_IO_CS,               
    .dc_gpio_num = -1,                  
    .spi_mode = ESP_PANEL_LCD_SPI_MODE,                     
    .pclk_hz = 3 * 1000 * 1000,
    .trans_queue_depth = ESP_PANEL_LCD_SPI_TRANS_QUEUE_SZ,            
    .on_color_trans_done = s_flush_cb,                            
    .user_ctx = s_flush_ctx,                       
    .lcd_cmd_bits = ESP_PANEL_LCD_SPI_CMD_BITS,                 
    .lcd_param_bits = ESP_PANEL_LCD_SPI_PARAM_BITS,                
    .flags = {                          
      .dc_low_on_data = 0,            
      .octal_mode = 0,                
      .quad_mode = 1,                 
      .sio_mode = 0,                  
      .lsb_first = 0,                 
      .cs_high_active = 0,            
    },                                  
  };
  esp_lcd_panel_io_handle_t io_handle = NULL;
  if(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)ESP_PANEL_HOST_SPI_ID_DEFAULT, &io_config, &io_handle) != ESP_OK){
    ESP_LOGE(TAG_LCD, "Failed to set LCD communication parameters -- SPI");
    return 0;
  }
  ESP_LOGD(TAG_LCD, "LCD communication parameters are set successfully -- SPI");

  ESP_LOGD(TAG_LCD, "Install LCD driver of st77916");
  st77916_vendor_config_t vendor_config={
    .flags = {
      .use_qspi_interface = 1,
    },
  };
  esp_err_t ret;
  int lcd_cmd = 0x04;
  uint8_t register_data[4] = {0};   // keep all-zero on read failure, so uninitialized stack data isn't compared against the vendor signature
  size_t param_size = sizeof(register_data);
  lcd_cmd &= 0xff;
  lcd_cmd <<= 8;
  lcd_cmd |= LCD_OPCODE_READ_CMD << 24;  // Use the read opcode instead of write
  ret = esp_lcd_panel_io_rx_param(io_handle, lcd_cmd, register_data, param_size); 
  if (ret == ESP_OK) {
    ESP_LOGI(TAG_LCD, "Register 0x04 data: %02x %02x %02x %02x", register_data[0], register_data[1], register_data[2], register_data[3]);
  } else {
    ESP_LOGW(TAG_LCD, "Failed to read register 0x04, error code: %d", ret);
  } 
  /* The 3 MHz probe device must be removed before registering the display
   * device at its normal clock. Otherwise its handle and bus-lock slot leak. */
  ret = esp_lcd_panel_io_del(io_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG_LCD, "Failed to release LCD probe IO: %s", esp_err_to_name(ret));
    return 0;
  }
  io_handle = NULL;
  io_config.pclk_hz = ESP_PANEL_LCD_SPI_CLK_HZ;
  if(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)ESP_PANEL_HOST_SPI_ID_DEFAULT, &io_config, &io_handle) != ESP_OK){
    ESP_LOGE(TAG_LCD, "Failed to set LCD communication parameters -- SPI");
    return 0;
  }
  ESP_LOGD(TAG_LCD, "LCD communication parameters are set successfully -- SPI");
  
  // The boot-time board profile (hw_board.h) supplies hw_lcd_init_seq[]; apply
  // it UNCONDITIONALLY — do not rely on the register read above, which may fail
  // or return an unknown panel ID (falling back to the driver's default table
  // previously made V3's panel not display at all).
  vendor_config.init_cmds = hw_lcd_init_seq;
  vendor_config.init_cmds_size = sizeof(hw_lcd_init_seq) / sizeof(st77916_lcd_init_cmd_t);
  ESP_LOGI(TAG_LCD, "Hardware %s: applying board init sequence.", HW_BOARD_NAME);

  esp_lcd_panel_dev_config_t panel_config={
    .reset_gpio_num = EXAMPLE_LCD_PIN_NUM_RST,                                
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,                   
    // .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,                       
    .bits_per_pixel = EXAMPLE_LCD_COLOR_BITS,                                 
    .flags = {                                                    
      .reset_active_high = 0,                                   
    },                                                            
    .vendor_config = (void *) &vendor_config,                                  
  };
  esp_lcd_new_panel_st77916(io_handle, &panel_config, &panel_handle);

  esp_lcd_panel_reset(panel_handle);
  esp_lcd_panel_init(panel_handle);
  // esp_lcd_panel_invert_color(panel_handle,false);

  esp_lcd_panel_disp_on_off(panel_handle, true);
  // LCD init complete
  return 1;
}

void ST77916_Init() {
  ST77916_Reset();
  if(!QSPI_Init()){
    ESP_LOGE(TAG_LCD, "ST77916 failed to be initialized");
  }
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Backlight program

uint8_t LCD_Backlight = 70;
static ledc_channel_config_t ledc_channel;
void LCD_BacklightOffEarly(void)
{
    /* Preload the inactive output latch before enabling the GPIO driver.
     * No PWM, panel, storage or I2C initialization is required here. */
    ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_LCD_PIN_NUM_BK_LIGHT, EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL));
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << EXAMPLE_LCD_PIN_NUM_BK_LIGHT
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
}

void Backlight_Init(void)
{
    ESP_LOGD(TAG_LCD, "Turn off LCD backlight");
    LCD_BacklightOffEarly();

    ledc_timer_config_t ledc_timer = {
        .duty_resolution = LEDC_TIMER_13_BIT,
        .freq_hz = 5000,
        .speed_mode = LEDC_LS_MODE,
        .timer_num = LEDC_HS_TIMER,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel.channel    = LEDC_HS_CH0_CHANNEL;
    ledc_channel.duty       = 0;
    ledc_channel.gpio_num   = EXAMPLE_LCD_PIN_NUM_BK_LIGHT;
    ledc_channel.speed_mode = LEDC_LS_MODE;
    ledc_channel.timer_sel  = LEDC_HS_TIMER;
    ledc_channel_config(&ledc_channel);
    ledc_fade_func_install(0);
    
    Set_Backlight(LCD_Backlight);      //0~100    
}
void Set_Backlight(uint8_t Light)
{   
    if(Light > Backlight_MAX) Light = Backlight_MAX;
    uint16_t Duty = LEDC_MAX_Duty-(81*(Backlight_MAX-Light));
    if(Light == 0) 
        Duty = 0;
    ledc_set_duty(ledc_channel.speed_mode, ledc_channel.channel, Duty);
    ledc_update_duty(ledc_channel.speed_mode, ledc_channel.channel);
}
// end Backlight program
