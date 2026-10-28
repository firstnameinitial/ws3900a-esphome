#pragma once
#include "FreeRTOS.h"
inline QueueHandle_t xQueueCreate(unsigned,unsigned) {return nullptr;}
inline int xQueueReceive(QueueHandle_t,void *,unsigned) {return 0;}
inline int xQueueSend(QueueHandle_t,const void *,unsigned) {return 0;}
