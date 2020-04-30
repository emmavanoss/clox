#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "compiler.h"
#include "object.h"
#include "scanner.h"

#ifdef DEBUG_PRINT_CODE
#include "debug.h"
#endif

typedef struct {
  Token previous;
  Token current;
  bool hadError;
  bool panicMode;
} Parser;

typedef enum {
  PREC_NONE,
  PREC_ASSIGNMENT, // =
  PREC_OR, // or
  PREC_AND, // and
  PREC_EQUALITY, // == !=
  PREC_COMPARISON, // < > <= >=
  PREC_TERM, // + -
  PREC_FACTOR, // * /
  PREC_UNARY, // - !
  PREC_CALL, // () .
  PREC_PRIMARY,
} Precedence;

typedef void (*ParseFn)(bool canAssign);

typedef struct {
  ParseFn prefix;
  ParseFn infix;
  Precedence precedence;
} ParseRule;

typedef struct {
  Token name;
  int depth; // scope depth of the block where the local var was declared
} Local;

typedef struct Compiler {
  Local locals[UINT8_COUNT]; // limited as index instruction is only 1 byte
  int localCount;
  int scopeDepth; // no of blocks surrounding the code curently being compiled
} Compiler;

Parser parser;
Compiler* current = NULL;
Chunk* compilingChunk;

static Chunk* currentChunk() {
  return compilingChunk;
}

static void errorAt(Token* token, const char* message) {
  if (parser.panicMode) return;
  parser.panicMode = true;

  fprintf(stderr, "[line %d] Error", token->line);

  if (token->type == TOKEN_EOF) {
    fprintf(stderr, " at end");
  } else if (token->type == TOKEN_ERROR) {
    // Nothing
  } else {
    fprintf(stderr, " at '%.*s'", token->length, token->start);
  }

  fprintf(stderr, ": %s\n", message);
  parser.hadError = true;
}

static void error(const char* message) {
  errorAt(&parser.previous, message);
}

static void errorAtCurrent(const char* message) {
  errorAt(&parser.current, message);
}

static void advance() {
  parser.previous = parser.current;

  for (;;) {
    parser.current = scanToken(); // set current to new token fr source stream
    if (parser.current.type != TOKEN_ERROR) break;

    errorAtCurrent(parser.current.start);
  }
}

static void consume(TokenType type, const char* message) {
  if (parser.current.type == type) {
    advance();
    return;
  }

  errorAtCurrent(message);
}

static bool check(TokenType type) {
  return parser.current.type == type;
}

static bool match(TokenType type) {
  if (!check(type)) return false;
  advance();
  return true;
}

static void emitByte(uint8_t byte) {
  writeChunk(currentChunk(), byte, parser.previous.line);
}

static void emitBytes(uint8_t byte1, uint8_t byte2) {
  emitByte(byte1);
  emitByte(byte2);
}

static int emitJump(uint8_t instruction) {
  emitByte(instruction);
  emitByte(0xff); // 255 in binary, i.e. one byte
  emitByte(0xff); // And a second byte

  return currentChunk()->count - 2; // return offset of emitted instruction
}

static void emitReturn() {
  emitByte(OP_RETURN);
}

static uint8_t makeConstant(Value value) {
  int constant = addConstant(currentChunk(), value);
  if (constant > UINT8_MAX) {
    error("Too many constants in one chunk.");
    return 0;
  }

  return (uint8_t)constant;
}

static void emitConstant(Value value) {
  emitBytes(OP_CONSTANT, makeConstant(value));
}

static void patchJump(int offset) {
  // how much then-branch code to jump over
  int jump = currentChunk()->count - offset - 2;

  if (jump > UINT16_MAX) {
    error("Too much code to jump over.");
  }

  currentChunk()->code[offset] = (jump >> 8) & 0xff; // >> to get first 8 bytes
  currentChunk()->code[offset + 1] = jump & 0xff;
}

static void initCompiler(Compiler* compiler) {
  compiler->localCount = 0;
  compiler->scopeDepth = 0;
  current = compiler;
}

static void endCompiler() {
  emitReturn();
#ifdef DEBUG_PRINT_CODE
  if (!parser.hadError) {
    disassembleChunk(currentChunk(), "code");
  }
#endif
}

static void beginScope() {
  current->scopeDepth++;
}

static void endScope() {
  current->scopeDepth--;

  // Clean up local variables from scope
  while (current->localCount > 0 &&
      current->locals[current->localCount - 1].depth > current->scopeDepth) {
    emitByte(OP_POP);       // Free up slot on stack
    current->localCount--;  // Discard local var from array
  }
}

static void expression();
static uint8_t identifierConstant(Token* name);
static int resolveLocal(Compiler* compiler, Token* name);
static void statement();
static void declaration();
static ParseRule* getRule(TokenType operatorType);
static void parsePrecedence(Precedence precedence);

static void binary(bool canAssign) {
  // Remember operator
  TokenType operatorType = parser.previous.type;

  // Compile right operand
  ParseRule* rule = getRule(operatorType);
  parsePrecedence((Precedence)(rule->precedence + 1));

  // Emit operator instruction
  switch (operatorType) {
    case TOKEN_BANG_EQUAL:    emitBytes(OP_EQUAL, OP_NOT); break;
    case TOKEN_EQUAL_EQUAL:   emitByte(OP_EQUAL); break;
    case TOKEN_GREATER:       emitByte(OP_GREATER); break;
    case TOKEN_GREATER_EQUAL: emitBytes(OP_LESS, OP_NOT); break;
    case TOKEN_LESS:          emitByte(OP_LESS); break;
    case TOKEN_LESS_EQUAL:    emitBytes(OP_GREATER, OP_NOT); break;
    case TOKEN_PLUS:          emitByte(OP_ADD); break;
    case TOKEN_MINUS:         emitByte(OP_SUBTRACT); break;
    case TOKEN_STAR:          emitByte(OP_MULTIPY); break;
    case TOKEN_SLASH:         emitByte(OP_DIVIDE); break;
    default: return; // unreachable
  }
}

static void literal(bool canAssign) {
  switch (parser.previous.type) {
    case TOKEN_FALSE: emitByte(OP_FALSE); break;
    case TOKEN_NIL: emitByte(OP_NIL); break;
    case TOKEN_TRUE: emitByte(OP_TRUE); break;
    default: return; // unreachable
  }
}

static void grouping(bool canAssign) {
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
}

static void number(bool canAssign) {
  double value = strtod(parser.previous.start, NULL);
  emitConstant(NUMBER_VAL(value));
}

static void string(bool canAssign) {
  emitConstant(OBJ_VAL(copyString(
          parser.previous.start + 1, parser.previous.length - 2)));
}

static void namedVariable(Token name, bool canAssign) {
  uint8_t getOp, setOp;
  int arg = resolveLocal(current, &name); // Look for local var with name
  if (arg != -1) {
    getOp = OP_GET_LOCAL;
    setOp = OP_SET_LOCAL;
  } else {
    arg = identifierConstant(&name);
    getOp = OP_GET_GLOBAL;
    setOp = OP_SET_GLOBAL;
  }

  if(canAssign && match(TOKEN_EQUAL)) {
    expression();
    emitBytes(setOp, (uint8_t)arg);
  } else {
    emitBytes(getOp, (uint8_t)arg);
  }
}

static void variable(bool canAssign) {
  namedVariable(parser.previous, canAssign);
}

static void unary(bool canAssign) {
  TokenType operatorType = parser.previous.type;

  // Compile operand
  parsePrecedence(PREC_UNARY);

  // Emit operator instruction
  switch (operatorType) {
    case TOKEN_BANG: emitByte(OP_NOT); break;
    case TOKEN_MINUS: emitByte(OP_NEGATE); break;
    default: return; // Unreachable
  }
}

ParseRule rules[] = {
  { grouping, NULL,   PREC_NONE },   // TOKEN_LEFT_PAREN
  { NULL,     NULL,   PREC_NONE },   // TOKEN_RIGHT_PAREN
  { NULL,     NULL,   PREC_NONE },   // TOKEN_LEFT_BRACE
  { NULL,     NULL,   PREC_NONE },   // TOKEN_RIGHT_BRACE
  { NULL,     NULL,   PREC_NONE },   // TOKEN_COMMA
  { NULL,     NULL,   PREC_NONE },   // TOKEN_DOT
  { unary,    binary, PREC_TERM },   // TOKEN_MINUS
  { NULL,     binary, PREC_TERM },   // TOKEN_PLUS
  { NULL,     NULL,   PREC_NONE },   // TOKEN_SEMICOLON
  { NULL,     binary, PREC_FACTOR }, // TOKEN_SLASH
  { NULL,     binary, PREC_FACTOR }, // TOKEN_STAR
  { unary,    NULL,   PREC_NONE },   // TOKEN_BANG
  { NULL,     binary, PREC_EQUALITY },   // TOKEN_BANG_EQUAL
  { NULL,     NULL,   PREC_NONE },   // TOKEN_EQUAL
  { NULL,     binary, PREC_EQUALITY },   // TOKEN_EQUAL_EQUAL
  { NULL,     binary, PREC_COMPARISON },   // TOKEN_GREATER
  { NULL,     binary, PREC_COMPARISON },   // TOKEN_GREATER_EQUAL
  { NULL,     binary, PREC_COMPARISON },   // TOKEN_LESS
  { NULL,     binary, PREC_COMPARISON },   // TOKEN_LESS_EQUAL
  { variable, NULL,   PREC_NONE },   // TOKEN_IDENTIFIER
  { string,   NULL,   PREC_NONE },   // TOKEN_STRING
  { number,   NULL,   PREC_NONE },   // TOKEN_NUMBER
  { NULL,     NULL,   PREC_NONE },   // TOKEN_AND
  { NULL,     NULL,   PREC_NONE },   // TOKEN_CLASS
  { NULL,     NULL,   PREC_NONE },   // TOKEN_ELSE
  { literal,  NULL,   PREC_NONE },   // TOKEN_FALSE
  { NULL,     NULL,   PREC_NONE },   // TOKEN_FOR
  { NULL,     NULL,   PREC_NONE },   // TOKEN_FUN
  { NULL,     NULL,   PREC_NONE },   // TOKEN_IF
  { literal,  NULL,   PREC_NONE },   // TOKEN_NIL
  { NULL,     NULL,   PREC_NONE },   // TOKEN_OR
  { NULL,     NULL,   PREC_NONE },   // TOKEN_PRINT
  { NULL,     NULL,   PREC_NONE },   // TOKEN_RETURN
  { NULL,     NULL,   PREC_NONE },   // TOKEN_SUPER
  { NULL,     NULL,   PREC_NONE },   // TOKEN_THIS
  { literal,  NULL,   PREC_NONE },   // TOKEN_TRUE
  { NULL,     NULL,   PREC_NONE },   // TOKEN_VAR
  { NULL,     NULL,   PREC_NONE },   // TOKEN_WHILE
  { NULL,     NULL,   PREC_NONE },   // TOKEN_ERROR
  { NULL,     NULL,   PREC_NONE },   // TOKEN_EOF
};

static void parsePrecedence(Precedence precedence) {
  advance();
  ParseFn prefixRule = getRule(parser.previous.type)->prefix;
  if (prefixRule == NULL) {
    error("Expect expression.");
    return;
  }

  bool canAssign = precedence <= PREC_ASSIGNMENT;
  prefixRule(canAssign);

  while (precedence <= getRule(parser.current.type)->precedence) {
    advance();
    ParseFn infixRule = getRule(parser.previous.type)->infix;
    infixRule(canAssign);
  }

  if (canAssign && match(TOKEN_EQUAL)) {
    error("Invalid assignment target.");
  }
}

static uint8_t identifierConstant(Token* name) {
  return makeConstant(OBJ_VAL(copyString(name->start, name->length)));
}

static bool identifiersEqual(Token* a, Token* b) {
  if (a->length != b->length) return false;
  return memcmp(a->start, b->start, a->length) == 0;
}

static int resolveLocal(Compiler* compiler, Token* name) {
  for (int i = compiler->localCount - 1; i >= 0; i--) {
    Local* local = &compiler->locals[i];
    if (identifiersEqual(name, &local->name)) {
      // check if variable has been initialized
      if (local->depth == -1) {
        error("Cannot read local variable in its own initializer.");
      }
      return i;
    }
  }

  return -1; // No local var found by this name
}

static void addLocal(Token name) {
  if (current->localCount == UINT8_COUNT) {
    error("Too many local variables in function (max 256).");
    return;
  }
  Local* local = &current->locals[current->localCount++];
  local->name = name;
  local->depth = -1; // Set depth to -1 to show not yet initialized
}

static void declareVariable() {
  // Global vars are late bound, so no need to keep track
  // For local vars, need to keep track of names declared
  if (current->scopeDepth == 0) return;

  Token* name = &parser.previous;

  // Check for exisiting vars with the same name in the same scope
  for (int i = current->localCount - 1; i >= 0; i--) {
    Local* local = &current->locals[i];
    if (local->depth != -1 && local->depth < current->scopeDepth) {
      break;
    }

    if (identifiersEqual(name, &local->name)) {
      error("Variable with this name already declared in this scope.");
    }
  }

  // add new local var to locals array
  addLocal(*name);
}

static uint8_t parseVariable(const char* errorMessage) {
  consume(TOKEN_IDENTIFIER, errorMessage);

  declareVariable();
  if (current->scopeDepth > 0) return 0; // if in a local scope, exit

  return identifierConstant(&parser.previous);
}

static void markInitialized() {
  // set variable scope to current scopeDepth
  current->locals[current->localCount - 1].depth = current->scopeDepth;
}

static ParseRule* getRule(TokenType operatorType) {
  return &rules[operatorType];
}

static void expression() {
  parsePrecedence(PREC_ASSIGNMENT);
}

static void block() {
  while (!check(TOKEN_RIGHT_BRACE) && !check(TOKEN_EOF)) {
    declaration();
  }

  consume(TOKEN_RIGHT_BRACE, "Expect '}' after block.");
}

static void defineVariable(uint8_t index) {
  // if currently in global scope, define as global var
  // otherwise, now that initializer has been compiled, mark initialized
  if (current->scopeDepth > 0) {
    markInitialized();
    return; // just leave value on stack
  }

  emitBytes(OP_DEFINE_GLOBAL, index);
}

static void varDeclaration() {
  // Parse variable name
  uint8_t global = parseVariable("Expect variable name.");

  if (match(TOKEN_EQUAL)) {
    // Parse expression assigned to variable
    expression();
  } else {
    // Initialize to nil if no value given
    emitByte(OP_NIL);
  }

  consume(TOKEN_SEMICOLON, "Expect ';' after variable declaration.");

  defineVariable(global);
}

static void expressionStatement() {
  expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after expression.");
  emitByte(OP_POP);
}

static void ifStatement() {
  consume(TOKEN_LEFT_PAREN, "Expect '(' after 'if'.");
  expression();
  consume(TOKEN_RIGHT_PAREN, "Expect ')' after condition.");

  int thenJump = emitJump(OP_JUMP_IF_FALSE); // save loc of JUMP instruction
  emitByte(OP_POP);
  statement();
  int elseJump = emitJump(OP_JUMP); // Jump else branch at end of then branch

  patchJump(thenJump);
  emitByte(OP_POP);

  if (match(TOKEN_ELSE)) statement();
  patchJump(elseJump);
}

static void printStatement() {
  expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after expression.");
  emitByte(OP_PRINT);
}

static void synchronize() {
  parser.panicMode = false;

  // Advance, looking for a statement boundary
  while (parser.current.type != TOKEN_EOF) {
    // previous token indicates end of statement
    if (parser.previous.type == TOKEN_SEMICOLON) return;

    // next token indicates start of new statement
    switch (parser.current.type) {
      case TOKEN_CLASS:
      case TOKEN_FUN:
      case TOKEN_VAR:
      case TOKEN_FOR:
      case TOKEN_IF:
      case TOKEN_WHILE:
      case TOKEN_PRINT:
      case TOKEN_RETURN:
        return;

      default:
        // Do nothing - continue searching for stmt boundary
        ;
    }

    advance();
  }
}

static void declaration() {
  if (match(TOKEN_VAR)) {
    varDeclaration();
  } else {
    statement();
  }

  if (parser.panicMode) synchronize();
}

static void statement() {
  if (match(TOKEN_PRINT)) {
    printStatement();
  } else if (match(TOKEN_IF)) {
    ifStatement();
  } else if (match(TOKEN_LEFT_BRACE)) {
    beginScope();
    block();
    endScope();
  } else {
    expressionStatement();
  }
}

bool compile(const char* source, Chunk* chunk) {
  initScanner(source);
  Compiler compiler;
  initCompiler(&compiler);
  compilingChunk = chunk;

  parser.hadError = false;
  parser.panicMode = false;
  advance();

  while (!match(TOKEN_EOF)) {
    declaration();
  }

  endCompiler();
  return !parser.hadError;
}
