#include <assert.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include "freertos/semphr.h"
#include "bsp_obd_dsp/i2c_driver/I2C_Driver.h"

static int bus;
static unsigned registrations;
static bool fail_registration;
static atomic_uint reads;
static uint8_t sent[8];
static size_t sent_len;

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *lock)
{ assert(pthread_mutex_init(lock, NULL) == 0); return lock; }
int xSemaphoreTake(SemaphoreHandle_t lock, uint32_t timeout)
{ (void)timeout; return pthread_mutex_lock(lock) == 0; }
int xSemaphoreGive(SemaphoreHandle_t lock) { return pthread_mutex_unlock(lock) == 0; }
esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *config, i2c_master_bus_handle_t *out)
{ assert(config && out); *out = &bus; return ESP_OK; }
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t handle,
                                   const i2c_device_config_t *config,
                                   i2c_master_dev_handle_t *out)
{
    assert(handle == &bus && config && out);
    registrations++;
    if (fail_registration) { fail_registration = false; return ESP_FAIL; }
    *out = (void *)(uintptr_t)(config->device_address + 1u);
    return ESP_OK;
}
esp_err_t i2c_master_multi_buffer_transmit(i2c_master_dev_handle_t dev,
                                         i2c_master_transmit_multi_buffer_info_t *parts,
                                         size_t count, int timeout)
{
    assert(dev && count == 2 && timeout == I2C_MASTER_TIMEOUT_MS);
    sent_len = 0;
    for (size_t i = 0; i < count; i++) {
        assert(sent_len + parts[i].buffer_size <= sizeof(sent));
        if (parts[i].buffer_size) memcpy(sent + sent_len, parts[i].write_buffer, parts[i].buffer_size);
        sent_len += parts[i].buffer_size;
    }
    return ESP_OK;
}
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *reg,
                                     size_t reg_len, uint8_t *data, size_t len, int timeout)
{
    assert(dev && reg && reg_len == 1 && data && len == 1 && timeout == I2C_MASTER_TIMEOUT_MS);
    data[0] = *reg;
    atomic_fetch_add(&reads, 1);
    return ESP_OK;
}
static void *reader(void *arg)
{
    (void)arg;
    for (unsigned i = 0; i < 100; i++) {
        uint8_t byte;
        assert(I2C_Read(0x48, 0x23, &byte, 1) == ESP_OK && byte == 0x23);
    }
    return NULL;
}
int main(void)
{
    uint8_t byte = 0;
    assert(I2C_Read(0x48, 0, &byte, 1) == ESP_ERR_INVALID_STATE);
    I2C_Init();
    fail_registration = true;
    assert(I2C_Read(0x48, 0, &byte, 1) == ESP_FAIL && registrations == 1);
    pthread_t a, b;
    assert(!pthread_create(&a, NULL, reader, NULL));
    assert(!pthread_create(&b, NULL, reader, NULL));
    assert(!pthread_join(a, NULL) && !pthread_join(b, NULL));
    assert(registrations == 2 && atomic_load(&reads) == 200);
    const uint8_t data[] = {0xab, 0xcd};
    assert(I2C_Write(0x48, 0x01, data, 2) == ESP_OK);
    assert(sent_len == 3 && sent[0] == 1 && sent[1] == 0xab && sent[2] == 0xcd);
    assert(I2C_Write(0x48, 0x02, NULL, 0) == ESP_OK && sent_len == 1 && sent[0] == 2);
    assert(I2C_Write(0x48, 0x02, NULL, 1) == ESP_ERR_INVALID_ARG);
    assert(I2C_Read(0x48, 0, NULL, 1) == ESP_ERR_INVALID_ARG);
    assert(I2C_Read(0x80, 0, &byte, 1) == ESP_ERR_INVALID_ARG);
    for (uint8_t addr = 0x20; addr < 0x27; addr++) assert(I2C_Read(addr, 0, &byte, 1) == ESP_OK);
    assert(registrations == 9);
    assert(I2C_Read(0x27, 0, &byte, 1) == ESP_ERR_NO_MEM && registrations == 9);
    assert(I2C_Read(0x48, 0, &byte, 1) == ESP_OK);
    return 0;
}
