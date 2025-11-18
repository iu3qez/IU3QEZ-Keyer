/**
 * @file cJSON.cpp
 * @brief Minimal cJSON stub implementation for host tests
 *
 * Provides minimal cJSON API stubs to allow config metadata code to compile.
 * This is not a functional implementation - just enough to satisfy linker.
 */

#include "cJSON.h"
#include <cstdlib>
#include <cstring>

// Dummy implementations - just enough to link
cJSON* cJSON_CreateObject(void) {
  return static_cast<cJSON*>(calloc(1, sizeof(cJSON)));
}

cJSON* cJSON_CreateArray(void) {
  return static_cast<cJSON*>(calloc(1, sizeof(cJSON)));
}

cJSON* cJSON_CreateString(const char* string) {
  (void)string;
  return static_cast<cJSON*>(calloc(1, sizeof(cJSON)));
}

cJSON* cJSON_CreateNumber(double num) {
  (void)num;
  return static_cast<cJSON*>(calloc(1, sizeof(cJSON)));
}

cJSON* cJSON_CreateBool(int boolean) {
  (void)boolean;
  return static_cast<cJSON*>(calloc(1, sizeof(cJSON)));
}

cJSON* cJSON_AddStringToObject(cJSON* object, const char* name, const char* string) {
  (void)object;
  (void)name;
  (void)string;
  return static_cast<cJSON*>(calloc(1, sizeof(cJSON)));
}

cJSON* cJSON_AddNumberToObject(cJSON* object, const char* name, double number) {
  (void)object;
  (void)name;
  (void)number;
  return static_cast<cJSON*>(calloc(1, sizeof(cJSON)));
}

cJSON* cJSON_AddBoolToObject(cJSON* object, const char* name, int boolean) {
  (void)object;
  (void)name;
  (void)boolean;
  return static_cast<cJSON*>(calloc(1, sizeof(cJSON)));
}

cJSON* cJSON_AddTrueToObject(cJSON* object, const char* name) {
  (void)object;
  (void)name;
  return static_cast<cJSON*>(calloc(1, sizeof(cJSON)));
}

cJSON* cJSON_AddFalseToObject(cJSON* object, const char* name) {
  (void)object;
  (void)name;
  return static_cast<cJSON*>(calloc(1, sizeof(cJSON)));
}

cJSON* cJSON_AddArrayToObject(cJSON* object, const char* name) {
  (void)object;
  (void)name;
  return static_cast<cJSON*>(calloc(1, sizeof(cJSON)));
}

cJSON* cJSON_AddItemToArray(cJSON* array, cJSON* item) {
  (void)array;
  (void)item;
  return nullptr;
}

cJSON* cJSON_AddItemToObject(cJSON* object, const char* name, cJSON* item) {
  (void)object;
  (void)name;
  (void)item;
  return nullptr;
}

void cJSON_Delete(cJSON* item) {
  free(item);
}

void cJSON_free(void* ptr) {
  free(ptr);
}

char* cJSON_Print(const cJSON* item) {
  (void)item;
  return strdup("{}");
}

char* cJSON_PrintUnformatted(const cJSON* item) {
  (void)item;
  return strdup("{}");
}

cJSON* cJSON_Parse(const char* value) {
  (void)value;
  return static_cast<cJSON*>(calloc(1, sizeof(cJSON)));
}
