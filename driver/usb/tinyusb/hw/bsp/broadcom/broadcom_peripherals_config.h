#pragma once

#include <stddef.h>

// struct _mp_obj_base_t {
//     const mp_obj_type_t *type MICROPY_OBJ_BASE_ALIGNMENT;
// };
// typedef struct _mp_obj_base_t mp_obj_base_t;

// extern const mp_obj_type_t mcu_pin_type;


// #define PIN_PREFIX_VALUES { &mcu_pin_type },
// #define PIN_PREFIX_FIELDS mp_obj_base_t base;

#define PIN_PREFIX_FIELDS 
#define PIN_PREFIX_VALUES