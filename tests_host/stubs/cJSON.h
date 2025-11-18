/**
 * @file cJSON.h
 * @brief Minimal cJSON stub for host tests
 *
 * Provides minimal cJSON API stubs to allow config metadata code to compile.
 * This is not a functional implementation - just enough to satisfy linker.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Opaque type
typedef struct cJSON {
  int dummy;
} cJSON;

// Object creation
cJSON* cJSON_CreateObject(void);
cJSON* cJSON_CreateArray(void);
cJSON* cJSON_CreateString(const char* string);
cJSON* cJSON_CreateNumber(double num);
cJSON* cJSON_CreateBool(int boolean);

// Add to object
cJSON* cJSON_AddStringToObject(cJSON* object, const char* name, const char* string);
cJSON* cJSON_AddNumberToObject(cJSON* object, const char* name, double number);
cJSON* cJSON_AddBoolToObject(cJSON* object, const char* name, int boolean);
cJSON* cJSON_AddTrueToObject(cJSON* object, const char* name);
cJSON* cJSON_AddFalseToObject(cJSON* object, const char* name);
cJSON* cJSON_AddArrayToObject(cJSON* object, const char* name);
cJSON* cJSON_AddItemToArray(cJSON* array, cJSON* item);
cJSON* cJSON_AddItemToObject(cJSON* object, const char* name, cJSON* item);

// Deletion
void cJSON_Delete(cJSON* item);
void cJSON_free(void* ptr);

// Serialization (not used in tests, but might be referenced)
char* cJSON_Print(const cJSON* item);
char* cJSON_PrintUnformatted(const cJSON* item);

// Parsing (not used in tests, but might be referenced)
cJSON* cJSON_Parse(const char* value);

#ifdef __cplusplus
}
#endif
