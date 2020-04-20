#include <stdio.h>

#include "common.h"
#include "debug.h"
#include "vm.h"

VM vm;

static void resetStack() {
  vm.stackTop = vm.stack; // set stackTop to point to start of array
}

static InterpretResult run() {
#define READ_BYTE() (*vm.ip++)
#define READ_CONSTANT() (vm.chunk->constants.values[READ_BYTE()])
  for (;;) {
#ifdef DEBUG_TRACE_EXECUTION
    disassembleInstruction(vm.chunk, (int)(vm.ip - vm.chunk->code));
#endif
    uint8_t instruction;
    switch (instruction = READ_BYTE()) {
      case OP_CONSTANT: {
        Value constant = READ_CONSTANT();
        printValue(constant);
        printf("\n");
        break;
      }
      case OP_RETURN: { return INTERPRET_OK; }
    }
  }
#undef READ_BYTE
#undef READ_CONSTANT
}

void initVM() {
  resetStack();
}

void freeVM() {
}

InterpretResult interpret(Chunk* chunk) {
  vm.chunk = chunk;
  vm.ip = vm.chunk->code;
  return run();
}

void push(Value value) {
  *vm.stackTop = value; // store value at top of stack array
  vm.stackTop++; // move pointer forward to next unused slot
}

Value pop() {
  vm.stackTop--; // move pointer back to most recent used slot
  /* no need to clear slot - it's now marked as unused */

  return *vm.stackTop; // return value
}
