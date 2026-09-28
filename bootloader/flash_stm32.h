#ifndef FLASH_STM32_H
#define FLASH_STM32_H

#include "flash_if.h"

// Fills in a flash_if_t backed by the real controller
void flash_stm32_bind(flash_if_t *out);

#endif // FLASH_STM32_H