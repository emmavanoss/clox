#include <stdio.h>
#include <stdlib.h>

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

Parser parser;
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

static void endCompiler() {
  emitReturn();
#ifdef DEBUG_PRINT_CODE
  if (!parser.hadError) {
    disassembleChunk(currentChunk(), "code");
  }
#endif
}

static void expression();
static uint8_t identifierConstant(Token* name);
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
  uint8_t arg = identifierConstant(&name);

  if(canAssign && match(TOKEN_EQUAL)) {
    expression();
    emitBytes(OP_SET_GLOBAL, arg);
  } else {
    emitBytes(OP_GET_GLOBAL, arg);
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

static uint8_t parseVariable(const char* errorMessage) {
  consume(TOKEN_IDENTIFIER, errorMessage);
  return identifierConstant(&parser.previous);
}

static ParseRule* getRule(TokenType operatorType) {
  return &rules[operatorType];
}

static void expression() {
  parsePrecedence(PREC_ASSIGNMENT);
}

static void defineVariable(uint8_t index) {
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
  } else {
    expressionStatement();
  }
}

bool compile(const char* source, Chunk* chunk) {
  initScanner(source);
  parser.hadError = false;
  parser.panicMode = false;
  compilingChunk = chunk;
  advance();

  while (!match(TOKEN_EOF)) {
    declaration();
  }

  endCompiler();
  return !parser.hadError;
}
