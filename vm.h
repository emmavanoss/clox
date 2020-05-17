#ifndef clox_vm_h
#define clox_vm_h

#include "chunk.h"
#include "object.h"
#include "table.h"
#include "value.h"

#define FRAMES_MAX 64 // maximum call depth we can handle
#define STACK_MAX (FRAMES_MAX * UINT8_COUNT)

typedef struct {
  ObjFunction* function;
  uint8_t* ip;
  Value* slots;
} Callframe;

typedef struct {
  Callframe frames[FRAMES_MAX]; // stack of Callframe structs
  int frameCount; // current height of stack i.e. number of ongoing fun calls

  Value stack[STACK_MAX]; // array of Values, size STACK_MAX
  Value* stackTop; // pointer to top of stack
  Table globals;
  Table strings;

  Obj* objects;
} VM;

typedef enum {
  INTERPRET_OK,
  INTERPRET_COMPILE_ERROR,
  INTERPRET_RUNTIME_ERROR,
} InterpretResult;

extern VM vm;

void initVM();
void freeVM();
InterpretResult interpret(const char* source);
void push(Value value);
Value pop();

#endif
