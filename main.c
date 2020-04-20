#include "common.h"
#include "chunk.h"
#include "debug.h"
#include "vm.h"

int main(int argc, const char* argv[]) {
  initVM();

  Chunk chunk;
  initChunk(&chunk);

  int preConstant = addConstant(&chunk, 0.7);
  int constant = addConstant(&chunk, 1.2);
  writeChunk(&chunk, OP_CONSTANT, 122);
  writeChunk(&chunk, preConstant, 122);
  writeChunk(&chunk, OP_CONSTANT, 123);
  writeChunk(&chunk, constant, 123);
  writeChunk(&chunk, OP_NEGATE, 123);

  writeChunk(&chunk, OP_RETURN, 124);
  /* disassembleChunk(&chunk, "test chunk"); */
  interpret(&chunk);
  freeVM();
  freeChunk(&chunk);
  return 0;
}
