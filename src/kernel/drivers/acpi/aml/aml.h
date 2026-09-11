#ifndef ACPI_AML_H
#define ACPI_AML_H

#include "ktypes.h"


typedef enum {
    AML_OK = 0,
    AML_ERR_NOT_LOADED = -1,
    AML_ERR_NOT_FOUND  = -2,
    AML_ERR_BAD_OPCODE = -3,
    AML_ERR_DEEP_NEST  = -4,
    AML_ERR_TYPE       = -5,
    AML_ERR_BOUNDS     = -6,
    AML_ERR_NOMEM      = -7,
    AML_ERR_REGION     = -8,
} aml_status_t;

typedef enum {
    AML_OBJ_UNINIT = 0,
    AML_OBJ_INTEGER,
    AML_OBJ_STRING,
    AML_OBJ_BUFFER,
    AML_OBJ_PACKAGE,
    AML_OBJ_DEVICE,
    AML_OBJ_METHOD,
    AML_OBJ_REGION,
    AML_OBJ_FIELD,
    AML_OBJ_SCOPE,
    AML_OBJ_NAME,
    AML_OBJ_MUTEX,
    AML_OBJ_EVENT,
    AML_OBJ_POWER_RES,
    AML_OBJ_PROCESSOR,
    AML_OBJ_THERMAL,
    AML_OBJ_ALIAS,
} aml_obj_type_t;

typedef struct aml_object aml_object_t;

aml_status_t aml_init(void);
aml_status_t aml_call_pic(uint32_t mode);
aml_status_t aml_eval(const char* path, aml_object_t** args, int argc,
                       aml_object_t** ret);
aml_status_t aml_find(const char* path, aml_object_t** out);
aml_status_t aml_eval_crs(const char* path, void* buf, size_t buf_len,
                          size_t* used);

aml_status_t aml_read_integer(const char* path, uint64_t* out);

aml_status_t aml_call_int(const char* path, uint64_t* args, int argc,
                           uint64_t* ret_value);

#endif