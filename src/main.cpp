#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdio>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "llvm/ADT/APFloat.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::orc;

// -----------------------------------------------------------------------------
// Lexer
// -----------------------------------------------------------------------------

enum Token {
  tok_eof = -1,
  tok_def = -2,
  tok_extern = -3,
  tok_identifier = -4,
  tok_number = -5,
  tok_if = -6,
  tok_then = -7,
  tok_else = -8,
  tok_for = -9,
  tok_in = -10,
};

static std::string IdentifierStr;
static double NumVal;

static int gettok() {
  static int LastChar = ' ';

  while (isspace(LastChar))
    LastChar = getchar();

  if (LastChar == '#') {
    do
      LastChar = getchar();
    while (LastChar != EOF && LastChar != '\n' && LastChar != '\r');
    if (LastChar != EOF)
      return gettok();
  }

  if (isalpha(LastChar)) {
    IdentifierStr = static_cast<char>(LastChar);
    while (isalnum((LastChar = getchar())))
      IdentifierStr += static_cast<char>(LastChar);

    if (IdentifierStr == "def") return tok_def;
    if (IdentifierStr == "extern") return tok_extern;
    if (IdentifierStr == "if") return tok_if;
    if (IdentifierStr == "then") return tok_then;
    if (IdentifierStr == "else") return tok_else;
    if (IdentifierStr == "for") return tok_for;
    if (IdentifierStr == "in") return tok_in;
    return tok_identifier;
  }

  if (isdigit(LastChar) || LastChar == '.') {
    std::string NumStr;
    do {
      NumStr += static_cast<char>(LastChar);
      LastChar = getchar();
    } while (isdigit(LastChar) || LastChar == '.');

    NumVal = strtod(NumStr.c_str(), nullptr);
    return tok_number;
  }

  if (LastChar == EOF)
    return tok_eof;

  int ThisChar = LastChar;
  LastChar = getchar();
  return ThisChar;
}

// -----------------------------------------------------------------------------
// AST
// -----------------------------------------------------------------------------

class ExprAST {
public:
  virtual ~ExprAST() = default;
  virtual Value *codegen() = 0;
};

class NumberExprAST final : public ExprAST {
  double Val;
public:
  explicit NumberExprAST(double val) : Val(val) {}
  Value *codegen() override;
};

class VariableExprAST final : public ExprAST {
  std::string Name;
public:
  explicit VariableExprAST(std::string name) : Name(std::move(name)) {}
  Value *codegen() override;
};

class BinaryExprAST final : public ExprAST {
  char Op;
  std::unique_ptr<ExprAST> LHS, RHS;
public:
  BinaryExprAST(char op, std::unique_ptr<ExprAST> lhs,
                std::unique_ptr<ExprAST> rhs)
      : Op(op), LHS(std::move(lhs)), RHS(std::move(rhs)) {}
  Value *codegen() override;
};

class CallExprAST final : public ExprAST {
  std::string Callee;
  std::vector<std::unique_ptr<ExprAST>> Args;
public:
  CallExprAST(std::string callee, std::vector<std::unique_ptr<ExprAST>> args)
      : Callee(std::move(callee)), Args(std::move(args)) {}
  Value *codegen() override;
};

class IfExprAST final : public ExprAST {
  std::unique_ptr<ExprAST> Cond, Then, Else;
public:
  IfExprAST(std::unique_ptr<ExprAST> cond,
            std::unique_ptr<ExprAST> thenExpr,
            std::unique_ptr<ExprAST> elseExpr)
      : Cond(std::move(cond)), Then(std::move(thenExpr)),
        Else(std::move(elseExpr)) {}
  Value *codegen() override;
};

class ForExprAST final : public ExprAST {
  std::string VarName;
  std::unique_ptr<ExprAST> Start, End, Step, Body;
public:
  ForExprAST(std::string varName, std::unique_ptr<ExprAST> start,
             std::unique_ptr<ExprAST> end, std::unique_ptr<ExprAST> step,
             std::unique_ptr<ExprAST> body)
      : VarName(std::move(varName)), Start(std::move(start)),
        End(std::move(end)), Step(std::move(step)), Body(std::move(body)) {}
  Value *codegen() override;
};

class PrototypeAST {
  std::string Name;
  std::vector<std::string> Args;
public:
  PrototypeAST(std::string name, std::vector<std::string> args)
      : Name(std::move(name)), Args(std::move(args)) {}
  const std::string &getName() const { return Name; }
  const std::vector<std::string> &getArgs() const { return Args; }
  Function *codegen();
};

class FunctionAST {
  std::unique_ptr<PrototypeAST> Proto;
  std::unique_ptr<ExprAST> Body;
public:
  FunctionAST(std::unique_ptr<PrototypeAST> proto,
              std::unique_ptr<ExprAST> body)
      : Proto(std::move(proto)), Body(std::move(body)) {}
  Function *codegen();
};

// -----------------------------------------------------------------------------
// Parser
// -----------------------------------------------------------------------------

static int CurTok;
static int getNextToken() { return CurTok = gettok(); }
static std::map<char, int> BinopPrecedence;

static std::unique_ptr<ExprAST> LogError(const char *str) {
  fprintf(stderr, "Error: %s\n", str);
  return nullptr;
}
static std::unique_ptr<PrototypeAST> LogErrorP(const char *str) {
  LogError(str);
  return nullptr;
}
static std::unique_ptr<FunctionAST> LogErrorF(const char *str) {
  LogError(str);
  return nullptr;
}

static int GetTokPrecedence() {
  if (!isascii(CurTok)) return -1;
  int TokPrec = BinopPrecedence[static_cast<char>(CurTok)];
  if (TokPrec <= 0) return -1;
  return TokPrec;
}

static std::unique_ptr<ExprAST> ParseExpression();

static std::unique_ptr<ExprAST> ParseNumberExpr() {
  auto Result = std::make_unique<NumberExprAST>(NumVal);
  getNextToken();
  return Result;
}

static std::unique_ptr<ExprAST> ParseParenExpr() {
  getNextToken();
  auto V = ParseExpression();
  if (!V) return nullptr;
  if (CurTok != ')') return LogError("expected ')'");
  getNextToken();
  return V;
}

static std::unique_ptr<ExprAST> ParseIdentifierExpr() {
  std::string IdName = IdentifierStr;
  getNextToken();

  if (CurTok != '(')
    return std::make_unique<VariableExprAST>(IdName);

  getNextToken();
  std::vector<std::unique_ptr<ExprAST>> Args;
  if (CurTok != ')') {
    while (true) {
      if (auto Arg = ParseExpression())
        Args.push_back(std::move(Arg));
      else
        return nullptr;

      if (CurTok == ')') break;
      if (CurTok != ',') return LogError("expected ')' or ',' in argument list");
      getNextToken();
    }
  }
  getNextToken();
  return std::make_unique<CallExprAST>(IdName, std::move(Args));
}

static std::unique_ptr<ExprAST> ParsePrimary() {
  switch (CurTok) {
  default: return LogError("unknown token when expecting an expression");
  case tok_identifier: return ParseIdentifierExpr();
  case tok_number: return ParseNumberExpr();
  case '(': return ParseParenExpr();
  case tok_if: {
    getNextToken();
    auto Cond = ParseExpression();
    if (!Cond) return nullptr;
    if (CurTok != tok_then) return LogError("expected then");
    getNextToken();
    auto Then = ParseExpression();
    if (!Then) return nullptr;
    if (CurTok != tok_else) return LogError("expected else");
    getNextToken();
    auto Else = ParseExpression();
    if (!Else) return nullptr;
    return std::make_unique<IfExprAST>(std::move(Cond), std::move(Then), std::move(Else));
  }
  case tok_for: {
    getNextToken();
    if (CurTok != tok_identifier) return LogError("expected identifier after for");
    std::string IdName = IdentifierStr;
    getNextToken();
    if (CurTok != '=') return LogError("expected '=' after for variable");
    getNextToken();
    auto Start = ParseExpression();
    if (!Start) return nullptr;
    if (CurTok != ',') return LogError("expected ',' after for start value");
    getNextToken();
    auto End = ParseExpression();
    if (!End) return nullptr;

    std::unique_ptr<ExprAST> Step;
    if (CurTok == ',') {
      getNextToken();
      Step = ParseExpression();
      if (!Step) return nullptr;
    }

    if (CurTok != tok_in) return LogError("expected 'in' after for");
    getNextToken();
    auto Body = ParseExpression();
    if (!Body) return nullptr;
    return std::make_unique<ForExprAST>(IdName, std::move(Start), std::move(End),
                                        std::move(Step), std::move(Body));
  }
  }
}

static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec,
                                               std::unique_ptr<ExprAST> LHS) {
  while (true) {
    int TokPrec = GetTokPrecedence();
    if (TokPrec < ExprPrec) return LHS;

    int BinOp = CurTok;
    getNextToken();
    auto RHS = ParsePrimary();
    if (!RHS) return nullptr;

    int NextPrec = GetTokPrecedence();
    if (TokPrec < NextPrec) {
      RHS = ParseBinOpRHS(TokPrec + 1, std::move(RHS));
      if (!RHS) return nullptr;
    }

    LHS = std::make_unique<BinaryExprAST>(static_cast<char>(BinOp),
                                           std::move(LHS), std::move(RHS));
  }
}

static std::unique_ptr<ExprAST> ParseExpression() {
  auto LHS = ParsePrimary();
  if (!LHS) return nullptr;
  return ParseBinOpRHS(0, std::move(LHS));
}

static std::unique_ptr<PrototypeAST> ParsePrototype() {
  if (CurTok != tok_identifier) return LogErrorP("expected function name in prototype");
  std::string FnName = IdentifierStr;
  getNextToken();
  if (CurTok != '(') return LogErrorP("expected '(' in prototype");

  std::vector<std::string> ArgNames;
  while (getNextToken() == tok_identifier) {
    ArgNames.push_back(IdentifierStr);
  }
  if (CurTok != ')') return LogErrorP("expected ')' in prototype");
  getNextToken();
  return std::make_unique<PrototypeAST>(FnName, std::move(ArgNames));
}

static std::unique_ptr<FunctionAST> ParseDefinition() {
  getNextToken();
  auto Proto = ParsePrototype();
  if (!Proto) return nullptr;
  if (auto E = ParseExpression())
    return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
  return nullptr;
}

static std::unique_ptr<PrototypeAST> ParseExtern() {
  getNextToken();
  return ParsePrototype();
}

static std::unique_ptr<FunctionAST> ParseTopLevelExpr() {
  if (auto E = ParseExpression()) {
    auto Proto = std::make_unique<PrototypeAST>("__anon_expr", std::vector<std::string>{});
    return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
  }
  return nullptr;
}

// -----------------------------------------------------------------------------
// LLVM code generation / JIT
// -----------------------------------------------------------------------------

static std::unique_ptr<LLJIT> TheJIT;
static std::unique_ptr<LLVMContext> TheContext;
static std::unique_ptr<Module> TheModule;
static std::unique_ptr<IRBuilder<>> Builder;
static std::map<std::string, AllocaInst *> NamedValues;

static void InitializeModule() {
  TheContext = std::make_unique<LLVMContext>();
  TheModule = std::make_unique<Module>("kaleidoscope", *TheContext);
  TheModule->setDataLayout(TheJIT->getDataLayout());
  Builder = std::make_unique<IRBuilder<>>(*TheContext);
  NamedValues.clear();
}

static Function *getFunction(std::string Name) {
  if (auto *F = TheModule->getFunction(Name)) return F;
  if (Name == "print") Name = "ks_print";
  return TheModule->getFunction(Name);
}

static AllocaInst *CreateEntryBlockAlloca(Function *F, const std::string &VarName) {
  IRBuilder<> TmpB(&F->getEntryBlock(), F->getEntryBlock().begin());
  return TmpB.CreateAlloca(Type::getDoubleTy(*TheContext), nullptr, VarName);
}

extern "C" double ks_print(double X) {
  std::cout << X << "\n";
  return X;
}

static Function *getOrCreatePrintFunction() {
  if (auto *F = TheModule->getFunction("ks_print")) return F;
  auto *Ty = FunctionType::get(Type::getDoubleTy(*TheContext),
                               {Type::getDoubleTy(*TheContext)}, false);
  return Function::Create(Ty, Function::ExternalLinkage, "ks_print", *TheModule);
}

Value *NumberExprAST::codegen() {
  return ConstantFP::get(*TheContext, APFloat(Val));
}

Value *VariableExprAST::codegen() {
  AllocaInst *A = NamedValues[Name];
  if (!A) {
    fprintf(stderr, "Unknown variable name '%s'\n", Name.c_str());
    return nullptr;
  }
  return Builder->CreateLoad(Type::getDoubleTy(*TheContext), A, Name.c_str());
}

Value *BinaryExprAST::codegen() {
  Value *L = LHS->codegen();
  Value *R = RHS->codegen();
  if (!L || !R) return nullptr;

  switch (Op) {
  case '+': return Builder->CreateFAdd(L, R, "addtmp");
  case '-': return Builder->CreateFSub(L, R, "subtmp");
  case '*': return Builder->CreateFMul(L, R, "multmp");
  case '/': return Builder->CreateFDiv(L, R, "divtmp");
  case '<': {
    Value *Cmp = Builder->CreateFCmpULT(L, R, "cmptmp");
    return Builder->CreateUIToFP(Cmp, Type::getDoubleTy(*TheContext), "booltmp");
  }
  default:
    fprintf(stderr, "invalid binary operator '%c'\n", Op);
    return nullptr;
  }
}

Value *CallExprAST::codegen() {
  if (Callee == "print" && Args.size() == 1) {
    auto *PrintFn = getOrCreatePrintFunction();
    Value *Arg = Args[0]->codegen();
    if (!Arg) return nullptr;
    return Builder->CreateCall(PrintFn, {Arg}, "printtmp");
  }

  Function *CalleeF = getFunction(Callee);
  if (!CalleeF) {
    fprintf(stderr, "Unknown function referenced: %s\n", Callee.c_str());
    return nullptr;
  }
  if (CalleeF->arg_size() != Args.size()) {
    fprintf(stderr, "Incorrect # arguments passed to %s\n", Callee.c_str());
    return nullptr;
  }

  std::vector<Value *> ArgsV;
  for (auto &Arg : Args) {
    Value *V = Arg->codegen();
    if (!V) return nullptr;
    ArgsV.push_back(V);
  }
  return Builder->CreateCall(CalleeF, ArgsV, "calltmp");
}

Value *IfExprAST::codegen() {
  Value *CondV = Cond->codegen();
  if (!CondV) return nullptr;
  CondV = Builder->CreateFCmpONE(
      CondV, ConstantFP::get(*TheContext, APFloat(0.0)), "ifcond");

  Function *F = Builder->GetInsertBlock()->getParent();
  BasicBlock *ThenBB = BasicBlock::Create(*TheContext, "then", F);
  BasicBlock *ElseBB = BasicBlock::Create(*TheContext, "else");
  BasicBlock *MergeBB = BasicBlock::Create(*TheContext, "ifcont");
  Builder->CreateCondBr(CondV, ThenBB, ElseBB);

  Builder->SetInsertPoint(ThenBB);
  Value *ThenV = Then->codegen();
  if (!ThenV) return nullptr;
  Builder->CreateBr(MergeBB);
  ThenBB = Builder->GetInsertBlock();

  F->insert(F->end(), ElseBB);
  Builder->SetInsertPoint(ElseBB);
  Value *ElseV = Else->codegen();
  if (!ElseV) return nullptr;
  Builder->CreateBr(MergeBB);
  ElseBB = Builder->GetInsertBlock();

  F->insert(F->end(), MergeBB);
  Builder->SetInsertPoint(MergeBB);
  PHINode *PN = Builder->CreatePHI(Type::getDoubleTy(*TheContext), 2, "iftmp");
  PN->addIncoming(ThenV, ThenBB);
  PN->addIncoming(ElseV, ElseBB);
  return PN;
}

Value *ForExprAST::codegen() {
  Function *F = Builder->GetInsertBlock()->getParent();
  AllocaInst *Alloca = CreateEntryBlockAlloca(F, VarName);
  Value *StartVal = Start->codegen();
  if (!StartVal) return nullptr;
  Builder->CreateStore(StartVal, Alloca);

  BasicBlock *LoopBB = BasicBlock::Create(*TheContext, "loop", F);
  Builder->CreateBr(LoopBB);
  Builder->SetInsertPoint(LoopBB);

  AllocaInst *OldVal = NamedValues[VarName];
  NamedValues[VarName] = Alloca;

  if (!Body->codegen()) return nullptr;

  Value *StepVal = nullptr;
  if (Step)
    StepVal = Step->codegen();
  else
    StepVal = ConstantFP::get(*TheContext, APFloat(1.0));

  Value *CurVar = Builder->CreateLoad(Type::getDoubleTy(*TheContext), Alloca, VarName.c_str());
  Value *NextVar = Builder->CreateFAdd(CurVar, StepVal, "nextvar");
  Builder->CreateStore(NextVar, Alloca);

  Value *EndCond = End->codegen();
  if (!EndCond) return nullptr;
  EndCond = Builder->CreateFCmpONE(
      EndCond, ConstantFP::get(*TheContext, APFloat(0.0)), "loopcond");
  BasicBlock *AfterBB = BasicBlock::Create(*TheContext, "afterloop", F);
  Builder->CreateCondBr(EndCond, LoopBB, AfterBB);
  Builder->SetInsertPoint(AfterBB);

  if (OldVal)
    NamedValues[VarName] = OldVal;
  else
    NamedValues.erase(VarName);
  return Constant::getNullValue(Type::getDoubleTy(*TheContext));
}

Function *PrototypeAST::codegen() {
  std::vector<Type *> Doubles(Args.size(), Type::getDoubleTy(*TheContext));
  FunctionType *FT = FunctionType::get(Type::getDoubleTy(*TheContext), Doubles, false);
  Function *F = Function::Create(FT, Function::ExternalLinkage, Name, *TheModule);
  unsigned Idx = 0;
  for (auto &Arg : F->args()) Arg.setName(Args[Idx++]);
  return F;
}

Function *FunctionAST::codegen() {
  Function *TheFunction = getFunction(Proto->getName());
  if (!TheFunction)
    TheFunction = Proto->codegen();
  if (!TheFunction) return nullptr;

  if (!TheFunction->empty()) {
    fprintf(stderr, "Function cannot be redefined.\n");
    return nullptr;
  }

  BasicBlock *BB = BasicBlock::Create(*TheContext, "entry", TheFunction);
  Builder->SetInsertPoint(BB);
  NamedValues.clear();

  for (auto &Arg : TheFunction->args()) {
    AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, std::string(Arg.getName()));
    Builder->CreateStore(&Arg, Alloca);
    NamedValues[std::string(Arg.getName())] = Alloca;
  }

  if (Value *RetVal = Body->codegen()) {
    Builder->CreateRet(RetVal);
    verifyFunction(*TheFunction);
    return TheFunction;
  }

  TheFunction->eraseFromParent();
  return nullptr;
}

// -----------------------------------------------------------------------------
// REPL driver
// -----------------------------------------------------------------------------

static void HandleDefinition() {
  if (auto FnAST = ParseDefinition()) {
    if (Function *F = FnAST->codegen()) {
      if (verifyFunction(*F)) return;
      errs() << "Defined: ";
      F->print(errs());
      errs() << "\n";

      auto TSM = ThreadSafeModule(std::move(TheModule), std::move(TheContext));
      if (auto Err = TheJIT->addIRModule(std::move(TSM))) {
        logAllUnhandledErrors(std::move(Err), errs(), "JIT error: ");
      }
      InitializeModule();
    }
  } else {
    getNextToken();
  }
}

static void HandleExtern() {
  if (auto ProtoAST = ParseExtern()) {
    if (Function *F = ProtoAST->codegen()) {
      F->print(errs());
      errs() << "\n";
    }
  } else {
    getNextToken();
  }
}

static void HandleTopLevelExpression() {
  if (auto FnAST = ParseTopLevelExpr()) {
    if (Function *F = FnAST->codegen()) {
      std::string FnName = F->getName().str();
      verifyFunction(*F);

      auto TSM = ThreadSafeModule(std::move(TheModule), std::move(TheContext));
      ResourceTrackerSP RT = TheJIT->getMainJITDylib().createResourceTracker();
      if (auto Err = TheJIT->addIRModule(RT, std::move(TSM))) {
        logAllUnhandledErrors(std::move(Err), errs(), "JIT error: ");
        InitializeModule();
        return;
      }

      auto ExprSymbol = TheJIT->lookup(FnName);
      if (!ExprSymbol) {
        logAllUnhandledErrors(ExprSymbol.takeError(), errs(), "Lookup error: ");
      } else {
        auto *FP = ExprSymbol->toPtr<double (*)()>();
        std::cout << "=> " << FP() << "\n";
      }
      if (auto Err = RT->remove())
        logAllUnhandledErrors(std::move(Err), errs(), "JIT cleanup error: ");
      InitializeModule();
    }
  } else {
    getNextToken();
  }
}

static void MainLoop() {
  while (true) {
    std::cout << "ready> " << std::flush;
    switch (CurTok) {
    case tok_eof: return;
    case ';': getNextToken(); break;
    case tok_def: HandleDefinition(); break;
    case tok_extern: HandleExtern(); break;
    default: HandleTopLevelExpression(); break;
    }
  }
}

int main() {
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  InitializeNativeTargetAsmParser();

  auto JITExpected = LLJITBuilder().create();
  if (!JITExpected) {
    logAllUnhandledErrors(JITExpected.takeError(), errs(), "JIT creation error: ");
    return 1;
  }
  TheJIT = std::move(*JITExpected);

  // Make the C++ print helper visible to JIT-compiled code.
  auto Gen = DynamicLibrarySearchGenerator::GetForCurrentProcess(
      TheJIT->getDataLayout().getGlobalPrefix());
  if (!Gen) {
    logAllUnhandledErrors(Gen.takeError(), errs(), "JIT symbol generator error: ");
    return 1;
  }
  TheJIT->getMainJITDylib().addGenerator(std::move(*Gen));

  BinopPrecedence['<'] = 10;
  BinopPrecedence['+'] = 20;
  BinopPrecedence['-'] = 20;
  BinopPrecedence['*'] = 40;
  BinopPrecedence['/'] = 40;

  InitializeModule();
  getNextToken();
  MainLoop();
  return 0;
}
