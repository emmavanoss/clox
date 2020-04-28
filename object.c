#include <stdio.h>
#include <string.h>

#include "memory.h"
#include "object.h"
#include "table.h"
#include "value.h"
#include "vm.h"

#define ALLOCATEOBJ(type, objectType) \
  (type*)allocateObject(sizeof(type), objectType)

static Obj* allocateObject(size_t size, ObjType type) {
  Obj* object = (Obj*)reallocate(NULL, 0, size);
  object->type = type;

  object->next = vm.objects; // update pointer to objects list
  vm.objects = object; // push new object into head of objects list
  return object;
}

ObjString* allocateString(char* chars, int length, uint32_t hash) {
  ObjString* string = ALLOCATEOBJ(ObjString, OBJ_STRING);
  string->length = length;
  string->chars = chars;
  string->hash = hash;

  tableSet(&vm.strings, string, NIL_VAL);

  return string;
}

static uint32_t hashString(const char* key, int length) {
  // FNV-1a hash function
  uint32_t hash = 2166136261u; // specifically chosen initial hash value

  for (int i = 0; i < length; i++) {
    hash ^= key[i];   // binary XOR to munge bits into hash value
    hash *= 16777619; // multiply to mix the resulting bits around some
  }

  return hash;
}

ObjString* takeString(char* chars, int length) {
  uint32_t hash = hashString(chars, length);
  ObjString* interned = tableFindString(&vm.strings, chars, length, hash);
  if (interned != NULL) {
    FREE_ARRAY(char, chars, length + 1);
    return interned;
  }

  return allocateString(chars, length, hash);
}

ObjString* copyString(const char* chars, int length) {
  uint32_t hash = hashString(chars, length);
  ObjString* interned = tableFindString(&vm.strings, chars, length, hash);
  if (interned != NULL) return interned;

  char* heapChars = ALLOCATE(char, length + 1);
  memcpy(heapChars, chars, length);
  heapChars[length] = '\0';

  return allocateString(heapChars, length, hash);
}

void printObject(Value value) {
  switch (OBJ_TYPE(value)) {
    case OBJ_STRING:
      printf("%s", AS_CSTRING(value)); break;
  }
}
