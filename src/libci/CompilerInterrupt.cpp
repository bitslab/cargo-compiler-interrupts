/* Predictive Compiler Interrupt Module Pass */

#include "unistd.h"
#include <fstream>
#include <map>
#include <ostream>
#include <sstream>

#include "llvm/ADT/SCCIterator.h"
#include "llvm/Analysis/BranchProbabilityInfo.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/IVDescriptors.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/MemorySSAUpdater.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Type.h"
// #include "llvm/IR/TypeBuilder.h"
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/Pass.h"
#include "llvm/Support/BranchProbability.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Scalar/LoopUnrollPass.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"
#include "llvm/Transforms/Utils/PromoteMemToReg.h"
#include "llvm/Transforms/Utils/UnrollLoop.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

using namespace llvm;

namespace {

/* NOTE: Although hasFence is declared in many containers, it has no use now. It was meant to notify the parent containers 
 * that some unknown inner container has a fence inside it. But it seems like making such a container has no use, since it 
 * cannot be used to aggregate costs around it */

/*
#define PASS_BASED_MLC // Master clock is created in this pass. Otherwise it is added manually to the source code 
#define HEAP_BASED_TL_LC // Local Counter is created as a thread local heap variable. Otherwise it is passed as parameter
#define WITHOUT_COUNTER // No local counter is used. All updates are made to global Master Clock
#define ACCURACY // For printing compiler interrupt per function or per block
#define PRINT_LC_DEBUG_INFO // For printing function name or basic block name at the time of printing compiler interrupt value
#define INTERVAL_ACCURACY
#define ADD_RUNTIME_PRINTS // adds prints at runtime for debugging
*/
//#define ALL_DEBUG // directive for all debug statements. Use this instead of #if 0

//#define LC_DEBUG // directive for current debug
//#define PROFILING
#define PRINT_LC_DEBUG_INFO // required for phoenix for benchmarking by finding "main" name

//#define CRNT_DEBUG // directive for current debug

//#define ACCURACY // For printing compiler interrupt per function or per block

//#define INTERVAL_ACCURACY

//#define LC_DEBUG

//#define PROFILING
#define EAGER_OPT // this instruments the cost in the bottom of the basic block representing the LCC container instead of the top. This allows compiler to optimizes certain instrumentations
//#define SHIFT

#ifdef ACCURACY
#define PRINT_LC_DEBUG_INFO
#endif

//#define ALL_DEBUG
//#define CRNT_DEBUG
//#define PROFILING

#ifdef ALL_DEBUG
#define LC_DEBUG
#endif

#ifdef LC_DEBUG
#define PROFILING
#define CRNT_DEBUG
#endif

//#define ALLOWED_DEVIATION (CommitInterval/5)
#define ALLOWED_DEVIATION 100

/******************************************* Section: Structure & Class Definitions ******************************************/

/* Contains list of different types of instrumentation levels - on adding/removing new configurations, update checkIfInstGranIs*() class of functions */
enum instrumentationLevel {
  OPTIMIZE_HEURISTIC = 1,
  NAIVE = 3,                                  /* 3 */
  LEGACY_HEURISTIC = 5,                       /* 5 */
  COREDET_HEURISTIC = 7,                      /* 7 */
  OPTIMIZE_HEURISTIC_WITH_TL = 2,             /* 2 - opt default */
  NAIVE_TL = 4,                               /* 4 - naive default */
  COREDET_HEURISTIC_TL = 6,                   /* 6 */
  LEGACY_HEURISTIC_TL = 10,                   /* 10 */
  LEGACY_ACCURATE = 8,                        /* 8 */
  OPTIMIZE_ACCURATE = 9,                      /* 9 */
  NAIVE_ACCURATE = 11,                        /* 11 */
  OPTIMIZE_INTERMEDIATE = 12,                 /* 12 */
  NAIVE_INTERMEDIATE = 13,                    /* 13 */
  OPTIMIZE_HEURISTIC_FIBER = 14,              /* 14 - for fiber, interrupts are not disabled */
  OPTIMIZE_HEURISTIC_INTERMEDIATE_FIBER = 15, /* 15 - for fiber, interrupts are not disabled */
  NAIVE_HEURISTIC_FIBER = 16,                 /* 16 - for fiber, interrupts are not disabled */
  OPTIMIZE_CYCLES = 17,                       /* 17 - instrument based on IR, but check cycles at runtime */
  NAIVE_CYCLES = 18                           /* 18 - instrument based on IR, but check cycles at runtime */
};

typedef enum instrumentType {
  ALL_IR = 0,
  PUSH_ON_CYCLES = 1,
  INCR_ON_CYCLES = 2
} eInstrumentType;

struct fstats {
  int blocks = 0;
  int unit_lcc = 0;
  int final_lcc = 0;
  int instrumentedCount = 0;
  int unhandledLoops = 0;
  int rule1Count = 0;
  int rule1ContCount = 0;
  int rule2Count = 0;
  int rule2ContCount = 0;
  int rule2SavedCount = 0;
  int rule3Count = 0;
  int rule3ContCount = 0;
  int rule3SavedCount = 0;
  int rule4Count = 0;
  int rule4SavedCount = 0;
  int rule5Count = 0;
  int rule5SavedCount = 0;
  int rule6Count = 0;
  int rule7Count = 0;
  int rule7ContCount = 0;
  int rule7SavedCount = 0;
  int ruleCoredet = 0;
  int self_loop_transform = 0;
  int generic_loop_transform = 0;
};

enum eClockType {
  PREDICTIVE = 0,
  INSTANTANEOUS
};

struct InstructionCost {
  enum type { ADD = scAddExpr,
              MUL = scMulExpr,
              UDIV = scUDivExpr,
              SMAX = scSMaxExpr,
              SMIN = scSMinExpr,
              UMAX = scUMaxExpr,
              UMIN = scUMinExpr,
              CONST = scConstant,
              ADD_REC_EXPR = scAddRecExpr,
              ZERO_EXT = scZeroExtend,
              SIGN_EXT = scSignExtend,
              TRUNC = scTruncate,
              CALL = 15,
              UNKNOWN,
              ARG } _type;
  typedef std::vector<const struct InstructionCost *> opvector;
  long _value; // Can be negative
  opvector _operands;
  Function *_function;
  const Loop *_loop;
  Type *_castExprType;
  GlobalValue *gVal;
  GlobalVariable *gVar;
  enum recExprType {
    LINEAR,
    QUADRATIC,
    HIGHER_DEGREE
  } _recExprType;
  llvm::SCEV::NoWrapFlags _flags;

  InstructionCost(InstructionCost::type type) : _type(type) {
    assert(type == UNKNOWN);
  }

  InstructionCost(InstructionCost::type type, long value) : _type(type), _value(value) {
    assert(type == ARG || type == CONST);
  }

  InstructionCost(InstructionCost::type type, opvector operands) : _type(type), _operands(operands) {
    assert(type == ADD || type == MUL || type == SMAX || type == SMIN || type == UMAX || type == UMIN);
  }

  InstructionCost(InstructionCost::type type, opvector operands, const Loop *loop, enum recExprType recExpr, llvm::SCEV::NoWrapFlags flags) : _type(type), _operands(operands), _loop(loop), _recExprType(recExpr), _flags(flags) {
    assert(type == ADD_REC_EXPR);
  }

  InstructionCost(InstructionCost::type type, Function *function, opvector operands) : _type(type), _operands(operands), _function(function) {
    assert(type == CALL);
  }

  InstructionCost(InstructionCost::type type, const InstructionCost *a, const InstructionCost *b) : _type(type) {
    assert(type == ADD || type == MUL || type == UDIV || type == SMAX || type == SMIN || type == UMAX || type == UMIN);
    _operands.push_back(a);
    _operands.push_back(b);
  }

  InstructionCost(InstructionCost::type type, Type *castExprType, opvector operands) : _type(type), _operands(operands), _castExprType(castExprType) {
    assert(type == ZERO_EXT || type == SIGN_EXT || type == TRUNC);
  }

  void print() {
    errs() << this;
  }
};

struct FuncInfo {
  bool hasFence;
  InstructionCost *cost;
};

/*********************************************** Section: Global Definitions *********************************************/

static cl::opt<int> InstGranularity("inst-gran", cl::desc("Select instrumentation granularity. 0: Per instruction, 1: Optimized instrumentation 2. Optimized instrumentation with statistics collection, 3. Per basic block, 4: Per Function"), cl::value_desc("0/1/2/3/4"), cl::init(1), cl::Optional);
static cl::opt<int> Configuration("config", cl::desc("Select configuration type. 0: Single-threaded thread-local compiler interrupt, 1: Single-threaded passed compiler interrupt 2. Multithreaded thread-local compiler interrupt, 3. Multithreaded passed compiler interrupt"), cl::value_desc("0/1/2/3/4"), cl::init(2), cl::Optional);
static cl::opt<bool> DefineClock("defclock", cl::desc("Choose whether to define clock in the pass. true: Yes, false: No"), cl::value_desc("true/false"), cl::init(true), cl::Optional);
static cl::opt<int> ClockType("clock-type", cl::desc("Choose clock type. 0: Predictive, 1: Instantaneous"), cl::value_desc("0/1"), cl::init(1), cl::Optional);
static cl::opt<int> MemOpsCost("mem-ops-cost", cl::desc("Cost of memory operations"), cl::value_desc("cost"), cl::init(1), cl::Optional);
static cl::opt<int> TargetInterval("push-intv", cl::desc("Interval in terms of number of instruction cost, for push to global compiler interrupt"), cl::value_desc("positive integer"));
static cl::opt<int> TargetIntervalInCycles("target-cycles", cl::desc("Target interval in cycles"), cl::value_desc("positive integer"), cl::init(0), cl::Optional);
static cl::opt<int> CommitInterval("commit-intv", cl::desc("Interval in terms of number of instruction cost, for commit to local counter"), cl::value_desc("positive integer")); /* Only needed for Instantaneous clock */
static cl::opt<int> ExtLibFuncCost("all-dev", cl::desc("Deviation allowed for branch costs for averaging"), cl::value_desc("positive integer"));
static cl::opt<std::string> ConfigFile("config-file", cl::desc("Configuration file path for the classes & cost of instructions"), cl::value_desc("filepath"), cl::Optional);
static cl::opt<std::string> InCostFilePath("in-cost-file", cl::desc("Cost file from where cost of library functions will be imported"), cl::value_desc("filepath"), cl::Optional);
static cl::opt<std::string> OutCostFilePath("out-cost-file", cl::desc("Cost file where cost of library functions will be exported"), cl::value_desc("filepath"), cl::Optional);
//static cl::opt<int> FiberConfig("fiber-config", cl::desc("Select configuration type. 0: No fiber_yield, 1: fiber_yield with master & local clock 2. fiber_yield with sub operation, 3. fiber_yield with remainder operation, 4. fiber_yield with reset operation, 5. call function intvActionHook() with reset operation, 6. call function intvActionHook() with reset operation without defining LocalLC"), cl::value_desc("0/1/2/3/4/5"), cl::init(0), cl::Optional);
static cl::opt<int> FiberConfig("fiber-config", cl::desc("Select percentage n for threshold for push interval"), cl::value_desc("25/50/75"), cl::init(50), cl::Optional);
LLVMContext *LLVMCtx;
PostDominatorTree *PDT;
DominatorTree *DT;
LoopInfo *LI;
ScalarEvolution *SE;
// MemorySSA *MSSA;
BranchProbabilityInfo *BPI;
std::set<std::string> fenceList;
std::set<Instruction *> callInstToReplaceForPC;         /* list of instructions whose calls will be replaced */
std::set<Instruction *> callInstToReplaceForIC;         /* list of instructions whose calls will be replaced */
std::map<BasicBlock *, InstructionCost *> directBranch; /* list of branch header blocks which need to instrument the direct branch. The Value is the instrumented branch */
std::map<Loop *, InstructionCost *> selfLoop;           /* list of branch header blocks which need to instrument the direct branch. The Value is the instrumented branch */
std::map<Loop *, InstructionCost *> seseLoop;           /* list of branch header blocks which need to instrument the direct branch. The Value is the instrumented branch */
std::map<std::string, const InstructionCost *> libraryInstructionCosts;
std::map<Function *, FuncInfo *> computedFuncInfo;
std::map<StringRef, bool> CGOrderedFunc; // list of functions in call graph order
std::map<Function *, struct fstats> FuncStat;
SmallVector<StringRef, 20> threadFunc; // contains list of all functions that begin a thread & main()
StringMap<unsigned char> ciFuncInApp;  // list of functions used as compiler interrupt in application code
std::map<Function *, AllocaInst *> gLocalCounter;
std::map<Function *, AllocaInst *> gLocalFLag;
int lccIDGen = 0;               /* Adds an ID for every LCC made. Helps in debugging*/
int applyrule1 = 0;             /* Signifies path rule */
int applycontrule1 = 0;         /* Signifies path container rule */
int applyrule2 = 0;             /* Signifies conditionals */
int applycontrule2 = 0;         /* Signifies conditionals container rule */
int rule2savedInst = 0;         /* Represents the number of blocks saved from instrumentation using rule 2*/
int applyrule3 = 0;             /* Signifies loops with known number of iterations */
int applycontrule3 = 0;         /* Signifies containers for loops with known number of iterations */
int rule3savedInst = 0;         /* Represents the number of blocks saved from instrumentation using rule 4 */
int applyrule4 = 0;             /* not used */
int rule4savedInst = 0;         /* not used */
int applyrule5 = 0;             /* Signifies inverted Y rule */
int rule5savedInst = 0;         /* Represents the number of blocks saved from instrumentation using rule 5 */
int applyrule6 = 0;             /* Signifies Y rule */
int applyrule7 = 0;             /* Signifies complex conditionals */
int applycontrule7 = 0;         /* Signifies complex conditionals container rule */
int rule7savedInst = 0;         /* Represents the number of blocks saved from instrumentation using rule 7*/
int ruleCoredet = 0;            /* rulecoredet */
int unhandled_loop = 0;         /* Exception case of loop header having multiple predecessors */
int instrumentedInst = 0;       /* global stats variable used for sanity checks */
int numUninstrumentedFunc = 0;  /* Number of functions whose cost was optimized out */
int self_loop_transform = 0;    /* Signifies path rule */
int generic_loop_transform = 0; /* Signifies path rule */
bool gIsOnlyThreadLocal = false;
bool gUseReadCycles = false;

/*********************************************** Section: Utility Functions *********************************************/

raw_ostream &operator<<(raw_ostream &os, InstructionCost const &fc) {
  switch (fc._type) {
  case InstructionCost::CONST:
    os << fc._value;
    break;
  case InstructionCost::ARG:
    os << "(ARG: " << fc._value << ")";
    break;
  case InstructionCost::ADD:
  case InstructionCost::MUL:
  case InstructionCost::UDIV:
  case InstructionCost::SMAX:
  case InstructionCost::SMIN:
  case InstructionCost::UMAX:
  case InstructionCost::UMIN:
    if (fc._operands.size() > 1) {
      os << "(";
    }
    switch (fc._type) {
    case InstructionCost::ADD:
      os << "+ ";
      break;
    case InstructionCost::MUL:
      os << "* ";
      break;
    case InstructionCost::UDIV:
      os << "/ ";
      break;
    case InstructionCost::SMAX:
      os << "smax ";
      break;
    case InstructionCost::SMIN:
      os << "smin ";
      break;
    case InstructionCost::UMAX:
      os << "umax ";
      break;
    case InstructionCost::UMIN:
      os << "umin ";
      break;
    default:
      assert(false);
    }
    for (auto op : fc._operands) {
      os << *op << " ";
    }
    if (fc._operands.size() > 1) {
      os << ")";
    }
    break;
  case InstructionCost::ZERO_EXT:
    os << "(zext " << *(fc._operands[0]) << " " << *(fc._castExprType) << ")";
    break;
  case InstructionCost::SIGN_EXT:
    os << "(sext " << *(fc._operands[0]) << " " << *(fc._castExprType) << ")";
    break;
  case InstructionCost::TRUNC:
    os << "(trunc " << *(fc._operands[0]) << " " << *(fc._castExprType) << ")";
    break;
  case InstructionCost::CALL:
    os << "call_cost(" << fc._function->getName() << "(";
    for (auto op : fc._operands) {
      os << *op << ", ";
    }
    os << ")) ";
    break;
  case InstructionCost::ADD_REC_EXPR:
    //os << "add_rec(start: " << *fc._operands[0] << ", step: " << *fc._operands[1] << ", loop:" << *fc._loop << ", flags:" << fc._flags << ")";
    os << "add_rec(start: " << *fc._operands[0] << ", step: " << *fc._operands[1] << ")";
    break;
  case InstructionCost::UNKNOWN:
    os << "(unknown)";
  }
  return os;
}

/* returns null if there are multiple return blocks */
BasicBlock *getFuncExitBlock(Function *F) {
  BasicBlock *exitBB = nullptr;
  for (auto &BB : *F) {
    auto termInst = BB.getTerminator();
    if (isa<ReturnInst>(termInst) || isa<UnreachableInst>(termInst)) {
      if (!exitBB) {
        exitBB = &BB;
      } else {
        return nullptr; /* since multiple return blocks are present, so return null */
      }
    }
  }
  return exitBB;
}

/* 0 is a valid value */
long hasConstCost(const InstructionCost *fc) {
  if (fc && fc->_type == InstructionCost::CONST) {
    return fc->_value;
  } else {
    return -1;
  }
}

/* same as hasConstCost, but will assert if a non-numeric cost is given as input */
long getConstCost(const InstructionCost *fc) {
  long numCost = -1;
  if (fc && fc->_type == InstructionCost::CONST) {
    numCost = fc->_value;
  }
  assert((numCost >= 0) && "Instruction cost is not a constant value!");
  return numCost;
}

/* same as hasConstCost, but will assert if a non-numeric cost is given as input */
long getConstCostWithoutAssert(const InstructionCost *fc) {
  long numCost = -1;
  if (fc && fc->_type == InstructionCost::CONST) {
    numCost = fc->_value;
  }
  return numCost;
}

InstructionCost *scevToCost(const SCEV *scev) {
  switch (scev->getSCEVType()) {
  case scConstant: {
    const SCEVConstant *c = dyn_cast<const SCEVConstant>(scev);
    int intVal = 0;
    if (ConstantInt *CI = dyn_cast<ConstantInt>(c->getValue())) {
      if (CI->getBitWidth() <= 64) {
        intVal = CI->getSExtValue();
      } else if (CI->getBitWidth() > 64) {
        errs() << "BitWidth of SCEV Constant is larger than 64. Cannot convert to InstructionCost type. \n";
        return new InstructionCost(InstructionCost::UNKNOWN);
      }
    } else {
      errs() << "SCEV Constant value is not a ConstantInt. Cannot convert to InstructionCost type. \n";
      return new InstructionCost(InstructionCost::UNKNOWN);
    }
    InstructionCost *cost = new InstructionCost(InstructionCost::CONST, intVal);
    //errs() << "Const scev: " << *scev << ", SCEVConstant: " << *c << ", passed value: (" << *(c->getValue()) << "," << (c->getValue()->getLimitedValue(UINT64_MAX - 1)) << "), inst cost: " << *cost << ", int val: " << intVal << "\n";
    return cost;
  }
  case scAddExpr:
  case scMulExpr:
  case scSMaxExpr:
  case scUMaxExpr:
  case scSMinExpr: {
    const SCEVNAryExpr *c = dyn_cast<const SCEVNAryExpr>(scev);
    InstructionCost::opvector ops;
    for (auto op : c->operands()) {
      ops.push_back(scevToCost(op));
    }
    InstructionCost *cost = new InstructionCost((InstructionCost::type)scev->getSCEVType(), ops);
    return cost;
  }
  case scUDivExpr: {
    const SCEVUDivExpr *c = dyn_cast<const SCEVUDivExpr>(scev);
    InstructionCost *fcLHS = scevToCost(c->getLHS());
    InstructionCost *fcRHS = scevToCost(c->getRHS());
    InstructionCost *cost = new InstructionCost((InstructionCost::type)scev->getSCEVType(), fcLHS, fcRHS);
    return cost;
  }
  case scUnknown: {
    const SCEVUnknown *c = dyn_cast<const SCEVUnknown>(scev);
    //errs() << "Received unknown scev. llvm type: " << *(scev->getType()) << ", scev: " << *scev << ", c id: " << c->getValue()->getValueID() << ", c: " << *c << ", c value: " << *(c->getValue()) << "\n";
    if (const Argument *a = dyn_cast<const Argument>(c->getValue())) {
      //errs() << "Got SCUnknown Arg type. Arg No.: " << a->getArgNo() << ", Arg: " << *a << "\n";
      return new InstructionCost(InstructionCost::ARG, a->getArgNo());
    }
#if 0
    else if (const GlobalValue *g = dyn_cast<GlobalValue>(c->getValue())) {
      errs() << "Got SCUnknown Global value type. Global Value: " << *g << "\n";
      return new InstructionCost(InstructionCost::UNKNOWN);
    } else if (const GlobalVariable *gv = dyn_cast<GlobalVariable>(c->getValue())) {
      errs() << "Got SCUnknown Global variable type. Global variable: " << *gv << "\n";
      return new InstructionCost(InstructionCost::UNKNOWN);
    }
#endif
    else {
      //errs() << "Got SCUnknown Unknown type. Value id: " << c->getValue()->getValueName() <<", c value: " << *(c->getValue()) << "\n";
      return new InstructionCost(InstructionCost::UNKNOWN);
    }
  }
  case scAddRecExpr: {
#if 0
    const SCEVAddRecExpr *c = dyn_cast<const SCEVAddRecExpr>(scev);
    auto v = expandAddRecExprLiterally(c);
    errs() << "Literal Representation of ADD REC: " << *v << "\n";
    InstructionCost::opvector ops;
    ops.push_back(scevToCost(c->getStart()));
    ops.push_back(scevToCost(c->getStepRecurrence(*SE)));
    InstructionCost *cost;
    if (c->isAffine())
      cost = new InstructionCost((InstructionCost::type)scev->getSCEVType(), ops, c->getLoop(), InstructionCost::LINEAR, c->getNoWrapFlags());
    else {
      cost = new InstructionCost(InstructionCost::UNKNOWN);
#if 0
      /* Not processing higher order ones */
      else if (c->isQuadratic())
          cost = new InstructionCost((InstructionCost::type)scev->getSCEVType(), ops, c->getLoop(), InstructionCost::QUADRATIC, c->getNoWrapFlags());
      else cost = new InstructionCost((InstructionCost::type)scev->getSCEVType(), ops, c->getLoop(), InstructionCost::HIGHER_DEGREE, c->getNoWrapFlags());
#endif
    }
    return cost;
#else
    return new InstructionCost(InstructionCost::UNKNOWN);
#endif
  }
  case scZeroExtend: {
    const SCEVZeroExtendExpr *c = dyn_cast<const SCEVZeroExtendExpr>(scev);
    InstructionCost::opvector ops;
    //errs() << "Zero Extend: operand " << *(c->getOperand()) << ", type " << *(c->getType()) << ", scev " << *scev << "\n";
    ops.push_back(scevToCost(c->getOperand()));
    return new InstructionCost((InstructionCost::type)scev->getSCEVType(), c->getType(), ops);
  }
  case scSignExtend: {
    const SCEVSignExtendExpr *c = dyn_cast<const SCEVSignExtendExpr>(scev);
    InstructionCost::opvector ops;
#ifdef LC_DEBUG
    errs() << "Sign Extend: operand " << *(c->getOperand()) << ", type " << *(c->getType()) << ", scev " << *scev << "\n";
#endif
    ops.push_back(scevToCost(c->getOperand()));
    return new InstructionCost((InstructionCost::type)scev->getSCEVType(), c->getType(), ops);
  }
  case scTruncate: {
    const SCEVTruncateExpr *c = dyn_cast<const SCEVTruncateExpr>(scev);
    InstructionCost::opvector ops;
#ifdef LC_DEBUG
    errs() << "Truncate: operand " << *(c->getOperand()) << ", type " << *(c->getType()) << ", scev " << *scev << "\n";
#endif
    ops.push_back(scevToCost(c->getOperand()));
    return new InstructionCost((InstructionCost::type)scev->getSCEVType(), c->getType(), ops);
  }
  case scCouldNotCompute: {
#ifdef LC_DEBUG
    errs() << "scCouldNotCompute SCEV type: " << scev->getSCEVType() << ", expression " << *scev << ". Don't know how to compute.\n";
#endif
    return new InstructionCost(InstructionCost::UNKNOWN);
  }
  default: {
#ifdef LC_DEBUG
    errs() << "Unknown SCEV type: " << scev->getSCEVType() << ", expression " << *scev << ". Don't know how to compute.\n";
#endif
    return new InstructionCost(InstructionCost::UNKNOWN);
  }
  }
}

const SCEV *costToSCEV(const InstructionCost *cost, std::vector<const SCEV *> args) {
  if (!cost) {
    return nullptr;
  }
  switch (cost->_type) {
  case InstructionCost::CONST: {
    const SCEV *scev = SE->getConstant(Type::getInt64Ty(*LLVMCtx), cost->_value, true);
    return scev;
  }
  case InstructionCost::ARG: {
    if (cost->_value >= (long)args.size()) {
      errs() << "index is " << cost->_value << ", max is " << (int)args.size() << ", value is " << *args[cost->_value] << "\n";
    }
    assert(cost->_value < (long)args.size());
    //errs() << "Arg[" << cost->_value << "]: " << *(args[cost->_value]) << "\n";
    return args[cost->_value];
  }
  case InstructionCost::UNKNOWN: {
    return SE->getCouldNotCompute();
  }
  case InstructionCost::ADD: {
    SmallVector<const SCEV *, 10> ops;
    Type *widestType = nullptr;
    for (auto op : cost->_operands) {
      const SCEV *scev = costToSCEV(op, args);
      if (!scev || (scev == SE->getCouldNotCompute())) {
        return scev;
      }
      if (widestType) {
        if (widestType != SE->getEffectiveSCEVType(scev->getType())) {
          widestType = SE->getWiderType(widestType, SE->getEffectiveSCEVType(scev->getType()));
        }
      } else {
        widestType = SE->getEffectiveSCEVType(scev->getType());
      }
    }
    for (auto op : cost->_operands) {
      const SCEV *scev = costToSCEV(op, args);
      /* getAddExpr does not allow operands of varying bit size. So the smaller operand is Zero extended to match the wider one. */
      if (widestType != SE->getEffectiveSCEVType(scev->getType())) {
        scev = SE->getZeroExtendExpr(scev, widestType);
      }
      ops.push_back(scev);
    }
    const SCEV *scev = nullptr;
    if (ops.size() >= 2) {
      scev = SE->getAddExpr(ops);
    } else if (ops.size() == 1) {
      scev = ops[0];
    }
    return scev;
  }
  case InstructionCost::MUL: {
    SmallVector<const SCEV *, 10> ops;
    Type *widestType = nullptr;
    for (auto op : cost->_operands) {
      const SCEV *scev = costToSCEV(op, args);
      if (!scev || (scev == SE->getCouldNotCompute())) {
        return scev;
      }
      if (widestType) {
        if (widestType != SE->getEffectiveSCEVType(scev->getType())) {
          widestType = SE->getWiderType(widestType, SE->getEffectiveSCEVType(scev->getType()));
        }
      } else {
        widestType = SE->getEffectiveSCEVType(scev->getType());
      }
    }
    for (auto op : cost->_operands) {
      const SCEV *scev = costToSCEV(op, args);
      /* getMulExpr does not allow operands of varying bit size. So the smaller operand is Zero extended to match the wider one. */
      if (widestType != SE->getEffectiveSCEVType(scev->getType())) {
        scev = SE->getZeroExtendExpr(scev, widestType);
      }
      ops.push_back(scev);
    }
    const SCEV *scev = SE->getMulExpr(ops);
    return scev;
  }
  case InstructionCost::UDIV: {
    const SCEV *left = costToSCEV(cost->_operands[0], args);
    const SCEV *right = costToSCEV(cost->_operands[1], args);
    const SCEV *leftScev = left, *rightScev = right;
    if (left == SE->getCouldNotCompute() || right == SE->getCouldNotCompute()) {
      return SE->getCouldNotCompute();
    }
    auto leftType = SE->getEffectiveSCEVType(left->getType());
    auto rightType = SE->getEffectiveSCEVType(right->getType());
    if ((leftType != rightType) && SE->getWiderType(leftType, rightType)) {
      //errs() << "Left greater --> Right type: " << *rightType << ", Left Type: " << *leftType << "\n";
      leftScev = SE->getZeroExtendExpr(left, rightType);
    } else if ((leftType != rightType) && SE->getWiderType(rightType, leftType)) {
      //errs() << "Right greater --> Right type: " << *rightType << ", Left Type: " << *leftType << "\n";
      rightScev = SE->getZeroExtendExpr(right, leftType);
    }

    const SCEV *scev = SE->getUDivExpr(leftScev, rightScev);
    return scev;
  }
  case InstructionCost::SMAX: {
    SmallVector<const SCEV *, 10> ops;
    const SCEV *prev = nullptr;
    for (auto op : cost->_operands) {
      const SCEV *scev = costToSCEV(op, args);
      if (scev == SE->getCouldNotCompute()) {
#ifdef LC_DEBUG
        errs() << "Cannot compute the operand of SMAX " << *op << " (" << *scev << ")\n";
#endif
        return scev;
      }
      assert(scev && "OOPS, this SMAX operand was null!");
      if (prev) {
        /* SCEV code does not allow operand SCEVs of different types (ScalarEvolution.cpp:3210, Assertion: "SCEVSMaxExpr operand types don't match!") */
        if (SE->getEffectiveSCEVType(prev->getType()) != SE->getEffectiveSCEVType(scev->getType())) {
          errs() << "SMAX: Types of operands are different. Prev is " << *(prev->getType()) << "(" << *(SE->getEffectiveSCEVType(prev->getType())) << ")"
                 << ", current is " << *(scev->getType()) << "(" << *(SE->getEffectiveSCEVType(scev->getType())) << ")\n";
          return SE->getCouldNotCompute();
        }
      } else {
        prev = scev;
      }

      ops.push_back(scev);
    }
    const SCEV *scev = SE->getSMaxExpr(ops);
    return scev;
  }
  case InstructionCost::SMIN: {
    SmallVector<const SCEV *, 10> ops;
    const SCEV *prev = nullptr;
    for (auto op : cost->_operands) {
      const SCEV *scev = costToSCEV(op, args);
      if (scev == SE->getCouldNotCompute()) {
#ifdef LC_DEBUG
        errs() << "Cannot compute the operand of SMIN " << *op << " (" << *scev << ")\n";
#endif
        return scev;
      }
      assert(scev && "OOPS, this SMIN operand was null!");
      if (prev) {
        /* SCEV code does not allow operand SCEVs of different types (ScalarEvolution.cpp:3210, Assertion: "SCEVSMinExpr operand types don't match!") */
        if (SE->getEffectiveSCEVType(prev->getType()) != SE->getEffectiveSCEVType(scev->getType())) {
          errs() << "SMIN: Types of operands are different. Prev is " << *(prev->getType()) << "(" << *(SE->getEffectiveSCEVType(prev->getType())) << ")"
                 << ", current is " << *(scev->getType()) << "(" << *(SE->getEffectiveSCEVType(scev->getType())) << ")\n";
          return SE->getCouldNotCompute();
        }
      } else {
        prev = scev;
      }

      ops.push_back(scev);
    }
    const SCEV *scev = SE->getSMinExpr(ops);
    return scev;
  }
  case InstructionCost::UMAX: {
    SmallVector<const SCEV *, 10> ops;
    const SCEV *prev = nullptr;
    for (auto op : cost->_operands) {
      const SCEV *scev = costToSCEV(op, args);
      if (scev == SE->getCouldNotCompute()) {
#ifdef LC_DEBUG
        errs() << "Cannot compute the operand of UMAX " << *op << " (" << *scev << ")\n";
#endif
        return scev;
      }
      assert(scev && "OOPS, this UMAX operand was null!");
      if (prev) {
        /* SCEV code does not allow operand SCEVs of different types (ScalarEvolution.cpp:3210, Assertion: "SCEVUMaxExpr operand types don't match!") */
        if (SE->getEffectiveSCEVType(prev->getType()) != SE->getEffectiveSCEVType(scev->getType())) {
          errs() << "UMAX: Types of operands are different. Prev is " << *(prev->getType()) << "(" << *(SE->getEffectiveSCEVType(prev->getType())) << ")"
                 << ", current is " << *(scev->getType()) << "(" << *(SE->getEffectiveSCEVType(scev->getType())) << ")\n";
          return SE->getCouldNotCompute();
        }
      } else {
        prev = scev;
      }
      ops.push_back(scev);
    }
    const SCEV *scev = SE->getUMaxExpr(ops);
    return scev;
  }
  case InstructionCost::UMIN: {
    SmallVector<const SCEV *, 10> ops;
    const SCEV *prev = nullptr;
    for (auto op : cost->_operands) {
      const SCEV *scev = costToSCEV(op, args);
      if (scev == SE->getCouldNotCompute()) {
#ifdef LC_DEBUG
        errs() << "Cannot compute the operand of UMIN " << *op << " (" << *scev << ")\n";
#endif
        return scev;
      }
      assert(scev && "OOPS, this UMIN operand was null!");
      if (prev) {
        /* SCEV code does not allow operand SCEVs of different types (ScalarEvolution.cpp:3210, Assertion: "SCEVUMinExpr operand types don't match!") */
        if (SE->getEffectiveSCEVType(prev->getType()) != SE->getEffectiveSCEVType(scev->getType())) {
          errs() << "UMIN: Types of operands are different. Prev is " << *(prev->getType()) << "(" << *(SE->getEffectiveSCEVType(prev->getType())) << ")"
                 << ", current is " << *(scev->getType()) << "(" << *(SE->getEffectiveSCEVType(scev->getType())) << ")\n";
          return SE->getCouldNotCompute();
        }
      } else {
        prev = scev;
      }
      ops.push_back(scev);
    }
    const SCEV *scev = SE->getUMinExpr(ops);
    return scev;
  }
  case InstructionCost::ZERO_EXT: {
    const SCEV *op = costToSCEV(cost->_operands[0], args);
    if (op == SE->getCouldNotCompute()) {
      return SE->getCouldNotCompute();
    }
    const SCEV *scev = op;
    if (SE->getTypeSizeInBits(op->getType()) < SE->getTypeSizeInBits(cost->_castExprType)) {
      scev = SE->getZeroExtendExpr(op, cost->_castExprType);
    }
    return scev;
  }
  case InstructionCost::SIGN_EXT: {
    const SCEV *op = costToSCEV(cost->_operands[0], args);
    if (op == SE->getCouldNotCompute()) {
      return SE->getCouldNotCompute();
    }
    const SCEV *scev = SE->getSignExtendExpr(op, cost->_castExprType);
    //errs() << "After reconverting sign extend: " << *scev << "\n";
    return scev;
  }
  case InstructionCost::TRUNC: {
    const SCEV *op = costToSCEV(cost->_operands[0], args);
    if (op == SE->getCouldNotCompute()) {
      return SE->getCouldNotCompute();
    }
    const SCEV *scev = SE->getTruncateExpr(op, cost->_castExprType);
    return scev;
  }
  case InstructionCost::ADD_REC_EXPR: {
#if 0
    const SCEV *start = costToSCEV(cost->_operands[0], args);
    const SCEV *step = costToSCEV(cost->_operands[1], args);
    if (start == SE->getCouldNotCompute() || step == SE->getCouldNotCompute())
      return SE->getCouldNotCompute();
    const SCEV *scev = SE->getAddRecExpr(start, step, cost->_loop, cost->_flags);
    return scev;
#else
    return SE->getCouldNotCompute();
#endif
  }
  case InstructionCost::CALL: {
    /* translate each argument in this call to SCEVs based on current context */
    std::vector<const SCEV *> argumentSCEVs;
    for (auto function_arg : cost->_operands) {
      const SCEV *scev = costToSCEV(function_arg, args);
      /* Not checking scev == SE->getCouldNotCompute() here since some of the arguments may not have been scevable, but they should not appear in the function costs as well. So it will not create any problem. */
      argumentSCEVs.push_back(scev);
    }
    /* find the InstructionCost description of the function's cost */
    const InstructionCost *fCost;
    auto found = computedFuncInfo.find(cost->_function);
    if (found != computedFuncInfo.end()) {
      /* Always use prefix cost for function cost evaluation. If there is a suffix, that will be taken care of at the time of initial container creation */
      fCost = found->second->cost;
#ifdef ALL_DEBUG
      errs() << "Cost for function call " << cost->_function->getName() << "() : " << *fCost << "\n";
#endif
    } else if (libraryInstructionCosts.count(cost->_function->getName().str())) {
      fCost = libraryInstructionCosts[cost->_function->getName().str()];
    } else {
      //errs() << "Unable to find cost for " << cost->_function->getName() << ". Assuming 0 cost.\n";
      const SCEV *scev = SE->getConstant(Type::getInt64Ty(*LLVMCtx), 0, false);
      return scev;
      //return nullptr;
      //return SE->getCouldNotCompute();
    }
    const SCEV *fSCEV = costToSCEV(fCost, argumentSCEVs);
    return fSCEV;
  }
  default:
    errs() << "Unknown InstructionCost type: " << cost->_type << ", returning null\n";
    return 0;
  }
}

InstructionCost *getConstantInstCost(long numCost) {
  return new InstructionCost(InstructionCost::CONST, numCost);
}

/* Simplifies the cost expression & returns it. doNotAssert is the special flag to explicitly turn off assertion 
   * - should always check against null return when turned off */
InstructionCost *simplifyCost(Function *F, InstructionCost *complexCost, bool doNotAssert = false) {
  if (!complexCost) {
    return nullptr;
  }
  std::vector<const SCEV *> funcArgs;
  for (auto &arg : F->args()) {
    if (SE->isSCEVable(arg.getType())) {
      funcArgs.push_back(SE->getSCEV(&arg));
    } else {
      /* if an argument is not scevable don't send null. Otherwise the argument scev will be read from the wrong index. */
      funcArgs.push_back(SE->getCouldNotCompute());
    }
  }
  //errs() << "Simplifying cost for " << *complexCost << ", for func " << F->getName() << " with "  << funcArgs.size() << " args\n";
  const SCEV *costScev = costToSCEV(complexCost, funcArgs);
  if (costScev) { // if cost is not present, it will return nullptr
    if (costScev == SE->getCouldNotCompute()) {
#ifdef LC_DEBUG
      int argNum = 0;
      errs() << "Cost " << *complexCost << " cannot be simplified\n";
      for (auto a : funcArgs) {
        argNum++;
        errs() << "Args [" << argNum << "] = " << *a << "\n";
      }
#endif
    }
    if (!doNotAssert) {
      assert((costScev != SE->getCouldNotCompute()) && "Cost scev cannot be uncomputable!");
    } else if (costScev == SE->getCouldNotCompute()) {
      return nullptr;
    }
    InstructionCost *simplifiedCost = scevToCost(costScev);
    return simplifiedCost;
  }
  return nullptr;
}

bool isThreadFunc(Function *F) {
  for (auto threadFNames : threadFunc) {
    if (F->getName().compare(threadFNames) == 0) {
      return true;
    }
  }
  return false;
}

bool isFenceFunc(Function *F) {
  for (auto fence : fenceList) {
    if (F->getName().compare(fence) == 0) {
      return true;
    }
  }
  return false;
}

__attribute__((unused)) inline bool isSuffix(std::string value, std::string ending) {
  if (ending.size() > value.size()) {
    return false;
  }
  int pos = value.size() - ending.size();
  std::string suffix = value.substr(pos, ending.size());
  if (suffix.compare(ending) == 0) {
    return true;
  } else {
    return false;
  }
}

/* Finds the cost of a particular instruction */
InstructionCost *getInstCostForPC(Instruction *I) {
  Function *F = I->getFunction();
  if (isa<PHINode>(I)) {
    /***************************** For Phi instructions ***************************/
    return getConstantInstCost(0);
  } else if (isa<LoadInst>(I) || isa<StoreInst>(I)) {
    /***************************** For memory operations ***************************/
    return getConstantInstCost(MemOpsCost);
  } else if (CallInst *ci = dyn_cast<CallInst>(I)) {
    /***************************** For call instructions ***************************/
    /* For function pointer calls, getCalledFunction() cannot return the desired info. So they call the fully 
       * instrumented function */
    InstructionCost::opvector callCost;
    callCost.push_back(new InstructionCost(InstructionCost::CONST, 1));
    Function *calledFunction = ci->getCalledFunction();

    if (calledFunction) {
      if (libraryInstructionCosts.count(calledFunction->getName().str())) {
        /* For library function call instructions */
        const InstructionCost *fCost = libraryInstructionCosts[calledFunction->getName().str()];
        callCost.push_back(fCost);
      } else {
        /* For internal function call instructions */
        /* Sanity check: fence instruction must have a configured cost */
        bool isFence = isFenceFunc(calledFunction);
        assert(!isFence && "Fence function costs were not found in the library function cost repository! Aborting.");

        if (!isThreadFunc(calledFunction)) {
          /* Only non-thread functions can have their costs sometimes uninstrumented inside them */
          const InstructionCost *fCost;
          auto found = computedFuncInfo.find(calledFunction);
          if (found != computedFuncInfo.end()) {
            /* Always use prefix cost for function cost evaluation. If there is a suffix, that will be taken care of at the time of initial container creation */
            fCost = found->second->cost;
            if (getConstCostWithoutAssert(fCost) != 0) {
              callCost.push_back(fCost);
            }
#ifdef ALL_DEBUG
            errs() << "Cost for function call " << calledFunction->getName() << "() : " << *fCost << "\n";
#endif
          }
        }
      }

      InstructionCost *ic = new InstructionCost(InstructionCost::ADD, callCost);
      InstructionCost *simplifiedCost = simplifyCost(F, ic);
      if (!simplifiedCost) {
        errs() << " cost that cannot be simplified for " << calledFunction->getName() << "\n";
        //assert(simplifiedCost && "Call instruction's simplified cost cannot be null!");
        return getConstantInstCost(1);
      } else {
        return simplifiedCost;
      }
    } else {
      /* When called function is not known */
      return getConstantInstCost(1);
    }
  } else {
    /************************** For all other instructions ************************/
    return new InstructionCost(InstructionCost::CONST, 1);
  }
  return nullptr; /* Control would & should never reach here */
}

bool checkIfExternalLibraryCall(Instruction *I) {
  if (CallInst *ci = dyn_cast<CallInst>(I)) {
    Function *calledFunction = ci->getCalledFunction();
    if (calledFunction) {
      if (isa<DbgInfoIntrinsic>(I)) {
        return false;
      }
#if 0
      else if (isa<IntrinsicInst>(I)) {
        errs() << "Intrinsic call: " << *I << "\n";
        return false;
      }
#endif
      //errs() << "Called function name: " << calledFunction->getName() << "\n";
      int foundInOwnLib = libraryInstructionCosts.count(calledFunction->getName().str());
      int foundInModule = CGOrderedFunc.count(calledFunction->getName());
      if (foundInOwnLib || foundInModule) {
#if 0
        if (foundInOwnLib != 0) {
          errs() << "Function is part of internal library. Count: " << foundInOwnLib << "\n";
        } else if (foundInModule != 0) {
          errs() << "Function is found in this module. Count: " << foundInModule << "\n";
        } else {
          errs() << "Code error\n";
          exit(1);
        }
#endif
        //errs() << calledFunction->getName() << "() is an internal library function\n";
        return false;
      } else {
        //errs() << calledFunction->getName() << "() is an external library function\n";
        return true;
      }
    }
  }
  return false;
}

/* only for debugging */
__attribute__((unused)) void findAllLibraryCalls(Module &M) {
  errs() << "Finding all library calls\n";
  for (auto &F : M) {
    if (F.isDeclaration()) {
      continue;
    }
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
      checkIfExternalLibraryCall(&*I);
    }
  }
}

int getLibCallCost() {
  if (InstGranularity == NAIVE_ACCURATE || InstGranularity == OPTIMIZE_ACCURATE) {
    return 50; /* 25*2 for 2 readcycle calls */
  } else {
    return ExtLibFuncCost;
  }
}

/* Finds the cost of a particular instruction */
InstructionCost *getInstCostForIC(Instruction *I) {

  long newCost = 0;

  if (isa<PHINode>(I)) {
    /***************************** For Phi instructions ***************************/
    return getConstantInstCost(0);
  } else if (isa<LoadInst>(I) || isa<StoreInst>(I)) {
    /***************************** For memory operations ***************************/
    return getConstantInstCost(MemOpsCost);
  } else if (CallInst *ci = dyn_cast<CallInst>(I)) {
    /***************************** For call instructions ***************************/
    /* For function pointer calls, getCalledFunction() cannot return the desired info. So they call the fully 
       * instrumented function */
    int callCost = 1;
    Function *calledFunction = ci->getCalledFunction();

    if (calledFunction) {
      /* external library call */
      if (checkIfExternalLibraryCall(I)) {
        callCost += getLibCallCost();
        //errs() << "Setting call cost " << ExtLibFuncCost << " for call " << *I << "\n";
      }
      /* own instrumented library function call */
      else if (libraryInstructionCosts.count(calledFunction->getName().str())) {
        const InstructionCost *fCost = libraryInstructionCosts[calledFunction->getName().str()];
        long funcCost = getConstCost(fCost);
        callCost += funcCost;
      }
      /* For internal function call */
      else {
        /* Sanity check: fence instruction must have a configured cost */
        bool isFence = isFenceFunc(calledFunction);
        assert(!isFence && "Fence function costs were not found in the library function cost repository! Aborting.");

        if (!isThreadFunc(calledFunction)) {
          /* Only non-thread functions can have their costs sometimes uninstrumented inside them */
          const InstructionCost *fCost;
          auto found = computedFuncInfo.find(calledFunction);
          if (found != computedFuncInfo.end()) {
            /* Always use prefix cost for function cost evaluation. If there is a suffix, that will be taken care of at the time of initial container creation */
            fCost = found->second->cost;
            long numCallCost = hasConstCost(fCost);
            if (numCallCost > 0) {
              callCost += numCallCost;
            }
          }
        }
      }

      newCost = callCost;
    }
  }
#if 0
  else if (Handle floating point differently) {
  }
#endif
  else {
    /************************** For all other instructions ************************/
    newCost = 1;
  }
  return getConstantInstCost(newCost); /* Control would & should never reach here */
}

Instruction *checkForPhi(Instruction *I) {
  Instruction *returnI = I;
  while (isa<PHINode>(returnI)) {
    BasicBlock::iterator it(returnI);
    it++;
    if (it == I->getParent()->end()) {
      return nullptr;
    }
    returnI = &*it;
  }
  return returnI;
}

bool checkIfInstGranIsOpt() {
  bool res = false;
  switch (InstGranularity) {
  case OPTIMIZE_HEURISTIC_WITH_TL:
  case OPTIMIZE_ACCURATE:
  case OPTIMIZE_INTERMEDIATE:
  case OPTIMIZE_HEURISTIC_INTERMEDIATE_FIBER:
  case OPTIMIZE_HEURISTIC_FIBER:
  case OPTIMIZE_CYCLES: {
    res = true;
    break;
  }
  }
  return res;
}

bool checkIfInstGranCycleBasedCounter() {
  bool res = false;
  switch (InstGranularity) {
  case LEGACY_ACCURATE:
  case OPTIMIZE_CYCLES:
  case NAIVE_CYCLES: {
    res = true;
    break;
  }
  }
  return res;
}

bool checkIfInstGranIsIntermediate() {
  bool res = false;
  switch (InstGranularity) {
  case OPTIMIZE_INTERMEDIATE:
  case OPTIMIZE_HEURISTIC_INTERMEDIATE_FIBER:
  case NAIVE_INTERMEDIATE: {
    res = true;
    break;
  }
  }
  return res;
}

bool checkIfInstGranIsDet() {
  bool res = false;
  switch (InstGranularity) {
  case NAIVE_TL:
  case OPTIMIZE_HEURISTIC_WITH_TL:
  case OPTIMIZE_HEURISTIC_FIBER:
  case COREDET_HEURISTIC_TL:
  case LEGACY_HEURISTIC_TL:
  case NAIVE_HEURISTIC_FIBER: {
    res = true;
    break;
  }
  }
  return res;
}

/*********************************************** Section: Container class definition *********************************************/

/* A Compiler Interrupt container node */
class LCCNode {
  /* set & map is used to keep avoid duplicate entries, vector is used to keep order of insertion intact */
public:
  typedef enum LCCTypes {
    UNIT_LCC = 0,
    PATH_LCC,
    BRANCH_LCC,
    COMPLEX_BRANCH_LCC,
    LOOP_LCC,
    INVERTEDV_LCC,
    V_LCC,
    UNKNOWN_LCC
  } LCCTypes;
  typedef std::pair<LCCNode *, LCCNode *> lccEdge; /* first element is pred LCC or succ LCC, second element is connected LCC */

private:
  LCCTypes _lccType;
  int _lccID;
  LCCNode *_enclosingLCC = nullptr; /* Keeps only the closest enclosing LCC */
  /* Key = {pred}, Value = set of edge info that has pred as an endpoint. 
     * Each edge info contains the connected node in the container & the type of edge*/
  std::map<LCCNode *, std::set<LCCNode *>> predSet; /* set of connected (unitLCC) nodes per pred */
  std::map<LCCNode *, std::set<LCCNode *>> succSet; /* set of connected (unitLCC) nodes per succ */
  std::map<lccEdge, bool> predEdgeInfo;             /* type of edge (fence or not) - true if fence */
  std::map<lccEdge, bool> succEdgeInfo;             /* type of edge (fence or not) - true if fence */

public:
  /* ----------------- Constructor ---------------------*/

  LCCNode(LCCTypes type, int id) {
    _lccType = type;
    _lccID = id;
  }

  /* ----------------- Set routines ------------------- */

  void setParentLCC(LCCNode *enclosingLCC) {
    assert(enclosingLCC && "Parent LCC cannot be null");
    //errs() << "Current ID: " << getID() << ", enclosing ID: " << enclosingLCC->getID() << "\n";
    _enclosingLCC = enclosingLCC;
  }

  void addPredLCC(LCCNode *predLCC, bool hasFence, LCCNode *connectedLCC = nullptr) {
    if (!connectedLCC) {
      connectedLCC = this;
    }
    auto predIt = predSet.find(predLCC);
    if (predIt != predSet.end()) {
      auto connIt = predIt->second.find(connectedLCC);
      /* Insert if not present already */
      if (connIt == predIt->second.end()) {
        predIt->second.insert(connectedLCC);
      }
    } else {
      std::set<LCCNode *> connLCC;
      connLCC.insert(connectedLCC);
      predSet[predLCC] = connLCC;
    }

    lccEdge predEdge = std::make_pair(predLCC, connectedLCC);
    predEdgeInfo[predEdge] = hasFence;
  }

  void addSuccLCC(LCCNode *succLCC, bool hasFence, LCCNode *connectedLCC = nullptr) {
    if (!connectedLCC) {
      connectedLCC = this;
    }
    auto succIt = succSet.find(succLCC);
    if (succIt != succSet.end()) {
      auto connIt = succIt->second.find(connectedLCC);
      /* Insert if not present already */
      if (connIt == succIt->second.end()) {
        succIt->second.insert(connectedLCC);
      }
    } else {
      std::set<LCCNode *> connLCC;
      connLCC.insert(connectedLCC);
      succSet[succLCC] = connLCC;
    }

    lccEdge succEdge = std::make_pair(succLCC, connectedLCC);
    succEdgeInfo[succEdge] = hasFence;
  }

  void removePredLCC(LCCNode *predLCC) {
    auto predIt = predSet.find(predLCC);
    if (predIt != predSet.end()) {
      for (auto connIt : predIt->second) {
        lccEdge predEdge = std::make_pair(predLCC, connIt);
        auto predEdgeIt = predEdgeInfo.find(predEdge);
        if (predEdgeIt != predEdgeInfo.end()) {
          predEdgeInfo.erase(predEdgeIt);
        }
      }
      predSet.erase(predIt);
    }
  }

  void removeSuccLCC(LCCNode *succLCC) {
    auto succIt = succSet.find(succLCC);
    if (succIt != succSet.end()) {
      for (auto connIt : succIt->second) {
        lccEdge succEdge = std::make_pair(succLCC, connIt);
        auto succEdgeIt = succEdgeInfo.find(succEdge);
        if (succEdgeIt != succEdgeInfo.end()) {
          succEdgeInfo.erase(succEdgeIt);
        }
      }
      succSet.erase(succIt);
    }
  }

  void replacePred(LCCNode *oldPred, LCCNode *newPred) {
    auto predIt = predSet.find(oldPred);
    assert((predIt != predSet.end()) && "Predecessor to be replaced in not present");
    auto connSet = predIt->second;
    for (auto connIt = connSet.begin(); connIt != connSet.end(); connIt++) {
      lccEdge oldPredEdge = std::make_pair(oldPred, *connIt);
      auto predEdgeInfoIt = predEdgeInfo.find(oldPredEdge);
      assert((predEdgeInfoIt != predEdgeInfo.end()) && "The pair of old predecessor to be replaced & its connected node is not present");
      auto predHasFence = predEdgeInfoIt->second;
      addPredLCC(newPred, predHasFence, *connIt);

      /* Erase old edge */
      predEdgeInfo.erase(predEdgeInfoIt);
    }
    /* Erase old predecessor */
    predSet.erase(predIt);
  }

  void replacePred(LCCNode *oldPred, LCCNode *newPred, bool predHasFence) {
    auto predIt = predSet.find(oldPred);
    assert((predIt != predSet.end()) && "Predecessor to be replaced in not present");
    auto connSet = predIt->second;
    for (auto connIt = connSet.begin(); connIt != connSet.end(); connIt++) {
      lccEdge oldPredEdge = std::make_pair(oldPred, *connIt);
      auto predEdgeInfoIt = predEdgeInfo.find(oldPredEdge);
      assert((predEdgeInfoIt != predEdgeInfo.end()) && "The pair of old predecessor to be replaced & its connected node is not present");
      addPredLCC(newPred, predHasFence, *connIt);

      /* Erase old edge */
      predEdgeInfo.erase(predEdgeInfoIt);
    }
    /* Erase old predecessor */
    predSet.erase(predIt);
  }

  void replaceSucc(LCCNode *oldSucc, LCCNode *newSucc) {
    auto succIt = succSet.find(oldSucc);
    assert((succIt != succSet.end()) && "Successor to be replaced in not present");
    auto connSet = succIt->second;
    for (auto connIt = connSet.begin(); connIt != connSet.end(); connIt++) {
      lccEdge oldSuccEdge = std::make_pair(oldSucc, *connIt);
      auto succEdgeInfoIt = succEdgeInfo.find(oldSuccEdge);
      assert((succEdgeInfoIt != succEdgeInfo.end()) && "The pair of old successor to be replaced & its connected node is not present");
      auto succHasFence = succEdgeInfoIt->second;
      addSuccLCC(newSucc, succHasFence, *connIt);

      /* Erase old edge */
      succEdgeInfo.erase(succEdgeInfoIt);
    }
    /* Erase old successor */
    succSet.erase(succIt);
  }

  void replaceSucc(LCCNode *oldSucc, LCCNode *newSucc, bool succHasFence) {
    auto succIt = succSet.find(oldSucc);
    assert((succIt != succSet.end()) && "Successor to be replaced in not present");
    auto connSet = succIt->second;
    for (auto connIt = connSet.begin(); connIt != connSet.end(); connIt++) {
      lccEdge oldSuccEdge = std::make_pair(oldSucc, *connIt);
      auto succEdgeInfoIt = succEdgeInfo.find(oldSuccEdge);
      assert((succEdgeInfoIt != succEdgeInfo.end()) && "The pair of old successor to be replaced & its connected node is not present");
      addSuccLCC(newSucc, succHasFence, *connIt);

      /* Erase old edge */
      succEdgeInfo.erase(succEdgeInfoIt);
    }
    /* Erase old successor */
    succSet.erase(succIt);
  }

  /* Copy all predecessor info from predConnLCC to this. 
     * Also, replace all connections to the predecessor from predConnLCC to this. */
  void makeNewPredConnections(LCCNode *predConnLCC) {
    auto connPredSet = predConnLCC->getPredSet();
    auto connPredEdgeInfo = predConnLCC->getPredEdgeInfo();

    /* Copying child's connections to itself. If the predecessor already exists, append the successor'c connections only */
    for (auto predOfPredIt = connPredSet.begin(); predOfPredIt != connPredSet.end(); predOfPredIt++) {
      //errs() << "1. For each pred (" << predOfPredIt->first->getID() << ")\n";
      auto predIt = predSet.find(predOfPredIt->first);
      if (predIt == predSet.end()) {
        predSet.insert(*predOfPredIt);
      } else {
        /* if connection's predecessor already exists in its own predessor list */
        auto connList = predIt->second;
        connList.insert(predOfPredIt->second.begin(), predOfPredIt->second.end());
      }
    }
    //predSet.insert(connPredSet.begin(), connPredSet.end());

    predEdgeInfo.insert(connPredEdgeInfo.begin(), connPredEdgeInfo.end());
#if 0
    for (auto predEdgeIt = connPredEdgeInfo.begin(); predEdgeIt != connPredEdgeInfo.end(); predEdgeIt++) {
      lccEdge oldEdge = predEdgeIt->first;
      bool isFencePresent = predEdgeIt->second;
      /* replace connected lcc of original edge to predConnLCC */
      lccEdge newEdge = std::make_pair(oldEdge.first, predConnLCC);
      assert((predEdgeInfo.find(newEdge) == predEdgeInfo.end()) && "Same predecessor edge cannot already exist!");
      predEdgeInfo[newEdge] = isFencePresent;
    }
#endif

    /* Replacing predecessor's (successor) links from child container to itself */
    for (auto predIt = connPredSet.begin(); predIt != connPredSet.end(); predIt++) {
      //errs() << "2. For each pred (" << predIt->first->getID() << ")\n";
      (predIt->first)->replaceSucc(predConnLCC, this);
    }
  }

  /* Copy all successor info from succConnLCC to this. 
     * Also, replace all connections to the successor from succConnLCC to this. */
  void makeNewSuccConnections(LCCNode *succConnLCC) {
    auto connSuccSet = succConnLCC->getSuccSet();
    auto connSuccEdgeInfo = succConnLCC->getSuccEdgeInfo();

    /* Copying child's connections to itself. If the successor already exists, append the successor'c connections only */
    for (auto succOfSuccIt = connSuccSet.begin(); succOfSuccIt != connSuccSet.end(); succOfSuccIt++) {
      auto succIt = succSet.find(succOfSuccIt->first);
      if (succIt == succSet.end()) {
        succSet.insert(*succOfSuccIt);
      } else {
        /* if connection's successor already exists in its own successor list */
        auto connList = succIt->second;
        connList.insert(succOfSuccIt->second.begin(), succOfSuccIt->second.end());
      }
    }
    //succSet.insert(connSuccSet.begin(), connSuccSet.end());

    succEdgeInfo.insert(connSuccEdgeInfo.begin(), connSuccEdgeInfo.end());
#if 0
    for (auto succEdgeIt = connSuccEdgeInfo.begin(); succEdgeIt != connSuccEdgeInfo.end(); succEdgeIt++) {
      lccEdge oldEdge = succEdgeIt->first;
      bool isFencePresent = succEdgeIt->second;
      /* replace connected lcc of original edge to succConnLCC */
      lccEdge newEdge = std::make_pair(oldEdge.first, succConnLCC);
      assert((succEdgeInfo.find(newEdge) == succEdgeInfo.end()) && "Same successor edge cannot already exist!");
      succEdgeInfo[newEdge] = isFencePresent;
    }
#endif
    /* Replacing successor's (predecessor) links from child container to itself */
    for (auto succIt = connSuccSet.begin(); succIt != connSuccSet.end(); succIt++) {
      (succIt->first)->replacePred(succConnLCC, this);
    }
  }

  /* ----------------- Get routines ------------------- */

  int getID() { /* return the ID of this LCC */
    return _lccID;
  }

  bool isUnitLCC() { return (_lccType == UNIT_LCC); }

  LCCNode *getOuterMostEnclosingLCC() {
    if (!_enclosingLCC) {
      return this;
    } else {
      return _enclosingLCC->getOuterMostEnclosingLCC();
    }
  }

  std::map<LCCNode *, std::set<LCCNode *>> getPredSet() {
    return predSet;
  }

  std::map<lccEdge, bool> getPredEdgeInfo() {
    return predEdgeInfo;
  }

  std::map<LCCNode *, std::set<LCCNode *>> getSuccSet() {
    return succSet;
  }

  std::map<lccEdge, bool> getSuccEdgeInfo() {
    return succEdgeInfo;
  }

  int getNumOfPredLCC() {
    return predSet.size();
  }

  int getNumOfSuccLCC() {
    return succSet.size();
  }

  /* Returns the single connected LCC for this pred */
  LCCNode *getPredSingleConnLCC(LCCNode *predLCC) {
    /* if pred is present */
    if (predSet.count(predLCC)) {
      auto connSet = predSet[predLCC];
      if (connSet.size() == 1) {
        auto connIt = connSet.begin();
        return *connIt;
      }
    }
    return nullptr;
  }

  /* Returns the only predecessor */
  LCCNode *getSinglePred() {
    if (predSet.size() == 1) {
      auto predInfoIt = predSet.begin();
      return predInfoIt->first;
    }
    return nullptr;
  }

  /* Returns the only predecessor that also has only one edge with the current node */
  LCCNode *getSinglePredWithSingleConn() {
    if (predSet.size() == 1) {
      auto predInfoIt = predSet.begin();
      auto connSet = predInfoIt->second; /* get set of current LCC nodes attached to this predecessor */
      if (connSet.size() == 1) {
        return predInfoIt->first;
      }
    }
    return nullptr;
  }

  /* Single predecessors not separated by a fence */
  LCCNode *getSinglePredWOFence() {
    LCCNode *singlePred = getSinglePred();
    if (!singlePred) {
      return nullptr;
    }
    LCCNode *connNode = getPredSingleConnLCC(singlePred);
    if (!connNode) {
      return nullptr;
    }
    lccEdge singlePredEdge = std::make_pair(singlePred, connNode);
    assert((predEdgeInfo.size() == 1) && "Single predecessor with single connected link cannot have multiple edge entries");
    /* if there is no fence separating them */
    auto predEdgeIt = predEdgeInfo.find(singlePredEdge);
    if (predEdgeIt != predEdgeInfo.end()) {
      /* return when there is no fence in between */
      if (!predEdgeIt->second) {
        return singlePred;
      } else {
        return nullptr;
      }
    } else {
      assert("Predecessor edge entry is incorrect!");
    }
    return nullptr;
  }

  /* Returns the single connected LCC for this succ */
  LCCNode *getSuccSingleConnLCC(LCCNode *succLCC) {
    /* if succ is present */
    if (succSet.count(succLCC)) {
      auto connSet = succSet[succLCC];
      if (connSet.size() == 1) {
        auto connIt = connSet.begin();
        return *connIt;
      }
    }
    return nullptr;
  }

  /* Returns the only successor */
  LCCNode *getSingleSucc() {
    if (succSet.size() == 1) {
      auto succInfoIt = succSet.begin();
      return succInfoIt->first;
    }
    return nullptr;
  }

  /* Returns the only successor that also has only one edge with the current node */
  LCCNode *getSingleSuccWithSingleConn() {
    if (succSet.size() == 1) {
      auto succInfoIt = succSet.begin();
      auto connSet = succInfoIt->second; /* get set of current LCC nodes attached to this successor */
      if (connSet.size() == 1) {
        return succInfoIt->first;
      }
    }
    return nullptr;
  }

  /* Single successor connected by a single link & not separated by a fence */
  LCCNode *getSingleSuccWOFence() {
    LCCNode *singleSucc = getSingleSucc();
    if (!singleSucc) {
      return nullptr;
    }
    LCCNode *connNode = getSuccSingleConnLCC(singleSucc);
    if (!connNode) {
      return nullptr;
    }
    lccEdge singleSuccEdge = std::make_pair(singleSucc, connNode);
    assert((succEdgeInfo.size() == 1) && "Single successor with single connected link cannot have multiple edge entries");
    /* if there is no fence separating them */
    auto succEdgeIt = succEdgeInfo.find(singleSuccEdge);
    if (succEdgeIt != succEdgeInfo.end()) {
      /* return when there is no fence in between */
      if (!succEdgeIt->second) {
        return singleSucc;
      } else {
        return nullptr;
      }
    } else {
      assert("Successor edge entry is incorrect!");
    }
    return nullptr;
  }

  /* if the current LCC is the entry point of a two node path, & both the nodes are 
     * connected by a non-fence edge. Important: the successor node cannot be loop header, 
     * since it will have another incoming edge there, which is not allowed */
  LCCNode *getSingleSuccOfPath() {
    LCCNode *singleSucc = getSingleSuccWOFence();
    if (!singleSucc) {
      return nullptr;
    }
    LCCNode *predOfSingleSucc = singleSucc->getSinglePredWOFence();
    if (!predOfSingleSucc) {
      return nullptr;
    }
    assert((predOfSingleSucc == this) && "The only predecessor of the only successor of the \
        current LCC, must be the current LCC");
    return singleSucc;
  }

  /* ------------- Check functions  ----------------- */

  /* Return true if there is only one edge between the current container & successor, & if that edge is not a fence */
  bool isSimpleSuccEdge(LCCNode *succLCC, std::set<LCCNode *> connectedLCCs) {
    if (connectedLCCs.size() != 1) {
      return false;
    }
    auto connectedLCC = *(connectedLCCs.begin());
    lccEdge succEdge = std::make_pair(succLCC, connectedLCC);
    auto succEdgeIt = succEdgeInfo.find(succEdge);
    assert((succEdgeIt != succEdgeInfo.end()) && "isSimpleSuccEdge: edge not found!");
    if (succEdgeIt != succEdgeInfo.end()) {
      return !(succEdgeIt->second); /* returning true if there is no fence */
    }
    return false;
  }

  /* ------------- Virtual functions  ----------------- */

  virtual Function *getFunction() = 0; /* return the function of this LCC */
  virtual LCCNode *getInnerMostEntryLCC() = 0;
  virtual LCCNode *getOneInnerMostEntryLCC() = 0;
  virtual std::set<LCCNode *> getAllInnerMostEntryLCC() = 0;
  virtual LCCNode *getInnerMostExitLCC() = 0;
  virtual std::set<LCCNode *> getAllInnerMostExitLCC() = 0;
  virtual Loop *getLoop() = 0;
  virtual LCCTypes getType() = 0;
  virtual InstructionCost *getCostForPC(bool) = 0;                    /* Parameter specifies true when the container needs to be instrumented */
  virtual InstructionCost *getCostForIC(bool, InstructionCost *) = 0; /* Parameter specifies true when the container needs to be instrumented */
  virtual void instrumentForPC(InstructionCost *) = 0;
  virtual void instrumentForIC(InstructionCost *) = 0;
  virtual bool isInstrumented() = 0; /* return true if any of its child containers are instrumented */
};

class UnitLCC : public LCCNode {
  BasicBlock *_currentBlock;
  Instruction *_firstInst = nullptr;
  Instruction *_lastInst = nullptr;
  bool _isExitLCC = false;
  bool toBeInstrumented = false;                         /* This information needs to be stored, as the cost evaluation required for deciding the locations of instrumentation changes with instrumentation */
  std::map<Instruction *, InstructionCost *> _instrInfo; /* Only available if toBeInstrumented flag is true, specifies the set of instructions to be instrumented (before) & the costs to be instrumented */
  std::map<Instruction *, Value *> _instrValInfo;        /* Same as _instrInfo, except the cost is kept in the form of Value, mostly used for loop transform optimizations */
  /* Only for instantaneous clock */
  InstructionCost *_initialCost = nullptr; /* if this node is evaluated multiple times, it should be evaluated with the same initial cost */

public:
  /* ----------------- Constructor ---------------------*/

  UnitLCC(int id, BasicBlock *block, Instruction *firstInst, Instruction *lastInst, bool hasFence) : LCCNode(UNIT_LCC, id), _currentBlock(block), _firstInst(firstInst), _lastInst(lastInst), _isExitLCC(false) {
    assert(block && "Unit LCC information is incomplete. Abort.");

    if (firstInst && lastInst) {
      return;
    }

    /* empty containers only used for proper cfg creation */
    if (!firstInst && !lastInst) {
#ifdef LC_DEBUG
      errs() << "Creating an empty container for " << block->getName() << " in " << block->getParent()->getName() << "\n";
#endif
    } else {
      errs() << "Cannot have the first or last instruction of a container as null!";
      exit(1);
    }
  }

  UnitLCC(int id, BasicBlock *block, Instruction *firstInst, Instruction *lastInst, bool hasFence, bool isExiting) : LCCNode(UNIT_LCC, id), _currentBlock(block), _firstInst(firstInst), _lastInst(lastInst), _isExitLCC(isExiting) {
    assert(block && "Unit LCC information is incomplete. Abort.");
    assert(isExiting && "not an exiting lcc!\n");

    if (firstInst && lastInst) {
      return;
    }

    /* empty containers only used for proper cfg creation */
    if (!firstInst && !lastInst) {
#ifdef LC_DEBUG
      errs() << "Creating an exiting container that ends with unreachable instruction for " << block->getName() << " in " << block->getParent()->getName() << "\n";
#endif
    } else {
      errs() << "Cannot have the first or last instruction of a container as null!";
      exit(1);
    }
  }

  /* ----------------- Get routines ------------------- */

  bool isEmptyLCC() {
    if (!_firstInst || !_lastInst) {
      return true;
    }
    return false;
  }

  bool isExitBlockLCC() {
    return _isExitLCC;
  }

  Instruction *getFirstInst() {
    return _firstInst;
  }

  Instruction *getLastInst() {
    return _lastInst;
  }

  BasicBlock *getBlock() {
    return _currentBlock;
  }

  std::map<Instruction *, InstructionCost *> getInstrInfo() {
    return _instrInfo;
  }

  std::map<Instruction *, Value *> getInstrValInfo() {
    return _instrValInfo;
  }

  bool getInstrumentFlag() {
    return toBeInstrumented;
  }

  /* ----------------- Set routines ------------------- */

  void setFirstInst(Instruction *I) {
    _firstInst = I;
  }

  void setLastInst(Instruction *I) {
    _lastInst = I;
  }

  void setInstrInfo(Instruction *I, InstructionCost *cost) {
    toBeInstrumented = true;

    /* Sanity checks */
    assert(cost && "Instruction cost for instrumenting cannot be null!");
    assert(I && "Instruction for instrumenting cannot be null!");
    /* Not checking if I falls in the range withing first & last instruction for this UnitLCC to avoid increasing compiling time. Need to ensure this before calling. */

    /* Add the pair to the instrument list */
    InstructionCost *newCost = cost;
    if (_instrInfo.end() != _instrInfo.find(I)) {
      auto prevCost = _instrInfo[I];
      if (getConstCost(prevCost) != getConstCost(cost)) {
        errs() << "Instruction " << *I << " in basic block " << I->getParent()->getName() << " (" << I->getFunction()->getName() << "()) has a previous cost of " << *prevCost << ", and gets a new cost of " << *cost << "\n";
        //int numPrevCost = getConstCost(prevCost);
        //int numNewCost = getConstCost(cost);
        //newCost = getConstantInstCost(numPrevCost+numNewCost);
        exit(1);
      }
    }
    _instrInfo[I] = newCost;
  }

  void setInstrInfo(Instruction *I, Value *cost) {
    toBeInstrumented = true;

    /* Sanity checks */
    assert(cost && "Instruction cost for instrumenting cannot be null!");
    assert(I && "Instruction for instrumenting cannot be null!");
    /* Not checking if I falls in the range withing first & last instruction for this UnitLCC to avoid increasing compiling time. Need to ensure this before calling. */

    /* Add the pair to the instrument list */
    if (_instrValInfo.end() != _instrValInfo.find(I)) {
      errs() << "Having multiple value based instrumentation at same instruction is not supported!\n";
      exit(1);
    }
    _instrValInfo[I] = cost;
  }

  void printInstr() {
    errs() << "printInstr:- Block: " << getBlock()->getName() << "\n";
    for (auto it : _instrInfo) {
      errs() << "printInstr:- Instrumented Instruction: " << *(it.first) << ", Cost: " << *(it.second) << "\n";
    }
  }

  void replaceInst(Instruction *oldI, Instruction *newI) {
#ifdef LC_DEBUG
    errs() << "Replacing " << *oldI << " with " << *newI << "\n";
#endif
    if (_firstInst == oldI) {
      _firstInst = newI;
    }
    if (_lastInst == oldI) {
      _lastInst = newI;
    }

#if 0
    /* Testing */
    for (auto it : _instrInfo) {
      errs() << "replaceInst: to be instrumented " << *(it.first) << "\n";
    }
#endif

    auto instrInfoIt = _instrInfo.find(oldI);
    if (instrInfoIt != _instrInfo.end()) {
      _instrInfo[newI] = instrInfoIt->second;
      _instrInfo.erase(instrInfoIt);
    }
#ifdef LC_DEBUG
    errs() << "Block : " << getBlock()->getName() << ", OldI : " << *oldI << ", NewI : " << *newI << ", first inst : " << *_firstInst << ", last inst : " << *_lastInst << "\n";
#endif
  }

#if 1
  std::pair<int, InstructionCost *> getNRemoveOldInstr(Instruction *oldI) {
    int ret = 0; /* 0 - no match, 1 - first inst, 2 - second inst, 3 - both */
    InstructionCost *cost = nullptr;
    bool first = false;
    bool last = false;

    if (_firstInst == oldI) {
      first = true;
    }
    if (_lastInst == oldI) {
      last = true;
    }

    if (first && last) {
      ret = 3;
    } else if (first) {
      ret = 1;
    } else if (last) {
      ret = 2;
    }

    auto instrInfoIt = _instrInfo.find(oldI);
    if (instrInfoIt != _instrInfo.end()) {
      cost = instrInfoIt->second;
      _instrInfo.erase(instrInfoIt);
    }

    std::pair<int, InstructionCost *> retPair = std::make_pair(ret, cost);
    return retPair;
  }

  void addInstr(Instruction *newI, int config, InstructionCost *cost) {
    switch (config) {
    case 1:
      _firstInst = newI;
      break;
    case 2:
      _lastInst = newI;
      break;
    case 3:
      _firstInst = newI;
      _lastInst = newI;
      break;
    }

    if (cost) {
      _instrInfo[newI] = cost;
    }
  }
#endif

  /* -------- Implementation of virtual functions -------*/

  Function *getFunction() {
    return _currentBlock->getParent();
  }

  LCCNode *getInnerMostEntryLCC() {
    return this;
  }

  LCCNode *getOneInnerMostEntryLCC() {
    return this;
  }

  std::set<LCCNode *> getAllInnerMostEntryLCC() {
    std::set<LCCNode *> innerLCCs;
    innerLCCs.insert(this);
    return innerLCCs;
  }

  LCCNode *getInnerMostExitLCC() {
    return this;
  }

  std::set<LCCNode *> getAllInnerMostExitLCC() {
    std::set<LCCNode *> innerLCCs;
    innerLCCs.insert(this);
    return innerLCCs;
  }

  Loop *getLoop() {
    return nullptr; /* not a loop container */
  }

  LCCTypes getType() {
    return UNIT_LCC;
  }

  /* for manageDanglingLCCs */
  void setInitialCost(InstructionCost *cost) {
    _initialCost = cost;
  }

  /* for manageDanglingLCCs */
  InstructionCost *getInitialCost() {
    return _initialCost;
  }

  InstructionCost *getCostForPC(bool toInstrument) {
    InstructionCost::opvector totalInstCost;
    BasicBlock::iterator instItr(_firstInst);
    do {
      auto instCost = getInstCostForPC(&*instItr);
      totalInstCost.push_back(instCost);
      if ((&*instItr) == _lastInst) {
        break;
      }
      instItr++;
    } while (true);
    InstructionCost *ic = new InstructionCost(InstructionCost::ADD, totalInstCost);
    InstructionCost *simplifiedCost = simplifyCost(_currentBlock->getParent(), ic);
    if (!simplifiedCost) {
      errs() << "cost that could not be simplified: " << *ic << "\n";
    }
    assert(simplifiedCost && "Basic Block's simplified cost cannot be null!");
    if (toInstrument) {
      instrumentForPC(simplifiedCost);
      /* Since it has been instrumented, cost is now 0 */
      simplifiedCost = getConstantInstCost(0);
    }
#ifdef ALL_DEBUG
    errs() << "Unit LCC id: " << getID() << " --> final cost: " << *simplifiedCost << "\n\n";
#endif
    return simplifiedCost;
  }

  InstructionCost *getCostForIC(bool toInstrument, InstructionCost *initialCost) {

    /* Sanity checks */
    long initialNumCost = getConstCost(initialCost);
#ifdef LC_DEBUG
    errs() << "Unit LCC id: " << getID() << " --> initial cost: " << initialNumCost << "\n";
#endif

    assert((initialNumCost != -1) && "Initial cost cannot be unknown!");
    assert((initialNumCost <= CommitInterval) && "Initial cost cannot be greater than the commit cost interval!");

    if (_initialCost) {
      long storedInitCost = getConstCost(_initialCost);
      /* either the initial cost came from previous cost evaluations without instrumentation, or it came from manageDanglingLCCs. It cannot be both. */
      if ((initialNumCost > 0) && (storedInitCost > 0) && (initialNumCost != storedInitCost)) {
        errs() << "Stored cost: " << storedInitCost << ", Current cost: " << initialNumCost << "\n";
      }
      assert(((initialNumCost <= 0) || (storedInitCost <= 0) || (initialNumCost == storedInitCost)) && "Initial cost cannot be different than the one used for last evaluation");
      if (storedInitCost > 0) {
        initialNumCost = storedInitCost;
      }
    } else {
      _initialCost = initialCost;
    }

    long totalNumCost = initialNumCost;
    /* Cost evaluation & instrumentation */
    BasicBlock::iterator instItr(_firstInst);
    do {
      InstructionCost *instCost = getInstCostForIC(&*instItr);
      long numInstCost = getConstCost(instCost);

#if 0
      if (InstGranularity == OPTIMIZE_ACCURATE) {
        if (checkIfExternalLibraryCall(&*instItr)) {
          instItr++;
          continue;
        }
      }
#endif

      /* if this is the last instruction, commit sum cost */
      if ((&*instItr) == _lastInst) {
        totalNumCost += numInstCost;
        if (toInstrument || (totalNumCost > CommitInterval)) {
          auto totalCost = getConstantInstCost(totalNumCost);
          instrumentForIC(totalCost);
          totalNumCost = 0;
        }
        break;
      }
#if 0
      else if (checkIfExternalLibraryCall(&*instItr)) {
        totalNumCost += numInstCost;
        if (totalNumCost > 1) { /* since minimum call cost is 1 */
          InstructionCost *prevCost = getConstantInstCost(totalNumCost);
          errs() << "Instrumenting library function call: " << *instItr << " with cost " << totalNumCost << "\n";
          setInstrInfo(&*instItr, prevCost);
          totalNumCost = 0;
        }
      }
#endif
      /* Instrument if the cost exceeded commit interval. Else keep evaluating. */
      else {
        if ((totalNumCost + numInstCost) > CommitInterval) {
          InstructionCost *prevCost = getConstantInstCost(totalNumCost);
          setInstrInfo(&*instItr, prevCost);
          totalNumCost = numInstCost;
        } else {
          totalNumCost += numInstCost;
        }
      }

      instItr++;
    } while (true);

    InstructionCost *cost = getConstantInstCost(totalNumCost);
#ifdef LC_DEBUG
    errs() << "Unit LCC id: " << getID() << " --> final cost: " << *cost << "\n\n";
#endif

    return cost;
  }

  /* For predictive, cost is instrumented at the top */
  void instrumentForPC(InstructionCost *cost) {
#ifndef EAGER_OPT
    Instruction *nextNonPhiInst = checkForPhi(_firstInst);
    setInstrInfo(nextNonPhiInst, cost);
#else
    setInstrInfo(_lastInst, cost);

#endif
  }

  /* For instantaneous, cost is instrumented at the bottom */
  void instrumentForIC(InstructionCost *cost) {
    long numCost = hasConstCost(cost);
    assert((numCost >= 0) && "Non-numeric cost cannot be instrumented!");
    if (numCost != 0) {
      setInstrInfo(_lastInst, cost);
    }
  }

  /* For instantaneous, cost is instrumented at the bottom. This special function is used for instrumenting the inner loops of unrolled loops */
  void instrumentValueForIC(Value *val) {
    assert(val && "Non-numeric cost cannot be instrumented!");
    setInstrInfo(_lastInst, val);
  }

  /* return true if any of its child containers are instrumented */
  bool isInstrumented() {
    return toBeInstrumented;
  }
};

void getSingleLCCRep(LCCNode *currLCC) {
  auto innerLCCs = currLCC->getAllInnerMostEntryLCC();
  auto innerLCCIt = innerLCCs.begin();
  auto innerLCC = static_cast<UnitLCC *>(*innerLCCIt);
  errs() << innerLCC->getBlock()->getName();
  innerLCCIt++;
  for (; innerLCCIt != innerLCCs.end(); innerLCCIt++) {
    auto innerLCC = static_cast<UnitLCC *>(*innerLCCIt);
    errs() << ", " << innerLCC->getBlock()->getName();
  }
}

void printUnitLCCSet(LCCNode *currLCC) {
  auto innerLCCs = currLCC->getAllInnerMostEntryLCC();
  auto innerLCCIt = innerLCCs.begin();
  auto innerLCC = static_cast<UnitLCC *>(*innerLCCIt);
  //errs() << innerLCC->getBlock()->getName();
  errs() << innerLCC->getBlock()->getName() << " (" << innerLCC->getID() << ")";
  innerLCCIt++;
  for (; innerLCCIt != innerLCCs.end(); innerLCCIt++) {
    auto innerLCC = static_cast<UnitLCC *>(*innerLCCIt);
    //errs() << ", " << innerLCC->getBlock()->getName();
    errs() << ", " << innerLCC->getBlock()->getName() << " (" << innerLCC->getID() << ")";
  }
}

class PathLCC : public LCCNode {
  LCCNode *_entryLCC;
  LCCNode *_exitLCC;

public:
  /* ----------------- Constructor ---------------------*/

  PathLCC(int id, LCCNode *entryLCC, LCCNode *exitLCC) : LCCNode(PATH_LCC, id), _entryLCC(entryLCC), _exitLCC(exitLCC) {

    assert(entryLCC && exitLCC && "entry or exit LCCs cannot be null for Path Container");
    _entryLCC->setParentLCC(this);
    _exitLCC->setParentLCC(this);
  }

  /* -------- Implementation of virtual functions -------*/

  Function *getFunction() {
    return _entryLCC->getFunction();
  }

  LCCNode *getInnerMostEntryLCC() {
    return _entryLCC->getInnerMostEntryLCC();
  }

  LCCNode *getOneInnerMostEntryLCC() {
    return _entryLCC->getOneInnerMostEntryLCC();
  }

  std::set<LCCNode *> getAllInnerMostEntryLCC() {
    auto innerLCCs = _entryLCC->getAllInnerMostEntryLCC();
    return innerLCCs;
  }

  LCCNode *getInnerMostExitLCC() {
    return _exitLCC->getInnerMostExitLCC();
  }

  std::set<LCCNode *> getAllInnerMostExitLCC() {
    auto innerLCCs = _exitLCC->getAllInnerMostExitLCC();
    return innerLCCs;
  }

  Loop *getLoop() {
    return nullptr; /* not a loop container */
  }

  LCCTypes getType() {
    return PATH_LCC;
  }

  InstructionCost *getCostForPC(bool toInstrument) {
    InstructionCost::opvector costs;
    auto entryLCCCost = _entryLCC->getCostForPC(false);
    auto exitLCCCost = _exitLCC->getCostForPC(false);
    if (entryLCCCost) {
      costs.push_back(entryLCCCost);
    }
    if (exitLCCCost) {
      costs.push_back(exitLCCCost);
    }
    InstructionCost *newCost = new InstructionCost(InstructionCost::ADD, costs);
    InstructionCost *simplifiedNewCost = simplifyCost(getFunction(), newCost);
    if (!simplifiedNewCost) {
      errs() << "Cost that could not be simplified : " << *newCost << "\n";
    }
    assert(simplifiedNewCost && "Simplified path cost cannot be null!");
    if (toInstrument) {
      instrumentForPC(simplifiedNewCost);
    }

    /*********************** Update statistics ***********************/
    applyrule1++;

    return simplifiedNewCost;
  }

  InstructionCost *getCostForIC(bool toInstrument, InstructionCost *initialCost) {

    /* Sanity checks */
    long initialNumCost = getConstCost(initialCost);
#ifdef LC_DEBUG
    errs() << "Path LCC id: " << getID() << " --> initial cost: " << initialNumCost << "\n";
#endif

    assert((initialNumCost != -1) && "Initial cost cannot be unknown!");
    assert((initialNumCost <= CommitInterval) && "Initial cost cannot be greater than the commit cost interval!");

    auto entryLCCCost = _entryLCC->getCostForIC(false, initialCost);
    auto exitLCCCost = _exitLCC->getCostForIC(false, entryLCCCost);
    long remCost = getConstCost(exitLCCCost);

    if (toInstrument || (remCost > CommitInterval)) {
      instrumentForIC(exitLCCCost);
      exitLCCCost = getConstantInstCost(0);
    }

#ifdef LC_DEBUG
    errs() << "Path LCC id: " << getID() << " --> final cost: " << *exitLCCCost << "\n";
#endif

    /*********************** Update statistics ***********************/
    applyrule1++;

    return exitLCCCost;
  }

  void instrumentForPC(InstructionCost *cost) {
    _entryLCC->instrumentForPC(cost);
  }

  void instrumentForIC(InstructionCost *cost) {
    _exitLCC->instrumentForIC(cost);
  }

  /* return true if any of its child containers are instrumented */
  bool isInstrumented() {
    return (_entryLCC->isInstrumented() || _exitLCC->isInstrumented());
  }
};

class BranchLCC : public LCCNode {
  LCCNode *_entryLCC;
  LCCNode *_exitLCC;
  std::map<LCCNode *, double> _branchLCCInfo;
  bool _hasDirectEdge;
  double _directBranchProb;
  BasicBlock *_domBlock;
  BasicBlock *_postdomBlock;

  //BasicBlock *postDom; // Kept for new rule to match smallest SESE region
public:
  /* ----------------- Constructor ---------------------*/

  BranchLCC(int id, LCCNode *entryLCC, LCCNode *exitLCC, std::map<LCCNode *, double> branchLCCInfo, bool hasDirectEdge, double directBranchProb, BasicBlock *domBlock, BasicBlock *postdomBlock, bool hasFence) : LCCNode(LCCNode::BRANCH_LCC, id), _entryLCC(entryLCC), _exitLCC(exitLCC), _branchLCCInfo(branchLCCInfo), _hasDirectEdge(hasDirectEdge), _directBranchProb(directBranchProb), _domBlock(domBlock), _postdomBlock(postdomBlock) {
    assert(entryLCC && exitLCC && "entry or exit LCCs cannot be null for a Branch Container");
    assert(!branchLCCInfo.empty() && "there should be at least one concrete branch for Branch Container");
    _domBlock = domBlock;
    _postdomBlock = postdomBlock;
    _entryLCC->setParentLCC(this);
    _exitLCC->setParentLCC(this);
    for (auto midLCCIt = _branchLCCInfo.begin(); midLCCIt != _branchLCCInfo.end(); midLCCIt++) {
      auto midLCC = midLCCIt->first;
      midLCC->setParentLCC(this);
    }
  }

  /* -------- Implementation of virtual functions -------*/

  Function *getFunction() {
    return _entryLCC->getFunction();
  }

  LCCNode *getInnerMostEntryLCC() {
    return _entryLCC->getInnerMostEntryLCC();
  }

  LCCNode *getOneInnerMostEntryLCC() {
    return _entryLCC->getOneInnerMostEntryLCC();
  }

  std::set<LCCNode *> getAllInnerMostEntryLCC() {
    auto innerLCCs = _entryLCC->getAllInnerMostEntryLCC();
    return innerLCCs;
  }

  LCCNode *getInnerMostExitLCC() {
    return _exitLCC->getInnerMostExitLCC();
  }

  std::set<LCCNode *> getAllInnerMostExitLCC() {
    auto innerLCCs = _exitLCC->getAllInnerMostExitLCC();
    return innerLCCs;
  }

  Loop *getLoop() {
    return nullptr; /* not a loop container */
  }

  LCCTypes getType() {
    return BRANCH_LCC;
  }

  InstructionCost *getCostForPC(bool toInstrument) {

    InstructionCost::opvector costs;
    std::map<LCCNode *, InstructionCost *> branchToCostMap;
    bool instrumentBranch = false;
    int numNonDirectEdges = 0;
    auto entryLCCCost = _entryLCC->getCostForPC(false);
    auto exitLCCCost = _exitLCC->getCostForPC(false);
    if (entryLCCCost) {
      costs.push_back(entryLCCCost);
    }
    if (exitLCCCost) {
      costs.push_back(exitLCCCost);
    }

    long avgBranchCost = 0, maxCost = 0, minCost = 0;
    long double avgFloatingBranchCost = 0;

    bool first = true;
    for (auto branchInfo : _branchLCCInfo) {
      LCCNode *branchLCC = branchInfo.first;
      double branchProb = branchInfo.second;
      InstructionCost *branchCost = branchLCC->getCostForPC(false);
      long numBranchCost = hasConstCost(branchCost);
      numNonDirectEdges++;
      if (numBranchCost == -1) {
        instrumentBranch = true;
      } else {
        if (first) {
          maxCost = numBranchCost;
          minCost = numBranchCost;
          first = false;
        } else {
          if (numBranchCost > maxCost) {
            maxCost = numBranchCost;
          }
          if (numBranchCost < minCost) {
            minCost = numBranchCost;
          }
        }
        long double weightedBranchCost = branchProb * numBranchCost;
        avgFloatingBranchCost += weightedBranchCost;
      }
      branchToCostMap[branchLCC] = branchCost;
    }
    avgBranchCost = (long)avgFloatingBranchCost;

    if (_hasDirectEdge) {
      minCost = 0;
    }

    long diffCost = maxCost - minCost;
    if (diffCost > ALLOWED_DEVIATION) {
#ifdef LC_DEBUG
      if (getFunction()->getName().compare("CSHIFT") == 0) {
        errs() << "Diff cost that is greater than allowed dev: " << diffCost << "\n";
      }
#endif
      instrumentBranch = true;
    }

    if (instrumentBranch) {
      /* Only dom & post dom cost are returned, every branch is instrumented */
      for (auto branchInfo : _branchLCCInfo) {
        LCCNode *branchLCC = branchInfo.first;
        auto mapIt = branchToCostMap.find(branchLCC);
        assert((mapIt != branchToCostMap.end()) && "Branch PC cost not found!");
        InstructionCost *branchCost = mapIt->second;
        branchLCC->instrumentForPC(branchCost);
      }
    } else {
      auto avgBranchLCCCost = getConstantInstCost(avgBranchCost);
      if (avgBranchLCCCost) {
        costs.push_back(avgBranchLCCCost);
      }

      /*********************** Update statistics ***********************/
      applyrule2++;
      rule2savedInst += numNonDirectEdges; /* For the child containers that are not the post dominator */
      rule2savedInst++;                    /* For the post dominator container */
    }

    InstructionCost *newCost = new InstructionCost(InstructionCost::ADD, costs);
    InstructionCost *simplifiedNewCost = simplifyCost(getFunction(), newCost);
    if (!simplifiedNewCost) {
      errs() << "Cost that could not be simplified : " << *newCost << "\n";
    }
    assert(simplifiedNewCost && "Simplified branch cost cannot be null!");
    if (toInstrument) {
      instrumentForPC(simplifiedNewCost);
    }
    return simplifiedNewCost;
  }

  InstructionCost *getCostForIC(bool toInstrument, InstructionCost *initialCost) {

    /* Sanity checks */
    long initialNumCost = getConstCost(initialCost);
    int numNonDirectEdges = 0;
#ifdef LC_DEBUG
    errs() << "Branch LCC id: " << getID() << " --> initial cost: " << initialNumCost << "\n";
#endif

    assert((initialNumCost != -1) && "Initial cost cannot be unknown!");
    assert((initialNumCost <= CommitInterval) && "Initial cost cannot be greater than the commit cost interval!");

    bool instrumentBranch = false;
    long avgBranchCost = 0, maxCost = 0, minCost = 0;
    long double avgFloatingBranchCost = 0;
    std::map<LCCNode *, InstructionCost *> branchToCostMap;

    auto entryLCCCost = _entryLCC->getCostForIC(false, initialCost);
#if 0
    /* if there is a direct edge, then the entry cost to branches must be made 0. 
       * Otherwise, the branches might over commit costs, which will again be instrumented
       * in the direct branch */
    if (_hasDirectEdge) {
      _entryLCC->instrumentForIC(entryLCCCost);
      entryLCCCost = getConstantInstCost(0);
    }
#endif

    bool first = true;
    for (auto branchInfo : _branchLCCInfo) {
      LCCNode *branchLCC = branchInfo.first;
      double branchProb = branchInfo.second;
      InstructionCost *branchCost = branchLCC->getCostForIC(false, entryLCCCost);
      long numBranchCost = getConstCost(branchCost);
      long double weightedBranchCost = branchProb * numBranchCost;
      numNonDirectEdges++;
      avgFloatingBranchCost += weightedBranchCost;
      if (first) {
        maxCost = numBranchCost;
        minCost = numBranchCost;
        first = false;
      } else {
        if (numBranchCost > maxCost) {
          maxCost = numBranchCost;
        }
        if (numBranchCost < minCost) {
          minCost = numBranchCost;
        }
      }
      branchToCostMap[branchLCC] = branchCost;
    }

    long numEntryLCCCost = getConstCost(entryLCCCost);
    if (_hasDirectEdge) {
      if (minCost > numEntryLCCCost) {
        minCost = numEntryLCCCost;
      }
      if (maxCost < numEntryLCCCost) {
        maxCost = numEntryLCCCost;
      }
      long double weightedBranchCost = _directBranchProb * numEntryLCCCost;
      avgFloatingBranchCost += weightedBranchCost;
    }
    avgBranchCost = (long)avgFloatingBranchCost;

    long diffCost = maxCost - minCost;
    if (diffCost > ALLOWED_DEVIATION) {
      instrumentBranch = true;
    }
#ifdef CRNT_DEBUG
    errs() << "Max: " << maxCost << ", Min: " << minCost << ", Diff: " << diffCost << ", Avg: " << avgBranchCost << ", to be instrumented: " << instrumentBranch << "\n";
#endif

    if (instrumentBranch) {
      /* Only dom & post dom cost are returned, every branch is instrumented */
      for (auto branchInfo : _branchLCCInfo) {
        LCCNode *branchLCC = branchInfo.first;
        auto mapIt = branchToCostMap.find(branchLCC);
        assert((mapIt != branchToCostMap.end()) && "Branch IC cost not found!");
        InstructionCost *branchCost = mapIt->second;
        branchLCC->instrumentForIC(branchCost);
      }
      /* Since all branches need to be instrumented, the direct branch needs to be instrumented with a block & cost */
      if (_hasDirectEdge) {
        if (directBranch.find(_domBlock) == directBranch.end()) {
          long numDirectBranchCost = numEntryLCCCost + 1; /* 1 for the new branch instruction created */
          directBranch[_domBlock] = getConstantInstCost(numDirectBranchCost);
          errs() << "Direct branch from " << _domBlock->getName() << " needs to be instrumented\n";
        }
      }
      avgBranchCost = 0;
    } else {
      /*********************** Update statistics ***********************/
      applyrule2++;
      rule2savedInst += numNonDirectEdges; /* For the child containers that are not the post dominator */
      rule2savedInst++;                    /* For the post dominator container */
    }

    auto avgBranchLCCCost = getConstantInstCost(avgBranchCost);
#ifdef ALL_DEBUG
    errs() << "Avg branch cost: " << *avgBranchLCCCost << "\n";
#endif
    auto exitLCCCost = _exitLCC->getCostForIC(false, avgBranchLCCCost);
    long remCost = getConstCost(exitLCCCost);

    if (toInstrument || (remCost > CommitInterval)) {
      instrumentForIC(exitLCCCost);
      exitLCCCost = getConstantInstCost(0);
    }

#ifdef LC_DEBUG
    errs() << "Branch LCC id: " << getID() << " --> initial cost: " << initialNumCost << "\n";
    errs() << "Branch LCC id: " << getID() << " --> final cost: " << *exitLCCCost << "\n";
    errs() << "Exit cost " << getConstCost(exitLCCCost) << "\n";
#endif
    return exitLCCCost;
  }

  void instrumentForPC(InstructionCost *cost) {
    _entryLCC->instrumentForPC(cost);
  }

  void instrumentForIC(InstructionCost *cost) {
    _exitLCC->instrumentForIC(cost);
  }

  /* return true if any of its child containers are instrumented */
  bool isInstrumented() {
    bool ret;
    ret = _entryLCC->isInstrumented() || _exitLCC->isInstrumented();
    for (auto branchInfo : _branchLCCInfo) {
      LCCNode *branchLCC = branchInfo.first;
      ret = ret || branchLCC->isInstrumented();
    }
    return ret;
  }
};

class ComplexBranchLCC : public LCCNode {

  LCCNode *_entryLCC;
  LCCNode *_exitLCC;
  std::map<std::list<LCCNode *> *, double> _branchPathLCCInfo; // The paths may have overlap. It should always include the entry & exit of the branch in every path
  std::list<LCCNode *> _innerLCCs;                             // used when every node of every path has to be instrumented, must have unique elements
  BasicBlock *_domBlock;
  BasicBlock *_postdomBlock;

  //BasicBlock *postDom; // Kept for new rule to match smallest SESE region
public:
  /* ----------------- Constructor ---------------------*/
  ComplexBranchLCC(int id, LCCNode *entryLCC, LCCNode *exitLCC, std::map<std::list<LCCNode *> *, double> branchPathLCCInfo, std::list<LCCNode *> innerLCCs, BasicBlock *domBlock, BasicBlock *postdomBlock) : LCCNode(LCCNode::COMPLEX_BRANCH_LCC, id), _entryLCC(entryLCC), _exitLCC(exitLCC), _branchPathLCCInfo(branchPathLCCInfo), _innerLCCs(innerLCCs), _domBlock(domBlock), _postdomBlock(postdomBlock) {
    assert(entryLCC && exitLCC && "entry or exit LCCs cannot be null for a Branch Container");
    assert(!branchPathLCCInfo.empty() && "there should be at least one concrete branch for Branch Container");
    _domBlock = domBlock;
    _postdomBlock = postdomBlock;
    _innerLCCs.unique();
    _entryLCC->setParentLCC(this);
    _exitLCC->setParentLCC(this);
    for (auto pathLCCIt = _branchPathLCCInfo.begin(); pathLCCIt != _branchPathLCCInfo.end(); pathLCCIt++) {
      auto pathLCCList = pathLCCIt->first;
      for (auto midLCCIt = pathLCCList->begin(); midLCCIt != pathLCCList->end(); midLCCIt++) {
        (*midLCCIt)->setParentLCC(this);
      }
    }
  }

  /* -------- Implementation of virtual functions -------*/

  Function *getFunction() {
    return _entryLCC->getFunction();
  }

  LCCNode *getInnerMostEntryLCC() {
    return _entryLCC->getInnerMostEntryLCC();
  }

  LCCNode *getOneInnerMostEntryLCC() {
    return _entryLCC->getOneInnerMostEntryLCC();
  }

  std::set<LCCNode *> getAllInnerMostEntryLCC() {
    auto innerLCCs = _entryLCC->getAllInnerMostEntryLCC();
    return innerLCCs;
  }

  LCCNode *getInnerMostExitLCC() {
    return _exitLCC->getInnerMostExitLCC();
  }

  std::set<LCCNode *> getAllInnerMostExitLCC() {
    auto innerLCCs = _exitLCC->getAllInnerMostExitLCC();
    return innerLCCs;
  }

  Loop *getLoop() {
    return nullptr; /* not a loop container */
  }

  LCCTypes getType() {
    return COMPLEX_BRANCH_LCC;
  }

  InstructionCost *getCostForPC(bool toInstrument) {

    errs() << "Not implemented for Complex branch. Exiting.";
    exit(1);
    return getConstantInstCost(0); /* Initial cost for any top level container is 0 */
  }

  InstructionCost *getCostForIC(bool toInstrument, InstructionCost *initialCost) {

    /* Sanity checks */
    long initialNumCost = getConstCost(initialCost);
#ifdef LC_DEBUG
    errs() << "Complex Branch LCC id: " << getID() << " --> initial cost: " << initialNumCost << "\n";
#endif

    assert((initialNumCost != -1) && "Initial cost cannot be unknown!");
    assert((initialNumCost <= CommitInterval) && "Initial cost cannot be greater than the commit cost interval!");

    bool instrumentBranch = false;
    long avgBranchCost = 0, maxCost = 0, minCost = 0;
    long double avgFloatingBranchCost = 0;
    InstructionCost *nodeEntryZeroCost = getConstantInstCost(0);
    std::map<LCCNode *, InstructionCost *> branchToCostMap;

    InstructionCost *entryCost = _entryLCC->getCostForIC(false, nodeEntryZeroCost);
    InstructionCost *exitCost = _exitLCC->getCostForIC(false, nodeEntryZeroCost);
    auto numEntryCost = getConstCost(entryCost);
    auto numExitCost = getConstCost(exitCost);
    //errs() << "For LCC (" << getID() << ") := Entry cost: " << numEntryCost << ", Exit cost: " << numExitCost << "\n";

    /* For each path */
    bool first = true;
    for (auto branchPathInfo : _branchPathLCCInfo) {
      /* entry & exit LCC will be evaluated as part of each path. Every node will be instrumented with zero cost because of overlapping paths */
      auto branchPathList = branchPathInfo.first;
      double branchProb = branchPathInfo.second;
      InstructionCost *nodeCost = nullptr;
      long numTotalPathCost = (numEntryCost + numExitCost);

      /* For each node of the path */
      for (auto branchLCC : *branchPathList) {
        if (branchLCC == _entryLCC || branchLCC == _exitLCC) {
          //errs() << "Entry & exit cost has already been checked. Entry cost: " << numEntryCost << ", Exit cost: " << numExitCost << "\n";
          continue;
        }
        nodeCost = branchLCC->getCostForIC(false, nodeEntryZeroCost);
        branchToCostMap[branchLCC] = nodeCost;
        long numNodeCost = getConstCost(nodeCost);
        numTotalPathCost += numNodeCost;
        //errs() << "For LCC (" << getID() << ") := Entry cost: " << numEntryCost << ", Exit cost: " << numExitCost << ", Branch cost: " << numNodeCost << "\n";
      }

      if (first) {
        maxCost = numTotalPathCost;
        minCost = numTotalPathCost;
        first = false;
      } else {
        if (numTotalPathCost > maxCost) {
          maxCost = numTotalPathCost;
        }
        if (numTotalPathCost < minCost) {
          minCost = numTotalPathCost;
        }
      }

      long double weightedBranchCost = branchProb * numTotalPathCost;
      avgFloatingBranchCost += weightedBranchCost;
    }

    avgBranchCost = (long)avgFloatingBranchCost;

    long diffCost = maxCost - minCost;
    if (diffCost > ALLOWED_DEVIATION) {
      instrumentBranch = true;
    }
#ifdef CRNT_DEBUG
    errs() << "Max: " << maxCost << ", Min: " << minCost << ", Diff: " << diffCost << ", Avg: " << avgBranchCost << ", to be instrumented: " << instrumentBranch << "\n";
#endif

    InstructionCost *exitLCCCost;
    if (instrumentBranch) {
      /* Only dom & post dom cost are returned, every branch is instrumented */
      for (auto branchLCC : _innerLCCs) {
        auto mapIt = branchToCostMap.find(branchLCC);
        assert((mapIt != branchToCostMap.end()) && "Complex Branch inner IC cost not found!");
        InstructionCost *branchCost = mapIt->second;
        branchLCC->instrumentForIC(branchCost);
      }

      exitLCCCost = exitCost;
      auto entryInstCost = getConstantInstCost(initialNumCost + numEntryCost);
      _entryLCC->instrumentForIC(entryInstCost);
      _exitLCC->instrumentForIC(exitLCCCost);
      exitLCCCost = getConstantInstCost(0);
    } else {
      /*********************** Update statistics ***********************/
      avgBranchCost += initialNumCost; // add the initial cost to all paths average
      applyrule7++;
      rule7savedInst += _innerLCCs.size() + 1; /* For the child containers that are not the post dominator & +1 for the entry LCC */

      exitLCCCost = getConstantInstCost(avgBranchCost);
#if 0
      auto entryBlock = static_cast<UnitLCC *>(_entryLCC)->getBlock();
      if (entryBlock) {
        errs() << "Average cost for the complex branch " << entryBlock->getName() << "(Function: " << entryBlock->getParent()->getName() << ", id: " << getID() << ") with cost " << avgBranchCost << "\n";
        if (getID() >= 883 && getID() <= 886) {
          auto exitBlock = static_cast<UnitLCC *>(_exitLCC)->getBlock();
          errs() << "PostDom: " << exitBlock->getName() << "\n";
        }
      }
#endif
      if (toInstrument || (avgBranchCost > CommitInterval)) {
#ifdef LC_DEBUG
        errs() << "Instrumenting complex branch exit block with " << *exitLCCCost << "\n";
#endif
        _exitLCC->instrumentForIC(exitLCCCost);
        exitLCCCost = getConstantInstCost(0);
      }
    }
#if 0
    auto exitLCCCost = _exitLCC->getCostForIC(false, avgBranchLCCCost);

    long remCost = getConstCost(exitLCCCost);

    if (toInstrument || (remCost > CommitInterval)) {
      instrumentForIC(exitLCCCost);
      exitLCCCost = getConstantInstCost(0);
    }
#endif

#ifdef LC_DEBUG
    errs() << "Complex branch LCC id: " << getID() << " --> initial cost: " << initialNumCost << "\n";
    errs() << "Complex branch LCC id: " << getID() << " --> final cost: " << *exitLCCCost << "\n";
    errs() << "Avg complex branch cost: " << *exitLCCCost << "\n";
#endif
    return exitLCCCost;
  }

  void instrumentForPC(InstructionCost *cost) {
    /* Not implemented
      _entryLCC->instrumentForPC(cost);
      */
  }

  void instrumentForIC(InstructionCost *cost) {
    _exitLCC->instrumentForIC(cost);
  }

  /* return true if any of its child containers are instrumented */
  bool isInstrumented() {
    bool ret;
    ret = _entryLCC->isInstrumented() || _exitLCC->isInstrumented();
    for (auto innerLCC : _innerLCCs) {
      ret = ret || innerLCC->isInstrumented();
    }
    return ret;
  }
};

class LoopLCC : public LCCNode {
  LCCNode *_headerLCC;
  LCCNode *_bodyLCC; /* only with header-non-colocated-exit */
  LCCNode *_preHeaderLCC;
  LCCNode *_postExitLCC;
  Loop *_loop;
  InstructionCost *_backEdges = nullptr;
  int _loopType;
  bool _hasHeaderWithExit; /* can be a self loop too, if true */

  //BasicBlock *postDom; // Kept for new rule to match smallest SESE region
public:
  /* ----------------- Constructor ---------------------*/

  enum LoopTypes {
    SELF_LOOP = 0,
    HEADER_COLOCATED_EXIT,
    HEADER_NONCOLOCATED_EXIT
  } LoopTypes;

  LoopLCC(int id, LCCNode *headerLCC, LCCNode *bodyLCC, LCCNode *preHeaderLCC, LCCNode *postExitLCC, Loop *loop, int loopType, InstructionCost *backEdges, bool hasHeaderWithExit, bool hasFence) : LCCNode(LCCNode::LOOP_LCC, id), _headerLCC(headerLCC), _bodyLCC(bodyLCC), _preHeaderLCC(preHeaderLCC), _postExitLCC(postExitLCC), _loop(loop), _backEdges(backEdges), _loopType(loopType), _hasHeaderWithExit(hasHeaderWithExit) {
    assert(loop && "loop must be specified!");
    assert(headerLCC && preHeaderLCC && postExitLCC && "entry, pre-header or post-exit LCCs cannot be null for a Loop Container");
    if (loopType == SELF_LOOP && bodyLCC) {
      assert("A self loop cannot have a body container!");
    }
    _headerLCC->setParentLCC(this);
    _preHeaderLCC->setParentLCC(this);
    _postExitLCC->setParentLCC(this);
    if (_bodyLCC) {
      _bodyLCC->setParentLCC(this);
    }
  }

  /* -------- Implementation of virtual functions -------*/

  Function *getFunction() {
    return _headerLCC->getFunction();
  }

  LCCNode *getInnerMostEntryLCC() {
    return _preHeaderLCC->getInnerMostEntryLCC();
  }

  LCCNode *getOneInnerMostEntryLCC() {
    return _preHeaderLCC->getOneInnerMostEntryLCC();
  }

  std::set<LCCNode *> getAllInnerMostEntryLCC() {
    auto innerLCCs = _preHeaderLCC->getAllInnerMostEntryLCC();
    return innerLCCs;
  }

  LCCNode *getInnerMostExitLCC() {
    return _postExitLCC->getInnerMostExitLCC();
  }

  std::set<LCCNode *> getAllInnerMostExitLCC() {
    auto innerLCCs = _postExitLCC->getAllInnerMostExitLCC();
    return innerLCCs;
  }

  Loop *getLoop() {
    return _loop;
  }

  LCCTypes getType() {
    return LOOP_LCC;
  }

  InstructionCost *getCostForPC(bool toInstrument) {

    InstructionCost::opvector costs;
    InstructionCost *iterations = nullptr;
    InstructionCost *bodyLCCCost = nullptr;
    InstructionCost *loopBodyLCCCost = nullptr;
    InstructionCost *totalLoopCost = nullptr;
    auto headerLCCCost = _headerLCC->getCostForPC(false);
    auto postExitLCCCost = _postExitLCC->getCostForPC(false);
    auto preHeaderLCCCost = _preHeaderLCC->getCostForPC(false);

    costs.push_back(preHeaderLCCCost);
    costs.push_back(postExitLCCCost);

    if (_bodyLCC) {
      bodyLCCCost = _bodyLCC->getCostForPC(false);
      loopBodyLCCCost = new InstructionCost(InstructionCost::ADD, bodyLCCCost, headerLCCCost);
    } else {
      loopBodyLCCCost = headerLCCCost;
    }

    /* if backedges are known, loop cost can be evaluated */
    if (_backEdges) {

      /* For header-non-colocated-exit, the body of the loop executes one extra time than the number of backedges */
      if (!_hasHeaderWithExit) {
        InstructionCost *one = getConstantInstCost(1);
        iterations = new InstructionCost(InstructionCost::ADD, _backEdges, one);
      } else {
        iterations = _backEdges;
      }

      InstructionCost *loopCost = new InstructionCost(InstructionCost::MUL, iterations, loopBodyLCCCost);

      /* For header-colocated-exit, the header is executed one extra time */
      if (_hasHeaderWithExit) {
        totalLoopCost = new InstructionCost(InstructionCost::ADD, loopCost, headerLCCCost);
      } else {
        totalLoopCost = loopCost;
      }

      costs.push_back(totalLoopCost);

      /*********************** Update statistics ***********************/
      applyrule3++;
      if (!_bodyLCC) {
        rule3savedInst += 2; /* for header & postExit */
      } else {
        rule3savedInst += 3; /* for header, body & postExit */
      }
    } else {

      /* Instrument the body */
      if (_bodyLCC) {
        /* For header-non-colocated-exit, header is executed one extra time. So one extra header cost is added. */
        costs.push_back(headerLCCCost);
        _bodyLCC->instrumentForPC(loopBodyLCCCost);
      } else {
        _headerLCC->instrumentForPC(loopBodyLCCCost);
      }
    }

    InstructionCost *newCost = new InstructionCost(InstructionCost::ADD, costs);
    InstructionCost *simplifiedNewCost = simplifyCost(getFunction(), newCost);
    if (!simplifiedNewCost) {
      errs() << "Cost that could not be simplified : " << *newCost << "\n";
    }
    assert(simplifiedNewCost && "Simplified loop cost cannot be null!");
    if (toInstrument) {
      errs() << "Cost for loop: " << *simplifiedNewCost << "\n";
      instrumentForPC(simplifiedNewCost);
    }
    return simplifiedNewCost;
  }

  InstructionCost *getCostForIC(bool toInstrument, InstructionCost *initialCost) {

    /* Sanity checks */
    long initialNumCost = getConstCost(initialCost);
    int numBackEdgeCost = -1;
    errs() << "Cost Evaluation of Loop: " << *_loop << "\n";
#ifdef LC_DEBUG
    //errs() << "Getting cost for loop: " << *_loop << "\n";
    errs() << "Loop LCC id: " << getID() << " --> initial cost: " << initialNumCost << "\n";
#endif

    assert((initialNumCost != -1) && "Initial cost cannot be unknown!");
    assert((initialNumCost <= CommitInterval) && "Initial cost cannot be greater than the commit cost interval!");

    if (_backEdges) {
      numBackEdgeCost = hasConstCost(_backEdges);
      errs() << "Has backedges: " << *_backEdges << " (numCost: " << numBackEdgeCost << ")\n";
      assert((numBackEdgeCost != 0) && "A self loop cost cannot be zero!");
    }

    InstructionCost *zeroCost = getConstantInstCost(0);
    InstructionCost *postExitLCCCost = nullptr;
    InstructionCost *preHeaderLCCCost = _preHeaderLCC->getCostForIC(false, initialCost);
    /* Loop header cannot start with an incoming cost, since it has multiple predecessors */
    InstructionCost *headerLCCCost = _headerLCC->getCostForIC(false, zeroCost); // preheader cost can only be added if the entire loop cost is available
    int numPreHeaderCost = getConstCost(preHeaderLCCCost);
    // bool headerInstrumented = _headerLCC->isInstrumented();

    if (_loopType == SELF_LOOP) {

      bool loopNeedsTransform = true;
      InstructionCost *bodyLCCCost = headerLCCCost; // for self loops, header & body cost are the same
      int numIterations = numBackEdgeCost + 1;
      int numBodyCost = getConstCost(bodyLCCCost);
      assert((numBodyCost >= 0) && "A self loop cannot have unknown body cost");
      errs() << "Self loop:- #backedges: " << numIterations << ", body cost: " << numBodyCost << ", preheader cost: " << *preHeaderLCCCost << "\n";

#ifdef LC_DEBUG
      if (!_backEdges)
        errs() << "Self loop:- No backedge available!!\n";
      else
        errs() << "Self loop:- Backedges: " << *_backEdges << "\n";
#endif

      //bool loopBodyInstrumented = headerInstrumented; -> Not used. To minimize instrumentation, the extra body cost will be tried to be instrumented in postexit, if within a limit
      if (numBackEdgeCost > 0) {
        /* numBodyCost can be 0 if the body is instrumented inside */
        //if(numBodyCost<=0)
        //errs() << "Self loop " << *_loop << " of " << _loop->getHeader()->getParent()->getName() << "() has unexpected body cost of " << numBodyCost << "\n";
        //assert((numBodyCost>0) && "A self loop cannot have unknown or zero loop body cost");
        int numLoopCost = numBodyCost * numIterations;
        int numLoopCostWithPreheader = numLoopCost + numPreHeaderCost;

        errs() << "############# For Self Loop " << *_loop << " #################\n";

        if (numLoopCostWithPreheader <= CommitInterval) {
          loopNeedsTransform = false;
          InstructionCost *loopCostWithPreheader = getConstantInstCost(numLoopCostWithPreheader);
          postExitLCCCost = _postExitLCC->getCostForIC(false, loopCostWithPreheader);
          errs() << "Self loop:- loop & preheader cost " << numLoopCostWithPreheader << " will be instrumented in post loop exit\n";
          applyrule3++;
          rule3savedInst++; /* for the header-cum-body */
        } else if (numLoopCost <= CommitInterval) {
          loopNeedsTransform = false;
          _preHeaderLCC->instrumentForIC(preHeaderLCCCost);
          InstructionCost *loopCost = getConstantInstCost(numLoopCost);
          postExitLCCCost = _postExitLCC->getCostForIC(false, loopCost);
          errs() << "Self loop:- loop cost " << numLoopCost << " will be instrumented later in post loop exit's cost computation in ";
          getSingleLCCRep(_postExitLCC);
          errs() << ", preheader cost " << numPreHeaderCost << " is definitely instrumented in preheader ";
          getSingleLCCRep(_preHeaderLCC);
          errs() << "\n";
          applyrule3++;
          rule3savedInst++; /* for the header-cum-body */
        }
      }

      if (loopNeedsTransform) {
#if 0
        /* postpone preheader cost to postexit */
        postExitLCCCost = _postExitLCC->getCostForIC(false, preHeaderLCCCost);
#else
        _preHeaderLCC->instrumentForIC(preHeaderLCCCost);
        postExitLCCCost = _postExitLCC->getCostForIC(false, zeroCost);
#endif
        /* transform later if possible, else instrument */
        if (selfLoop.find(_loop) == selfLoop.end()) {
          errs() << "Self loop will be transformed, & body cost is " << *bodyLCCCost << " for loop " << *_loop << "\n";
          selfLoop[_loop] = bodyLCCCost;
        }
      }
    } else if (_loopType == HEADER_COLOCATED_EXIT) {

      bool loopNeedsTransform = true;
      /* it should always have a valid header & body */
      //bool bodyInstrumented = false;
      InstructionCost *bodyLCCCost = nullptr;
      int numBodyCost = -1;
      int numHeaderCost = getConstCost(headerLCCCost);
      //if(toInstrument)
      //_preHeaderLCC->instrumentForIC(preHeaderLCCCost);

      assert((_bodyLCC) && "A loop with header-colocated exit, must have a body LCC separate than the headerLCC!");
      assert((numHeaderCost >= 0) && "A loop with header-colocated exit, must have a constant header cost!");

      //errs() << "Not a self loop:- " << *_loop << "\n";

      errs() << "############# For Header-Colocated-Exit Loop " << *_loop << " #################\n";
      bodyLCCCost = _bodyLCC->getCostForIC(false, headerLCCCost);
      //bodyInstrumented = _bodyLCC->isInstrumented();
      numBodyCost = hasConstCost(bodyLCCCost);
      if (numBodyCost > 0 && numBackEdgeCost > 0) {
        // !headerInstrumented && !bodyInstrumented : even if the header & body are instrumented, we are using the residual cost to calculate the loop cost, to avoid extra instrumentation
        int numLoopBodyLCCCost = numHeaderCost + numBodyCost;
        int numTotalLoopCost = numBackEdgeCost * numLoopBodyLCCCost;
        int numLoopCostWithPreheader = numPreHeaderCost + numTotalLoopCost;

        if (numLoopCostWithPreheader <= CommitInterval) {
          loopNeedsTransform = false;
          InstructionCost *loopCost = getConstantInstCost(numLoopCostWithPreheader);
          postExitLCCCost = _postExitLCC->getCostForIC(false, loopCost);
          errs() << "Header-colocated-exit loop:- loop & preheader cost " << numLoopCostWithPreheader << " will be instrumented in post loop exit\n";
          applyrule3++;
          rule3savedInst += 4; /* for preheader, header, body & postExit */
        } else if (numTotalLoopCost <= CommitInterval) {
          loopNeedsTransform = false;
          _preHeaderLCC->instrumentForIC(preHeaderLCCCost);
          InstructionCost *loopCost = getConstantInstCost(numTotalLoopCost);
          postExitLCCCost = _postExitLCC->getCostForIC(false, loopCost);
          errs() << "Header-colocated-exit loop:- loop cost " << loopCost << " will be instrumented in post loop exit\n";
          applyrule3++;
          rule3savedInst += 3; /* for header, body & postExit */
        }
      }

      if (loopNeedsTransform) {
        //InstructionCost* loopBodyCost = getConstantInstCost(numHeaderCost + numBodyCost); // header cost has already been used up in body cost, so this is not needed
        /* transform later if possible, else instrument */
        if (numBodyCost > 0) {
          if (seseLoop.find(_loop) == seseLoop.end()) {
            errs() << "Header-colocated-exit loop will be transformed, & body cost is " << *bodyLCCCost << " for loop with header " << _loop->getHeader()->getName() << "\n";
            seseLoop[_loop] = bodyLCCCost;
          }
        }

#if 0
        /* postpone preheader cost commit */
        int remCostForExit = numPreHeaderCost + numHeaderCost;
        if (remCostForExit > CommitInterval) {
          _preHeaderLCC->instrumentForIC(preHeaderLCCCost);
          remCostForExit = numHeaderCost;
        }
        InstructionCost *costToPostExit = getConstantInstCost(remCostForExit);
        /* since header is colocated with exit, header will run an extra time */
        postExitLCCCost = _postExitLCC->getCostForIC(false, costToPostExit);
#else
        _preHeaderLCC->instrumentForIC(preHeaderLCCCost);
        /* Since header is colocated with exit, there will be one pending header cost */
        postExitLCCCost = _postExitLCC->getCostForIC(false, headerLCCCost);
#endif
      }
    } else if (_loopType == HEADER_NONCOLOCATED_EXIT) {

      errs() << "############# For Header-NonColocated-Exit Loop " << *_loop << " ###############\n";
      bool loopNeedsTransform = true;
      //bool headerInstrumented = false;
      /* since header is non-colocated with exit, loop will run an extra time */
      int numIterations = numBackEdgeCost + 1;
      int numHeaderCost = getConstCost(headerLCCCost);
      //if(toInstrument)
      //_preHeaderLCC->instrumentForIC(preHeaderLCCCost);

      /* it should always have a valid header, & the body & header maybe clubbed together */
      assert((!_bodyLCC) && "A loop with non-header-colocated exit, must not have a body LCC & be collapsed in the headerLCC!");
      assert((numHeaderCost >= 0) && "A loop with header-non-colocated exit, must have a constant header cost!");

      if (numBackEdgeCost > 0 && numHeaderCost > 0) {
        // !headerInstrumented : even if the header is instrumented, we are using the residual cost to calculate the loop cost, to avoid extra instrumentation
        int numTotalLoopCost = numIterations * numHeaderCost;
        int numLoopCostWithPreheader = numPreHeaderCost + numTotalLoopCost;

        if (numLoopCostWithPreheader <= CommitInterval) {
          loopNeedsTransform = false;
          InstructionCost *loopCost = getConstantInstCost(numLoopCostWithPreheader);
          postExitLCCCost = _postExitLCC->getCostForIC(false, loopCost);
          errs() << "Non-header-colocated-exit loop:- loop & preheader cost " << numLoopCostWithPreheader << " will be instrumented in post loop exit\n";
          applyrule3++;
          rule3savedInst += 3; /* for preheader, header & postExit */
        } else if (numTotalLoopCost <= CommitInterval) {
          loopNeedsTransform = false;
          _preHeaderLCC->instrumentForIC(preHeaderLCCCost);
          InstructionCost *loopCost = getConstantInstCost(numTotalLoopCost);
          postExitLCCCost = _postExitLCC->getCostForIC(false, loopCost);
          errs() << "Non-header-colocated-exit loop:- loop cost " << loopCost << " will be instrumented in post loop exit\n";
          applyrule3++;
          rule3savedInst += 2; /* for header & postExit */
        }
      }

      if (loopNeedsTransform) {
        errs() << "Instrumenting header-non-colocated-exit loop with body cost: " << numHeaderCost << "\n";
        errs() << "Header LCC is: ";
        getSingleLCCRep(_headerLCC);
        errs() << "\n";
        /* transform later if possible, else instrument */
        if (numHeaderCost > 0) {
          if (seseLoop.find(_loop) == seseLoop.end()) {
            errs() << "Non-header-colocated-exit loop will be transformed, & body cost is " << *headerLCCCost << " for loop with header " << _loop->getHeader()->getName() << "\n";
            seseLoop[_loop] = headerLCCCost;
          }
        }
#if 0
        /* postpone preheader cost commit */
        postExitLCCCost = _postExitLCC->getCostForIC(false, preHeaderLCCCost);
#else
        _preHeaderLCC->instrumentForIC(preHeaderLCCCost);
        postExitLCCCost = _postExitLCC->getCostForIC(false, zeroCost);
#endif
      }
    }

    long remCost = getConstCost(postExitLCCCost);
    if (toInstrument || (remCost > CommitInterval)) {
      instrumentForIC(postExitLCCCost);
      postExitLCCCost = getConstantInstCost(0);
    }

#ifdef LC_DEBUG
    errs() << "Loop LCC id: " << getID() << " --> final cost: " << *postExitLCCCost << "\n";
#endif
    return postExitLCCCost;
  }

  void instrumentForPC(InstructionCost *cost) {
    _preHeaderLCC->instrumentForPC(cost);
  }

  void instrumentForIC(InstructionCost *cost) {
    _postExitLCC->instrumentForIC(cost);
  }

  /* return true if any of its child containers are instrumented */
  bool isInstrumented() {
    bool ret;
    ret = _preHeaderLCC->isInstrumented() || _postExitLCC->isInstrumented() || _headerLCC->isInstrumented();
    if (_bodyLCC) {
      ret = ret || _bodyLCC->isInstrumented();
    }
    return ret;
  }
};

class InvertedVLCC : public LCCNode {
  LCCNode *_entryLCC;
  std::map<LCCNode *, double> _childLCCInfo;

public:
  /* ----------------- Constructor ---------------------*/

  InvertedVLCC(int id, LCCNode *entryLCC, std::map<LCCNode *, double> childLCCInfo) : LCCNode(LCCNode::INVERTEDV_LCC, id), _entryLCC(entryLCC), _childLCCInfo(childLCCInfo) {
    assert(entryLCC && "entry LCC cannot be null for a InvertedV Container");
    _entryLCC->setParentLCC(this);
    for (auto childLCCIt = _childLCCInfo.begin(); childLCCIt != _childLCCInfo.end(); childLCCIt++) {
      auto childLCC = childLCCIt->first;
      childLCC->setParentLCC(this);
    }
  }

  /* -------- Implementation of virtual functions -------*/

  Function *getFunction() {
    return _entryLCC->getFunction();
  }

  LCCNode *getInnerMostEntryLCC() {
    return _entryLCC->getInnerMostEntryLCC();
  }

  LCCNode *getOneInnerMostEntryLCC() {
    return _entryLCC->getOneInnerMostEntryLCC();
  }

  std::set<LCCNode *> getAllInnerMostEntryLCC() {
    auto innerLCCs = _entryLCC->getAllInnerMostEntryLCC();
    return innerLCCs;
  }

  LCCNode *getInnerMostExitLCC() {
    return nullptr;
  }

  std::set<LCCNode *> getAllInnerMostExitLCC() {
    std::set<LCCNode *> innerLCCs;
    for (auto childInfo = _childLCCInfo.begin(); childInfo != _childLCCInfo.end(); childInfo++) {
      auto childLCC = childInfo->first;
      auto childInnerLCCs = childLCC->getAllInnerMostExitLCC();
      for (auto childInnerLCC = childInnerLCCs.begin(); childInnerLCC != childInnerLCCs.end(); childInnerLCC++) {
        if (innerLCCs.find(*childInnerLCC) == innerLCCs.end()) {
          innerLCCs.insert(*childInnerLCC);
        } else {
          errs() << "WARNING: In InvertedV->getAllInnerMostExitLCC(), non-unique inner LCCs turned up!\n";
        }
      }
    }
    return innerLCCs;
  }

  Loop *getLoop() {
    return nullptr; /* not a loop container */
  }

  LCCTypes getType() {
    return INVERTEDV_LCC;
  }

  InstructionCost *getCostForPC(bool toInstrument) {

    InstructionCost::opvector costs;
    std::map<LCCNode *, InstructionCost *> childToCostMap;
    bool instrumentChild = false;
    auto entryLCCCost = _entryLCC->getCostForPC(false);
    if (entryLCCCost) {
      costs.push_back(entryLCCCost);
    }

    long avgChildCost = 0, maxCost = 0, minCost = 0;

    for (auto childInfo : _childLCCInfo) {
      LCCNode *childLCC = childInfo.first;
      double childProb = childInfo.second;
      InstructionCost *childCost = childLCC->getCostForPC(false);
      long numChildCost = hasConstCost(childCost);
      if (numChildCost == -1) {
        instrumentChild = true;
        break;
      } else {
        long weightedChildCost = childProb * numChildCost;
        avgChildCost += weightedChildCost;
        if (numChildCost > maxCost) {
          maxCost = numChildCost;
        }
        if (numChildCost < minCost) {
          minCost = numChildCost;
        }
      }
      childToCostMap[childLCC] = childCost;
    }

    long diffCost = maxCost - minCost;
    if (diffCost > ALLOWED_DEVIATION) {
      instrumentChild = true;
    }

    if (instrumentChild) {
      /* Only dom & post dom cost are returned, every child is instrumented */
      for (auto childInfo : _childLCCInfo) {
        LCCNode *childLCC = childInfo.first;
        auto mapIt = childToCostMap.find(childLCC);
        assert((mapIt != childToCostMap.end()) && "Child PC cost not found!");
        InstructionCost *childCost = mapIt->second;
        childLCC->instrumentForPC(childCost);
      }
    } else {
      auto avgChildLCCCost = getConstantInstCost(avgChildCost);
      if (avgChildLCCCost) {
        costs.push_back(avgChildLCCCost);
      }
    }

    InstructionCost *newCost = new InstructionCost(InstructionCost::ADD, costs);
    InstructionCost *simplifiedNewCost = simplifyCost(getFunction(), newCost);
    if (!simplifiedNewCost) {
      errs() << "Cost that could not be simplified : " << *newCost << "\n";
    }
    assert(simplifiedNewCost && "Simplified invertedV cost cannot be null!");
    if (toInstrument) {
      instrumentForPC(simplifiedNewCost);
    }
    return simplifiedNewCost;
  }

  InstructionCost *getCostForIC(bool toInstrument, InstructionCost *initialCost) {

    /* Sanity checks */
    long initialNumCost = getConstCost(initialCost);
#ifdef LC_DEBUG
    errs() << "Inverted-V LCC id: " << getID() << " --> initial cost: " << initialNumCost << "\n";
#endif

    assert((initialNumCost != -1) && "Initial cost cannot be unknown!");
    assert((initialNumCost <= CommitInterval) && "Initial cost cannot be greater than the commit cost interval!");

    auto entryLCCCost = _entryLCC->getCostForIC(false, initialCost);

    for (auto childInfo : _childLCCInfo) {
      /* Every child is instrumented */
      LCCNode *childLCC = childInfo.first;
      InstructionCost *childCost = childLCC->getCostForIC(false, entryLCCCost);
      childLCC->instrumentForIC(childCost);
    }

    return nullptr; /* Inverted-V LCC is demarcated by a fence at the bottom */
  }

  void instrumentForPC(InstructionCost *cost) {
    _entryLCC->instrumentForPC(cost);
  }

  void instrumentForIC(InstructionCost *cost) {
    for (auto childInfo = _childLCCInfo.begin(); childInfo != _childLCCInfo.end(); childInfo++) {
      auto childLCC = childInfo->first;
      childLCC->instrumentForIC(cost);
    }
  }

  /* return true if any of its child containers are instrumented */
  bool isInstrumented() {
    bool ret = _entryLCC->isInstrumented();
    for (auto childInfo = _childLCCInfo.begin(); childInfo != _childLCCInfo.end(); childInfo++) {
      auto childLCC = childInfo->first;
      ret = ret || childLCC->isInstrumented();
    }
    return ret;
  }
};

class VLCC : public LCCNode {
  std::map<LCCNode *, double> _parentLCCInfo;
  LCCNode *_exitLCC;

public:
  /* ----------------- Constructor ---------------------*/

  VLCC(int id, LCCNode *exitLCC, std::map<LCCNode *, double> parentLCCInfo) : LCCNode(LCCNode::V_LCC, id), _parentLCCInfo(parentLCCInfo), _exitLCC(exitLCC) {
    assert(exitLCC && "exit LCC cannot be null for a V Container");
    _exitLCC->setParentLCC(this);
    for (auto parentLCCIt = _parentLCCInfo.begin(); parentLCCIt != _parentLCCInfo.end(); parentLCCIt++) {
      auto parentLCC = parentLCCIt->first;
      parentLCC->setParentLCC(this);
    }
  }

  /* -------- Implementation of virtual functions -------*/

  Function *getFunction() {
    return _exitLCC->getFunction();
  }

  LCCNode *getInnerMostEntryLCC() {
    return nullptr;
  }

  LCCNode *getOneInnerMostEntryLCC() {
    LCCNode *firstParent = _parentLCCInfo.begin()->first;
    return firstParent->getOneInnerMostEntryLCC();
  }

  std::set<LCCNode *> getAllInnerMostEntryLCC() {
    std::set<LCCNode *> innerLCCs;
    for (auto parentInfo = _parentLCCInfo.begin(); parentInfo != _parentLCCInfo.end(); parentInfo++) {
      auto parentLCC = parentInfo->first;
      auto parentInnerLCCs = parentLCC->getAllInnerMostEntryLCC();
      for (auto parentInnerLCC = parentInnerLCCs.begin(); parentInnerLCC != parentInnerLCCs.end(); parentInnerLCC++) {
        if (innerLCCs.find(*parentInnerLCC) == innerLCCs.end()) {
          innerLCCs.insert(*parentInnerLCC);
        } else {
          errs() << "WARNING: In VLCC->getAllInnerMostEntryLCC(), non-unique inner LCCs turned up!\n";
        }
      }
    }
    return innerLCCs;
  }

  LCCNode *getInnerMostExitLCC() {
    return _exitLCC->getInnerMostExitLCC();
  }

  std::set<LCCNode *> getAllInnerMostExitLCC() {
    auto innerLCCs = _exitLCC->getAllInnerMostExitLCC();
    return innerLCCs;
  }

  Loop *getLoop() {
    return nullptr; /* not a loop container */
  }

  LCCTypes getType() {
    return V_LCC;
  }

  InstructionCost *getCostForPC(bool toInstrument) {

    for (auto parentInfo = _parentLCCInfo.begin(); parentInfo != _parentLCCInfo.end(); parentInfo++) {
      InstructionCost::opvector costs;
      auto parentLCC = parentInfo->first;

      auto parentLCCCost = parentLCC->getCostForPC(false);
      auto exitLCCCost = _exitLCC->getCostForPC(false);
      if (parentLCCCost) {
        costs.push_back(parentLCCCost);
      }
      if (exitLCCCost) {
        costs.push_back(exitLCCCost);
      }
      InstructionCost *newCost = new InstructionCost(InstructionCost::ADD, costs);
      InstructionCost *simplifiedNewCost = simplifyCost(getFunction(), newCost);
      if (!simplifiedNewCost) {
        errs() << "Cost that could not be simplified : " << *newCost << "\n";
      }
      assert(simplifiedNewCost && "Simplified V-LCC cost cannot be null!");
      if (toInstrument) {
        instrumentForPC(simplifiedNewCost);
      }
    }
    return nullptr; /* V_LCC is demarcated by a fence on the top */
  }

  InstructionCost *getCostForIC(bool toInstrument, InstructionCost *initialCost) {

    /* Sanity checks */
    long initialNumCost = getConstCost(initialCost);
#ifdef LC_DEBUG
    errs() << "VLCC id: " << getID() << " --> initial cost: " << initialNumCost << "\n";
#endif

    assert((initialNumCost != -1) && "Initial cost cannot be unknown!");
    assert((initialNumCost <= CommitInterval) && "Initial cost cannot be greater than the commit cost interval!");

    bool instrumentParent = false;
    long avgParentCost = 0, maxCost = 0, minCost = 0;
    std::map<LCCNode *, InstructionCost *> parentToCostMap;

    for (auto parentInfo : _parentLCCInfo) {
      LCCNode *parentLCC = parentInfo.first;
      double parentProb = parentInfo.second;
      InstructionCost *parentCost = parentLCC->getCostForIC(false, initialCost);
      long numParentCost = getConstCost(parentCost);
      long weightedParentCost = parentProb * numParentCost;
      avgParentCost += weightedParentCost;
      if (numParentCost > maxCost) {
        maxCost = numParentCost;
      }
      if (numParentCost < minCost) {
        minCost = numParentCost;
      }
      parentToCostMap[parentLCC] = parentCost;
    }

    long diffCost = maxCost - minCost;
    if (diffCost > ALLOWED_DEVIATION) {
      instrumentParent = true;
    }

    if (instrumentParent) {
      /* Only exit cost are returned, every parent is instrumented */
      for (auto parentInfo : _parentLCCInfo) {
        LCCNode *parentLCC = parentInfo.first;
        auto mapIt = parentToCostMap.find(parentLCC);
        assert((mapIt != parentToCostMap.end()) && "Parent IC cost not found!");
        InstructionCost *parentCost = mapIt->second;
        parentLCC->instrumentForIC(parentCost);
      }
      avgParentCost = 0;
    }

    auto avgParentLCCCost = getConstantInstCost(avgParentCost);
    auto exitLCCCost = _exitLCC->getCostForIC(false, avgParentLCCCost);
    long remCost = getConstCost(exitLCCCost);

    if (toInstrument || (remCost > CommitInterval)) {
      instrumentForIC(exitLCCCost);
      exitLCCCost = getConstantInstCost(0);
    }
    return exitLCCCost;
  }

  void instrumentForPC(InstructionCost *cost) {
    for (auto parentInfo = _parentLCCInfo.begin(); parentInfo != _parentLCCInfo.end(); parentInfo++) {
      auto parentLCC = parentInfo->first;
      parentLCC->instrumentForPC(cost);
    }
  }

  void instrumentForIC(InstructionCost *cost) {
    _exitLCC->instrumentForIC(cost);
  }

  /* return true if any of its child containers are instrumented */
  bool isInstrumented() {
    bool ret = _exitLCC->isInstrumented();
    for (auto parentInfo = _parentLCCInfo.begin(); parentInfo != _parentLCCInfo.end(); parentInfo++) {
      auto parentLCC = parentInfo->first;
      ret = ret || parentLCC->isInstrumented();
    }
    return ret;
  }
};

class UnknownLCC : public LCCNode {
  LCCNode *_entryLCC; /* Should be dominator for branch & preheader for loop */
  std::set<LCCNode *> _childLCCs;
  /* either will have a exit LCC or a loop */
  LCCNode *_exitLCC; /* Should be postdominator for branch & loop */
  Loop *_loop;

public:
  /* ----------------- Constructor ---------------------*/

  UnknownLCC(int id, LCCNode *entryLCC, std::set<LCCNode *> childLCCs, LCCNode *exitLCC, Loop *loop) : LCCNode(LCCNode::UNKNOWN_LCC, id), _entryLCC(entryLCC), _childLCCs(childLCCs), _exitLCC(exitLCC), _loop(loop) {
    assert(entryLCC && exitLCC && "entry & exit LCCs cannot be null for a Unknown Container");
    _entryLCC->setParentLCC(this);
    _exitLCC->setParentLCC(this);
    for (auto childLCCIt = _childLCCs.begin(); childLCCIt != _childLCCs.end(); childLCCIt++) {
      auto childLCC = *childLCCIt;
      childLCC->setParentLCC(this);
    }
  }

  /* -------- Implementation of virtual functions -------*/

  Function *getFunction() {
    return _entryLCC->getFunction();
  }

  LCCNode *getInnerMostEntryLCC() {
    return _entryLCC->getInnerMostEntryLCC();
  }

  LCCNode *getOneInnerMostEntryLCC() {
    return _entryLCC->getOneInnerMostEntryLCC();
  }

  std::set<LCCNode *> getAllInnerMostEntryLCC() {
    auto innerLCCs = _entryLCC->getAllInnerMostEntryLCC();
    return innerLCCs;
  }

  LCCNode *getInnerMostExitLCC() {
    return _exitLCC->getInnerMostExitLCC();
  }

  std::set<LCCNode *> getAllInnerMostExitLCC() {
    auto innerLCCs = _exitLCC->getAllInnerMostExitLCC();
    return innerLCCs;
  }

  Loop *getLoop() {
    return _loop;
  }

  LCCTypes getType() {
    return UNKNOWN_LCC;
  }

  InstructionCost *getCostForPC(bool toInstrument) {
    InstructionCost::opvector costs;
    auto entryLCCCost = _entryLCC->getCostForPC(false);
    auto exitLCCCost = _exitLCC->getCostForPC(false);
    if (entryLCCCost) {
      costs.push_back(entryLCCCost);
    }
    if (exitLCCCost) {
      costs.push_back(exitLCCCost);
    }

    for (auto childLCC : _childLCCs) {
      InstructionCost *childCost = childLCC->getCostForPC(false);
      childLCC->instrumentForPC(childCost);
    }

    InstructionCost *newCost = new InstructionCost(InstructionCost::ADD, costs);
    InstructionCost *simplifiedNewCost = simplifyCost(getFunction(), newCost);
    if (!simplifiedNewCost) {
      errs() << "Cost that could not be simplified : " << *newCost << "\n";
    }
    assert(simplifiedNewCost && "Simplified path cost cannot be null!");
    if (toInstrument) {
      instrumentForPC(simplifiedNewCost);
    }
    return simplifiedNewCost;
  }

  InstructionCost *getCostForIC(bool toInstrument, InstructionCost *initialCost) {

    /* Sanity checks */
    long initialNumCost = getConstCost(initialCost);
#ifdef LC_DEBUG
    errs() << "Unknown LCC id: " << getID() << " --> initial cost: " << initialNumCost << "\n";
#endif

    assert((initialNumCost != -1) && "Initial cost cannot be unknown!");
    assert((initialNumCost <= CommitInterval) && "Initial cost cannot be greater than the commit cost interval!");

    InstructionCost *zeroCost = getConstantInstCost(0); /* Initial cost for any top level container is 0 */

    _entryLCC->getCostForIC(true, initialCost); /* entryLCC should be fully instrumented */

    for (auto childLCC : _childLCCs) {
      childLCC->getCostForIC(true, zeroCost); /* the interconnection between children & entry & exit are unknown, so each child is fully instrumented */
    }

    auto exitLCCCost = _exitLCC->getCostForIC(false, zeroCost);
    long remCost = getConstCost(exitLCCCost);

    if (toInstrument || (remCost > CommitInterval)) {
      instrumentForIC(exitLCCCost);
      exitLCCCost = getConstantInstCost(0);
    }
    return exitLCCCost;
  }

  void instrumentForPC(InstructionCost *cost) {
    _entryLCC->instrumentForPC(cost);
  }

  void instrumentForIC(InstructionCost *cost) {
    _exitLCC->instrumentForIC(cost);
  }

  /* return true if any of its child containers are instrumented */
  bool isInstrumented() {
    return true; /* Since all are instrumented */
  }
};

/*********************************************** Section: Compiler Interrupt Pass *********************************************/

struct CompilerInterrupt : public ModulePass {

  static char ID;
  SmallVector<StringRef, 100> funcUsedAsPointers; // contains list of all functions that begin a thread & main()
  //std::set<std::string> instrumentedFuncs; // contains list of functions that have unresolvable arguments in function calls
  std::map<Function *, Value *> localClock; // list of local variables to be passed as parameter, corresponding to each function in threadFunc
  std::map<std::string, bool> isRecursiveFunc;
  /* Basic block may have fence instructions, which will require multiple containers for a single block. Order of blocks must be preserved for which vector is used */
  std::map<BasicBlock *, std::vector<LCCNode *>> bbToContainersMap;
  /* Contains the final set of outer most containers after the last reduction */
  std::map<Function *, std::vector<LCCNode *>> globalOuterLCCList;

  CompilerInterrupt() : ModulePass(ID) {}

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<CallGraphWrapperPass>();
    AU.addRequired<PostDominatorTreeWrapperPass>();
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.addRequired<LoopInfoWrapperPass>();
    AU.addRequired<BranchProbabilityInfoWrapperPass>();
    AU.addRequired<ScalarEvolutionWrapperPass>();
    AU.addRequired<MemorySSAWrapperPass>();
    AU.addPreserved<MemorySSAWrapperPass>();
  }

  /* First LCC for the same basic block */
  LCCNode *getFirstLCCofBB(BasicBlock *currentBB) {
    std::vector<LCCNode *> containers = bbToContainersMap[currentBB];
    return containers.front();
  }

  /* Last LCC for the same basic block */
  LCCNode *getLastLCCofBB(BasicBlock *currentBB) {
    std::vector<LCCNode *> containers = bbToContainersMap[currentBB];
    return containers.back();
  }

  /* Get number of LCC for the same basic block */
  int getNumLCCofBB(BasicBlock *currentBB) {
    std::vector<LCCNode *> containers = bbToContainersMap[currentBB];
    return containers.size();
  }

  /* Only LCC for the same basic block */
  LCCNode *getSingleLCCofBB(BasicBlock *block) {
    auto bbLCCSet = bbToContainersMap[block];
    if (bbLCCSet.size() == 1) {
      return *(bbLCCSet.begin());
    }
    return nullptr;
  }

  /* Only LCC for the same basic block */
  std::vector<LCCNode *> getAllLCCofBB(BasicBlock *block) {
    auto bbLCCSet = bbToContainersMap[block];
    return bbLCCSet;
  }

  /*************************************** Sub Section: Compiler Interrupt Utility Functions ***************************************/

  bool presentInGlobalLCCList(LCCNode *depricatedLCC) {
    Function *F = depricatedLCC->getFunction();
    assert(globalOuterLCCList.count(F) && "Function has no containers to be removed");
    auto depricatedLCCIt = find(globalOuterLCCList[F].begin(), globalOuterLCCList[F].end(), depricatedLCC);
    if (depricatedLCCIt == globalOuterLCCList[F].end()) {
      return false;
    } else {
      return true;
    }
  }

  std::vector<LCCNode *>::iterator eraseFromGlobalLCCList(LCCNode *depricatedLCC) {
    Function *F = depricatedLCC->getFunction();
    assert(globalOuterLCCList.count(F) && "Function has no containers to be removed");
    auto depricatedLCCIt = find(globalOuterLCCList[F].begin(), globalOuterLCCList[F].end(), depricatedLCC);
    assert((depricatedLCCIt != globalOuterLCCList[F].end()) && "Node had already been removed from global list of containers");
    globalOuterLCCList[F].erase(depricatedLCCIt);
#ifdef ALL_DEBUG
    //auto num = globalOuterLCCList[F].count(depricatedLCC);
    errs() << "Erasing ";
    printUnitLCCSet(depricatedLCC);
    errs() << " from global list of outer LCCs\n";
    errs() << "Number of outer level LCCS: " << globalOuterLCCList[F].size() << "\n";
#endif
    return depricatedLCCIt;
  }

  /*************************************** Sub Section: Production Rule System ***************************************/

  /* Check & create simple path container - LCC1==>LCC2 (LCC1->exitBlock==>LCC2->entryBlock) */
  bool checkNCreatePathLCC(LCCNode *currentLCC) {

#if 0
    /* For debug only */
    LCCNode *exitLCC = currentLCC->getInnerMostExitLCC();
    if (exitLCC) {
      auto exitBlock = (static_cast<UnitLCC *>(exitLCC))->getBlock();
      errs() << "checkNCreatePathLCC(): checking for " << exitBlock->getName() << "\n";
    }
#endif

    /*********************** Check for path *************************/
    LCCNode *succLCC = currentLCC->getSingleSuccOfPath();
    if (!succLCC) {
      return false;
    }

    /* Sanity check - successor cannot be loop header */
    LCCNode *succUnitLCC = succLCC->getInnerMostExitLCC();
    BasicBlock *succBB = (static_cast<UnitLCC *>(succUnitLCC))->getBlock();
    if (LI->isLoopHeader(succBB)) {
      return false;
    }

    /********************* Create new container *********************/
    LCCNode *newLCC = new PathLCC(lccIDGen++, currentLCC, succLCC);

    /******************** Create new connections *********************/
    newLCC->makeNewSuccConnections(succLCC);    /* succLCC is the exit LCC for newLCC */
    newLCC->makeNewPredConnections(currentLCC); /* currentLCC is the entry LCC for newLCC */

    /******************** Update global LCC list *********************/
    auto F = currentLCC->getFunction();
    auto position = eraseFromGlobalLCCList(currentLCC);
    globalOuterLCCList[F].insert(position, newLCC);
    eraseFromGlobalLCCList(succLCC);

    /************************ Test Printing **************************/
#ifdef LC_DEBUG
    errs() << "\nPath Container(" << newLCC->getID() << "):- ";
    errs() << "Entry LCC(" << currentLCC->getID() << "): (";
    printUnitLCCSet(currentLCC);
    errs() << "), Exit LCC(" << succLCC->getID() << "): (";
    printUnitLCCSet(succLCC);
    errs() << ")\n";
#endif

    applycontrule1++;
    return true;
  }

  /********************************************* Copied from BasicBlockUtils.cpp ********************************************/
  /// Update DominatorTree, LoopInfo, and LCCSA analysis information.
  void UpdateAnalysisInformation(BasicBlock *OldBB, BasicBlock *NewBB,
                                 ArrayRef<BasicBlock *> Preds,
                                 DominatorTree *DT, LoopInfo *LI,
                                 bool PreserveLCSSA, bool &HasLoopExit) {
    // Update dominator tree if available.
    if (DT) {
      if (OldBB == DT->getRootNode()->getBlock()) {
        assert(NewBB == &NewBB->getParent()->getEntryBlock());
        DT->setNewRoot(NewBB);
      } else {
        // Split block expects NewBB to have a non-empty set of predecessors.
        DT->splitBlock(NewBB);
      }
    }

    // The rest of the logic is only relevant for updating the loop structures.
    if (!LI) {
      return;
    }

    assert(DT && "DT should be available to update LoopInfo!");
    Loop *L = LI->getLoopFor(OldBB);

    // If we need to preserve loop analyses, collect some information about how
    // this split will affect loops.
    bool IsLoopEntry = !!L;
    bool SplitMakesNewLoopHeader = false;
    for (BasicBlock *Pred : Preds) {
      // Preds that are not reachable from entry should not be used to identify if
      // OldBB is a loop entry or if SplitMakesNewLoopHeader. Unreachable blocks
      // are not within any loops, so we incorrectly mark SplitMakesNewLoopHeader
      // as true and make the NewBB the header of some loop. This breaks LI.
      if (!DT->isReachableFromEntry(Pred)) {
        continue;
      }
      // If we need to preserve LCSSA, determine if any of the preds is a loop
      // exit.
      if (PreserveLCSSA) {
        if (Loop *PL = LI->getLoopFor(Pred)) {
          if (!PL->contains(OldBB)) {
            HasLoopExit = true;
          }
        }
      }

      // If we need to preserve LoopInfo, note whether any of the preds crosses
      // an interesting loop boundary.
      if (!L) {
        continue;
      }
      if (L->contains(Pred)) {
        IsLoopEntry = false;
      } else {
        SplitMakesNewLoopHeader = true;
      }
    }

    // Unless we have a loop for OldBB, nothing else to do here.
    if (!L) {
      return;
    }

    if (IsLoopEntry) {
      // Add the new block to the nearest enclosing loop (and not an adjacent
      // loop). To find this, examine each of the predecessors and determine which
      // loops enclose them, and select the most-nested loop which contains the
      // loop containing the block being split.
      Loop *InnermostPredLoop = nullptr;
      for (BasicBlock *Pred : Preds) {
        if (Loop *PredLoop = LI->getLoopFor(Pred)) {
          // Seek a loop which actually contains the block being split (to avoid
          // adjacent loops).
          while (PredLoop && !PredLoop->contains(OldBB)) {
            PredLoop = PredLoop->getParentLoop();
          }

          // Select the most-nested of these loops which contains the block.
          if (PredLoop && PredLoop->contains(OldBB) &&
              (!InnermostPredLoop ||
               InnermostPredLoop->getLoopDepth() < PredLoop->getLoopDepth())) {
            InnermostPredLoop = PredLoop;
          }
        }
      }

      if (InnermostPredLoop) {
        InnermostPredLoop->addBasicBlockToLoop(NewBB, *LI);
      }
    } else {
      L->addBasicBlockToLoop(NewBB, *LI);
      if (SplitMakesNewLoopHeader) {
        L->moveToHeader(NewBB);
      }
    }
  }

  /// Update the PHI nodes in OrigBB to include the values coming from NewBB.
  /// This also updates AliasAnalysis, if available.
  void UpdatePHINodes(BasicBlock *OrigBB, BasicBlock *NewBB,
                      ArrayRef<BasicBlock *> Preds, BranchInst *BI,
                      bool HasLoopExit) {
    // Otherwise, create a new PHI node in NewBB for each PHI node in OrigBB.
    SmallPtrSet<BasicBlock *, 16> PredSet(Preds.begin(), Preds.end());
    for (BasicBlock::iterator I = OrigBB->begin(); isa<PHINode>(I);) {
      PHINode *PN = cast<PHINode>(I++);

      // Check to see if all of the values coming in are the same.  If so, we
      // don't need to create a new PHI node, unless it's needed for LCSSA.
      Value *InVal = nullptr;
      if (!HasLoopExit) {
        InVal = PN->getIncomingValueForBlock(Preds[0]);
        for (unsigned i = 0, e = PN->getNumIncomingValues(); i != e; ++i) {
          if (!PredSet.count(PN->getIncomingBlock(i))) {
            continue;
          }
          if (!InVal) {
            InVal = PN->getIncomingValue(i);
          } else if (InVal != PN->getIncomingValue(i)) {
            InVal = nullptr;
            break;
          }
        }
      }

      if (InVal) {
        // If all incoming values for the new PHI would be the same, just don't
        // make a new PHI.  Instead, just remove the incoming values from the old
        // PHI.

        // NOTE! This loop walks backwards for a reason! First off, this minimizes
        // the cost of removal if we end up removing a large number of values, and
        // second off, this ensures that the indices for the incoming values
        // aren't invalidated when we remove one.
        for (int64_t i = PN->getNumIncomingValues() - 1; i >= 0; --i) {
          if (PredSet.count(PN->getIncomingBlock(i))) {
            PN->removeIncomingValue(i, false);
          }
        }

        // Add an incoming value to the PHI node in the loop for the preheader
        // edge.
        PN->addIncoming(InVal, NewBB);
        continue;
      }

      // If the values coming into the block are not the same, we need a new
      // PHI.
      // Create the new PHI node, insert it into NewBB at the end of the block
      PHINode *NewPHI =
          PHINode::Create(PN->getType(), Preds.size(), PN->getName() + ".ph", BI);

      // NOTE! This loop walks backwards for a reason! First off, this minimizes
      // the cost of removal if we end up removing a large number of values, and
      // second off, this ensures that the indices for the incoming values aren't
      // invalidated when we remove one.
      for (int64_t i = PN->getNumIncomingValues() - 1; i >= 0; --i) {
        BasicBlock *IncomingBB = PN->getIncomingBlock(i);
        if (PredSet.count(IncomingBB)) {
          Value *V = PN->removeIncomingValue(i, false);
          NewPHI->addIncoming(V, IncomingBB);
        }
      }

      PN->addIncoming(NewPHI, NewBB);
    }
  }

  BasicBlock *SplitPostDomPredecessors(BasicBlock *BB,
                                       ArrayRef<BasicBlock *> Preds,
                                       const char *Suffix,
                                       DominatorTree *DT, LoopInfo *LI,
                                       bool PreserveLCSSA) {
    // Do not attempt to split that which cannot be split.
    if (!BB->canSplitPredecessors()) {
      return nullptr;
    }

    // For the landingpads we need to act a bit differently.
    // Delegate this work to the SplitLandingPadPredecessors.
    if (BB->isLandingPad()) {
      errs() << "Code is not present for handling Landing pads yet. However, if needed it can be implemented. Aborted.\n";
      exit(1);
    }

    // Create new basic block, insert right before the original block.
    BasicBlock *NewBB = BasicBlock::Create(
        BB->getContext(), BB->getName() + Suffix, BB->getParent(), BB);

    // The new block unconditionally branches to the old block.
    BranchInst *BI = BranchInst::Create(BB, NewBB);
    // Splitting the predecessors of a loop header creates a preheader block.
    if (LI && LI->isLoopHeader(BB)) {
      // Using the loop start line number prevents debuggers stepping into the
      // loop body for this instruction.
      BI->setDebugLoc(LI->getLoopFor(BB)->getStartLoc());
    } else {
      BI->setDebugLoc(BB->getFirstNonPHIOrDbg()->getDebugLoc());
    }

    // Move the edges from Preds to point to NewBB instead of BB.
    for (unsigned i = 0, e = Preds.size(); i != e; ++i) {
      // This is slightly more strict than necessary; the minimum requirement
      // is that there be no more than one indirectbr branching to BB. And
      // all BlockAddress uses would need to be updated.
      assert(!isa<IndirectBrInst>(Preds[i]->getTerminator()) &&
             "Cannot split an edge from an IndirectBrInst");
      assert(!isa<CallBrInst>(Preds[i]->getTerminator()) &&
             "Cannot split an edge from a CallBrInst");
      Preds[i]->getTerminator()->replaceUsesOfWith(BB, NewBB);
    }

    // Insert a new PHI node into NewBB for every PHI node in BB and that new PHI
    // node becomes an incoming value for BB's phi node.  However, if the Preds
    // list is empty, we need to insert dummy entries into the PHI nodes in BB to
    // account for the newly created predecessor.
    if (Preds.empty()) {
      // Insert dummy values as the incoming value.
      for (BasicBlock::iterator I = BB->begin(); isa<PHINode>(I); ++I) {
        cast<PHINode>(I)->addIncoming(UndefValue::get(I->getType()), NewBB);
      }
    }

    // Update DominatorTree, LoopInfo, and LCCSA analysis information.
    bool HasLoopExit = false;
    UpdateAnalysisInformation(BB, NewBB, Preds, DT, LI, PreserveLCSSA,
                              HasLoopExit);

    if (!Preds.empty()) {
      // Update the PHI nodes in BB with the values coming from NewBB.
      UpdatePHINodes(BB, NewBB, Preds, BI, HasLoopExit);
    }

    return NewBB;
  }

  BasicBlock *SplitDomSuccessors(BasicBlock *OrigBB,
                                 ArrayRef<BasicBlock *> Succs,
                                 const char *Suffix1,
                                 DominatorTree *DT, LoopInfo *LI,
                                 bool PreserveLCSSA) {
    //assert(OrigBB->isLandingPad() && "Trying to split a non-landing pad!");

    // Create a new basic block for OrigBB's predecessors listed in Succs. Insert
    // it right before the original block.
    BasicBlock *NewBB1 = BasicBlock::Create(OrigBB->getContext(),
                                            OrigBB->getName() + Suffix1,
                                            OrigBB->getParent(), OrigBB);

    // The old block unconditionally branches to the new block.
    BranchInst *BI1 = BranchInst::Create(NewBB1, OrigBB);

    // Move the edges from Succs to point to NewBB1 instead of OrigBB.
    for (unsigned i = 0, e = Succs.size(); i != e; ++i) {
      // This is slightly more strict than necessary; the minimum requirement
      // is that there be no more than one indirectbr branching to BB. And
      // all BlockAddress uses would need to be updated.
      assert(!isa<IndirectBrInst>(Succs[i]->getTerminator()) &&
             "Cannot split an edge from an IndirectBrInst");
      Succs[i]->getTerminator()->replaceUsesOfWith(OrigBB, NewBB1);
    }
    BI1->setDebugLoc(OrigBB->getFirstNonPHI()->getDebugLoc());

    bool HasLoopExit = false;
    UpdateAnalysisInformation(OrigBB, NewBB1, Succs, DT, LI, PreserveLCSSA,
                              HasLoopExit);

    // Update the PHI nodes in OrigBB with the values coming from NewBB1.
    UpdatePHINodes(OrigBB, NewBB1, Succs, BI1, HasLoopExit);
    return NewBB1;
  }

  /* Check & create simple branch container */
  bool checkNCreateBranchLCC(LCCNode *currentLCC) {

    /*********************** Check for branch *************************/
    LCCNode *exitLCC = currentLCC->getInnerMostExitLCC();
    if (!exitLCC) {
      return false; /* when the current container is a invertedV container */
    }

    int numSuccLCC = currentLCC->getNumOfSuccLCC();                                                                                   /* Number of branches */
    assert((numSuccLCC == exitLCC->getNumOfSuccLCC()) && "Inner most exiting LCC & current LCC has different number of successors!"); /* Sanity check */
    if (numSuccLCC <= 1) {
      return false; /* Cannot be a branch */
    }

    auto exitBlock = (static_cast<UnitLCC *>(exitLCC))->getBlock();
    LCCNode *exitLCCForCheck = getLastLCCofBB(exitBlock);
    assert((exitLCC == exitLCCForCheck) && "exit LCC check failed"); /* Sanity check : only the last lcc of a basic block can have multiple branches coming out of it */

    //errs() << "checkNCreateBranchLCC(): checking for block " << exitBlock->getName() << "\n";

    auto termInst = exitBlock->getTerminator();
    if (!isa<BranchInst>(termInst) && !isa<SwitchInst>(termInst)) {
      if (!isa<UnreachableInst>(termInst) && !isa<ReturnInst>(termInst)) { /* TODO: add extra checks for fences that are instructions & not called functions like pthread_mutex_lock */
        errs() << "Unhandled instruction: " << *termInst << "\n";
        assert("This type of branching instruction is not handled");
      }
      /* This check is not valid since unreachable instruction may not have branches?? */
      else {
        return false; /* Its a fence */
      }
    } else { /* When its a proper branch */

      /* Check for a single entry single exit branch */
      DomTreeNode *currentPDNode = PDT->getNode(exitBlock);
      if (!currentPDNode) {
        return false;
      }
      DomTreeNode *postDomNode = currentPDNode->getIDom();
      if (!postDomNode) {
        return false;
      }
      BasicBlock *postDomBB = postDomNode->getBlock();
      if (!postDomBB) {
        return false;
      }

      DomTreeNode *postDomDNode = DT->getNode(postDomBB);
      if (!postDomDNode) {
        return false;
      }
      DomTreeNode *domNode = postDomDNode->getIDom();
      if (!domNode) {
        return false;
      }
      BasicBlock *domBB = domNode->getBlock();
      if (!domBB) {
        return false;
      }

      if (domBB != exitBlock) {
        return false; /* This is not a single entry single exit branch */
      }

      /* Check all the blocks belong to the same loop */
      auto L1 = LI->getLoopFor(domBB);
      auto L2 = LI->getLoopFor(postDomBB);
      if (L1 != L2) {
        return false;
      }

      /* Branch exit cannot be a loop header */
      if (LI->isLoopHeader(postDomBB)) {
        return false;
      }

      /* Branch cannot be the loop latch or exiting IR level branch of the enclosing loop 
         * Latch, although could have been handled. But the post dominator then will be the 
         * loop header, which might be tricky to instrument. */
      if (L1 && (L1->isLoopLatch(domBB) || L1->isLoopExiting(domBB))) {
        return false;
      }

      /* Sanity check */
      int numBranchSucc = termInst->getNumSuccessors();
      if (numSuccLCC != numBranchSucc) {
        errs() << "WARNING: Number of successor branches & containers should be same! This can happen when two cases of a switch point to the same code.\n";
        errs() << "#branches: " << numBranchSucc << ", #successors: " << numSuccLCC << "\n";
        auto succSetOfEntryLCC = currentLCC->getSuccSet();
        for (auto succIt = succSetOfEntryLCC.begin(); succIt != succSetOfEntryLCC.end(); succIt++) {
          errs() << "Succs are:- ";
          printUnitLCCSet(succIt->first);
          errs() << "\n";
        }
        /* will not handle this case */
        return false;
      }

      /* Check if there is at most one container between the dom container & postdom container */
      LCCNode *postDomUnitLCC = getFirstLCCofBB(postDomBB);
      if (!postDomUnitLCC) {
        errs() << "Post dom block whose LCC is not found: " << postDomBB->getName() << "\n";
      }

      LCCNode *postDomLCC = postDomUnitLCC->getOuterMostEnclosingLCC();

      auto succSetOfEntryLCC = currentLCC->getSuccSet();
      bool directEdge = false; /* True if at least one direct edge is present */
      double directEdgeProb = 0;
      std::map<LCCNode *, double> middleLCCInfo;

      /* Iterate over all the successors */
      for (auto succIt = succSetOfEntryLCC.begin(); succIt != succSetOfEntryLCC.end(); succIt++) {
        /* If it is a fence edge, or the successor is connected to multiple child LCCs of current LCC */
        auto succLCC = succIt->first;

        if (!currentLCC->isSimpleSuccEdge(succLCC, succIt->second)) {
          return false;
        }

        /* When the successor is the post dominator, it is a direct edge */
        if (succLCC == postDomLCC) {
          directEdge = true;
          BranchProbability bp = BPI->getEdgeProbability(domBB, postDomBB);
          uint32_t numeratorBP = bp.getNumerator();
          uint32_t denominatorBP = bp.getDenominator();
          directEdgeProb = ((double)numeratorBP / denominatorBP);
          continue;
        }

        auto succSetOfSuccLCC = succLCC->getSuccSet();

        /* The middle containers may not have been reduced yet */
        if (succSetOfSuccLCC.size() != 1) {
          return false;
        }

        /* The middle containers may not have been reduced yet */
        auto succLCCOfSucc = succSetOfSuccLCC.begin()->first;
        if (succLCCOfSucc != postDomLCC) {
          return false;
        }

        /* Check edge between middle container & post dom container */
        if (!succLCC->isSimpleSuccEdge(postDomLCC, succSetOfSuccLCC.begin()->second)) {
          return false;
        }

        auto succUnitLCC = succLCC->getInnerMostEntryLCC();
        BasicBlock *middleEnBlock = (static_cast<UnitLCC *>(succUnitLCC))->getBlock();
        BranchProbability bp = BPI->getEdgeProbability(domBB, middleEnBlock);
        uint32_t numeratorBP = bp.getNumerator();
        uint32_t denominatorBP = bp.getDenominator();
        double numBP = ((double)numeratorBP / denominatorBP);
        middleLCCInfo[succLCC] = numBP;
      }

      /********************* Create new container *********************/
      LCCNode *newLCC = new BranchLCC(lccIDGen++, currentLCC, postDomLCC, middleLCCInfo, directEdge, directEdgeProb, domBB, postDomBB, false);

      /******************** Create new connections *********************/
#if 0
      errs() << "\nBranch Container for test (" << newLCC->getID() << "):- ";
      errs() << "Entry LCC(" << currentLCC->getID() << "): (";
      printUnitLCCSet(currentLCC);
      errs() << "), Middle LCC( ";
      for (auto middleLCCIt = middleLCCInfo.begin(); middleLCCIt != middleLCCInfo.end(); middleLCCIt++) {
        printUnitLCCSet(middleLCCIt->first);
        errs() << "(" << middleLCCIt->first->getID() << ")\t";
      }
      errs() << "), Exit LCC(" << postDomLCC->getID() << "): (";
      printUnitLCCSet(postDomLCC);
      errs() << ")\n";
      auto predSet = currentLCC->getPredSet();
      errs() << "Preds entry LCC:-\n";
      for (auto predConnLCC : predSet) {
        auto connLCC = predConnLCC.first;
        errs() << "Conn LCC: ";
        printUnitLCCSet(connLCC);
        errs() << "\n";
        for (auto predLCC : predConnLCC.second) {
          errs() << "\t";
          printUnitLCCSet(predLCC);
          errs() << ": Its succ set:-\n";
          auto succSet = predLCC->getSuccSet();
          for (auto succConnLCC : succSet) {
            auto connSuccLCC = succConnLCC.first;
            errs() << "\t\tSucc Conn LCC: ";
            printUnitLCCSet(connSuccLCC);
            errs() << "\n";
            for (auto succLCC : succConnLCC.second) {
              errs() << "\t\t\t";
              printUnitLCCSet(succLCC);
              errs() << "\n";
            }
          }
        }
      }
#endif
      newLCC->makeNewSuccConnections(postDomLCC); /* succLCC is the exit LCC for newLCC */
      newLCC->makeNewPredConnections(currentLCC); /* currentLCC is the entry LCC for newLCC */

#ifdef ALL_DEBUG
      errs() << "\n\n\n*************************** Matched Branch header: ****************************\n";
      printUnitLCCSet(currentLCC);
      /* Iterate over all the successors */
      for (auto succIt = succSetOfEntryLCC.begin(); succIt != succSetOfEntryLCC.end(); succIt++) {
        /* If it is a fence edge, or the successor is connected to multiple child LCCs of current LCC */
        auto succLCC = succIt->first;
        errs() << "\nBranch succ: ";
        printUnitLCCSet(succLCC);
        if (succIt->second.size() == 1) {
          errs() << ", connected LCC: ";
          LCCNode *connLCC = *(succIt->second.begin());
          printUnitLCCSet(connLCC);
          errs() << "\n";
        }
      }
      errs() << "\n";

      errs() << "\nNew Succ for New Branch container: ";
      printUnitLCCSet(currentLCC);
      errs() << " --> ";
      auto newSuccSetOfEntryLCC = newLCC->getSuccSet();
      for (auto succIt = newSuccSetOfEntryLCC.begin(); succIt != newSuccSetOfEntryLCC.end(); succIt++) {
        auto succLCC = succIt->first;
        printUnitLCCSet(succLCC);
        errs() << "\t";
      }
      errs() << "\n";
#endif

      /******************** Update global LCC list *********************/
      auto F = currentLCC->getFunction();
      auto position = eraseFromGlobalLCCList(currentLCC);
      globalOuterLCCList[F].insert(position, newLCC);
#ifdef ALL_DEBUG
      errs() << "Adding ";
      printUnitLCCSet(newLCC);
      errs() << " to the global list of outer LCCs\n";
#endif
      eraseFromGlobalLCCList(postDomLCC);
      for (auto middleLCCIt = middleLCCInfo.begin(); middleLCCIt != middleLCCInfo.end(); middleLCCIt++) {
        eraseFromGlobalLCCList(middleLCCIt->first);
      }

      /************************ Test Printing **************************/
#ifdef LC_DEBUG
      errs() << "\nBranch Container(" << newLCC->getID() << "):- ";
      errs() << "Entry LCC(" << currentLCC->getID() << "): (";
      printUnitLCCSet(currentLCC);
      errs() << "), Middle LCC( ";
      for (auto middleLCCIt = middleLCCInfo.begin(); middleLCCIt != middleLCCInfo.end(); middleLCCIt++) {
        printUnitLCCSet(middleLCCIt->first);
        errs() << "(" << middleLCCIt->first->getID() << ")\t";
      }
      errs() << "), Exit LCC(" << postDomLCC->getID() << "): (";
      printUnitLCCSet(postDomLCC);
      errs() << ")\n";
#endif

      applycontrule2++;
      return true;
    }

    return false;
  }

  /* Check & create complex branch container */
  bool checkNCreateComplexBranchLCC(LCCNode *currentLCC) {

    /*********************** Check for branch *************************/
    LCCNode *exitLCC = currentLCC->getInnerMostExitLCC();
    if (!exitLCC) {
      return false; /* when the current container is a invertedV container */
    }

    int numSuccLCC = currentLCC->getNumOfSuccLCC();                                                                                   /* Number of branches */
    assert((numSuccLCC == exitLCC->getNumOfSuccLCC()) && "Inner most exiting LCC & current LCC has different number of successors!"); /* Sanity check */
    if (numSuccLCC <= 1) {
      return false; /* Cannot be a branch */
    }

    auto exitBlock = (static_cast<UnitLCC *>(exitLCC))->getBlock();
    LCCNode *exitLCCForCheck = getLastLCCofBB(exitBlock);
    assert((exitLCC == exitLCCForCheck) && "exit LCC check failed"); /* Sanity check : only the last lcc of a basic block can have multiple branches coming out of it */

    auto termInst = exitBlock->getTerminator();
    if (!isa<BranchInst>(termInst) && !isa<SwitchInst>(termInst)) {
      if (!isa<UnreachableInst>(termInst) && !isa<ReturnInst>(termInst)) { /* TODO: add extra checks for fences that are instructions & not called functions like pthread_mutex_lock */
        errs() << "Unhandled instruction: " << *termInst << "\n";
        assert("This type of branching instruction is not handled");
      }
      /* This check is not valid since unreachable instruction may not have branches?? */
      else {
        return false; /* Its a fence */
      }
    } else { /* When its a proper branch */

      /* Check for a single entry single exit branch */
      DomTreeNode *currentPDNode = PDT->getNode(exitBlock);
      if (!currentPDNode) {
        return false;
      }
      DomTreeNode *postDomNode = currentPDNode->getIDom();
      if (!postDomNode) {
        return false;
      }
      BasicBlock *postDomBB = postDomNode->getBlock();
      if (!postDomBB) {
        return false;
      }

      DomTreeNode *postDomDNode = DT->getNode(postDomBB);
      if (!postDomDNode) {
        return false;
      }
      DomTreeNode *domNode = postDomDNode->getIDom();
      if (!domNode) {
        return false;
      }
      BasicBlock *domBB = domNode->getBlock();
      if (!domBB) {
        return false;
      }

      if (domBB != exitBlock) {
        return false; /* This is not a single entry single exit branch */
      }

      /* Check all the blocks belong to the same loop */
      auto L1 = LI->getLoopFor(domBB);
      auto L2 = LI->getLoopFor(postDomBB);
      if (L1 != L2) {
        return false;
      }

      /* Branch exit cannot be a loop header */
      if (LI->isLoopHeader(postDomBB)) {
        return false;
      }

      /* Branch cannot be the loop latch or exiting IR level branch of the enclosing loop 
         * Latch, although could have been handled. But the post dominator then will be the 
         * loop header, which might be tricky to instrument. */
      if (L1 && (L1->isLoopLatch(domBB) || L1->isLoopExiting(domBB))) {
        return false;
      }

      /* Post dom cannot be a loop latch, otherwise it will end up looping back to the dom node since they are in the same loop. */
      if (L2 && L2->isLoopLatch(postDomBB)) {
        return false;
      }

      if (DT->dominates(postDomBB, domBB) || PDT->dominates(domBB, postDomBB)) {
        //errs() << "Dom: " << domBB->getName() << ", Post dom: " << postDomBB->getName() << " dominate or postdominate each other\n";
        return false;
      }

      /* Important condition for generic conditional structures */
      if (isPotentiallyReachable(postDomBB, domBB, DT, LI)) {
        return false;
      }

      /* Sanity check */
      int numBranchSucc = termInst->getNumSuccessors();
      if (numSuccLCC != numBranchSucc) {
        errs() << "WARNING: Number of successor branches & containers should be same! This can happen when two cases of a switch point to the same code.\n";
        errs() << "#branches: " << numBranchSucc << ", #successors: " << numSuccLCC << "\n";
        auto succSetOfEntryLCC = currentLCC->getSuccSet();
        for (auto succIt = succSetOfEntryLCC.begin(); succIt != succSetOfEntryLCC.end(); succIt++) {
          errs() << "Succs are:- ";
          printUnitLCCSet(succIt->first);
          errs() << "\n";
        }
        /* will not handle this case */
        return false;
      }

      /* Check if there is at most one container between the dom container & postdom container */
      LCCNode *postDomUnitLCC = getFirstLCCofBB(postDomBB);
      if (!postDomUnitLCC) {
        errs() << "Post dom block whose LCC is not found: " << postDomBB->getName() << "\n";
      }

      LCCNode *postDomLCC = postDomUnitLCC->getOuterMostEnclosingLCC();

      auto succSetOfEntryLCC = currentLCC->getSuccSet();
      std::map<std::list<LCCNode *> *, double> pathLCCInfo;
      std::list<LCCNode *> innerLCCs;
#ifdef ALL_DEBUG
      errs() << "\n\n\n*************************** Checking for Complex Branch header ****************************\n";
      errs() << "Entry LCC: ";
      printUnitLCCSet(currentLCC);
      errs() << ", Exit LCC: ";
      printUnitLCCSet(postDomLCC);
      errs() << "\n";
#endif
      bool canBeReduced = checkNgetChildLCCOfSESE(currentLCC, postDomLCC, &pathLCCInfo, &innerLCCs, L1);

      /* return if pattern is not matched */
      if (!canBeReduced) {
        return false;
      }

      /********************* Create new container *********************/
      LCCNode *newLCC = new ComplexBranchLCC(lccIDGen++, currentLCC, postDomLCC, pathLCCInfo, innerLCCs, domBB, postDomBB);

      /******************** Create new connections *********************/
      newLCC->makeNewSuccConnections(postDomLCC); /* succLCC is the exit LCC for newLCC */
      newLCC->makeNewPredConnections(currentLCC); /* currentLCC is the entry LCC for newLCC */

#ifdef ALL_DEBUG
      errs() << "\n\n\n*************************** Matched Complex Branch header ****************************\n";
      errs() << "Current LCC: ";
      printUnitLCCSet(currentLCC);
      errs() << "\n";
      /* Iterate over all the successors */
      for (auto succIt = succSetOfEntryLCC.begin(); succIt != succSetOfEntryLCC.end(); succIt++) {
        /* If it is a fence edge, or the successor is connected to multiple child LCCs of current LCC */
        auto succLCC = succIt->first;
        errs() << "\nBranch succ: ";
        printUnitLCCSet(succLCC);
        if (succIt->second.size() == 1) {
          errs() << ", connected LCC: ";
          LCCNode *connLCC = *(succIt->second.begin());
          printUnitLCCSet(connLCC);
          errs() << "\n";
        }
      }
      errs() << "\n";

      errs() << "\nNew Succ for New Branch container: ";
      printUnitLCCSet(currentLCC);
      errs() << " --> ";
      auto newSuccSetOfEntryLCC = newLCC->getSuccSet();
      for (auto succIt = newSuccSetOfEntryLCC.begin(); succIt != newSuccSetOfEntryLCC.end(); succIt++) {
        auto succLCC = succIt->first;
        printUnitLCCSet(succLCC);
        errs() << "\t";
      }
      errs() << "\n";
#endif

      /******************** Update global LCC list *********************/
      auto F = currentLCC->getFunction();
      auto position = eraseFromGlobalLCCList(currentLCC);
      globalOuterLCCList[F].insert(position, newLCC);
      eraseFromGlobalLCCList(postDomLCC);
      for (auto innerLCCIt = innerLCCs.begin(); innerLCCIt != innerLCCs.end(); innerLCCIt++) {
        eraseFromGlobalLCCList(*innerLCCIt);
      }
      /************************ Test Printing **************************/

#ifdef LC_DEBUG
      errs() << "\nComplex Branch Container(" << newLCC->getID() << "):- ";
      errs() << "Entry LCC(" << currentLCC->getID() << "): (";
      printUnitLCCSet(currentLCC);
      errs() << "), Inner LCCs( ";
      for (auto innerLCCIt = innerLCCs.begin(); innerLCCIt != innerLCCs.end(); innerLCCIt++) {
        printUnitLCCSet(*innerLCCIt);
        errs() << "(" << (*innerLCCIt)->getID() << ")\t";
      }
      errs() << "), Exit LCC(" << postDomLCC->getID() << "): (";
      printUnitLCCSet(postDomLCC);
      errs() << ")\n";

      errs() << "Printing path set of complex branch:-\n";
      for (auto path : pathLCCInfo) {
        errs() << "Path (prob: " << path.second << "): ";
        for (auto lcc : *(path.first)) {
          printUnitLCCSet(lcc);
          errs() << "(" << lcc->getID() << "), ";
        }
        errs() << "\n";
      }

      applycontrule7++;
#endif
      return true;
    }

    return false;
  }

  /* Check & create simple loop container */
  bool checkNCreateLoopLCC(LCCNode *currentLCC) {

    /************************ Check for loop **************************/

    /* Header of a loop will not be combined by any rule as an exiting container, until the loop has already been reduced */
    LCCNode *entryLCC = currentLCC->getInnerMostEntryLCC();
    if (!entryLCC) {
      return false; /* when the current container is a invertedV container */
    }
    auto entryBlock = (static_cast<UnitLCC *>(entryLCC))->getBlock();
    Loop *currentLoop = LI->getLoopFor(entryBlock);

    /* Proceed only if inside a loop */
    if (!currentLoop) {
      return false;
    }

    /* Proceed only if currentLCC is the header of a loop */
    if (!LI->isLoopHeader(entryBlock)) {
      return false;
    }

    /* Proceed only if entry block does not have a fence inside it */
    if (getNumLCCofBB(entryBlock) > 1) {
      return false; /* Header cannot have a fence inside */
    }

    BasicBlock *currLoopLatch = currentLoop->getLoopLatch();
    BasicBlock *currLoopExBlock = currentLoop->getExitingBlock();
    const SCEV *backEdgeTakenCount = SE->getBackedgeTakenCount(currentLoop);
    InstructionCost *simplifiedBackEdges = nullptr;

    /* Proceed only if loop is simple, that is, has single latch, and single exiting block */
    if (!currLoopLatch || !currLoopExBlock) {
      return false;
    }
    //errs() << entryBlock->getParent()->getName() << "(): Checking if simple for Loop " << *currentLoop << ". Latch: " << currLoopLatch->getName() << ", Exiting block: " << currLoopExBlock->getName() << "\n";

    BasicBlock *loopPredBB = currentLoop->getLoopPreheader();
    BasicBlock *loopSuccBB = currentLoop->getExitBlock();
    if (!loopSuccBB || !loopPredBB) {
#ifdef CRNT_DEBUG
      errs() << "Function: " << entryBlock->getParent()->getName() << ", loop: " << entryBlock->getName() << " has : \n";
      if (!loopPredBB)
        errs() << "\tmultiple predecessor\n";
      if (!loopSuccBB)
        errs() << "\tmultiple successor\n";
#endif
      return false;
    }

    assert(loopPredBB && "Loop having multiple predecessors require extra instrumentation. Support not present yet.");
    //assert(loopSuccBB && loopPredBB && "Loop having multiple successors & predecessors require extra instrumentation. Support not present yet.");

    LCCNode *loopPredUnitLCC = getLastLCCofBB(loopPredBB);
    //errs() << "Loop Pred Unit LCC (" << loopPredUnitLCC->getID() << "):- ";
    //printUnitLCCSet(loopPredUnitLCC);

    LCCNode *loopPredLCC = loopPredUnitLCC->getOuterMostEnclosingLCC();
    //errs() << " - Outer LCC (" << loopPredLCC->getID() << "): ";
    //printUnitLCCSet(loopPredLCC);
    //errs() << "\n";

    LCCNode *loopSuccUnitLCC = getFirstLCCofBB(loopSuccBB);
    //errs() << "Loop Succ Unit LCC (" << loopSuccUnitLCC->getID() << "):- ";
    //printUnitLCCSet(loopSuccUnitLCC);

    LCCNode *loopSuccLCC = loopSuccUnitLCC->getOuterMostEnclosingLCC();
    //errs() << " - Outer LCC (" << loopSuccLCC->getID() << "): ";
    //printUnitLCCSet(loopSuccLCC);
    //errs() << "\n";

    if (!loopPredBB || !loopSuccBB) {
      unhandled_loop++;
      return false;
    }

    /* if the loop has already been reduced */
    Loop *lccLoop = currentLCC->getLoop();
    if (lccLoop == currentLoop) {
      return false;
    }

    errs() << entryBlock->getParent()->getName() << "(): Attempting to create LCC for simple loop " << *currentLoop << ". Latch: " << currLoopLatch->getName() << ", Exiting block: " << currLoopExBlock->getName() << "\n";

    /* Find if the exiting block is co-located with the header block */
    bool isHeaderWithExitBlock = false;
    int loopType;
    bool loopBodyReduced = false;
    auto succOfHeaderLCC = currentLCC->getSuccSet();
    LCCNode *loopBodyLCC = nullptr;
    if (currentLoop->isLoopExiting(entryBlock)) {
      isHeaderWithExitBlock = true;
    }

    if (backEdgeTakenCount && (backEdgeTakenCount != SE->getCouldNotCompute())) {
      InstructionCost *backEdges = scevToCost(backEdgeTakenCount);
      simplifiedBackEdges = simplifyCost(currentLCC->getFunction(), backEdges, true);
    }

    if (isHeaderWithExitBlock) {
      loopType = LoopLCC::HEADER_COLOCATED_EXIT;

      /* Since the header is a branch statement, it couldn't have been reduced earlier */
      /* Trying to find the branch that goes inside the loop */
      if ((succOfHeaderLCC.size() != 1) && ((succOfHeaderLCC.size() != 2))) { /* 2 for self loop, 1 for others(as one latch) */
        return false;
      }

      for (auto succIt = succOfHeaderLCC.begin(); succIt != succOfHeaderLCC.end(); succIt++) {
        auto succLCC = succIt->first;
        LCCNode *succInnerLCC = succLCC->getInnerMostEntryLCC();
        if (!succInnerLCC) {
          return false; /* when the successor container is a invertedV container */
        }
        BasicBlock *succEnBlock = (static_cast<UnitLCC *>(succInnerLCC))->getBlock();

        /* Ignore the exiting block */
        if (!currentLoop->contains(succEnBlock)) {
          continue;
        }

        /* If it is a fence edge, or the successor is connected to multiple child LCCs of current LCC */
        if (!currentLCC->isSimpleSuccEdge(succLCC, succIt->second)) {
          return false;
        }

        /* Self loop */
        if (currentLCC == succLCC) {
          loopBodyReduced = true;
          loopType = LoopLCC::SELF_LOOP;

          break;
        }

        /* To consider the loop body as reduced, the successor of the loop body must be the header LCC, & there must not be any fence */
        LCCNode *succOfSuccLCC = succLCC->getSingleSuccWOFence();
        if (succOfSuccLCC && (succOfSuccLCC == currentLCC)) {
          loopBodyLCC = succLCC;
          loopBodyReduced = true;
          break;
        } else {
          return false;
        }
      }
    } else {
      loopType = LoopLCC::HEADER_NONCOLOCATED_EXIT;

      /* Since there is one latch & one exiting block, and header is not exiting, latch & exiting block must be the same. */
      /* Header & exiting block must be reduced inside one container, with one edge to itself & another outside loop */
      if (succOfHeaderLCC.size() != 2) {
        return 0;
      }

      for (auto succIt = succOfHeaderLCC.begin(); succIt != succOfHeaderLCC.end(); succIt++) {
        auto succLCC = succIt->first;
        LCCNode *succInnerLCC = succLCC->getInnerMostEntryLCC();
        if (!succInnerLCC) {
          return false; /* when the successor container is a invertedV container */
        }
        BasicBlock *succEnBlock = (static_cast<UnitLCC *>(succInnerLCC))->getBlock();

        /* Ignore the exiting block */
        if (!currentLoop->contains(succEnBlock)) {
          continue;
        }

        if (succLCC == currentLCC) {
          /* If it is a fence edge, or the successor is connected to multiple child LCCs of current LCC */
          if (!currentLCC->isSimpleSuccEdge(succLCC, succIt->second)) { /* currentLCC has self loop to succLCC */
            return false;
          }

          loopBodyReduced = true;
          break;
        }
      }
    }

    if (!loopBodyReduced) {
      return false;
    }

    /********************* Create new container **********************/
    LCCNode *newLCC = new LoopLCC(lccIDGen++, currentLCC, loopBodyLCC, loopPredLCC, loopSuccLCC, currentLoop, loopType, simplifiedBackEdges, isHeaderWithExitBlock, false);

    /******************** Create new connections *********************/
    newLCC->makeNewSuccConnections(loopSuccLCC); /* loopSuccLCC is the exit LCC for newLCC */
    newLCC->makeNewPredConnections(loopPredLCC); /* loopPredLCC is the entry LCC for newLCC */

    /******************** Update global LCC list *********************/
    auto F = currentLCC->getFunction();
    auto position = eraseFromGlobalLCCList(loopPredLCC);
    globalOuterLCCList[F].insert(position, newLCC);
    eraseFromGlobalLCCList(currentLCC);
    if (loopBodyLCC) {
      eraseFromGlobalLCCList(loopBodyLCC);
    }
    eraseFromGlobalLCCList(loopSuccLCC);

    /************************ Test Printing **************************/
#ifdef LC_DEBUG
    errs() << "\n\n\n************************** Matched Loop Header: ****************************\n";
    errs() << "\nLoop Container(" << newLCC->getID() << "):- ";
    errs() << "PreHeader LCC(" << loopPredLCC->getID() << "): (";
    printUnitLCCSet(loopPredLCC);
    errs() << "), Header LCC(" << currentLCC->getID() << " ): (";
    printUnitLCCSet(currentLCC);
    if (loopBodyLCC) {
      errs() << "), Body LCC(" << loopBodyLCC->getID() << "): (";
      printUnitLCCSet(loopBodyLCC);
    }
    errs() << "), PostExit LCC(" << loopSuccLCC->getID() << "): (";
    printUnitLCCSet(loopSuccLCC);
    errs() << "), New Loop LCC(";
    printUnitLCCSet(newLCC);
    errs() << ")\n";

    if (simplifiedBackEdges)
      errs() << ", Backedge: " << *simplifiedBackEdges << " [ Original SCEV Backedge : " << *backEdgeTakenCount << " ]\n";
    else {
      errs() << ", no simplified backedges\n";

      if (backEdgeTakenCount && (backEdgeTakenCount != SE->getCouldNotCompute())) {
        InstructionCost *backEdges = scevToCost(backEdgeTakenCount);
        errs() << "Unsimplified backedge: " << *backEdges << "\n[ Original SCEV Backedge: " << *backEdgeTakenCount << " ]\n";
      } else {
        if (backEdgeTakenCount)
          errs() << "The Backedge that could not be computed: " << *backEdgeTakenCount << "\n";
        else
          errs() << "No Backedge info is present to the IR\n";
      }
    }
#endif

    applycontrule3++;
    return true;
  }

  /* Check & create inverted V container demarcated by fences on the south side */
  bool checkNCreateInvertedVLCC(LCCNode *currentLCC) {

    /**************** Check for invertedV structure *******************/
    LCCNode *exitLCC = currentLCC->getInnerMostExitLCC();
    if (!exitLCC) {
      return false; /* when the current container is already an invertedV container */
    }
    int numSuccLCC = currentLCC->getNumOfSuccLCC();                                                                                   /* Number of branches */
    assert((numSuccLCC == exitLCC->getNumOfSuccLCC()) && "Inner most exiting LCC & current LCC has different number of successors!"); /* Sanity check */
    if (numSuccLCC == 1) {
      return false; /* Cannot be a branch */
    }

    auto exitBlock = (static_cast<UnitLCC *>(exitLCC))->getBlock();
    LCCNode *exitLCCForCheck = getLastLCCofBB(exitBlock);
    assert((exitLCC == exitLCCForCheck) && "exit LCC check failed"); /* Sanity check : only the last lcc of a basic block can have multiple branches coming out of it */

    auto termInst = exitBlock->getTerminator();
    if (!isa<BranchInst>(termInst) && !isa<SwitchInst>(termInst)) {
      if (!isa<UnreachableInst>(termInst) && !isa<ReturnInst>(termInst)) { /* TODO: add extra checks for fences that are instructions & not called functions like pthread_mutex_lock */
        errs() << "Unhandled instruction: " << *termInst << "\n";
        assert("This type of branching instruction is not handled");
      }
      /* This check is not valid since unreachable instruction may not have branches?? */
      else {
        return false; /* Its a fence */
      }
    } else { /* When its a proper branch */
      /* Get the postdominator block */
      DomTreeNode *currentPDNode = PDT->getNode(exitBlock);
      if (!currentPDNode) {
        return false;
      }
      DomTreeNode *postDomNode = currentPDNode->getIDom();
      if (!postDomNode) {
        return false;
      }
      BasicBlock *postDomBB = postDomNode->getBlock();
      if (!postDomBB) {
        return false;
      }

      /* Sanity check */
      int numBranchSucc = termInst->getNumSuccessors();
      assert((numSuccLCC == numBranchSucc) && "Number of successor branches & containers should be same!");

      auto succSetOfEntryLCC = currentLCC->getSuccSet();
      std::map<LCCNode *, double> childLCCInfo;
      auto L1 = LI->getLoopFor(exitBlock);

      /* Iterate over all the successors & check if every branch ends in a fence*/
      for (auto succIt = succSetOfEntryLCC.begin(); succIt != succSetOfEntryLCC.end(); succIt++) {

        auto succLCC = succIt->first;
        LCCNode *singlePred = succLCC->getSinglePred();

        /* if the successor has other predecessors, then it does not match the inverted V pattern */
        if (!singlePred) {
          return false;
        }
        if (!succLCC->getPredSingleConnLCC(singlePred)) {
          return false;
        }

        LCCNode *succUnitLCC = succLCC->getInnerMostEntryLCC();
        BasicBlock *succBlock = (static_cast<UnitLCC *>(succUnitLCC))->getBlock();
        /* Check header & successor blocks belong to the same loop */
        auto L2 = LI->getLoopFor(succBlock);
        if (L1 != L2) {
          return false;
        }

        /* Branch cannot be the loop latch or exiting IR level branch of the enclosing loop */
        if (L1 && L1->isLoopExiting(exitBlock)) {
          return false;
        }

        /* when the successor is connected via a fence */
        if (!currentLCC->isSimpleSuccEdge(succLCC, succIt->second)) {
          /* since the child is separated via fence, it will not be added in the LCC */
          continue;
        }
        /* Check if the successor has a fence to its successor */
        else {

          /* The successor is the post dominator, & there is no fence between them. We cannot traverse to the next level */
          if (postDomBB == succBlock) {
            return false;
          }

          auto succSetOfSuccLCC = succLCC->getSuccSet();

          /* The child containers may not have been reduced yet */
          if (succSetOfSuccLCC.size() != 1) {
            return false;
          }

          /* Check edge between child container & the grand child */
          if (!succLCC->isSimpleSuccEdge(succLCC, succSetOfSuccLCC.begin()->second)) {
            return false;
          }

          /* The successor is followed by a fence. So its added to the container. */
          BranchProbability bp = BPI->getEdgeProbability(exitBlock, succBlock);
          uint32_t numeratorBP = bp.getNumerator();
          uint32_t denominatorBP = bp.getDenominator();
          double numBP = ((double)numeratorBP / denominatorBP);
          childLCCInfo[succLCC] = numBP;
        }
      }

      /********************* Create new container *********************/
      LCCNode *newLCC = new InvertedVLCC(lccIDGen++, currentLCC, childLCCInfo);

      /******************** Create new connections *********************/
      newLCC->makeNewPredConnections(currentLCC); /* currentLCC is the entry LCC for newLCC */
      for (auto childIt = childLCCInfo.begin(); childIt != childLCCInfo.end(); childIt++) {
        auto succLCC = childIt->first;
        newLCC->makeNewSuccConnections(succLCC); /* they are already supposed to have fences after them */
      }

      /******************** Update global LCC list *********************/
      auto F = currentLCC->getFunction();
      auto position = eraseFromGlobalLCCList(currentLCC);
      globalOuterLCCList[F].insert(position, newLCC);
      for (auto childLCCIt = childLCCInfo.begin(); childLCCIt != childLCCInfo.end(); childLCCIt++) {
        eraseFromGlobalLCCList(childLCCIt->first);
      }

      return true;
    }

    return false;
  }

  /* Check & create inverted V container demarcated by fences on the south side */
  bool checkNCreateVLCC(LCCNode *currentLCC) {

    /**************** Check for invertedV structure *******************/
    LCCNode *exitLCC = currentLCC->getInnerMostExitLCC();
    if (!exitLCC) {
      return false; /* when the current container is already an invertedV container */
    }
    int numSuccLCC = currentLCC->getNumOfSuccLCC();                                                                                   /* Number of branches */
    assert((numSuccLCC == exitLCC->getNumOfSuccLCC()) && "Inner most exiting LCC & current LCC has different number of successors!"); /* Sanity check */
    if (numSuccLCC == 1) {
      return false; /* Cannot be a branch */
    }

    auto exitBlock = (static_cast<UnitLCC *>(exitLCC))->getBlock();
    LCCNode *exitLCCForCheck = getLastLCCofBB(exitBlock);
    assert((exitLCC == exitLCCForCheck) && "exit LCC check failed"); /* Sanity check : only the last lcc of a basic block can have multiple branches coming out of it */

    auto termInst = exitBlock->getTerminator();
    if (!isa<BranchInst>(termInst) && !isa<SwitchInst>(termInst)) {
      if (!isa<UnreachableInst>(termInst) && !isa<ReturnInst>(termInst)) { /* TODO: add extra checks for fences that are instructions & not called functions like pthread_mutex_lock */
        errs() << "Unhandled instruction: " << *termInst << "\n";
        assert("This type of branching instruction is not handled");
      }
      /* This check is not valid since unreachable instruction may not have branches?? */
      else {
        return false; /* Its a fence */
      }
    } else { /* When its a proper branch */
      /* Get the postdominator block */
      DomTreeNode *currentPDNode = PDT->getNode(exitBlock);
      if (!currentPDNode) {
        return false;
      }
      DomTreeNode *postDomNode = currentPDNode->getIDom();
      if (!postDomNode) {
        return false;
      }
      BasicBlock *postDomBB = postDomNode->getBlock();
      if (!postDomBB) {
        return false;
      }

      /* Sanity check */
      int numBranchSucc = termInst->getNumSuccessors();
      assert((numSuccLCC == numBranchSucc) && "Number of successor branches & containers should be same!");

      auto succSetOfEntryLCC = currentLCC->getSuccSet();
      std::map<LCCNode *, double> childLCCInfo;
      auto L1 = LI->getLoopFor(exitBlock);

      /* Iterate over all the successors & check if every branch ends in a fence*/
      for (auto succIt = succSetOfEntryLCC.begin(); succIt != succSetOfEntryLCC.end(); succIt++) {

        auto succLCC = succIt->first;
        LCCNode *singlePred = succLCC->getSinglePred();

        /* if the successor has other predecessors, then it does not match the inverted V pattern */
        if (!singlePred) {
          return false;
        }
        if (!succLCC->getPredSingleConnLCC(singlePred)) {
          return false;
        }

        LCCNode *succUnitLCC = succLCC->getInnerMostEntryLCC();
        BasicBlock *succBlock = (static_cast<UnitLCC *>(succUnitLCC))->getBlock();
        /* Check header & successor blocks belong to the same loop */
        auto L2 = LI->getLoopFor(succBlock);
        if (L1 != L2) {
          return false;
        }

        /* Branch cannot be the loop latch or exiting IR level branch of the enclosing loop */
        if (L1 && L1->isLoopExiting(exitBlock)) {
          return false;
        }

        /* when the successor is connected via a fence */
        if (!currentLCC->isSimpleSuccEdge(succLCC, succIt->second)) {
          /* since the child is separated via fence, it will not be added in the LCC */
          continue;
        }
        /* Check if the successor has a fence to its successor */
        else {

          /* The successor is the post dominator, & there is no fence between them. We cannot traverse to the next level */
          if (postDomBB == succBlock) {
            return false;
          }

          auto succSetOfSuccLCC = succLCC->getSuccSet();

          /* The child containers may not have been reduced yet */
          if (succSetOfSuccLCC.size() != 1) {
            return false;
          }

          /* Check edge between child container & the grand child */
          if (!succLCC->isSimpleSuccEdge(succLCC, succSetOfSuccLCC.begin()->second)) {
            return false;
          }

          /* The successor is followed by a fence. So its added to the container. */
          BranchProbability bp = BPI->getEdgeProbability(exitBlock, succBlock);
          uint32_t numeratorBP = bp.getNumerator();
          uint32_t denominatorBP = bp.getDenominator();
          double numBP = ((double)numeratorBP / denominatorBP);
          childLCCInfo[succLCC] = numBP;
        }
      }

      /********************* Create new container *********************/
      LCCNode *newLCC = new InvertedVLCC(lccIDGen++, currentLCC, childLCCInfo);

      /******************** Create new connections *********************/
      newLCC->makeNewPredConnections(newLCC); /* currentLCC is the entry LCC for newLCC */
      for (auto childIt = childLCCInfo.begin(); childIt != childLCCInfo.end(); childIt++) {
        auto succLCC = childIt->first;
        newLCC->makeNewSuccConnections(succLCC); /* they are already supposed to have fences after them */
      }

      /******************** Update global LCC list *********************/
      auto F = currentLCC->getFunction();
      auto position = eraseFromGlobalLCCList(currentLCC);
      globalOuterLCCList[F].insert(position, newLCC);
      for (auto childLCCIt = childLCCInfo.begin(); childLCCIt != childLCCInfo.end(); childLCCIt++) {
        eraseFromGlobalLCCList(childLCCIt->first);
      }

      return true;
    }

    return false;
  }

  /* if there is a fence inside the loop, this will return false */
  bool getChildrenOfLoop(LCCNode *currLCC, Loop *currLoop, std::set<LCCNode *> childSet) {

    /* if currLCC is not supplied, means this is the starting call & it should point to the loop header */
    auto succSetOfEntryLCC = currLCC->getSuccSet();
    for (auto succIt = succSetOfEntryLCC.begin(); succIt != succSetOfEntryLCC.end(); succIt++) {
      /* If it is a fence edge, or the successor is connected to multiple child LCCs of current LCC */
      auto succLCC = succIt->first;
      LCCNode *innerSuccEnLCC = succLCC->getInnerMostEntryLCC();
      LCCNode *innerSuccExLCC = succLCC->getInnerMostExitLCC();
      LCCNode *oneInnerSuccEnLCC = succLCC->getOneInnerMostEntryLCC();
      BasicBlock *succBlock;
      if (innerSuccEnLCC) {
        succBlock = (static_cast<UnitLCC *>(innerSuccEnLCC))->getBlock();
      } else if (innerSuccExLCC) {
        succBlock = (static_cast<UnitLCC *>(innerSuccExLCC))->getBlock();
      } else if (oneInnerSuccEnLCC) {
        succBlock = (static_cast<UnitLCC *>(oneInnerSuccEnLCC))->getBlock();
      } else {
        assert("Container should have at least one entry LCC");
      }

      /* This successor is outside the loop, so will not be added to children */
      if (!currLoop->contains(succBlock)) {
        continue;
      }

      /* succLCC is a child of the loop */

      if (!currLCC->isSimpleSuccEdge(succLCC, succIt->second)) {
        return false;
      }

      auto found = childSet.count(succLCC);
      if (found) {
        continue;
      } else {
        childSet.insert(succLCC);
        bool canBeReduced = getChildrenOfLoop(succLCC, currLoop, childSet);
        if (!canBeReduced) {
          return canBeReduced;
        }
      }
    }
    return true;
  }

  /* direction = 1 means traverse forward from start to end, = 0 means traverse backward from start to end */
  /* the segment in question must not have loops back to it */
  /* checks if startBB dominates everything on the path to endBB, except endBB */
  bool DFSCheckForComplexBr(BasicBlock *startBB, BasicBlock *endBB, bool direction, BasicBlock *currentBB, std::list<BasicBlock *> *blocksTraversed = nullptr) {

    if (!blocksTraversed) {
      blocksTraversed = new std::list<BasicBlock *>();
    }

    /* forward direction */
    if (direction) {
      for (auto succIt = succ_begin(currentBB), succEnd = succ_end(currentBB); succIt != succEnd; ++succIt) {
        BasicBlock *succBB = *succIt;

        /* Check if node has been processed already */
        auto foundIt = find(blocksTraversed->begin(), blocksTraversed->end(), succBB);
        if (foundIt != blocksTraversed->end()) {
          continue;
        }
        blocksTraversed->push_back(succBB);

        //errs() << "Current Block: " << currentBB->getName() << ", Succ Block: " << succBB->getName() << "\n";
        if (succBB == endBB) {
          continue;
        } else {
          if (!DT->dominates(startBB, succBB)) {
            return false;
          }
        }
        bool res = DFSCheckForComplexBr(startBB, endBB, direction, succBB, blocksTraversed);
        if (!res) {
          return false;
        }
      }
    }
    /* backward direction */
    else {
      for (auto predIt = pred_begin(currentBB), predEnd = pred_end(currentBB); predIt != predEnd; ++predIt) {
        BasicBlock *predBB = *predIt;

        /* Check if node has been processed already */
        auto foundIt = find(blocksTraversed->begin(), blocksTraversed->end(), predBB);
        if (foundIt != blocksTraversed->end()) {
          continue;
        }
        blocksTraversed->push_back(predBB);

        //errs() << "Current Block: " << currentBB->getName() << ", Succ Block: " << predBB->getName() << "\n";
        if (predBB == endBB) {
          continue;
        } else {
          if (!PDT->dominates(startBB, predBB)) {
            return false;
          }
        }
        bool res = DFSCheckForComplexBr(startBB, endBB, direction, predBB, blocksTraversed);
        if (!res) {
          return false;
        }
      }
    }
    return true;
  }

  bool checkNgetChildLCCOfSESE(LCCNode *domLCC, LCCNode *postDomLCC, std::map<std::list<LCCNode *> *, double> *pathSet, std::list<LCCNode *> *childSet, Loop *L, LCCNode *currLCC = nullptr, std::list<LCCNode *> *currPath = nullptr) {
    LCCNode *innerDomLCC = domLCC->getInnerMostEntryLCC();
    LCCNode *innerPostDomLCC = postDomLCC->getInnerMostEntryLCC();
    BasicBlock *domBB = (static_cast<UnitLCC *>(innerDomLCC))->getBlock();
    BasicBlock *postDomBB = (static_cast<UnitLCC *>(innerPostDomLCC))->getBlock();
    bool firstSucc = true; // first successor of the currentLCC - used for forking new paths for multiple successors
    std::list<LCCNode *> *pathHeader = nullptr;
    std::list<LCCNode *> *copyPathTillNow = nullptr;
    double copyBP = 0;

    auto pathInPathSet = pathSet->find(currPath);
    if (pathInPathSet != pathSet->end()) {
      copyBP = pathInPathSet->second;
    }

    /* if currLCC is not supplied, means this is the starting call & it should point to the dominator LCC */
    if (!currLCC) {
      currLCC = domLCC;
      /* no currPath should be present yet */
    } else {
      /* Keep a copy of the path till here, in case it has multiple successors, & new path has to be forked with the same initial prefix */
      copyPathTillNow = new std::list<LCCNode *>(*currPath);
    }

    LCCNode *innerCurrExLCC = currLCC->getInnerMostExitLCC();
    BasicBlock *currBlock = nullptr;
    if (innerCurrExLCC) {
      currBlock = (static_cast<UnitLCC *>(innerCurrExLCC))->getBlock();
    } else {
      assert("Current container should have at least one entry LCC");
    }

    auto succSetOfEntryLCC = currLCC->getSuccSet();
    for (auto succIt = succSetOfEntryLCC.begin(); succIt != succSetOfEntryLCC.end(); succIt++) {

      //LCCNode* innerSuccEnLCC = succLCC->getInnerMostEntryLCC();
      //LCCNode* oneInnerSuccEnLCC = succLCC->getOneInnerMostEntryLCC();
      auto succLCC = succIt->first;
      LCCNode *innerSuccExLCC = succLCC->getInnerMostExitLCC();
      BasicBlock *succBlock;
      /*
        if(innerSuccEnLCC) 
          succBlock = (static_cast<UnitLCC*>(innerSuccEnLCC))->getBlock();
        else if(oneInnerSuccEnLCC) {
          succBlock = (static_cast<UnitLCC*>(oneInnerSuccEnLCC))->getBlock();
        }
        else 
*/

      if (innerSuccExLCC) {
        succBlock = (static_cast<UnitLCC *>(innerSuccExLCC))->getBlock();
      } else {
        assert("Successor container should have at least one entry LCC");
      }

      auto L1 = LI->getLoopFor(succBlock);
      /* containers belong to different loops */
      if (L != L1) {
        return false;
      }

      if (!DT->dominates(domBB, succBlock) || !PDT->dominates(postDomBB, succBlock)) {
        assert("This is not right. The block is not dominated or postdominated by the right containers.");
      }

#if 1
      /* allow only unitLCCs as successor inner layer containers */
      if (!succLCC->isUnitLCC()) {
        //errs() << "Child LCCs in this SESE region are not unitLCCs!\n";
        return false;
      } else {
        //errs() << "Succ in complex branch: " << succBlock->getName() << " from branch starting at " << currBlock->getName() << "\n";
      }
#endif

      /* If it is a fence edge, or the successor is connected to multiple child LCCs of current LCC */
      if (!currLCC->isSimpleSuccEdge(succLCC, succIt->second)) {
        return false;
      }

      BranchProbability bp = BPI->getEdgeProbability(currBlock, succBlock);
      uint32_t numeratorBP = bp.getNumerator();
      uint32_t denominatorBP = bp.getDenominator();
      double numBP = ((double)numeratorBP / denominatorBP);

      /* first call to recursive function - add a new path for every successor */
      if (domLCC == currLCC) {
        //errs() << "In first layer with curr "; printUnitLCCSet(currLCC); errs() << ", succ "; printUnitLCCSet(succLCC); errs() << "\n";
        assert((!currPath) && "currPath must be null since this is the first call to the recursive function!");
        pathHeader = new std::list<LCCNode *>;
        pathHeader->push_back(domLCC);     // added once for every path
        pathHeader->push_back(postDomLCC); // added once for every path
        (*pathSet)[pathHeader] = numBP;    // for paths created at the top level
        //errs() << "Created top layer new path with prob: " << numBP << "\n";
      } else {
        /* for every first successor of second layer onwards */
        if (firstSucc) { // only needed for subsequent recursive calls
          //errs() << "In second layer, first succ: with curr "; printUnitLCCSet(currLCC); errs() << ", succ "; printUnitLCCSet(succLCC); errs() << "\n";
          firstSucc = false;
          pathHeader = currPath; // append nodes to this path only
          auto oldBP = (*pathSet)[pathHeader];
          auto newBP = oldBP * copyBP;
          (*pathSet)[pathHeader] = newBP;
          //errs() << "Updated path with prob: " << newBP << " from " << oldBP << "\n";
        } else {
          /* for all 2nd layer second successor onwards */
#if 0
          //errs() << "In second layer, second succ: with curr "; printUnitLCCSet(currLCC); errs() << ", succ "; printUnitLCCSet(succLCC); errs() << "\n";
          /* Debug prints */
          errs() << "Copy Path till now for second layer second node onwards. curr node " << currBlock->getName() << ":\n";
          for (auto path : *copyPathTillNow) {
            printUnitLCCSet(path);
            errs() << ", ";
          }
          errs() << "\n";
#endif

          pathHeader = new std::list<LCCNode *>(*copyPathTillNow); // because the currPath may have extended beyond the parent node
          numBP *= copyBP;                                         /* calculating the overall probability from the percentage of the parent path's probability */
          (*pathSet)[pathHeader] = numBP;
          //errs() << "Created second layer new path with prob: " << numBP << "(branch BP was: " << copyBP << ")\n";
        }
      }

      /* childSet should not include the dom or postdom, pathset already includes it when it was created . */
      if (succLCC == postDomLCC) {
        continue;
      } else {
        if (childSet->end() == std::find(childSet->begin(), childSet->end(), succLCC)) {
          childSet->push_back(succLCC);
          childSet->unique();
        }
        pathHeader->push_back(succLCC);
        pathHeader->unique();
#if 0
        errs() << "Adding ";
        printUnitLCCSet(succLCC);
        errs() << " to path\n";
#endif
        bool canBeReduced = checkNgetChildLCCOfSESE(domLCC, postDomLCC, pathSet, childSet, L, succLCC, pathHeader);
        if (!canBeReduced) {
          //errs() << "Branch at " << domBB->getName() << " cannot be reduced into a complex branch with simple elements. Returning false.\n";
          return canBeReduced;
        }
      }
    }

    if (currLCC == domLCC) {
      childSet->unique();

#if 0
      errs() << "Printing path set for first recursive call:-\n";
      for (auto path : *pathSet) {
        errs() << "New path (prob: " << path.second << ") ";
        for (auto lcc : *(path.first)) {
          printUnitLCCSet(lcc);
          errs() << ", ";
        }
        errs() << "\n";
      }
#endif
    }
    return true;
  }

  /* if there is a fence inside the branch, this will return false */
  bool getChildLCCOfSESE(LCCNode *currLCC, LCCNode *domLCC, LCCNode *postDomLCC, std::set<LCCNode *> childSet) {
    LCCNode *innerDomLCC = domLCC->getInnerMostEntryLCC();
    LCCNode *innerPostDomLCC = postDomLCC->getInnerMostEntryLCC();
    BasicBlock *domBB = (static_cast<UnitLCC *>(innerDomLCC))->getBlock();
    BasicBlock *postDomBB = (static_cast<UnitLCC *>(innerPostDomLCC))->getBlock();

    /* if currLCC is not supplied, means this is the starting call & it should point to the dominator LCC */
    if (!currLCC) {
      currLCC = domLCC;
    }

    auto succSetOfEntryLCC = currLCC->getSuccSet();
    for (auto succIt = succSetOfEntryLCC.begin(); succIt != succSetOfEntryLCC.end(); succIt++) {
      /* If it is a fence edge, or the successor is connected to multiple child LCCs of current LCC */
      auto succLCC = succIt->first;
      if (!currLCC->isSimpleSuccEdge(succLCC, succIt->second)) {
        return false;
      }
      auto found = childSet.count(succLCC);
      if (found) {
        continue;
        /* postDomLCC should not be added to child set */
      } else if (succLCC == postDomLCC) {
        continue;
      } else {
        LCCNode *innerSuccEnLCC = succLCC->getInnerMostEntryLCC();
        LCCNode *innerSuccExLCC = succLCC->getInnerMostExitLCC();
        LCCNode *oneInnerSuccEnLCC = succLCC->getOneInnerMostEntryLCC();
        BasicBlock *succBlock;
        if (innerSuccEnLCC) {
          succBlock = (static_cast<UnitLCC *>(innerSuccEnLCC))->getBlock();
        } else if (innerSuccExLCC) {
          succBlock = (static_cast<UnitLCC *>(innerSuccExLCC))->getBlock();
        } else if (oneInnerSuccEnLCC) {
          succBlock = (static_cast<UnitLCC *>(oneInnerSuccEnLCC))->getBlock();
        } else {
          assert("Container should have at least one entry LCC");
        }

        if (!DT->dominates(domBB, succBlock) || !PDT->dominates(postDomBB, succBlock)) {
          assert("This is not right. The block is not dominated or postdominated by the right containers.");
        }

        childSet.insert(succLCC);
        bool canBeReduced = getChildLCCOfSESE(succLCC, domLCC, postDomLCC, childSet);
        if (!canBeReduced) {
          return canBeReduced;
        }
      }
    }
    return true;
  }

  /* Check & create minimal LCCs starting from this, when all the rules have failed to make a change 
     * Recursive function. Enclosing loops & postDoms are passes if present. 
     * Returns inner containers if found. */
  bool checkForMinimalLCCs(LCCNode *currentLCC, Loop *enLoop, BasicBlock *enDom, BasicBlock *enPostDom) {
    auto lccType = currentLCC->getType();
    std::set<BasicBlock *> blocksToCheck;
    BasicBlock *exitBlock = nullptr;  /* For finding branch regions */
    BasicBlock *entryBlock = nullptr; /* For finding loop regions */
    bool found = false;

    /* For each type of container, get the list of blocks for checking to be part of the 
       * enclosing region, & find the exiting container which will be used for matching 
       * unknown rule entry points */
    switch (lccType) {
    case LCCNode::UNIT_LCC: {
      BasicBlock *currBlock = (static_cast<UnitLCC *>(currentLCC))->getBlock();
      blocksToCheck.insert(currBlock);
      exitBlock = currBlock;
      break;
    }
    case LCCNode::PATH_LCC:
    case LCCNode::BRANCH_LCC:
    case LCCNode::LOOP_LCC:
    case LCCNode::UNKNOWN_LCC: {
      LCCNode *entryLCC = currentLCC->getInnerMostEntryLCC();
      LCCNode *exitLCC = currentLCC->getInnerMostExitLCC();
      BasicBlock *enBlock = nullptr, *exBlock = nullptr;
      if (entryLCC) {
        enBlock = (static_cast<UnitLCC *>(entryLCC))->getBlock();
        blocksToCheck.insert(enBlock);
        entryBlock = enBlock;
      }
      if (exitLCC) {
        exBlock = (static_cast<UnitLCC *>(exitLCC))->getBlock();
        blocksToCheck.insert(exBlock);
        exitBlock = exBlock;
      }
      break;
    }
    case LCCNode::INVERTEDV_LCC: {
      LCCNode *entryLCC = currentLCC->getInnerMostEntryLCC();
      auto exitLCCs = currentLCC->getAllInnerMostExitLCC();
      BasicBlock *enBlock = nullptr, *exBlock = nullptr;
      if (entryLCC) {
        enBlock = (static_cast<UnitLCC *>(entryLCC))->getBlock();
        blocksToCheck.insert(enBlock);
        entryBlock = enBlock;
      }
      for (auto exitLCCIt = exitLCCs.begin(); exitLCCIt != exitLCCs.end(); exitLCCIt++) {
        LCCNode *exitLCC = *exitLCCIt;
        exBlock = (static_cast<UnitLCC *>(exitLCC))->getBlock();
        blocksToCheck.insert(exBlock);
        exitBlock = nullptr; /* since it has a fence after it */
      }
      break;
    }
    case LCCNode::V_LCC: {
      auto entryLCCs = currentLCC->getAllInnerMostEntryLCC();
      LCCNode *exitLCC = currentLCC->getInnerMostExitLCC();
      BasicBlock *enBlock = nullptr, *exBlock = nullptr;
      for (auto entryLCCIt = entryLCCs.begin(); entryLCCIt != entryLCCs.end(); entryLCCIt++) {
        LCCNode *entryLCC = *entryLCCIt;
        enBlock = (static_cast<UnitLCC *>(entryLCC))->getBlock();
        blocksToCheck.insert(enBlock);
        entryBlock = nullptr;
      }
      if (exitLCC) {
        exBlock = (static_cast<UnitLCC *>(exitLCC))->getBlock();
        blocksToCheck.insert(exBlock);
        exitBlock = exBlock;
      }
      break;
    }
    default:
      assert("This lcc type does not exist!");
    }

    /* current block must be dominated & postdominated by the enclosing doms & postdoms, 
       * and must be a part of the enclosing loop. Otherwise return nullptr. */
    for (auto blockIt = blocksToCheck.begin(); blockIt != blocksToCheck.end(); blockIt++) {
      BasicBlock *currBlock = *blockIt;
      if (enLoop) {
        if (!enLoop->contains(currBlock)) {
          return false;
        }
      }

      if (enDom) {
        if (!DT->dominates(enDom, currBlock)) {
          return false;
        }
      }

      if (enPostDom) {
        if (!PDT->dominates(enPostDom, currBlock)) {
          return false;
        }
      }
    }

    /* Container is delimited by a fence. Loop region cannot be checked as it will contain a fence */
    if (!exitBlock) {
      return false;
    }

    /* exitBlock is the entry point of the unknown branch region to match */
    Loop *currLoop = LI->getLoopFor(exitBlock);
    DomTreeNode *currentPDNode = nullptr;
    DomTreeNode *postDomNode = nullptr;
    DomTreeNode *postDomDNode = nullptr;
    DomTreeNode *domNode = nullptr;
    BasicBlock *domBB = nullptr;
    BasicBlock *postDomBB = nullptr;
    LCCNode *newLCC;
    LCCNode *postDomLCC;
    std::set<LCCNode *> childLCCs;

    /* Check for an enclosed branch inside the current loop */
    currentPDNode = PDT->getNode(exitBlock);
    if (currentPDNode) {
      postDomNode = currentPDNode->getIDom();
      if (postDomNode) {
        postDomBB = postDomNode->getBlock();
        if (postDomBB) {
          postDomDNode = DT->getNode(postDomBB);
          if (postDomDNode) {
            domNode = postDomDNode->getIDom();
            if (domNode) {
              domBB = domNode->getBlock();
              /* This is a enclosed single entry single exit region */
              if (domBB) {
                /* This is the entry point of the single entry single exit region */
                if (domBB == exitBlock) {
                  Loop *postDomLoop = LI->getLoopFor(postDomBB);
                  /************* This is a branch contained entirely inside a loop that can be potentially reduced **************/
                  if (postDomLoop == currLoop) {
                    auto succSetOfEntryLCC = currentLCC->getSuccSet();
                    for (auto succIt = succSetOfEntryLCC.begin(); succIt != succSetOfEntryLCC.end(); succIt++) {
                      bool matched = checkForMinimalLCCs(succIt->first, currLoop, domBB, postDomBB);
                      if (matched) {
                        return true;
                      } else {
                        LCCNode *postDomUnitLCC = getFirstLCCofBB(postDomBB);
                        LCCNode *postDomLCC = postDomUnitLCC->getOuterMostEnclosingLCC();
                        /* Find all LCCs on the path to postdom LCC & create new LCC */
                        bool canBeReduced = getChildLCCOfSESE(nullptr, currentLCC, postDomLCC, childLCCs);
                        /* Unknown branch region has a fence */
                        if (!canBeReduced) {
                          return false;
                        } else {
                          newLCC = new UnknownLCC(lccIDGen++, currentLCC, childLCCs, postDomLCC, nullptr);
                          found = true;
                        }
                      }
                    }
                  }
                } else if (DT->dominates(domBB, exitBlock)) {
                  return false;
                } else if (DT->dominates(exitBlock, domBB)) {
                  assert("This cannot happen since everything from current block goes out through post dom block");
                } else {
                  assert("This is an invalid option!");
                }
              }
            }
          }
        }
      }
    }

    /* Container is delimited by a fence on the top. Cannot be the header of a loop */
    if (!entryBlock) {
      return false;
    }

    /* entryBlock is the entry point of the unknown loop region to match */
    /* Check for an enclosed branch inside the current loop */
    if (!found) {
      currentPDNode = PDT->getNode(entryBlock);
      if (currentPDNode) {
        postDomNode = currentPDNode->getIDom();
        if (postDomNode) {
          postDomBB = postDomNode->getBlock();
          if (postDomBB) {
            postDomDNode = DT->getNode(postDomBB);
            if (postDomDNode) {
              domNode = postDomDNode->getIDom();
              if (domNode) {
                domBB = domNode->getBlock();
                /* This is a enclosed single entry single exit region */
                if (domBB) {
                  /* This is the entry point of the single entry single exit region */
                  if (domBB == entryBlock) {
                    /* currLoop must exist if this block is a loop header */
                    bool isHeader = LI->isLoopHeader(entryBlock);
                    if (isHeader) {
                      BasicBlock *currLoopLatch = currLoop->getLoopLatch();
                      BasicBlock *currLoopExBlock = currLoop->getExitingBlock();
                      /* This is an enclosed loop region */
                      if (currLoopLatch && currLoopExBlock) {
                        LCCNode *postDomUnitLCC = getFirstLCCofBB(postDomBB);
                        postDomLCC = postDomUnitLCC->getOuterMostEnclosingLCC();
                        bool canBeReduced = getChildrenOfLoop(currentLCC, currLoop, childLCCs);
                        /* if loop had a fence inside it, this loop cannot be reduced */
                        if (!canBeReduced) {
                          return false;
                        } else {
                          newLCC = new UnknownLCC(lccIDGen++, currentLCC, childLCCs, postDomLCC, currLoop);
                          found = true;
                        }
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }

    if (found) {
      /******************** Create new connections *********************/
      newLCC->makeNewSuccConnections(postDomLCC); /* succLCC is the exit LCC for newLCC */
      newLCC->makeNewPredConnections(currentLCC); /* currentLCC is the entry LCC for newLCC */

      /******************** Update global LCC list *********************/
      auto F = currentLCC->getFunction();
      auto position = eraseFromGlobalLCCList(currentLCC);
      globalOuterLCCList[F].insert(position, newLCC);
      eraseFromGlobalLCCList(postDomLCC);
      for (auto childLCCIt = childLCCs.begin(); childLCCIt != childLCCs.end(); childLCCIt++) {
        eraseFromGlobalLCCList(*childLCCIt);
      }

      /*********************** Update statistics ***********************/
      applyrule6++;

      /************************ Test Printing **************************/
      errs() << "\nUnknown Container(" << newLCC->getID() << "):- ";
      errs() << "Entry LCC(" << currentLCC->getID() << "): (";
      printUnitLCCSet(currentLCC);
      errs() << "), Child LCC( ";
      for (auto childLCCIt = childLCCs.begin(); childLCCIt != childLCCs.end(); childLCCIt++) {
        printUnitLCCSet(*childLCCIt);
        errs() << "(" << (*childLCCIt)->getID() << ")\t";
      }
      errs() << "), Exit LCC(" << postDomLCC->getID() << "): (";
      printUnitLCCSet(postDomLCC);
      errs() << ")\n";

      return true;
    }

    /* It is neither a loop header nor a single entry single exit non-loop region. Should return nullptr. */
    return false;
  }

  bool checkNApplyRules2(LCCNode *currentLCC) {
    return checkNCreateComplexBranchLCC(currentLCC);
  }

  bool checkNApplyRules(LCCNode *currentLCC) {
    int ruleIndex = 1;

    /* There is an ordering of the rules. If the first one does not work, then it falls back to the 
       * second, & so on. Otherwise returns from here. */
    switch (ruleIndex) {
    case 1: {
      bool result = checkNCreatePathLCC(currentLCC);
      if (result) {
        return result;
      }
    }
    case 2: {
      int result = checkNCreateBranchLCC(currentLCC);
      if (result) {
        return result;
      }
    }
#if 1
    case 3: {
      int result = checkNCreateLoopLCC(currentLCC);
      if (result) {
        return result;
      }
    }
#endif
#if 0
    case 4: {
      int result = checkNCreateComplexBranchLCC(currentLCC);
      if (result) {
        return result;
      }
    }
    case 5: {
      int result = checkNTransformComplexBranch(currentLCC);
      if (result) {
        return result;
      }
    }
    case 6: {
      int result = checkNCreateInvertedVLCC(currentLCC);
      if (result) {
        return result;
      }
    }
    case 7: {
      int result = checkNCreateVLCC(currentLCC);
      if (result) {
        return result;
      }
    }
#endif
    }
    return false;
  }

  /* traverseNReduce() signifies one pass of iterating over the entire CFG, & applying rules whenever they match */
  void traverseNReduce(Function *F) {

#ifdef LC_DEBUG
    errs() << "\n************************ Creating container structure **********************\n";
#endif
    int passes = 0;
    bool unknownRuleApplied = false;
    do {
      bool ruleApplied = false;
      do {
        passes++;
        for (auto itLCC = globalOuterLCCList[F].begin(), backLCC = globalOuterLCCList[F].end(); itLCC != backLCC; ++itLCC) {
          if ((ruleApplied = checkNApplyRules(*itLCC))) {
            break; /* iterate over new list of containers */
          }
        }
      } while (ruleApplied);

      unknownRuleApplied = ruleApplied;
    } while (unknownRuleApplied);

#ifdef CRNT_DEBUG
    errs() << "Function " << F->getName() << "() has " << passes << " passes.\n";
#endif
  }

  void manageDanglingLCCs(Function *F) {

    if (ClockType != INSTANTANEOUS) {
      errs() << "Invalid clock type\n";
      exit(1);
    }

#ifdef LC_DEBUG
    errs() << "\n******************** Managing dangling containers for " << F->getName() << " ********************\n";
#endif

    /* for Inverted V shape */
    bool checkAgain = false;
    do {
      checkAgain = false;
      for (auto itLCC = globalOuterLCCList[F].begin(), backLCC = globalOuterLCCList[F].end(); itLCC != backLCC; ++itLCC) {
        if (!(*itLCC)->isUnitLCC()) {
          continue;
        }
        UnitLCC *currentLCC = (static_cast<UnitLCC *>(*itLCC));
        BasicBlock *currBB = currentLCC->getBlock();
        auto lastLCC = getLastLCCofBB(currBB);

        /* the lcc must be the last for the block */
        if (lastLCC != currentLCC) {
          continue;
        }

        if (checkIfBackedge(currBB)) {
          continue;
        }

        auto succSetOfLCC = currentLCC->getSuccSet();
        bool succIsComplex = false;
        for (auto succIt = succSetOfLCC.begin(); succIt != succSetOfLCC.end(); succIt++) {
          if (!(succIt->first->isUnitLCC())) {
            succIsComplex = true;
            break;
          }
          // UnitLCC* succLCC = (static_cast<UnitLCC*>(succIt->first));
        }

        /* No successor check */
        if (succSetOfLCC.empty()) {
          continue;
        }

        if (succIsComplex) {
          continue;
        }

        /* check if any of the successor is a merge node */
        bool succIsMergeNode = false;
        for (auto it = succ_begin(currBB), et = succ_end(currBB); it != et; ++it) {
          BasicBlock *succBB = *it;
          if (succBB->getSinglePredecessor()) {
            continue;
          } else {
            succIsMergeNode = true;
            break;
          }
        }

        /* cannot combine if successor is merge node */
        if (succIsMergeNode) {
          continue;
        }

        /* get cost of currentLCC */
        InstructionCost *zeroCost = getConstantInstCost(0);
        InstructionCost *predCost = currentLCC->getCostForIC(false, zeroCost);

        /* delete from global outer list */
        eraseFromGlobalLCCList(currentLCC);
        errs() << "manageDanglingLCCs(inverted V shape): Removing cost " << *predCost << " of " << currBB->getName() << "\n";

        /* update successor initial costs using setInitialCost of UnitLCC */
        for (auto succIt = succSetOfLCC.begin(); succIt != succSetOfLCC.end(); succIt++) {
          UnitLCC *succLCC = (static_cast<UnitLCC *>(succIt->first));
          BasicBlock *succBB = succLCC->getBlock();
          succLCC->setInitialCost(predCost);
          ruleCoredet++;
          errs() << "manageDanglingLCCs(inverted V shape): Adding pred cost " << *predCost << " of " << currBB->getName() << " to successor " << succBB->getName() << "\n";
        }

        checkAgain = true;
        break;
      }

      /* break from loop when there has been no changes */
    } while (checkAgain);

    /* for V shape */
    InstructionCost *zeroCost = getConstantInstCost(0);
    do {
      checkAgain = false;
      for (auto itLCC = globalOuterLCCList[F].begin(), backLCC = globalOuterLCCList[F].end(); itLCC != backLCC; ++itLCC) {
        if (!(*itLCC)->isUnitLCC()) {
          continue;
        }
        UnitLCC *currentLCC = (static_cast<UnitLCC *>(*itLCC));
        BasicBlock *currBB = currentLCC->getBlock();
        auto firstLCC = getFirstLCCofBB(currBB);

        /* the lcc must be the first for the block */
        if (firstLCC != currentLCC) {
          continue;
        }

        auto predSetOfLCC = currentLCC->getPredSet();
        bool predIsComplex = false;
        int alreadyVisited = 0, numPredLCCs = 0;
        for (auto predIt = predSetOfLCC.begin(); predIt != predSetOfLCC.end(); predIt++) {
          if (!(predIt->first->isUnitLCC())) {
            predIsComplex = true;
            //break;
          }

          numPredLCCs++;
          if (!presentInGlobalLCCList(predIt->first)) {
            alreadyVisited++;
          }
        }

        /* No predecessor check */
        if (predSetOfLCC.empty()) {
          continue;
        }

        if (predIsComplex) {
          continue;
        }

        /* this merge node had been visited before */
        if (alreadyVisited == numPredLCCs) {
          continue;
        }

        /* check if any of the predecessor has multitple successors */
        int minCost = -1, maxCost = -1, sumCost = 0, numPreds = 0;
        bool predHasBackedge = false, hasSiblings = false;
        for (auto predIt = pred_begin(currBB), predEt = pred_end(currBB); predIt != predEt; ++predIt) {
          BasicBlock *predBB = *predIt;

          /* check backedge */
          if (checkIfBackedge(predBB)) {
            predHasBackedge = true;
            break;
          }

          /* Merge node has siblings from one of its parents, removing commit would mean it has to be effective in every sibling, which needs checking whether they are merge nodes again */
          if (!predBB->getSingleSuccessor()) {
            hasSiblings = true;
            break;
          }

          /* get cost of predLCC */
          LCCNode *predLCC = getLastLCCofBB(predBB);
          InstructionCost *predCost = predLCC->getCostForIC(false, zeroCost);
          auto predNumCost = getConstCost(predCost);
          errs() << "Pred cost for " << predBB->getName() << " is " << *predCost << "\n";

          if (predNumCost == -1) { /* has no cost - might arise when these nodes have been processed in some previous pass */
            //errs() << "Current pred cost for " << predBB->getName() << " of merge node " << currBB->getName() << " (" << predBB->getParent()->getName() << ") is -1. This is not expected. Perhaps processing merge nodes in topographical order will help.\n";
            continue;
            //exit(1);
          }
          numPreds++;

          if (minCost == -1 || maxCost == -1) {
            minCost = predNumCost;
            maxCost = predNumCost;
          } else {
            if (minCost > predNumCost) {
              minCost = predNumCost;
            }
            if (maxCost < predNumCost) {
              maxCost = predNumCost;
            }
          }
          sumCost += predNumCost;
        }

        if (predHasBackedge || hasSiblings) { /* CDCommit cannot be removed */
          continue;
        }

        if (numPreds <= 0) {
          continue;
        }

        /* delete from global outer list */
        for (auto predIt = predSetOfLCC.begin(); predIt != predSetOfLCC.end(); predIt++) {
          UnitLCC *predLCC = static_cast<UnitLCC *>(predIt->first);

          BasicBlock *predBB = predLCC->getBlock();
          InstructionCost *predCost = predLCC->getCostForIC(false, zeroCost);
          errs() << "manageDanglingLCCs(V shape): Removing cost " << *predCost << " of predecessor " << predBB->getName() << " for merge node " << currBB->getName() << "\n";

          eraseFromGlobalLCCList(predLCC);
        }

        /* update successor initial costs using setInitialCost of UnitLCC */
        int avgCost = (sumCost / numPreds);
        InstructionCost *currCost = getConstantInstCost(avgCost);
        currentLCC->setInitialCost(currCost);
        ruleCoredet++;
        errs() << "manageDanglingLCCs(V shape): Settting average cost " << *currCost << " as initial cost for merge node " << currBB->getName() << "\n";

        checkAgain = true;
        break;
      }

      /* break from loop when there has been no changes */
    } while (checkAgain);
  }

  int func_opts = 0;
  void costEvaluate(Function *F) {
    //bool isClone = isClonedFunc(F);
    bool isThread = isThreadFunc(F); // Function cost will always be instrumented in the caller, & not in the entry or exit LCC of the function for PC or IC
    assert((isRecursiveFunc.end() != isRecursiveFunc.find(F->getName().str())) && "Function name is not found in recursive list!");
    bool isRecursive = isRecursiveFunc[F->getName().str()];
    bool costWritten = false;

    if (ClockType == PREDICTIVE) {
#ifdef LC_DEBUG
      errs() << "\n********************** Predictive Clock Cost Evaluation **********************\n";
#endif
      LCCNode *entryLCC = nullptr;
      BasicBlock *entryBB = &(F->getEntryBlock());
      LCCNode *entryUnitLCC = getFirstLCCofBB(entryBB);
      if (entryUnitLCC) {
        entryLCC = entryUnitLCC->getOuterMostEnclosingLCC();
      }

      for (auto itLCC = globalOuterLCCList[F].begin(), backLCC = globalOuterLCCList[F].end(); itLCC != backLCC; ++itLCC) {
        LCCNode *currLCC = *itLCC;
        /* Only if this is a clone & the LCC of a entry block, the function cost will be computed from this */
        if (!isThread && !isRecursive && (entryLCC == currLCC)) {
          InstructionCost *cost = currLCC->getCostForPC(false);
          int numCost = hasConstCost(cost);
          InstructionCost *simplifiedCost = nullptr;
          if (numCost <= 0) {
            simplifiedCost = simplifyCost(F, cost);
          }

          if (simplifiedCost) {
            auto funcInfoIt = computedFuncInfo.find(F);
            assert((funcInfoIt != computedFuncInfo.end()) && "Function Info must have been initialized at the time of container creation!");
            auto fInfo = funcInfoIt->second;
            fInfo->cost = cost;
            costWritten = true;
            //errs() << "Func cost " << *cost << " for " << F->getName() << " has been optimized!\n";
            func_opts++;
          }
        } else {
          currLCC->getCostForPC(true); /* Instrument container. No cost is expected to be returned */
        }
      }
    } else if (ClockType == INSTANTANEOUS) {
#ifdef LC_DEBUG
      errs() << "\n******************** Instantaneous Clock Cost Evaluation ********************\n";
#endif
      LCCNode *entryLCC = nullptr;
      BasicBlock *entryBB = &(F->getEntryBlock());
      LCCNode *entryUnitLCC = getFirstLCCofBB(entryBB);
      if (entryUnitLCC) {
        entryLCC = entryUnitLCC->getOuterMostEnclosingLCC();
      }

      LCCNode *exitLCC = nullptr;
      BasicBlock *exitBB = getFuncExitBlock(F);
      if (exitBB) { /* return block may not be present */
        LCCNode *exitUnitLCC = getLastLCCofBB(exitBB);
        if (exitUnitLCC) {
          exitLCC = exitUnitLCC->getOuterMostEnclosingLCC();
        }
      }

      int numCostEnEx = 0;
      for (auto itLCC = globalOuterLCCList[F].begin(), backLCC = globalOuterLCCList[F].end(); itLCC != backLCC; ++itLCC) {
        LCCNode *currLCC = *itLCC;

#ifdef ALL_DEBUG
        errs() << "/*********************** Cost Evaluating for ";
        printUnitLCCSet(currLCC);
        errs() << " (" << currLCC->getID() << ") ***********************/\n";
#endif

        /* Initial cost for any top level container is 0 */
        InstructionCost *zeroCost = getConstantInstCost(0);
        bool instrumentLCC = true;
        bool first = true;

        /* the function cost will include the entry and exit cost */
        if (!isThread && !isRecursive && ((entryLCC == currLCC) || (exitLCC == currLCC))) {
          /* if entry == exit, cost should be accounted for only once */
          if ((entryLCC != exitLCC) || first) {
            instrumentLCC = false;
            first = false;
            /* cost evaluate */
            InstructionCost *cost = currLCC->getCostForIC(false, zeroCost);
            int numCost = hasConstCost(cost);
            if ((numCostEnEx + numCost) > 0 && (numCostEnEx + numCost) < CommitInterval) {
#if 1
              if (entryLCC == currLCC) {
                errs() << "Func cost " << numCost << " for " << F->getName() << " has been optimized for entry cost!\n";
              } else if (exitLCC == currLCC) {
                errs() << "Func cost " << numCost << " for " << F->getName() << " has been optimized for exit cost!\n";
              }
#endif
              numCostEnEx += numCost;
              func_opts++;
            }
          }
        }

        if (instrumentLCC) {
          currLCC->getCostForIC(true, zeroCost); /* Instrument non-entry-or-exit container. No cost is expected to be returned */
        } else {
          /* not instrumenting entry or exit lcc */
          //errs() << "Not instrumenting LCC!\n";
        }
      }

      if (numCostEnEx) {
        auto funcInfoIt = computedFuncInfo.find(F);
        assert((funcInfoIt != computedFuncInfo.end()) && "Function Info must have been initialized at the time of container creation!");
        auto fInfo = funcInfoIt->second;
        errs() << "Storing cost of " << F->getName() << "() : " << numCostEnEx << "\n";
        fInfo->cost = getConstantInstCost(numCostEnEx);
        costWritten = true;
      }
    } else {
      errs() << "Invalid clock type\n";
      exit(1);
    }

    if (!costWritten) {
      auto funcInfoIt = computedFuncInfo.find(F);
      assert((funcInfoIt != computedFuncInfo.end()) && "Function Info must have been initialized at the time of container creation!");
      auto fInfo = funcInfoIt->second;
      /* Cost of 0 gives error in costToScev transform */
      fInfo->cost = getConstantInstCost(1);
    }
  }

  /* instrType: ALL_IR(0) - increment & push based on cost value passed (mostly IR count)
     * instrType: PUSH_ON_CYCLES(1) - increment based on IR & push based on cycles
     * instrType: INCR_ON_CYCLES(2) - increment based on cycles & push based on cycles
     */
  void instrumentGlobal(Instruction *I, eInstrumentType instrType, Value *val, LoadInst *loadDisFlag = nullptr) {
    Value *loadedLC = nullptr;
    if (instrType == INCR_ON_CYCLES) {
      assert(!val && "Not expecting a pre-calculated cost value for this configuration");
      loadedLC = incrementTLLCWithCycles((*I));
    } else {
      loadedLC = incrementTLLC((*I), val);
    }

    if (instrType == PUSH_ON_CYCLES) {
      testNpushMLCfromTLLC(*I, loadedLC, loadDisFlag, true);
    } else {
      testNpushMLCfromTLLC(*I, loadedLC, loadDisFlag);
    }
    instrumentedInst++;
  }

  Value *createLocalCounter(Instruction *I) {
    IRBuilder<> IR(I);
    AllocaInst *alloca_inst = IR.CreateAlloca(IR.getInt64Ty(), 0, "localCounter");
    gLocalCounter[I->getFunction()] = alloca_inst;
    //errs() << "Created gLocalCounter for " << I->getFunction()->getName() << "\n";
    return alloca_inst;
  }

  Value *createLocalFlag(Instruction *I) {
    IRBuilder<> IR(I);
    AllocaInst *alloca_inst = IR.CreateAlloca(IR.getInt32Ty(), 0, "localFlag");
    gLocalFLag[I->getFunction()] = alloca_inst;
    //errs() << "Created gLocalFLag for " << I->getFunction()->getName() << "\n";
    return alloca_inst;
  }

  /* Load from thread local LocalLC to local variable localCounter */
  void loadCounterInLocal(Instruction *I, Value *alloca_inst, std::string gvName) {
    IRBuilder<> IR(I);
    Function *F = I->getFunction();
    GlobalVariable *lc = F->getParent()->getGlobalVariable(gvName);
    LoadInst *Load = IR.CreateLoad(lc);
    IR.CreateStore(Load, alloca_inst);
  }

  /* Store from local variable localCounter to thread local LocalLC */
  void storeCounterFromLocal(Instruction *I, Value *alloca_inst, std::string gvName) {
    IRBuilder<> IR(I);
    Function *F = I->getFunction();
    GlobalVariable *lc = F->getParent()->getGlobalVariable(gvName);
    LoadInst *Load = IR.CreateLoad(alloca_inst);
    IR.CreateStore(Load, lc);
  }

  void findCIfunctions(Module &M) {
    for (auto &F : M) {
      for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
        if (CallInst *ci = dyn_cast<CallInst>(&*I)) {
          if (ci->getCalledFunction()) {
            auto calleeName = ci->getCalledFunction()->getName();
            if (calleeName.compare("register_ci") == 0) {
              auto ciFunc = ci->getArgOperand(0);
              //Value* func = funcArg->stripPointerCasts();
              StringRef fName = ciFunc->getName();
              //#ifdef LC_DEBUG
              errs() << "Compiler Interrupt function: " << fName << "\n";
              //#endif
              ciFuncInApp[fName] = 1;
            }
          }
        }
      }
    }
  }

  bool isRestrictedFunction(Function *F) {

#if 0
    /* CI function might be available in a library instead of the app. */
    assert(!ciFuncInApp.empty() && "Compiler interrupt functions has not been read yet. Aborting.");
    if (ciFuncInApp.empty()) {
      errs() << "Compiler interrupt functions has not been read yet. Aborting.\n";
      exit(1);
    }
#endif

    if ((F->getName().compare("printCountersPi") == 0) || (F->getName().compare("_Z14intvActionHookl") == 0) || (F->getName().compare("intvActionHook") == 0) || (ciFuncInApp.find(F->getName()) != ciFuncInApp.end())) {
      return true;
    }

    return false;
  }

  void handleUnreachable(Function *F) {
    for (auto &BB : *F) {
      for (BasicBlock::iterator I = BB.begin(), ie = BB.end(); I != ie; ++I) {
        BasicBlock::iterator instIt = I;
        if (isa<UnreachableInst>(I)) {
          if (BB.getFirstNonPHI() != (&*I)) {
            instIt--; // the instruction before the unreachable instruction makes it unreachable, if only its not the first instruction of the block
            storeCounterFromLocal(&*instIt, gLocalCounter[F], "LocalLC");
            storeCounterFromLocal(&*instIt, gLocalFLag[F], "lc_disabled_count");
          }
        }
      }
    }
  }

  void initializeLocals(Function *F) {

    /* Locals are no longer supported */
    return;

    if (gIsOnlyThreadLocal) {
      errs() << "initializeLocals(): Thread local configuration is enabled. Cannot instrument.\n";
      return;
    }

    if (isRestrictedFunction(F)) {
      return;
    }

    /* At the beginning of the function */
    Instruction *I = F->front().getFirstNonPHI();
    auto localCounterVar = createLocalCounter(I);
    loadCounterInLocal(I, localCounterVar, "LocalLC");
    auto localCounterFlag = createLocalFlag(I);
    loadCounterInLocal(I, localCounterFlag, "lc_disabled_count");

#if 0
    /* At the end of the function */ -/* return should handle this */
        I = &(F->back().back());
    storeCounterFromLocal(I, localCounterVar, "LocalLC");
    storeCounterFromLocal(I, localCounterFlag, "lc_disabled_count");
#endif

    handleUnreachable(F);
  }

  void instrumentLocals(Function *F) {

    if (gIsOnlyThreadLocal) {
      errs() << "instrumentLocals(): Thread local configuration is enabled. Cannot instrument.\n";
      return;
    }

    if (isRestrictedFunction(F)) {
      return;
    }

    /* Before every function call */
    for (auto &BB : *F) {

      //errs() << "Function " << F->getName() << " -----> BB " << BB.getName() << "\n";
      StringRef ownBlock1("pushBlock");
      StringRef ownBlock2("if_clock_enabled");
      StringRef ownBlock3("postPushBlock");
      StringRef ownBlock4("postClockEnabledBlock");
      //errs() << "Find returns : " << BB.getName().find(ownBlock1) << ", " << BB.getName().find(ownBlock2) << ", " << BB.getName().find(ownBlock3) << "\n";
      if ((BB.getName().find(ownBlock1) == 0) || (BB.getName().find(ownBlock2) == 0) || (BB.getName().find(ownBlock3) == 0) || (BB.getName().find(ownBlock4) == 0)) {
        //errs() << "Found the culprit: " << BB.getName() << "\n";
        continue;
      }

#if 0
      if (
          F->getName().compare("start_workers") == 0) {
        if (
            (
                BB.getName().compare("sw.bb3.i") == 0)) {
          errs() << "Not instrumenting locals for basic block " << BB.getName() << " of Function " << F->getName() << "\n";
          continue;
        }
        //continue;
      }
#endif

#if 0
      if (
          (BB.getName().compare("for.inc83.us.i") == 0) || (BB.getName().compare("for.end85.us.i") == 0)
          /*
            (BB.getName().compare("for.body50.us.i")==0)
            || (BB.getName().compare("if.then59.us.i")==0)  
            */
          //|| (BB.getName().compare("")==0)
      ) {
        errs() << "Not instrumenting locals for basic block " << BB.getName() << " of Function " << F->getName() << "\n";
        continue;
      }
#endif

#if 0
      if (InstGranularity == OPTIMIZE_HEURISTIC || InstGranularity == OPTIMIZE_HEURISTIC_WITH_TL) {
        /* For debugging, don't instrument self loop blocks */
        Loop *L = LI->getLoopFor(&BB);
        if (L) {
          BasicBlock *headerBBL = L->getHeader();
          if (headerBBL == &BB) { /* necessary for a self loop */
            bool isSelfLoop = false;
            succ_iterator succBB = succ_begin(headerBBL), succEndBB = succ_end(headerBBL);
            for (; succBB != succEndBB; ++succBB) {
              if (*succBB == headerBBL) {
                isSelfLoop = true;
                break;
              }
            }
            if (isSelfLoop) {
              errs() << "Not instrumenting locals for self-loop basic block " << BB.getName() << " of Function " << F->getName() << "\n";
              continue;
            }
          }
        }
      }
#endif

      bool instrument = false;
      for (BasicBlock::iterator I = BB.begin(), ie = BB.end(); I != ie; ++I) {
        /* instrument marked instruction */
        if (instrument) {
          loadCounterInLocal(&*I, gLocalCounter[F], "LocalLC");
          loadCounterInLocal(&*I, gLocalFLag[F], "lc_disabled_count");
          instrument = false;
        }
#if 1
        /* mark instructions to instrument */
        BasicBlock::iterator instIt = I;
        if (!isa<PHINode>(I) && !isa<DbgInfoIntrinsic>(I)) {
          if (isa<CallInst>(I)) {
            if (checkIfExternalLibraryCall(&*I)) {
              instrument = false;
              continue;
            } else {
              CallInst *ci = dyn_cast<CallInst>(I);
              Function *calledFunction = ci->getCalledFunction();
              if (calledFunction) {
                if (calledFunction->getName().compare("llvm.readcyclecounter") == 0) {
                  instrument = false;
                  continue;
                }
              }
            }
            storeCounterFromLocal(&*instIt, gLocalCounter[F], "LocalLC");
            storeCounterFromLocal(&*instIt, gLocalFLag[F], "lc_disabled_count");
            instrument = true;
          } else if (isa<ReturnInst>(I)) {
            storeCounterFromLocal(&*instIt, gLocalCounter[F], "LocalLC");
            storeCounterFromLocal(&*instIt, gLocalFLag[F], "lc_disabled_count");
            instrument = false;
          }
#if 0
          else if (isa<UnreachableInst>(I)) {
            instIt--; // the instruction before the unreachable instruction makes it unreachable
            storeCounterFromLocal(&*instIt, gLocalCounter[F], "LocalLC");
            storeCounterFromLocal(&*instIt, gLocalFLag[F], "lc_disabled_count");
            instrument = false;
          }
#endif
        }
#else
        if (ci) {
          if (ci->getCalledFunction()) {
            /* if the call is to a function that has a function pointer, it needs to pass the clock via thread local */
            if (isFuncPointer(ci->getCalledFunction())) {
              storeCounterFromLocal(ci, gLocalCounter[F], "LocalLC");
              storeCounterFromLocal(ci, gLocalFLag[F], "lc_disabled_count");
              instrument = true;
            }
          } else {
            /* The call function is not available, so it is high likely a function pointer call */
            storeCounterFromLocal(ci, gLocalCounter[F], "LocalLC");
            storeCounterFromLocal(ci, gLocalFLag[F], "lc_disabled_count");
            instrument = true;
          }
        }
#endif
      }
    }
  }

#if 0
  void instrumentLibCallsWithCycleIntrinsic(Function *F) {
    std::vector<Instruction *> externalCalls;
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
      if (checkIfExternalLibraryCall(&*I))
        externalCalls.push_back(&*I);
    }
    for (auto Ilist : externalCalls) {
      //instrumentExternalCalls(I);
      //errs() << "first: " << *Irange.first << ", second: " << *Irange.second << "\n";
      instrumentExternalCallsWithIntrinsic(Ilist);
    }
  }
#else
  /* Opt & Naive */
  void instrumentLibCallsWithCycleIntrinsic(Function *F) {
    std::vector<std::list<Instruction *> *> externalCalls;
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
      // bool first = true;
      // bool found = false;
      // Instruction *firstInst = nullptr, *lastInst = nullptr;
      std::list<Instruction *> *listInst = nullptr;
      while (I != E) {
        if (checkIfExternalLibraryCall(&*I)) {
          if (!listInst) {
            listInst = new std::list<Instruction *>();
          }
          listInst->push_back(&*I);
        } else {
          break;
        }
        I++;
      }
      if (listInst) {
        externalCalls.push_back(listInst);
      }
    }
    for (auto Ilist : externalCalls) {
      //instrumentExternalCalls(I);
      //errs() << "first: " << *Irange.first << ", second: " << *Irange.second << "\n";
      instrumentExternalCallsWithIntrinsic(Ilist);
    }
  }
#endif

  void instrumentFunc(Function *F) {

#ifdef LC_DEBUG
    errs() << "\n************************ Instrumenting Function " << F->getName() << "**************************\n";
#endif
    int numInstrumented = 0;

    for (auto &currBB : *F) {
#if 0
      if (InstGranularity == OPTIMIZE_HEURISTIC || InstGranularity == OPTIMIZE_HEURISTIC_WITH_TL) {
        /* For debugging, don't instrument self loop blocks */
        Loop *L = LI->getLoopFor(&currBB);
        if (L) {
          BasicBlock *headerBBL = L->getHeader();
          if (headerBBL == &currBB) { /* necessary for a self loop */
            bool isSelfLoop = false;
            succ_iterator succBB = succ_begin(headerBBL), succEndBB = succ_end(headerBBL);
            for (; succBB != succEndBB; ++succBB) {
              if (*succBB == headerBBL) {
                isSelfLoop = true;
                break;
              }
            }
            if (isSelfLoop) {
              errs() << "Not instrumenting function for self-loop basic block " << currBB.getName() << " of Function " << F->getName() << "\n";
              continue;
            }
          }
        }
      }
#endif

      auto LCCs = bbToContainersMap[&currBB];
      for (auto LCC : LCCs) {
        UnitLCC *unitLCC = static_cast<UnitLCC *>(LCC);

        /* Decide whether to instrument this container */
        bool toInstrument = false;

        if (checkIfInstGranIsOpt()) {
          if (unitLCC->getInstrumentFlag()) {
            toInstrument = true;
          }
        } else {
          //toInstrument = true;
          errs() << "Naive instrumentation is done separately. This is an invalid path. Aborting.\n";
          exit(1);
        }

        if (toInstrument) {
          auto instrLCCInfo = unitLCC->getInstrInfo();
          auto instrValLCCInfo = unitLCC->getInstrValInfo();

#ifdef LC_DEBUG
          //if(F->getName().compare("gravsub") == 0) {
          errs() << "Instrumenting Unit LCC Block: " << unitLCC->getBlock()->getName() << "\n";
          for (auto instrInfoIt = instrLCCInfo.begin(); instrInfoIt != instrLCCInfo.end(); instrInfoIt++) {
            Instruction *I = instrInfoIt->first;
            errs() << "\tInstrumenting Instruction: " << *I << " with cost " << *(instrInfoIt->second) << "\n";
          }
          //}
#endif

          for (auto instrInfoIt = instrLCCInfo.begin(); instrInfoIt != instrLCCInfo.end(); instrInfoIt++) {
            Instruction *I = instrInfoIt->first;
            InstructionCost *instCost = instrInfoIt->second;
            /******************************** Instrument the instruction *********************************/
#ifdef ALL_DEBUG
            if (hasConstCost(instCost) < 0) {
              errs() << "Instrumenting block " << I->getParent()->getName() << " (Inst: " << *I << ") with non-numeric cost : " << *instCost << "\n";
            } else {
              errs() << "Instrumenting block " << I->getParent()->getName() << " (Inst: " << *I << ") : " << *instCost << "\n";
            }
            //errs() << "Instrumenting Instruction " << *I << " in block " << currBB.getName() << "\n";
#endif
            Value *val = scevToIR(I, instCost);
            instrumentCI(I, val);
            numInstrumented++;
          }

          for (auto instrInfoIt = instrValLCCInfo.begin(); instrInfoIt != instrValLCCInfo.end(); instrInfoIt++) {
            Instruction *I = instrInfoIt->first;
            Value *val = instrInfoIt->second;
            instrumentCI(I, val);
            numInstrumented++;
          }
        }
      }
    }

#if 0
    /* Code for calling fiber_yield() for libfiber */
    Module *M = I->getModule();
    FunctionType *hook_type = TypeBuilder<void(long), false>::get(M->getContext());
    Function *hookFunc = cast<Function>(M->getOrInsertFunction("commit_logical_clock", hook_type));
    std::vector<llvm::Value *> args;
    args.push_back(val);
    IRBuilder<> IR(I);
    IR.CreateCall(hookFunc, args);
#endif

    /* Instrument load & stores to local variable for keeping the local counter */
    if (numInstrumented) {
      if (InstGranularity == OPTIMIZE_HEURISTIC || InstGranularity == OPTIMIZE_ACCURATE) {
        instrumentLocals(F);
      }
    } else {
#ifdef LC_DEBUG
      errs() << "Function " << F->getName() << "() has no instrumentation.\n";
#endif
      numUninstrumentedFunc++;
    }

    if (InstGranularity == OPTIMIZE_ACCURATE) {
      instrumentLibCallsWithCycleIntrinsic(F);
    }

    return;
  }

  /* CIs need to be instrumented using this abstraction for CI Opt */
  void instrumentCI(Instruction *I, Value *val) {
    switch (InstGranularity) {
    case OPTIMIZE_HEURISTIC:
    case OPTIMIZE_HEURISTIC_WITH_TL:
    case OPTIMIZE_ACCURATE: {
      //Value *val = scevToIR(I, instCost);
      instrumentIfLCEnabled(I, ALL_IR, val);
      break;
    }
    case OPTIMIZE_INTERMEDIATE: {
      //Value *val = scevToIR(I, instCost);
      instrumentIfLCEnabled(I, PUSH_ON_CYCLES, val);
      break;
    }
    case OPTIMIZE_HEURISTIC_INTERMEDIATE_FIBER: {
      //Value *val = scevToIR(I, instCost);
      instrumentGlobal(I, PUSH_ON_CYCLES, val);
      break;
    }
    case OPTIMIZE_HEURISTIC_FIBER: {
      //Value *val = scevToIR(I, instCost);
      instrumentGlobal(I, ALL_IR, val);
      break;
    }
    case OPTIMIZE_CYCLES: {
      instrumentIfLCEnabled(I, INCR_ON_CYCLES);
      break;
    }
    default: {
      errs() << "This level of instrumentation granularity is not present!\n";
      exit(1);
    }
    }
  }

  void computeCostEvalStats(Function *F) {

    int num_of_unit_lcc = 0;
    int num_blocks = 0;
    int num_of_final_lcc = 0;

    for (auto &BB : *F) {
      std::vector<LCCNode *> containers = bbToContainersMap[&BB];
      num_blocks++;
      num_of_unit_lcc += containers.size();
#ifdef LC_DEBUG
      if (containers.size() == 0) {
        errs() << "Block " << BB.getName() << " of Function " << F->getName() << " has 0 containers!\n";
      }
#endif
    }

    for (auto itLCC = globalOuterLCCList[F].begin(), backLCC = globalOuterLCCList[F].end(); itLCC != backLCC; ++itLCC) {
      num_of_final_lcc++;
    }

    struct fstats fstat;
    fstat.blocks = num_blocks;
    fstat.unit_lcc = num_of_unit_lcc;
    fstat.final_lcc = num_of_final_lcc;
    fstat.unhandledLoops = unhandled_loop;
    fstat.rule1Count = applyrule1;
    fstat.rule1ContCount = applycontrule1;
    fstat.rule2Count = applyrule2;
    fstat.rule2ContCount = applycontrule2;
    fstat.rule2SavedCount = rule2savedInst;
    fstat.rule3Count = applyrule3;
    fstat.rule3ContCount = applycontrule3;
    fstat.rule3SavedCount = rule3savedInst;
    fstat.rule4Count = applyrule4;
    fstat.rule4SavedCount = rule4savedInst;
    fstat.rule5Count = applyrule5;
    fstat.rule5SavedCount = rule5savedInst;
    fstat.rule6Count = applyrule6;
    fstat.rule7Count = applyrule7;
    fstat.rule7ContCount = applycontrule7;
    fstat.rule7SavedCount = rule7savedInst;
    fstat.ruleCoredet = ruleCoredet;
    fstat.self_loop_transform = self_loop_transform;
    fstat.generic_loop_transform = generic_loop_transform;
    FuncStat[F] = fstat;
  }

  void computeInstrStats(Function *F) {
    if (F) {
      auto fstatIt = FuncStat.find(F);
      assert((fstatIt != FuncStat.end()) && "At this point function stat should have been created in the cost evaluation stage!");
      FuncStat[F].instrumentedCount = instrumentedInst;
    }
  }

  void printStats(Function *F) {
    if (F) {
      /* print per function stats */
      auto fstatIt = FuncStat.find(F);
      assert((fstatIt != FuncStat.end()) && "At this point function stat should have been created in the cost evaluation stage!");
      auto fstat = fstatIt->second;
      errs() << "\n********************** Printing " << F->getName() << " Statistics **********************\n";
      errs() << "#blocks : " << fstat.blocks << "\n";
      errs() << "#unit containers : " << fstat.unit_lcc << "\n";
      errs() << "#final containers : " << fstat.final_lcc << "\n";
      errs() << "#instrumentations : " << fstat.instrumentedCount << "\n";
      errs() << "#Loops with multiple predecessors/successors : " << fstat.unhandledLoops << "\n";
      //errs() << "(Ideally, total final containers == number of instrumentations (+1), but the latter may be greater sometimes (for instantaneous clock))\n";
      if (checkIfInstGranIsOpt()) {
        errs() << "Rules applied :-\n";
        errs() << "#container1 : " << fstat.rule1ContCount << "\n";
        errs() << "#container2 : " << fstat.rule2ContCount << "\n";
        errs() << "#container3 : " << fstat.rule3ContCount << "\n";
        errs() << "#rule1 : " << fstat.rule1Count << " times (saved " << fstat.rule1Count << " containers)\n";
        errs() << "#rule2 : " << fstat.rule2Count << " times (saved " << fstat.rule2SavedCount << " containers)\n";
        errs() << "#rule3 : " << fstat.rule3Count << " times (saved " << fstat.rule3SavedCount << " containers)\n";
        //errs() << "#rule4 : " << fstat.rule4Count << " times (saved " << fstat.rule4SavedCount << " containers)\n";
        //errs() << "#rule5 : " << fstat.rule5Count << " times (saved " << fstat.rule5SavedCount << " containers)\n";
        //errs() << "#rule6 : " << fstat.rule6Count << " times (saved " << fstat.rule6Count << " containers)\n";
        errs() << "#rule7 : " << fstat.rule7Count << " times (saved " << fstat.rule7SavedCount << " containers)\n";
        errs() << "#rule coredet : " << fstat.ruleCoredet << "\n";
        errs() << "#self loop transforms : " << fstat.self_loop_transform << "\n";
        errs() << "#generic loop transforms : " << fstat.generic_loop_transform << "\n";

#ifdef PROFILING
        int expected_saved_lcc = fstat.rule1Count + fstat.rule2SavedCount + fstat.rule3SavedCount + fstat.rule4SavedCount + fstat.rule5SavedCount + fstat.rule6Count;
        int saved_lcc = fstat.unit_lcc - fstat.final_lcc;
        if (ClockType == PREDICTIVE) {
          if (expected_saved_lcc != saved_lcc)
            errs() << "Warning: #Saved LCC: " << saved_lcc << ", #Expected Saved LCC: " << expected_saved_lcc << " do not match!!\n";
        }
#endif
      }
    } else {
      /* print total stats */
      errs() << "\n\n********************** Printing Total Statistics **********************\n";
      int blocks = 0;
      int unit_lcc = 0;
      int final_lcc = 0;
      int instrumentedCount = 0;
      int unhandledLoops = 0;
      int rule1Count = 0;
      int rule1ContCount = 0;
      int rule2Count = 0;
      int rule2ContCount = 0;
      int rule2SavedCount = 0;
      int rule3Count = 0;
      int rule3ContCount = 0;
      int rule3SavedCount = 0;
      int rule4Count = 0;
      //int rule4SavedCount = 0;
      int rule5Count = 0;
      //int rule5SavedCount = 0;
      int rule6Count = 0;
      int rule7Count = 0;
      int rule7ContCount = 0;
      int rule7SavedCount = 0;
      int ruleCoredetCount = 0;
      int self_loop_transform_count = 0;
      int generic_loop_transform_count = 0;
      for (auto fstatIt = FuncStat.begin(); fstatIt != FuncStat.end(); fstatIt++) {
        auto fstat = fstatIt->second;
        blocks += fstat.blocks;
        unit_lcc += fstat.unit_lcc;
        final_lcc += fstat.final_lcc;
        instrumentedCount += fstat.instrumentedCount;
        unhandledLoops += fstat.unhandledLoops;
        rule1Count += fstat.rule1Count;
        rule1ContCount += fstat.rule1ContCount;
        rule2Count += fstat.rule2Count;
        rule2ContCount += fstat.rule2ContCount;
        rule2SavedCount += fstat.rule2SavedCount;
        rule3Count += fstat.rule3Count;
        rule3ContCount += fstat.rule3ContCount;
        rule3SavedCount += fstat.rule3SavedCount;
        rule4Count += fstat.rule4Count;
        rule5Count += fstat.rule5Count;
        rule6Count += fstat.rule6Count;
        rule7Count += fstat.rule7Count;
        rule7ContCount += fstat.rule7ContCount;
        rule7SavedCount += fstat.rule7SavedCount;
        ruleCoredetCount += fstat.ruleCoredet;
        self_loop_transform_count += fstat.self_loop_transform;
        generic_loop_transform_count += fstat.generic_loop_transform;
      }
      errs() << "#total blocks : " << blocks << "\n";
      errs() << "#total unit containers : " << unit_lcc << "\n";
      errs() << "#total final containers : " << final_lcc << "\n";
      errs() << "#total instrumentations : " << instrumentedCount << "\n";
      errs() << "#total loops with multiple predecessors/successors : " << unhandledLoops << "\n";
      //errs() << "(Ideally, total final containers == number of instrumentations (+1), but the latter may be greater sometimes (for instantaneous clock))\n";
      if (checkIfInstGranIsOpt()) {
        errs() << "Total Rules applied :-\n";
        errs() << "#total container1 : " << rule1ContCount << "\n";
        errs() << "#total container2 : " << rule2ContCount << "\n";
        errs() << "#total container3 : " << rule3ContCount << "\n";
        errs() << "#total rule1 : " << rule1Count << " times (saved " << rule1Count << " containers)\n";
        errs() << "#total rule2 : " << rule2Count << " times (saved " << rule2SavedCount << " containers)\n";
        errs() << "#total rule3 : " << rule3Count << " times (saved " << rule3SavedCount << " containers)\n";
#if 0
        //errs() << "#rule4 : " << rule4Count << " times (saved " << rule4SavedCount << " containers)\n";
        //errs() << "#rule5 : " << rule5Count << " times (saved " << rule5SavedCount << " containers)\n";
        //errs() << "#rule6 : " << rule6Count << " times (saved " << rule6Count << " containers)\n";
#endif
        errs() << "#total rule7 : " << rule7Count << " times (saved " << rule7SavedCount << " containers)\n";
        errs() << "#total coredet transforms : " << ruleCoredetCount << "\n";
        errs() << "#total self loop transforms : " << self_loop_transform_count << "\n";
        errs() << "#total generic loop transforms : " << generic_loop_transform_count << "\n";
      }
    }
  }

  void printInstrStats(Function *F) {
    errs() << "\n**************** Printing " << F->getName() << "() Instrumentation Statistics ****************\n";
    if (ClockType == PREDICTIVE) {
      for (auto itLCC = globalOuterLCCList[F].begin(), backLCC = globalOuterLCCList[F].end(); itLCC != backLCC; ++itLCC) {
        UnitLCC *unitLCC = (static_cast<UnitLCC *>((*itLCC)->getInnerMostEntryLCC()));
        if (unitLCC) {
          unitLCC->printInstr();
        }
      }
    } else if (ClockType == INSTANTANEOUS) {
      for (Function::iterator BB = F->begin(), endBB = F->end(); BB != endBB; ++BB) {
        auto bbIt = bbToContainersMap.find(&*BB);
        if (bbIt != bbToContainersMap.end()) {
          std::vector<LCCNode *> containers = bbIt->second;
          for (LCCNode *blockContainer : containers) {
            UnitLCC *unitLCC = static_cast<UnitLCC *>(blockContainer);
            unitLCC->printInstr();
          }
        }
      }
    }
  }

  bool checkIfSelfLoop(Loop *L) {
    bool isSelfLoop = false;
    BasicBlock *headerBBL = L->getHeader();
    succ_iterator succBB = succ_begin(headerBBL), succEndBB = succ_end(headerBBL);
    for (; succBB != succEndBB; ++succBB) {
      if (*succBB == headerBBL) {
        isSelfLoop = true;
        assert((L->getNumBlocks() == 1) && "A self loop cannot have multiple blocks");
        assert((L->getSubLoops().empty()) && "A self loop cannot have subloops");
        break;
      }
    }
    return isSelfLoop;
  }

  bool checkIfSESELoop(Loop *L) {

    if (!L->getExitingBlock()) {
      return false;
    }
    if (!L->getLoopLatch()) {
      return false;
    }
    if (!L->getExitBlock()) {
      return false;
    }

    return true;
  }

  void instrumentSelfLoop(Loop *L) {
    auto selfLoopInfo = selfLoop.find(L);
    BasicBlock *headerBBL = L->getHeader();

#if 0
    /* Temporary - on debug purpose */
    BasicBlock *frontBlock = &(headerBBL->getParent()->getEntryBlock());
    if (headerBBL->getParent()->getName().compare("add_to_sum") == 0 && headerBBL->getName().compare("for.body") == 0) {
      errs() << "Adding self loop debug test for " << headerBBL->getName() << " (" << headerBBL->getParent()->getName() << ")\n";
      Module *M = headerBBL->getModule();
      BasicBlock::iterator inst(frontBlock->getFirstNonPHI());
      inst++;
      inst++;
      auto newI = &*inst;

      IRBuilder<> IR(newI);
      Function *printf_func = printf_prototype(M);
      llvm::Value *formatStr = IR.CreateGlobalStringPtr("\nTESTING : %ld\n", "test");
      GlobalVariable *tllc = M->getGlobalVariable("LocalLC");
      auto lc_val = IR.CreateLoad(tllc);
      std::vector<llvm::Value *> args;
      args.push_back(formatStr);
      args.push_back(lc_val);
      auto call = IR.CreateCall(printf_func, args);
      call->setTailCall();
    }
#endif

    if (selfLoopInfo == selfLoop.end()) {
      errs() << "\nThis selfloop has fixed allowable cost & is not scheduled for instrumentation in the body --> " << headerBBL->getName() << "\n";
      return;
    }

#if 0
#if 1
    /* For Debugging */
    if (
        headerBBL->getName().compare("for.body3.i") != 0
        //|| headerBBL->getName().compare("for.body71.us")==0
        //|| headerBBL->getName().compare("for.body3.i179")==0
        //|| headerBBL->getName().compare("for.body")==0
    ) {
      errs() << "Not instrumenting " << headerBBL->getName() << " for debugging\n";
      selfLoop.erase(selfLoopInfo);
      return;
    }
#else
    if (
        headerBBL->getName().compare("while.cond39.i") == 0) {
      errs() << "Not instrumenting " << headerBBL->getName() << " for debugging\n";
      selfLoop.erase(selfLoopInfo);
      return;
    }
#endif
#endif

    InstructionCost *selfLoopCost = selfLoopInfo->second;
    int numSelfLoopCost = hasConstCost(selfLoopCost);
    if (numSelfLoopCost == 0) {
      errs() << "Warning: Self loop " << *L << " has cost 0\n";
      selfLoop.erase(selfLoopInfo);
      return;
    }
    int innerLoopIterations = CommitInterval / numSelfLoopCost;
    LCCNode *loopLCC = getLastLCCofBB(headerBBL);
    UnitLCC *unitLoopLCC = static_cast<UnitLCC *>(loopLCC);

#if 0
    /* No self loop optimization for now. Remove the next three lines to enable it. */
    unitLoopLCC->instrumentForIC(selfLoopCost);
    selfLoop.erase(selfLoopInfo);
    return;
#endif

    assert((numSelfLoopCost <= CommitInterval) && "Target interval cannot be less than or equal to loop body cost!");
    assert((innerLoopIterations >= 0) && "Inner loop iterations cannot be 0 or 1");
    assert(loopLCC && "Self loop must have a container");
    assert(loopLCC->isUnitLCC() && "The last LCC of the self loop basic block must be a unit LCC");
    //if(numSelfLoopCost > CommitInterval)
    //errs() << "Self loop body cost " << numSelfLoopCost << " is greater than the Target interval " << CommitInterval << ". Therefore, every iteration will be instrumented with body cost\n";

#if 0
    /* For Debugging - comment this out once done */
    profile_loop_advanced(L);
#endif

    int innerIterationThresh = 10;
    bool hasInductionVar = false;
    if (L->getInductionVariable(*SE)) {
      hasInductionVar = true;
    }
    //if(innerLoopIterations <= innerIterationThresh || !L->isCanonical(*SE)) {
    if (innerLoopIterations <= innerIterationThresh || !hasInductionVar) {
      if (!hasInductionVar) {
        errs() << "\nThis selfloop will not be transformed since it has no induction variable --> " << headerBBL->getName() << "( " << headerBBL->getParent()->getName() << "() )\n";
      } else {
        errs() << "\nThis selfloop will not be transformed because of too low iteration count --> " << headerBBL->getName() << "( " << headerBBL->getParent()->getName() << "() ). Self Loop cost: " << numSelfLoopCost << ". Iterations: " << innerLoopIterations << "\n";
      }

      /* Generic instrumentation */
      unitLoopLCC->instrumentForIC(selfLoopCost);
    } else {
#if 1
      /* Transformation */
      errs() << "\nThis selfloop will be transformed & instrumented --> " << headerBBL->getName() << "( " << headerBBL->getParent()->getName() << "() ). Self Loop cost: " << numSelfLoopCost << ". Iterations: " << innerLoopIterations << "\n";

      // int residualCost = CommitInterval % numSelfLoopCost;
      /* Instrumenting the inner loop whole cost + residue. However, if the inner loop exited earlier because the outer loop exit cost has been reached, it will add an extra amount to the clock. This can be fixed by instrumenting the clock update based on the inner loop canonical induction variable creater in transformSelfLoopWithoutBounds. For now, we hardcode the clock update. */
      // InstructionCost *totalLoopCost = getConstantInstCost(CommitInterval - residualCost);
      BasicBlock *exitBlock = nullptr;
#if 1
      exitBlock = transformGenericSelfLoopWithoutBounds(L, innerLoopIterations, numSelfLoopCost);
#endif

      if (!exitBlock) {
        errs() << "Self loop cannot be transformed. Therefore instrumenting it.\n";
        /* Generic instrumentation */
        unitLoopLCC->instrumentForIC(selfLoopCost);
      } else {
        self_loop_transform++;
      }
#if 0
      if (L->isCanonical(*SE)) {
        errs() << "Self loop is canonical. Going for special transformation with " << innerLoopIterations << " iterations.\n";
        exitBlock = transformSelfLoopWithoutBounds(L, innerLoopIterations, numSelfLoopCost);
      } else {
        errs() << "Self loop is not canonical. Going for generic transformation with " << innerLoopIterations << " iterations.\n";
#if 0
        if (headerBBL->getName().compare("for.body.i.us.i") == 0 ||
            //headerBBL->getParent()->getName().compare("lu")==0 ||
            headerBBL->getName().compare("for.body.i.us.us.i458.us") == 0 ||
            headerBBL->getName().compare("for.body3.us57.i.us") == 0 ||
            headerBBL->getName().compare("for.body.i.us.us.i477.us") == 0 ||
            headerBBL->getName().compare("for.body.i.us.us.i.us.us") == 0 ||
            headerBBL->getName().compare("for.body3.i") == 0)
          exitBlock = transformGenericSelfLoopWithoutBounds(L, innerLoopIterations);
#else
        //if(headerBBL->getName().compare("for.body.i.us.us.i.us.us")==0) {
        //addDebugPrints(L);
        exitBlock = transformGenericSelfLoopWithoutBounds(L, innerLoopIterations, numSelfLoopCost);
        //}
#endif
      }
#endif
#else
      unitLoopLCC->instrumentForIC(selfLoopCost);
#endif
    }

    /* remove loop from map after processing */
    selfLoop.erase(selfLoopInfo);
  }

  void instrumentSESELoop(Loop *L) {
    auto seseLoopInfo = seseLoop.find(L);
    BasicBlock *headerBBL = L->getHeader();
    BasicBlock *latchBBL = L->getLoopLatch();

    if (seseLoopInfo == seseLoop.end()) {
      errs() << "This seseloop has fixed cost & is not scheduled for instrumentation in the body --> " << headerBBL->getName() << "\n";
      return;
    }

    InstructionCost *seseLoopCost = seseLoopInfo->second;
    int numSESELoopCost = hasConstCost(seseLoopCost);
    if (numSESELoopCost == 0) {
      errs() << "Warning: SESE loop " << *L << " has cost 0\n";
      seseLoop.erase(seseLoopInfo);
      return;
    }
    int innerLoopIterations = CommitInterval / numSESELoopCost;
    LCCNode *loopLCC = getLastLCCofBB(latchBBL);
    //LCCNode *loopLCC = getLastLCCofBB(headerBBL);
    UnitLCC *unitLoopLCC = static_cast<UnitLCC *>(loopLCC);

#if 0
    /* No sese loop optimization for now. Remove the next three lines to enable it. */
    unitLoopLCC->instrumentForIC(seseLoopCost);
    seseLoop.erase(seseLoopInfo);
    return;
#endif

    assert((numSESELoopCost <= CommitInterval) && "Target interval cannot be less than or equal to loop body cost!");
    assert((innerLoopIterations >= 0) && "Inner loop iterations cannot be 0 or 1");
    assert(loopLCC && "SESE loop must have a container");
    assert(loopLCC->isUnitLCC() && "The last LCC of the sese loop basic block must be a unit LCC");
    //if(numSESELoopCost > CommitInterval)
    //errs() << "SESE loop body cost " << numSESELoopCost << " is greater than the Target interval " << CommitInterval << ". Therefore, every iteration will be instrumented with body cost\n";

#if 0
    /* For Debugging - comment this out once done */
    profile_loop_advanced(L);
#endif

    int innerIterationThresh = 10;
    bool hasInductionVar = false;
    if (L->getInductionVariable(*SE)) {
      hasInductionVar = true;
    }
    //if(innerLoopIterations <= innerIterationThresh || !L->isCanonical(*SE)) {
    if (innerLoopIterations <= innerIterationThresh || !hasInductionVar) {
      if (!hasInductionVar) {
        errs() << "\nThis seseloop will not be transformed since it has no induction variable --> " << headerBBL->getName() << "( " << headerBBL->getParent()->getName() << "() )\n";
      } else {
        errs() << "\nThis seseloop will not be transformed because of too low iteration count --> Header: " << headerBBL->getName() << ", Latch: " << latchBBL->getName() << "( " << headerBBL->getParent()->getName() << "() ). SESE Loop cost: " << numSESELoopCost << ". Iterations: " << innerLoopIterations << "\n";
      }

      /* Generic instrumentation */
      unitLoopLCC->instrumentForIC(seseLoopCost);
    } else {
#if 1
      /* Transformation */
      errs() << "\nThis seseloop will be transformed & instrumented --> " << headerBBL->getName() << "( " << headerBBL->getParent()->getName() << "() ). SESE Loop cost: " << numSESELoopCost << ". Iterations: " << innerLoopIterations << "\n";

      // int residualCost = CommitInterval % numSESELoopCost;
      /* Instrumenting the inner loop whole cost + residue. However, if the inner loop exited earlier because the outer loop exit cost has been reached, it will add an extra amount to the clock. This can be fixed by instrumenting the clock update based on the inner loop canonical induction variable creater in transformSESELoopWithoutBounds. For now, we hardcode the clock update. */
      // InstructionCost* totalLoopCost = getConstantInstCost(CommitInterval - residualCost);
      BasicBlock *exitBlock = nullptr;
#if 1
      exitBlock = transformSESELoopWithoutBounds(L, innerLoopIterations, numSESELoopCost);
#endif

      if (!exitBlock) {
        errs() << "SESE loop cannot be transformed. Therefore instrumenting it.\n";
        /* Generic instrumentation */
        unitLoopLCC->instrumentForIC(seseLoopCost);
      } else {
        generic_loop_transform++;
      }
#else
      unitLoopLCC->instrumentForIC(seseLoopCost);
#endif
    }

    /* remove loop from map after processing */
    seseLoop.erase(seseLoopInfo);
  }

  /* This function instruments extra blocks for direct branches. 
     * It works on the global list of header blocks, which get refreshed for every function. */
  void instrumentLoops(Function *F) {
    if (!checkIfInstGranIsOpt()) {
      errs() << "Non-optimistic instrumentations are done separately. This is an invalid path. Aborting.\n";
      exit(1);
    }

#if 0
    errs() << "Loops in function:- \n";
    std::set<Loop *> visitedLoops;
    std::set<Loop *> unvisitedLoops;
    if (LI->begin() == LI->end())
      return;

    for (LoopInfo::iterator itLI = LI->begin(), itLIEnd = LI->end(); itLI != itLIEnd; ++itLI) {
      Loop *firstLoop = *itLI;
      unvisitedLoops.insert(firstLoop);
      errs() << "Start Loop: " << *firstLoop << "\n";
    }

    while (!unvisitedLoops.empty()) {
      auto itUVL = unvisitedLoops.begin();
      Loop *L = *itUVL;
      errs() << "Size of unvisited loops set: " << unvisitedLoops.size() << "\n";
      errs() << "Loop: " << *L << "\n";
      unvisitedLoops.erase(itUVL);
      auto presentInVisited = visitedLoops.find(L);
      if (presentInVisited == visitedLoops.end()) {
        visitedLoops.insert(L);
      }

      //errs() << "Neighbour size: " << ;
      for (LoopInfo::iterator itL = L->begin(), itLEnd = L->end(); itL != itLEnd; ++itL) {
        Loop *neighbourL = *itL;
        //auto presentInUnvisited = unvisitedLoops.find(neighbourL);
        //if(presentInUnvisited == unvisitedLoops.end()) {
        unvisitedLoops.insert(neighbourL);
        //}
      }
    }
#endif

    int selfLoopCount = selfLoop.size();
    int seseLoopCount = seseLoop.size();
    errs() << "\nInstrumenting loops (self loop count: " << selfLoopCount << ", sese loop count: " << seseLoopCount << ") for function " << F->getName() << "\n";

    errs() << "\nLoops scheduled for transform for " << F->getName() << ":- \n";
    for (auto selfLoopInfo : selfLoop) {
      errs() << "Self Loop: " << *(selfLoopInfo.first) << "\n";
    }
    for (auto seseLoopInfo : seseLoop) {
      errs() << "Sese Loop: " << *(seseLoopInfo.first) << "\n";
    }
    errs() << "\n";

    //if(F->getName().compare("main") == 0)
    //return;

    /* Find all loops first */
    std::set<Loop *> visitedSelfLoops;
    std::set<Loop *> visitedSeseLoops;
    std::set<Loop *> unvisitedLoops;
    if (LI->begin() == LI->end()) {
      return;
    }

    /* Initializing unvisitedLoops with the first level loops */
    for (LoopInfo::iterator itLI = LI->begin(), itLIEnd = LI->end(); itLI != itLIEnd; ++itLI) {
      Loop *firstLoop = *itLI;
      unvisitedLoops.insert(firstLoop);
      //errs() << "Start Loop: " << *firstLoop << "\n";
    }

    /* Visiting all loops by breadth first traversal */
    while (!unvisitedLoops.empty()) {
      auto itUVL = unvisitedLoops.begin();
      Loop *L = *itUVL;
      unvisitedLoops.erase(itUVL);
      //errs() << "Loop: " << *L << "\n";

      if (checkIfSelfLoop(L)) {
        auto presentInVisited = visitedSelfLoops.find(L);
        if (presentInVisited == visitedSelfLoops.end()) {
          visitedSelfLoops.insert(L);
        }
      } else if (checkIfSESELoop(L)) {
        auto presentInVisited = visitedSeseLoops.find(L);
        if (presentInVisited == visitedSeseLoops.end()) {
          visitedSeseLoops.insert(L);
        }
      }

      //errs() << "Neighbour size: " << ;
      for (LoopInfo::iterator itL = L->begin(), itLEnd = L->end(); itL != itLEnd; ++itL) {
        Loop *neighbourL = *itL;
        //auto presentInUnvisited = unvisitedLoops.find(neighbourL);
        //if(presentInUnvisited == unvisitedLoops.end()) {
        unvisitedLoops.insert(neighbourL);
        //}
      }
    }

    /* Instrument all self loops first */
    for (auto L : visitedSelfLoops) {
      errs() << "\nAttempting to transform function " << F->getName() << "()'s self loop " << *L << "\n";
      //if(L->getHeader()->getName().compare("for.body3.us.i")==0)
      instrumentSelfLoop(L);
      //errs() << "\n" << F->getName() << "(): Transformed Self loop " << *L << "\n";
    }

    /* Instrument all sese loops in depth-wise order next */
    while (!visitedSeseLoops.empty()) {
      int maxDepth = 0;
      Loop *maxDepthLoop = nullptr;
      for (auto L : visitedSeseLoops) {
        int depth = L->getLoopDepth();
        if (maxDepth <= depth) {
          maxDepth = depth;
          maxDepthLoop = L;
        }
      }

      visitedSeseLoops.erase(maxDepthLoop);
      errs() << "\nAttempting to transform function " << F->getName() << "()'s max-depth sese loop " << *maxDepthLoop << "\n";
      instrumentSESELoop(maxDepthLoop);
      //errs() << "\n" << F->getName() << "(): Transformed loop " << *maxDepthLoop << "\n";
    }

    /* Sanity check */
    if (selfLoop.size() > 0) {
      errs() << "Following self loops(curr count: " << selfLoop.size() << ", initial count: " << selfLoopCount << ") are not found in " << F->getName() << "\n";
      for (auto selfLoopInfo : selfLoop) {
        Loop *L = selfLoopInfo.first;
        errs() << *L << "\n";
      }
#if 1
      exit(1);
#endif
    }

    /* Sanity check */
    if (seseLoop.size() > 0) {
      errs() << "Following sese loops(curr count: " << seseLoop.size() << ", initial count: " << seseLoopCount << ") are not found in " << F->getName() << "\n";
      for (auto seseLoopInfo : seseLoop) {
        Loop *L = seseLoopInfo.first;
        errs() << *L << "\n";
      }
#if 1
      /* open this after done with debugging */
      exit(1);
#endif
    }
  }

  /* This function instruments extra blocks for direct branches. 
     * It works on the global list of header blocks, which get refreshed for every function. */
  void instrumentBlocks(Function *F) {

    if (!checkIfInstGranIsOpt()) {
      errs() << "Naive instrumentation is done separately. This is an invalid path. Aborting.\n";
      exit(1);
    }

    for (auto headerIt = directBranch.begin(); headerIt != directBranch.end(); headerIt++) {
      BasicBlock *head = headerIt->first;
      DomTreeNode *currentPDTNode = PDT->getNode(head);
      DomTreeNode *postDomNode = currentPDTNode->getIDom();
      BasicBlock *tail = postDomNode->getBlock();
      auto headTermInst = head->getTerminator();
#ifdef CRNT_DEBUG
      errs() << "Instrument between " << head->getName() << " and " << tail->getName() << ". Adding cost " << *headerIt->second << " to it!\n";
#endif
      std::string name(head->getName());
      name.append("DirectSucc");

#if 0
      /* Just prints for debugging */
      if (Loop *L = LI->getLoopFor(head)) {
        errs() << "Before creating new block " << name << ", loop was: " << *L;
        if (auto exitingBB = L->getExitingBlock())
          errs() << "Loop had exiting block " << exitingBB->getName() << "\n";
        if (auto latchBB = L->getLoopLatch())
          errs() << "Loop had latch block " << latchBB->getName() << "\n";
        if (auto exitBB = L->getExitBlock())
          errs() << "Loop had exit block " << exitBB->getName() << "\n";
      }
#endif

      BasicBlock *directBlock = BasicBlock::Create(head->getContext(), name, head->getParent(), tail);
      IRBuilder<> IR(directBlock);

      /* Fix the passes that were hampered due to this instrumentation */
      assert((LI->getLoopFor(head) == LI->getLoopFor(tail)) && "For creating the direct block the predecessor & successor are supposed to be part of the same loop!");
      if (Loop *L = LI->getLoopFor(head)) {
        L->addBasicBlockToLoop(directBlock, *LI);
      }

      if (DT) {
        DT->addNewBlock(directBlock, head);
      }
      if (PDT) {
        PDT->addNewBlock(directBlock, tail);
      }

      /* Replace the branch instruction successor for the header */
      if (BranchInst *branchInst = dyn_cast<BranchInst>(headTermInst)) {
        int numOfSucc = branchInst->getNumSuccessors();
        /* Find the direct successor */
        for (int idx = 0; idx < numOfSucc; idx++) {
          BasicBlock *succ = branchInst->getSuccessor(idx);
          if (succ == tail) {
            //errs() << "Found tail " << tail->getName() << " at index " << idx << "\n";
            branchInst->setSuccessor(idx, directBlock);
            /* Instrument the cost in this new block */
            break;
          }
        }
      } else if (SwitchInst *switchInst = dyn_cast<SwitchInst>(headTermInst)) {
        int numOfSucc = switchInst->getNumSuccessors();
        /* Find the direct successor */
        for (int idx = 0; idx < numOfSucc; idx++) {
          BasicBlock *succ = switchInst->getSuccessor(idx);
          if (succ == tail) {
            //errs() << "Found tail " << tail->getName() << " at index " << idx << "\n";
            switchInst->setSuccessor(idx, directBlock);
            /* Instrument the cost in this new block */
            break;
          }
        }
      } else {
        assert("This is not a proper direct branch to instrument");
      }

      /* Loop over any phi node in the basic block, updating the BB field of incoming values */
      PHINode *PN;
      for (BasicBlock::iterator phiIt = tail->begin(); (PN = dyn_cast<PHINode>(phiIt)); ++phiIt) {
        //errs() << "For tail block " << tail->getName() << " --> changing phi inst: " << *PN << "\n";
        int IDX = PN->getBasicBlockIndex(head);
        while (IDX != -1) {
          PN->setIncomingBlock((unsigned)IDX, directBlock);
          IDX = PN->getBasicBlockIndex(head);
        }
      }

      /* Instrumenting the cost in the new branch */
      auto newI = IR.CreateBr(tail);
      Value *costVal = scevToIR(newI, headerIt->second);
#if 0
      instrumentCI(newI, costVal); // instrumenting here leads to problems with broken loop info & dominator trees. So schedule it later.
#else
      LCCNode *newLCC = new UnitLCC(lccIDGen++, directBlock, directBlock->getFirstNonPHI(), &(directBlock->back()), false);
      UnitLCC *newUnitLCC = static_cast<UnitLCC *>(newLCC);
      newUnitLCC->instrumentValueForIC(costVal);
      std::vector<LCCNode *> containers;
      containers.push_back(newLCC);
      bbToContainersMap[directBlock] = containers;
#endif
    }

    DT->recalculate(*F);
    PDT->recalculate(*F);
    BPI->calculate(*F, *LI, nullptr, DT, PDT);
  }

  void runPasses(Function *F) {

    if (!checkIfInstGranIsOpt()) {
      errs() << "This is not the path for non-opt configurations. Aborting.\n";
      exit(1);
    }

    //if(F->getName().compare("sha256_blocks") != 0)
    //return;
    errs() << "************************ Function " << F->getName() << " ************************\n";

    /* order of the calls are important */

#if 0
    if ((F->getName().compare("Transpose") != 0) && (F->getName().compare("main") != 0))
      return;
    errs() << "Running CI for " << F->getName() << "\n";

    //profile_loops(F);
#endif
    /* Traverse & reduce graph */
    traverseNReduce(F);

    /* Manage the V & inverted V shape LCCs & annotate them for instrumentation */
    manageDanglingLCCs(F);

    /* Cost evaluation & annotate instrumentation */
    costEvaluate(F);

#if 0
    /* Create local variables for loading & storing the thread local counter & flag */
    if (InstGranularity == OPTIMIZE_HEURISTIC || InstGranularity == OPTIMIZE_ACCURATE) {
      initializeLocals(F);
    }
#endif

    /* Instrument new blocks for branches, where needed & instruments costs in them */
    instrumentBlocks(F);
    /* Instrument new inner loops for self loops */
    instrumentLoops(F);

    /* Replace any function calls if needed */
#ifdef ALL_DEBUG
    errs() << "\nPrinting list of instrumentation :-\n";
    printInstrStats(F);
#endif
    //replaceCallInst(F);
#ifdef ALL_DEBUG
    //errs() << "\nPrinting list of instrumentation after replacing instructions:-\n";
    //printInstrStats(F);
#endif

    computeCostEvalStats(F);

    /* Instrument function */
    instrumentFunc(F);

    computeInstrStats(F);

    /* Print Statistics */
#ifdef PROFILING
    printStats(F);
#endif
#ifdef LC_DEBUG
    printInstrStats(F);
#endif
  }

  /* returns true if the function contains a fence */
  bool makeContainersOfBB(BasicBlock *block) {

    bool hasFence = false;

    Instruction *startInst = checkForPhi(&(block->front()));
    assert(startInst);
    BasicBlock *currentBB = startInst->getParent();

    /* In rare cases, for thread functions, an empty container may have been created earlier 
       * for this block, because it did not have the first valid instruction. In this case, the
       * the block is skipped. This is not added to the global list of containers, & is only
       * used to ensure that it does not get processed again while creating a container or 
       * the graph */
    if (currentBB != block) {
      for (Function::iterator BB(block), endBB(currentBB); BB != endBB; ++BB) {
        BasicBlock *ignBlock = &*BB;
        std::vector<LCCNode *> empty_container;
        LCCNode *newLCC = new UnitLCC(lccIDGen++, ignBlock, nullptr, nullptr, false);
        empty_container.push_back(newLCC);
        bbToContainersMap[ignBlock] = empty_container;
#ifdef LC_DEBUG
        errs() << "Created empty container for thread function " << block->getParent()->getName() << "()'s block " << ignBlock->getName() << "\n";
#endif
      }
    }

    std::vector<LCCNode *> containers;
    BasicBlock::iterator instItr(startInst);

    for (; instItr != currentBB->end(); instItr++) {
      Instruction *inst = &(*instItr);
      if (CallInst *ci = dyn_cast<CallInst>(inst)) {
        auto calledFunc = ci->getCalledFunction();
        if (calledFunc) {
          if (isFenceFunc(calledFunc)) {
            hasFence = true;
#ifdef LC_DEBUG
            errs() << "At a fence: " << calledFunc->getName() << "\n";
#endif
            if (!libraryInstructionCosts.count(calledFunc->getName().str())) {
              errs() << "Fence function " << calledFunc->getName() << "()'s cost is not found in the library. Aborting\n";
              assert("Fence function costs are not found in the library function cost repository!");
            }
          }
          /* if called function is an internal function with a fence inside it */
          else {
            auto funcInfoIt = computedFuncInfo.find(calledFunc);
            if (funcInfoIt != computedFuncInfo.end()) {
              hasFence = funcInfoIt->second->hasFence;
#ifdef LC_DEBUG
              if (hasFence)
                errs() << "At a fence for calling an internal function with fence: " << calledFunc->getName() << "\n";
#endif
            }
          }

          if (hasFence) {
            /* For sync operation fences, new containers are created
               * Important: Fence is always included in the preceding container */
            LCCNode *newLCC = new UnitLCC(lccIDGen++, currentBB, startInst, inst, true);
            /************************ Test Printing **************************/
#ifdef LC_DEBUG
            errs() << "\nUnit Container(" << newLCC->getID() << "):- (";
            printUnitLCCSet(newLCC);
            errs() << ")\n";
#endif
            //errs() << "Start: " << *startInst << ", End: " << inst << "\n";
            containers.push_back(newLCC);
            BasicBlock::iterator instIt(inst);
            instIt++;
            if (instIt == currentBB->end()) {
              startInst = nullptr;
            } else {
              startInst = &*instIt;
            }
          }
        }
      }
      /************************** For unreachable instructions************************/
      else if (isa<UnreachableInst>(inst)) {

        Instruction *termInst = currentBB->getTerminator();
        int numSucc = termInst->getNumSuccessors();

        LCCNode *newLCC;
        if (numSucc) {
          newLCC = new UnitLCC(lccIDGen++, currentBB, startInst, inst, true);
        } else {
          newLCC = new UnitLCC(lccIDGen++, currentBB, startInst, inst, false, true);
        }

        /************************ Test Printing **************************/
#ifdef LC_DEBUG
        errs() << "\nUnit Container(" << newLCC->getID() << "):- (";
        printUnitLCCSet(newLCC);
        errs() << ")\n";
#endif
        //errs() << "Start: " << *startInst << ", End: " << inst << "\n";
        containers.push_back(newLCC);
        if (numSucc) {
          BasicBlock::iterator instIt(inst);
          instIt++;
          if (instIt == currentBB->end()) {
            startInst = nullptr;
          } else {
            startInst = &*instIt;
          }
        } else {
          startInst = nullptr;
          break;
        }
      }
    }

    /* If we haven't already created all containers */
    if (startInst) {
      LCCNode *newLCC = new UnitLCC(lccIDGen++, currentBB, startInst, &(currentBB->back()), false);

      /************************ Test Printing **************************/
#ifdef LC_DEBUG
      errs() << "\nUnit Container(" << newLCC->getID() << "):- (";
      printUnitLCCSet(newLCC);
      errs() << ")\n";
#endif
      containers.push_back(newLCC);
      //errs() << "Start: " << *startInst << ", End: " << currentBB->back() << "\n";
      //errs() << "Last container:- " << currentBB->getParent()->getName() << "() : " << currentBB->getName() << "\n";
    }

    bbToContainersMap[currentBB] = containers;
#ifdef LC_DEBUG
    if (containers.size() > 1)
      errs() << currentBB->getParent()->getName() << "() : " << currentBB->getName() << " ---> " << containers.size() << " containers \n";
#endif
    /* Add the new containers to the global list */
    for (LCCNode *container : containers) {
      globalOuterLCCList[currentBB->getParent()].push_back(container);
    }

    return hasFence;
  }

  /* these update rules are not generic & used for specific cases where a new block is inserted to transform a branch to a known pattern */
  void updateContainerCFG(BasicBlock *newBlock, SmallVector<BasicBlock *, 10> nearestPreds, BasicBlock *nearestSucc) {

    if (bbToContainersMap.end() == bbToContainersMap.find(newBlock)) {
      errs() << "New block's container has not be created yet.";
      exit(1);
    }

    /* get new block's container */
    std::vector<LCCNode *> containers = bbToContainersMap[newBlock];
    if (containers.size() != 1) {
      errs() << "New block has multiple containers, which is not possible.";
      exit(1);
    }
    LCCNode *newLCC = getSingleLCCofBB(newBlock);

    /* Get predecessor LCCs of successor block */
    if (bbToContainersMap.end() == bbToContainersMap.find(nearestSucc)) {
      errs() << "New block's successor's (" << nearestSucc->getName() << ") container has not be created yet.";
      exit(1);
    }

    auto succUnitLCC = getFirstLCCofBB(nearestSucc);
    if (!succUnitLCC) {
      errs() << "New block's successor container's (" << nearestSucc->getName() << ")  cannot be found.";
      exit(1);
    }
    auto succLCC = succUnitLCC->getOuterMostEnclosingLCC();
    auto predOfSuccLCC = succLCC->getPredSet();
    bool addedSuccLCC = false;

    for (auto predBB : nearestPreds) {

      /* For each predecessor block, get successor LCCs */
      if (bbToContainersMap.end() == bbToContainersMap.find(predBB)) {
        errs() << "New block's predecessor's (" << predBB->getName() << ") container has not be created yet.";
        exit(1);
      }

      auto predUnitLCC = getLastLCCofBB(predBB);
      if (!predUnitLCC) {
        errs() << "New block's predecessor's (" << predBB->getName() << ") container cannot be found.";
        exit(1);
      }
      auto predLCC = predUnitLCC->getOuterMostEnclosingLCC();
      auto succOfPredLCC = predLCC->getSuccSet();

      /* Find the pred LCC in the pred set of the Succ LCC, & add them to the new LCC */
      auto connectedPredLCCIt = predOfSuccLCC.find(predLCC);
      /* sanity check */
      if (connectedPredLCCIt == predOfSuccLCC.end()) {
        errs() << "Block " << predBB->getName() << "'s LCC was not found in the pred LCC list of successor " << nearestSucc->getName() << "\n";
        exit(1);
      }
      auto connectedPredLCC = *connectedPredLCCIt;
      newLCC->addPredLCC(predLCC, false); /* Making an assumption that such connections are never fence */
#ifdef ALL_DEBUG
      errs() << "Added predecessor ";
      printUnitLCCSet(predLCC);
      errs() << " (" << predBB->getName() << ") to new block " << newBlock->getName() << "\n";
#endif

      /* Find the succ LCC in the succ set of the Pred LCC, & add them to the new LCC. Done only once since there is only one succ LCC */
      if (!addedSuccLCC) {
        addedSuccLCC = true;
        auto connectedSuccLCCIt = succOfPredLCC.find(succLCC);
        /* sanity check */
        if (connectedSuccLCCIt == succOfPredLCC.end()) {
          errs() << "Block " << nearestSucc->getName() << "'s LCC was not found in the succ LCC list of predecessor " << predBB->getName() << "\n";
          exit(1);
        }
        newLCC->addSuccLCC(succLCC, false); /* Making an assumption that such connections are never fence */
        succLCC->addPredLCC(newLCC, false); /* Making an assumption that such connections are never fence */
#ifdef ALL_DEBUG
        errs() << "Added successor ";
        printUnitLCCSet(succLCC);
        errs() << " (" << nearestSucc->getName() << ") to new block " << newBlock->getName() << "\n";
        errs() << "Added predecessor " << newBlock->getName() << " to postdom " << nearestSucc->getName() << "\n";
#endif
      }

      /* first pred is replaced by new LCC. Rest are removed. */
      succLCC->removeSuccLCC(predLCC);
#ifdef ALL_DEBUG
      errs() << "Removed predecessor ";
      printUnitLCCSet(predLCC);
      errs() << " (" << predBB->getName() << ") of postdom ";
      printUnitLCCSet(succLCC);
      errs() << " (" << nearestSucc->getName() << ")\n";
#endif

      //predLCC->replaceSucc(succLCC, newLCC);
      predLCC->removeSuccLCC(succLCC);
      predLCC->addSuccLCC(newLCC, false); /* Making an assumption that such connections are never fence */
#ifdef ALL_DEBUG
      errs() << "Removed successor ";
      printUnitLCCSet(succLCC);
      errs() << " (" << nearestSucc->getName() << ") of predecessor ";
      printUnitLCCSet(predLCC);
      errs() << " (" << predBB->getName() << ")\n";
      errs() << "Added successor " << newBlock->getName() << " to predecessor ";
      printUnitLCCSet(predLCC);
      errs() << " (" << predBB->getName() << ")\n";
#endif
    }

    /* Add newLCC to the global list of LCCs - already added through makeContainersOfBB */
    //globalOuterLCCList[F].push_back(newLCC);
#ifdef ALL_DEBUG
    Function *F = newBlock->getParent();
    errs() << "Not adding ";
    printUnitLCCSet(newLCC);
    errs() << " to the global list of outer LCCs\n";
    errs() << "Number of outer level LCCS: " << globalOuterLCCList[F].size() << "\n";
    errs() << "Global set: ";
    for (auto lcc : globalOuterLCCList[F]) {
      printUnitLCCSet(lcc);
      errs() << ",";
    }
    errs() << "\n";
#endif
  }

  void createContainerCFG(BasicBlock *currentBB) {

    std::vector<LCCNode *> containers = bbToContainersMap[currentBB];
    /* Create connections between containers of the same basicblock due to fences in between */
    if (containers.size() > 1) {
      bool first = true;
      LCCNode *prevLCC;
      for (LCCNode *blockContainer : containers) {
        if (first) {
          first = false;
          prevLCC = blockContainer;
        } else {
          UnitLCC *blockUnitContainer = static_cast<UnitLCC *>(blockContainer);
          if (!blockUnitContainer->isExitBlockLCC()) {
            prevLCC->addSuccLCC(blockContainer, true);
            blockContainer->addPredLCC(prevLCC, true);
          }
          prevLCC = blockContainer;
        }
      }
    }

    if (containers.size() == 1) {
      LCCNode *singleLCC = *(containers.begin());
      UnitLCC *unitSingleLCC = static_cast<UnitLCC *>(singleLCC);
      if (unitSingleLCC->isEmptyLCC() || unitSingleLCC->isExitBlockLCC()) {
#ifdef LC_DEBUG
        errs() << "Skipping edge creation for empty or unreachable exiting block " << currentBB->getName() << "\n";
#endif
        if (unitSingleLCC->isExitBlockLCC()) {
          errs() << "Skipping unreachable block " << currentBB->getName() << "\n";
        }
        return;
      }
    }

    /* --- Create connections with other blocks' containers. If a block's container or its preceding block's container ends with a fence, add fence edges between them --- */

    /* --- Creating connections with predecessors --- */
    LCCNode *firstContainer = containers.front();
    for (pred_iterator PI = pred_begin(currentBB), E = pred_end(currentBB); PI != E; ++PI) {
      LCCNode *lastLCCofPrev = getLastLCCofBB(*PI);

      //errs() << "Block " << currentBB->getName() << " has predecessor " << (*PI)->getName() << "\n";

      UnitLCC *lastUnitLCCofPrev = static_cast<UnitLCC *>(lastLCCofPrev);

      /* Create no edge with an empty LCC */
      if (lastUnitLCCofPrev->isEmptyLCC() || lastUnitLCCofPrev->isExitBlockLCC()) {
#ifdef LC_DEBUG
        errs() << "Skipping edge creation with predecessor " << (*PI)->getName() << " of block " << currentBB->getName() << " in function " << currentBB->getParent()->getName() << "\n";
#endif
        if (lastUnitLCCofPrev->isExitBlockLCC()) {
          errs() << "Skipping unreachable block " << currentBB->getName() << "\n";
        }
        continue;
      }

      Instruction *lastInst = lastUnitLCCofPrev->getLastInst();
      bool isFenceSeparated = false;
      bool isUnreachable = false;
      if (isa<UnreachableInst>(lastInst)) {
        isUnreachable = true;
      } else if (CallInst *ci = dyn_cast<CallInst>(lastInst)) {
        auto calledFunc = ci->getCalledFunction();
        if (calledFunc) {
          if (isFenceFunc(calledFunc)) {
            isFenceSeparated = true;
          }
        }
      }

      /* Fence & unreachable paths are treated in a similar way */
      if (isFenceSeparated || isUnreachable) {
        errs() << "Added fence between preceding block " << lastInst->getParent()->getName() << " and " << currentBB->getName() << "\n";
        firstContainer->addPredLCC(lastLCCofPrev, true);
      } else {
        firstContainer->addPredLCC(lastLCCofPrev, false);
      }
    }

    /* --- Creating connections with successors --- */
    LCCNode *lastContainer = containers.back();
    for (succ_iterator SI = succ_begin(currentBB), E = succ_end(currentBB); SI != E; ++SI) {
      LCCNode *firstLCCofSucc = getFirstLCCofBB(*SI);
      //errs() << "Block " << currentBB->getName() << " has successor " << (*SI)->getName() << "\n";

      /* Create no edge with an empty LCC */
      UnitLCC *firstUnitLCCofSucc = static_cast<UnitLCC *>(firstLCCofSucc);
      if (firstUnitLCCofSucc->isEmptyLCC() || firstUnitLCCofSucc->isExitBlockLCC()) {
#ifdef LC_DEBUG
        errs() << "Skipping edge creation with predecessor " << (*SI)->getName() << " of block " << currentBB->getName() << " in function " << currentBB->getParent()->getName() << "\n";
#endif
        continue;
      }
      //assert((!firstUnitLCCofSucc->isEmptyLCC()) && "Successor cannot be an empty LCC!");

      UnitLCC *lastUnitContainer = static_cast<UnitLCC *>(lastContainer);
      Instruction *lastInst = lastUnitContainer->getLastInst();
      bool isFenceSeparated = false;
      bool isUnreachable = false;
      if (isa<UnreachableInst>(lastInst)) {
        isUnreachable = true;
      } else if (CallInst *ci = dyn_cast<CallInst>(lastInst)) {
        auto calledFunc = ci->getCalledFunction();
        if (calledFunc) {
          if (isFenceFunc(calledFunc)) {
            isFenceSeparated = true;
          }
        }
      }

      /* Fence & unreachable paths are treated in a similar way */
      if (isFenceSeparated || isUnreachable) {
        errs() << "Added fence between preceding block " << lastInst->getParent()->getName() << " and " << SI->getName() << "\n";
        lastContainer->addSuccLCC(firstLCCofSucc, true);
      } else {
        lastContainer->addSuccLCC(firstLCCofSucc, false);
      }
    }
  }

  /* Initialize the container graph from the actual graph */
  void initializeLCCGraph(Function *F) {

    /* --- re-initializations per function --- */
#ifdef LC_DEBUG
    errs() << "\n***************** LCC Graph Initialization *******************\n";
#endif
    lccIDGen = 0;
    bbToContainersMap.clear();
    callInstToReplaceForPC.clear();
    callInstToReplaceForIC.clear();
    directBranch.clear();
    selfLoop.clear();
    seseLoop.clear();

    lccIDGen = 0;
    applyrule1 = 0;
    applycontrule1 = 0;
    applyrule2 = 0;
    applycontrule2 = 0;
    rule2savedInst = 0;
    applyrule3 = 0;
    applycontrule3 = 0;
    rule3savedInst = 0;
    applyrule4 = 0;
    rule4savedInst = 0;
    applyrule5 = 0;
    rule5savedInst = 0;
    applyrule6 = 0;
    unhandled_loop = 0;
    instrumentedInst = 0;
    applyrule7 = 0;
    applycontrule7 = 0;
    rule7savedInst = 0;
    ruleCoredet = 0;
    self_loop_transform = 0;
    generic_loop_transform = 0;

    /* Set the function fence info */
    FuncInfo *fInfo = new FuncInfo();
    fInfo->cost = nullptr;   /* will be computed at the time of cost evaluation */
    fInfo->hasFence = false; /* Initialize to false. Set to true even if there is one fence */

    /* Create unit containers */
    for (Function::iterator BB = F->begin(), endBB = F->end(); BB != endBB; ++BB) {

      /* Read rare case comment in makeContainersOfBB */
      auto bbIt = bbToContainersMap.find(&*BB);
      if (bbIt != bbToContainersMap.end()) {
#ifdef LC_DEBUG
        errs() << "Block " << BB->getName() << " had already been processed before\n";
        continue;
#endif
      }

      bool hasFence = makeContainersOfBB(&*BB);
      if (hasFence) {
#ifdef LC_DEBUG
        errs() << "Internal function " << F->getName() << " has a fence!!!\n";
#endif
        fInfo->hasFence = true;
      }
    }

    /* Create container graph */
    for (Function::iterator BB = F->begin(), endBB = F->end(); BB != endBB; ++BB) {
      createContainerCFG(&*BB);
    }

    /* Set function info */
    computedFuncInfo[F] = fInfo;
  }

  /* if the dominator of the postdominator of start, is not equal to start & dominates start */
  bool matchComplexBranchForward(BasicBlock *start, BasicBlock **end) {

    DomTreeNode *startPDN = PDT->getNode(start);
    if (!startPDN) {
      return false;
    }
    DomTreeNode *pdnToStart = startPDN->getIDom();
    if (!pdnToStart) {
      return false;
    }
    BasicBlock *postDomBB = pdnToStart->getBlock();
    if (!postDomBB) {
      return false;
    }

    *end = postDomBB;

    DomTreeNode *endDN = DT->getNode(postDomBB);
    if (!endDN) {
      return false;
    }
    DomTreeNode *dnToEnd = endDN->getIDom();
    if (!dnToEnd) {
      return false;
    }
    BasicBlock *domBB = dnToEnd->getBlock();
    if (!domBB) {
      return false;
    }

    /* This is already a single entry single exit branch */
    if (domBB == start) {
      return false;
    }

    /* This will be taken care of in the backward direction for the end node */
    if (!DT->dominates(domBB, start)) {
      return false;
    }

    /* both blocks must belong to the same loop */
    auto L1 = LI->getLoopFor(start);
    auto L2 = LI->getLoopFor(*end);
    if (L1 != L2) {
      return false;
    }

#if 0
    /* Start should not be reachable from the end in the forward direction */
    if (isPotentiallyReachable(*end, start, DT, LI)) {
      return false;
    }
#endif
    if (!DFSCheckForComplexBr(start, *end, true, start)) {
      //errs() << "There are edges to this section before the postdom, that arrive from before the section\n";
      return false;
    }

    int numCountMergeEdges = 0;
    for (auto predIt = pred_begin(*end), predEnd = pred_end(*end); predIt != predEnd; ++predIt) {
      auto predBB = *predIt;
      if (DT->dominates(start, predBB)) {
        numCountMergeEdges++;
      }
    }

    /* must have at least 2 edges to merge */
    if (numCountMergeEdges <= 1) {
      return false;
    }

    return true;
  }

  /* start is the partial postdom, end is the dominator */
  bool matchComplexBranchBackward(BasicBlock *start, BasicBlock **end) {

    DomTreeNode *startDN = DT->getNode(start);
    if (!startDN) {
      return false;
    }
    DomTreeNode *dnToStart = startDN->getIDom();
    if (!dnToStart) {
      return false;
    }
    BasicBlock *domBB = dnToStart->getBlock();
    if (!domBB) {
      return false;
    }

    *end = domBB;

    DomTreeNode *endPDN = PDT->getNode(domBB);
    if (!endPDN) {
      return false;
    }
    DomTreeNode *pdnToEnd = endPDN->getIDom();
    if (!pdnToEnd) {
      return false;
    }
    BasicBlock *postDomBB = pdnToEnd->getBlock();
    if (!postDomBB) {
      return false;
    }

    /* This is already a single entry single exit branch */
    if (postDomBB == start) {
      return false;
    }

    /* This will be taken care of in the backward direction for the end node */
    if (!PDT->dominates(postDomBB, start)) {
      return false;
    }

    /* both blocks must belong to the same loop */
    auto L1 = LI->getLoopFor(start);
    auto L2 = LI->getLoopFor(*end);
    if (L1 != L2) {
      return false;
    }

    /* Start should not be reachable from the end in the forward direction */
    if (isPotentiallyReachable(start, *end, DT, LI)) {
      return false;
    }

    /* call after isPotentiallyReachable check to avoid infinite loop in dfs traversal */
    if (!DFSCheckForComplexBr(start, *end, false, start)) {
      //errs() << "There are edges to this section before the postdom, that arrive from before the section\n";
      return false;
    }

    int numCountMergeEdges = 0;
    for (auto succIt = succ_begin(*end), succEnd = succ_end(*end); succIt != succEnd; ++succIt) {
      auto succBB = *succIt;
      if (PDT->dominates(start, succBB)) {
        numCountMergeEdges++;
      }
    }

    /* must have at least 2 edges to merge */
    if (numCountMergeEdges <= 1) {
      return false;
    }

    errs() << "matchComplexBranchBackward() :- In " << start->getParent()->getName() << "(), found a backward complex branch match starting at block " << start->getName() << " and ending at its dominator " << (*end)->getName() << "\n";

    return true;
  }

  bool matchComplexBranch(BasicBlock *start, BasicBlock **end, bool *direction) {
    if (matchComplexBranchForward(start, end)) {
      *direction = true;
      return true;
    }

#if 0
    if (matchComplexBranchBackward(start, end)) {
      *direction = false;
      return true;
    }
#endif

    return false;
  }

  void transformComplexBranchForward(BasicBlock *start, BasicBlock *end) {

    SmallVector<BasicBlock *, 10> nearestPreds;
    /* For forward direction, find all predecessors of postDominator that are dominated by start */
    for (auto predIt = pred_begin(end), predEnd = pred_end(end); predIt != predEnd; ++predIt) {
      auto predBB = *predIt;
      if (!DT->dominates(start, predBB)) {
        continue;
      } else {
        nearestPreds.push_back(predBB);
      }
    }
    ArrayRef<BasicBlock *> predArrList(nearestPreds.begin(), nearestPreds.end());
    BasicBlock *newBlock = SplitPostDomPredecessors(end, predArrList, "_dummy", DT, LI, true);
    if (!newBlock) {
      errs() << "SplitPostDomPredecessors() could not split the predecessors of the postdominator. Aborting.\n";
      exit(1);
    }

    Function *F = start->getParent();
    DT->recalculate(*F);
    PDT->recalculate(*F);
    BPI->calculate(*F, *LI, nullptr, DT, PDT);

#ifdef CRNT_DEBUG
    /********************************* Debug prints *****************************/
    errs() << "Transformation rule applied on func " << F->getName() << " for branch starting at " << start->getName() << " and ending at " << end->getName() << "\n";
    errs() << "Closest predecessors to postDom:- \n";
    for (auto pred : nearestPreds) {
      errs() << pred->getName() << "\n";
    }
    errs() << "New succ created: " << newBlock->getName() << "\n";
    errs() << "Successors of new block:- \n";
    for (auto it = succ_begin(newBlock), et = succ_end(newBlock); it != et; ++it) {
      errs() << (*it)->getName() << "\n";
    }
    errs() << "Predecessors of new block:- \n";
    for (auto it = pred_begin(newBlock), et = pred_end(newBlock); it != et; ++it) {
      errs() << (*it)->getName() << "\n";
    }
    errs() << "Predecessors of postdom " << (end)->getName() << " block:- \n";
    for (auto it = pred_begin(end), et = pred_end(end); it != et; ++it) {
      errs() << (*it)->getName() << "\n";
    }
    /******************************************************************************/
#endif

    /* sanity check */
    DomTreeNode *startPDN = PDT->getNode(start);
    assert(startPDN && "cannot find postdom node of start block");
    DomTreeNode *pdnToStart = startPDN->getIDom();
    assert(pdnToStart && "cannot find post dominator node of start node");
    BasicBlock *newPostDomBB = pdnToStart->getBlock();
    assert(newPostDomBB && "cannot find the new post dominator");
    //errs() << "New postdom of " << start->getName() << " : " << newPostDomBB->getName() << "\n";
    if (newPostDomBB != newBlock) {
      errs() << "The postdominator of " << start->getName() << " did not get changed from " << end->getName() << " to " << newBlock->getName() << "\n";
      exit(1);
    }
  }

  void transformComplexBranchBackward(BasicBlock *start, BasicBlock *end) {

    SmallVector<BasicBlock *, 10> nearestSuccs;
    /* For forward direction, find all successors of postDominator that are dominated by start */
    for (auto succIt = succ_begin(end), succEnd = succ_end(end); succIt != succEnd; ++succIt) {
      auto succBB = *succIt;
      if (!DT->dominates(start, succBB)) {
        continue;
      } else {
        nearestSuccs.push_back(succBB);
      }
    }
    ArrayRef<BasicBlock *> succArrList(nearestSuccs.begin(), nearestSuccs.end());
    BasicBlock *newBlock = SplitDomSuccessors(end, succArrList, "_dummy", DT, LI, true);

    Function *F = start->getParent();
    DT->recalculate(*F);
    PDT->recalculate(*F);
    BPI->calculate(*F, *LI, nullptr, DT, PDT);

#ifdef CRNT_DEBUG
    /********************************* Debug prints *****************************/
    errs() << "Transformation rule applied on func " << F->getName() << " for branch ending at " << start->getName() << " and starting at " << end->getName() << "\n";
    errs() << "Closest successors to dominator:- \n";
    for (auto succ : nearestSuccs) {
      errs() << succ->getName() << "\n";
    }
    errs() << "New succ created: " << newBlock->getName() << "\n";
    errs() << "Predecessors of new block:- \n";
    for (auto it = pred_begin(newBlock), et = pred_end(newBlock); it != et; ++it) {
      errs() << (*it)->getName() << "\n";
    }
    errs() << "Successors of new block:- \n";
    for (auto it = succ_begin(newBlock), et = succ_end(newBlock); it != et; ++it) {
      errs() << (*it)->getName() << "\n";
    }
    errs() << "Successors of dominator " << (end)->getName() << " block:- \n";
    for (auto it = succ_begin(end), et = succ_end(end); it != et; ++it) {
      errs() << (*it)->getName() << "\n";
    }
    /******************************************************************************/
#endif

    /* sanity check */
    DomTreeNode *startDN = PDT->getNode(start);
    assert(startDN && "cannot find dom node of start block");
    DomTreeNode *dnToStart = startDN->getIDom();
    assert(dnToStart && "cannot find dominator node of start node");
    BasicBlock *newDomBB = dnToStart->getBlock();
    assert(newDomBB && "cannot find the new dominator");
    //errs() << "New dom of " << start->getName() << " : " << newDomBB->getName() << "\n";
    if (newDomBB != newBlock) {
      errs() << "The dominator of " << start->getName() << " did not get changed from " << end->getName() << " to " << newBlock->getName() << "\n";
      exit(1);
    }
  }

  int preprocessing = 0;
  void transformGraph(Function *F) {
    bool res;
    do {
      res = false;
      for (Function::iterator itBB = F->begin(), itEnd = F->end(); itBB != itEnd; ++itBB) {
        bool direction = true; /* true - forward, false - backward */
        BasicBlock *startBB = &*itBB, *endBB = nullptr;
        res = matchComplexBranch(startBB, &endBB, &direction);
        if (res) {
          preprocessing++;
          if (direction) {
            transformComplexBranchForward(startBB, endBB);
            errs() << F->getName() << "(): Transformed branch between " << startBB->getName() << " and " << endBB->getName() << " in the forward direction\n";
          } else {
            transformComplexBranchBackward(startBB, endBB);
            errs() << F->getName() << "(): Transformed branch between " << startBB->getName() << " and " << endBB->getName() << " in the backward direction\n";
          }
          break;
        }
      }
    } while (res);
  }

  /// Get the latch condition instruction.
  static ICmpInst *getLatchCmpInst(const Loop &L) {
    if (BasicBlock *Latch = L.getLoopLatch()) {
      if (BranchInst *BI = dyn_cast_or_null<BranchInst>(Latch->getTerminator())) {
        if (BI->isConditional()) {
          return dyn_cast<ICmpInst>(BI->getCondition());
        }
      }
    }

    return nullptr;
  }

  BasicBlock *transformLoopWithoutBounds(Loop *L, int iterations) {
    BasicBlock *firstBlock = L->getHeader();
    Function *F = firstBlock->getParent();
    // auto lBounds = L->getBounds(*SE);
    bool isInverseCond = false; /* inverse condition is when first successor of loop condition is not the header of the loop */

    errs() << "Attempting to transform loop " << firstBlock->getName() << " of " << F->getName() << " with " << iterations << " inner loop iterations --> " << *L;

    Value *indVarPhiInst = L->getCanonicalInductionVariable();
    Instruction *splitFrontInst = firstBlock->getFirstNonPHI();
    if (!splitFrontInst) {
      errs() << "Self loop block does not have any non-phi instructions. Not handled.";
      exit(1);
    }

    /* Store IV Phi Nodes */
    InductionDescriptor IndDesc;
    SmallVector<PHINode *, 20> pnList;
    for (BasicBlock::iterator I = firstBlock->begin(); isa<PHINode>(I); ++I) {
      if (isa<PHINode>(I)) {
        PHINode *PN = cast<PHINode>(I);
        //if (!InductionDescriptor::isInductionPHI(&*PN, *&L, SE, IndDesc))
        //continue;
        pnList.push_back(PN);
        //errs() << "Need to copy Phi Node to new block --> " << *PN << "\n";
      }
    }

    BasicBlock *loopLatch = L->getLoopLatch();
    BasicBlock *loopHeader = L->getHeader();
    BranchInst *BI = dyn_cast_or_null<BranchInst>(loopLatch->getTerminator());
    //ICmpInst *LatchCmpInst = dyn_cast<ICmpInst>(BI->getCondition());

    //assert(LatchCmpInst && "Expecting the latch compare instruction to be a CmpInst");
    assert(BI && BI->isConditional() && "Expecting conditional latch branch");
    assert((BI->getNumSuccessors() == 2) && "Self loop with more than 2 successors is not handled");
    assert(indVarPhiInst->getType()->isIntegerTy() && "Canonical Induction variable is not of integer type!");

    if (BI->getSuccessor(0) != loopHeader) {
      isInverseCond = true;
    }

    Value *valOrigCond = BI->getOperand(0);
    // BasicBlock *trueOperand = BI->getSuccessor(0);
    //errs() << "Orig cond: " << *valOrigCond << "\n";
    //ICmpInst *splitBackInst = dyn_cast<ICmpInst>(BI->getCondition());

    /******************* For first split *******************/
    IRBuilder<> IR1(splitFrontInst);
    //AllocaInst *allocaIterator = IR1.CreateAlloca(IR1.getInt32Ty(), 0, "canIndVar");
    //Value* valOne = IR1.getInt32(1);
    //IR1.CreateStore(valIncInitial, allocaIterator);
    //errs() << "Canonical Ind Var Type: " << *indVarPhiInst->getType() << "\n";
    Value *valIterations = IR1.getIntN(SE->getTypeSizeInBits(indVarPhiInst->getType()), iterations - 1); // since we are comparing with old value of induction variable, current value should be 1 more for canonical variable
    Value *valIncInitial = IR1.CreateAdd(valIterations, indVarPhiInst);
    //AllocaInst *allocaLim = IR1.CreateAlloca(indVarPhiInst->getType(), 0, "newLimit");
    //IR1.CreateStore(valIncInitial, allocaLim);

    /******************* For second split *******************/
    Instruction *splitBackInst = BI;
    IRBuilder<> IR2(splitBackInst);

#if 1
    //Value* valOne = IR2.getInt32(1);
    //Value* valIterator = IR2.CreateLoad(allocaIterator);
    //Value *valInc = IR2.CreateAdd(valOne, valIterator);
    //IR2.CreateStore(valInc, allocaIterator);

    //Value *valTargetInterval = IR2.getInt32(iterations);
    Value *valInnerCICond = nullptr;
    Value *valNewCond = nullptr;

#if 0
    if (!isInverseCond)
      valInnerCICond = IR2.CreateICmpULE(valInc, valTargetInterval, "canIndVarPredicate");
    else
      valInnerCICond = IR2.CreateICmpUGT(valInc, valTargetInterval, "canIndVarPredicate");
#endif

    //Value* valLimit = IR2.CreateLoad(allocaLim);
    if (!isInverseCond) {
      valInnerCICond = IR2.CreateICmpNE(indVarPhiInst, valIncInitial, "canIndVarPredicate");
    } else {
      valInnerCICond = IR2.CreateICmpEQ(indVarPhiInst, valIncInitial, "canIndVarPredicate");
    }

    Value *valInnerCICondExt = valInnerCICond;
    //errs() << "Old cond type: " << *valOrigCond->getType() << ", new cond type: " << *valInnerCICond->getType() << "\n";
    if (valOrigCond->getType() != valInnerCICond->getType()) {
      valInnerCICondExt = IR2.CreateZExt(valInnerCICond, valOrigCond->getType(), "zeroExtend");
    }

    if (!isInverseCond) {
      valNewCond = IR2.CreateAnd(valOrigCond, valInnerCICondExt, "newCond");
    } else {
      valNewCond = IR2.CreateOr(valOrigCond, valInnerCICondExt, "newCond");
    }
    //Value *valNegCondComp = IR2.CreateICmpNE(valNewCond, valZero, "newComp");
#endif

    //MemorySSAUpdater MSSAU(MSSA);
    BranchInst *newBranch = nullptr;
    BasicBlock *newBlock = SplitBlock(firstBlock, splitFrontInst, DT, LI, nullptr);
    BasicBlock *endBlock = loopLatch->splitBasicBlock(splitBackInst->getIterator());
    //BranchInst *newBranch = IR2.CreateCondBr(valNegCondComp,newBlock,endBlock);
    if (!isInverseCond) {
      newBranch = BranchInst::Create(/*ifTrue*/ newBlock, /*ifFalse*/ endBlock, valNewCond);
    } else {
      newBranch = BranchInst::Create(/*ifTrue*/ endBlock, /*ifFalse*/ newBlock, valNewCond);
    }

    Instruction *toBeReplacedTerm = newBlock->getTerminator();
    ReplaceInstWithInst(toBeReplacedTerm, newBranch);

    newBlock->setName("selfLoopOptBlock");
    endBlock->setName("selfLoopOptExitBlock");

    for (auto PN : pnList) {
      PHINode *newPN = PHINode::Create(PN->getType(), 2, "phiIVClone", &newBlock->front());

      for (int64_t i = PN->getNumIncomingValues() - 1; i >= 0; --i) {
        BasicBlock *incomingBB = PN->getIncomingBlock(i);
        Value *incomingVal = PN->getIncomingValue(i);
        if (incomingBB == endBlock) {
          newPN->addIncoming(incomingVal, newBlock);
        }
      }
      newPN->addIncoming(PN, firstBlock);
      for (Value::use_iterator UI = PN->use_begin(), UE = PN->use_end(); UI != UE;) {
        Use &U = *UI++;
        auto *Usr = dyn_cast<Instruction>(U.getUser());
        if (Usr && Usr->getParent() != firstBlock) {
          //errs() << "User to replace Phi: " << *Usr << "\n";
          if (Usr != newPN) {
            U.set(newPN);
          }
        } else {
          errs() << "User in header block to replace Phi: " << *Usr << "\n";
        }
      }
    }

    //DT->recalculate(*(newBlock->getParent()));
    //if(SE) SE->forgetLoop(L);

    Loop *newInnerLoop = LI->AllocateLoop();
    L->addChildLoop(newInnerLoop);
    L->addBlockEntry(newBlock);
    newInnerLoop->addBlockEntry(newBlock);
    newInnerLoop->moveToHeader(newBlock);
    return endBlock;
  }

  BasicBlock *transformSelfLoopWithoutBounds(Loop *L, int iterations, int numSelfLoopCost) {
    BasicBlock *onlyBlock = L->getHeader();
    Function *F = onlyBlock->getParent();
    auto lBounds = L->getBounds(*SE);
    bool isInverseCond = false; /* inverse condition is when first successor of loop condition is not the header of the loop */

    /* Checking preconditions */
    assert((iterations > 1) && "Too small number of iterations to instrument!");

    if (!lBounds) {
      errs() << "Bounds are not present. Cannot transform!\n";
      return nullptr;
    }

    Value *FinalIVValue = &lBounds->getFinalIVValue();

    if (!FinalIVValue) {
      errs() << "No final value present. Cannot transform loop.\n";
      return nullptr;
    }

    errs() << "Attempting to transform self loop " << onlyBlock->getName() << " of " << F->getName() << " with " << iterations << " inner loop iterations --> " << *L;

    Value *indVarPhiInst = L->getCanonicalInductionVariable();
    assert(indVarPhiInst->getType()->isIntegerTy() && "Induction variable is not of integer type!");

    /* Store Phi Nodes */
    InductionDescriptor IndDesc;
    SmallVector<PHINode *, 20> pnList;
    for (BasicBlock::iterator I = onlyBlock->begin(); isa<PHINode>(I); ++I) {
      if (isa<PHINode>(I)) {
        PHINode *PN = cast<PHINode>(I);
        pnList.push_back(PN);
      }
    }

    BasicBlock *loopLatch = L->getLoopLatch();
    BasicBlock *loopHeader = L->getHeader();
    BranchInst *BI = dyn_cast_or_null<BranchInst>(loopLatch->getTerminator());

    if (loopLatch != onlyBlock) {
      errs() << "Self loop has different header " << onlyBlock->getName() << " & latches " << loopLatch->getName() << " in function " << onlyBlock->getParent()->getName() << "\n";
    }
    assert((loopLatch == onlyBlock) && "A self loop cannot have separate latches & headers!");
    assert((loopHeader == onlyBlock) && "A self loop cannot have headers & body!");
    //assert(LatchCmpInst && "Expecting the latch compare instruction to be a CmpInst");
    assert(BI && BI->isConditional() && "Expecting conditional latch branch");
    assert((BI->getNumSuccessors() == 2) && "Self loop with more than 2 successors is not handled");
    assert(indVarPhiInst->getType()->isIntegerTy() && "Canonical Induction variable is not of integer type!");

    if (BI->getSuccessor(0) != loopHeader) {
      isInverseCond = true;
    }

    Value *valOrigCond = BI->getOperand(0);
    // BasicBlock *trueOperand = BI->getSuccessor(0);

    Instruction *splitFrontInst = onlyBlock->getFirstNonPHI();
    assert(splitFrontInst && "Self loop block does not have any non-phi instructions. Not handled.");

    Instruction *splitBackInst = BI;

    /******************* For first split *******************/
    BasicBlock *newBlock = SplitBlock(onlyBlock, splitFrontInst, DT, LI, nullptr);
    newBlock->setName("selfLoopOptBlock");

    /******************* For second split *******************/
    BasicBlock *endBlock = SplitBlock(newBlock, splitBackInst, DT, LI, nullptr);
    //BasicBlock *endBlock = newBlock->splitBasicBlock(splitBackInst->getIterator()); // SplitBlock preserves passes unlike this
    endBlock->setName("selfLoopOptExitBlock");

    /* Creating condition argument in the outer loop header */
    auto loopHdrCondArgInst = onlyBlock->getFirstNonPHI();
    IRBuilder<> IRHead(loopHdrCondArgInst);
    Value *valIterations = IRHead.getIntN(SE->getTypeSizeInBits(indVarPhiInst->getType()), iterations);
    Value *valEndCond = IRHead.CreateAdd(valIterations, indVarPhiInst);

    /* Substituting PN values in new inner loop */
    Value *localIndVarPN = nullptr, *localIndVar = nullptr;
    for (auto PN : pnList) {
      PHINode *newPN = PHINode::Create(PN->getType(), 2, "phiIVClone", &newBlock->front());

      if (PN == indVarPhiInst) {
        localIndVarPN = newPN;
        errs() << "Found local ind var: " << *PN << ", mapping it to " << *newPN << "\n";
      }

      for (int64_t i = PN->getNumIncomingValues() - 1; i >= 0; --i) {
        BasicBlock *incomingBB = PN->getIncomingBlock(i);
        Value *incomingVal = PN->getIncomingValue(i);
        if (incomingBB == endBlock) {
          newPN->addIncoming(incomingVal, newBlock);
          if (PN == indVarPhiInst) {
            errs() << "2. Found local ind var: " << *PN << ", mapping it to " << *newPN << "\n";
            localIndVarPN = newPN;
            if (!localIndVar) {
              localIndVar = incomingVal;
              errs() << "Setting local ind var: " << *incomingVal << "\n";
            } else {
              errs() << "Local ind var can't come twice. Old: " << *localIndVar << ", New:" << *incomingVal << "\n";
              exit(1);
            }
          }
        }
      }

      newPN->addIncoming(PN, onlyBlock);

      for (Value::use_iterator UI = PN->use_begin(), UE = PN->use_end(); UI != UE;) {
        Use &U = *UI++;
        auto *Usr = dyn_cast<Instruction>(U.getUser());
        if (Usr && Usr->getParent() != onlyBlock) {
          //errs() << "User to replace Phi: " << *Usr << "\n";
          if (Usr != newPN) {
            U.set(newPN);
          }
        } else {
          errs() << "User in header block to replace Phi: " << *Usr << "\n";
        }
      }
    }

    /* check if local induction variables in the inner loop is found */
    if (!localIndVarPN || !localIndVar) {
      errs() << "Local induction variables are not available. Aborting.\n";
      if (!localIndVarPN) {
        errs() << "localIndVarPN absent\n";
      }
      if (!localIndVar) {
        errs() << "localIndVar absent\n";
      }
      exit(1);
    }

    /******************* Creating new conditions for inner loop *******************/
    Instruction *innerLoopTermInst = dyn_cast_or_null<BranchInst>(newBlock->getTerminator());
    IRBuilder<> IRInnerLoop(innerLoopTermInst);

    Value *valInnerCICond = nullptr;
    Value *valNewCond = nullptr;
    if (!isInverseCond) {
      valInnerCICond = IRInnerLoop.CreateICmpNE(localIndVar, valEndCond, "canIndVarPredicate");
    } else {
      valInnerCICond = IRInnerLoop.CreateICmpEQ(localIndVar, valEndCond, "canIndVarPredicate");
    }

    Value *valInnerCICondExt = valInnerCICond;
    //errs() << "Old cond type: " << *valOrigCond->getType() << ", new cond type: " << *valInnerCICond->getType() << "\n";
    if (valOrigCond->getType() != valInnerCICond->getType()) {
      valInnerCICondExt = IRInnerLoop.CreateZExt(valInnerCICond, valOrigCond->getType(), "zeroExtend");
    }

    if (!isInverseCond) {
      valNewCond = IRInnerLoop.CreateAnd(valOrigCond, valInnerCICondExt, "newCond");
    } else {
      valNewCond = IRInnerLoop.CreateOr(valOrigCond, valInnerCICondExt, "newCond");
    }

    BranchInst *newBranch = nullptr;
    if (!isInverseCond) {
      newBranch = BranchInst::Create(/*ifTrue*/ newBlock, /*ifFalse*/ endBlock, valNewCond);
    } else {
      newBranch = BranchInst::Create(/*ifTrue*/ endBlock, /*ifFalse*/ newBlock, valNewCond);
    }

    Instruction *toBeReplacedTerm = newBlock->getTerminator();
    ReplaceInstWithInst(toBeReplacedTerm, newBranch);

    //DT->recalculate(*(newBlock->getParent()));
    //if(SE) SE->forgetLoop(L);

    /* Add loop in loop info */
    Loop *newInnerLoop = LI->AllocateLoop();
    L->addChildLoop(newInnerLoop);
    L->addBlockEntry(newBlock);
    newInnerLoop->addBlockEntry(newBlock);
    newInnerLoop->moveToHeader(newBlock);

    /* Instrument the outer loop */
    BasicBlock *headerBBL = L->getHeader();
    errs() << "Applied self loop transform on " << headerBBL->getName() << "( " << headerBBL->getParent()->getName() << "() )\n";
    LCCNode *newLCC = new UnitLCC(lccIDGen++, endBlock, endBlock->getFirstNonPHI(), &(endBlock->back()), false);
    UnitLCC *newUnitLCC = static_cast<UnitLCC *>(newLCC);

    /* instrumenting the new outer loop */
    Instruction *endBlockTermInst = dyn_cast_or_null<BranchInst>(endBlock->getTerminator());
    IRBuilder<> IREnd(endBlockTermInst);

    /* Adding prints for debugging */
//#if 1
#ifdef ADD_RUNTIME_PRINTS
    {
      Module *M = onlyBlock->getModule();
      Function *printf_func = printf_prototype(M);
      llvm::Value *formatStr = IRHead.CreateGlobalStringPtr("\nLoop preheader()->ind var:%d, end cond:%d\n", "printInHeader");
      std::vector<llvm::Value *> args;
      args.push_back(formatStr);
      args.push_back(indVarPhiInst);
      args.push_back(valEndCond);
      IRHead.CreateCall(printf_func, args);
    }
#endif

//#if 1
#ifdef ADD_RUNTIME_PRINTS
    {
      Instruction *innerLoopNewTermInst = dyn_cast_or_null<BranchInst>(newBlock->getTerminator());
      IRBuilder<> IRInnerLoopNew(innerLoopNewTermInst);
      Module *M = onlyBlock->getModule();
      Function *printf_func = printf_prototype(M);
      llvm::Value *formatStr = IRInnerLoopNew.CreateGlobalStringPtr("\nInside inner loop():- ind var: %d, own cond: %d, orig cond: %d, combined cond: %d\n", "printInInnerLoop");
      //llvm::Value *formatStr = IRInnerLoopNew.CreateGlobalStringPtr("\nInside inner loop()\n", "printInInnerLoop");

      std::vector<llvm::Value *> args;
      args.push_back(formatStr);
      args.push_back(localIndVar);
      args.push_back(valInnerCICondExt);
      args.push_back(valOrigCond);
      args.push_back(valNewCond);
      IRInnerLoopNew.CreateCall(printf_func, args);
    }
#endif

//#if 1
#ifdef ADD_RUNTIME_PRINTS
    {
      Module *M = endBlock->getModule();
      Function *printf_func = printf_prototype(M);
      llvm::Value *formatStr = IREnd.CreateGlobalStringPtr("\nIn outer loop after inner:- outer ind var: %d, inner ind var: %d, own cond: %d, orig cond: %d, combined cond: %d\n", "printInEndBlock");
      std::vector<llvm::Value *> args;
      args.push_back(formatStr);
      args.push_back(indVarPhiInst);
      args.push_back(localIndVar);
      args.push_back(valInnerCICondExt);
      args.push_back(valOrigCond);
      args.push_back(valNewCond);
      IREnd.CreateCall(printf_func, args);
    }
#endif

    /*
      Value *FinalIVValueExt = FinalIVValue;
      if( FinalIVValue->getType() != loopBodyCost->getType()) {
        FinalIVValueExt = IREnd.CreateZExt(FinalIVValue, loopBodyCost->getType(), "zeroExtendSLF");
      }
      //errs() << "Final Value: " << FinalIVValue->getName() << "\n";
      */

    Value *loopIterationsExt = localIndVar;
    Value *loopBodyCost = IREnd.getInt64(numSelfLoopCost);
    if (localIndVar->getType() != loopBodyCost->getType()) {
      loopIterationsExt = IREnd.CreateZExt(localIndVar, loopBodyCost->getType(), "zeroExtendSLI");
    }
    Value *loopCost = IREnd.CreateMul(loopIterationsExt, loopBodyCost);

    /* register the outer loop for the cost instrumentation phase */
    newUnitLCC->instrumentValueForIC(loopCost);
    std::vector<LCCNode *> containers;
    containers.push_back(newLCC);
    bbToContainersMap[endBlock] = containers;

    return endBlock;
  }

  BasicBlock *transformSESELoopWithoutBounds(Loop *L, int iterations, int numSelfLoopCost) {
    BasicBlock *headerBlock = L->getHeader();
    // Function *F = headerBlock->getParent();
    auto lBounds = L->getBounds(*SE);
    bool isCanonical = false;
    bool isInverseCond = false; /* inverse condition is when first successor of loop condition is not the header of the loop */
    int loopType;

    if (L->isCanonical(*SE)) {
      isCanonical = true;
    }

    if (L->isLoopExiting(headerBlock)) {
      loopType = LoopLCC::HEADER_COLOCATED_EXIT;
    } else {
      loopType = LoopLCC::HEADER_NONCOLOCATED_EXIT;
    }

    /* Checking preconditions */
    assert((L->getNumBlocks() != 1) && "Self loops are handled separately");
    assert((iterations > 1) && "Too small number of iterations to instrument!");

    if (!lBounds) {
      errs() << "Bounds are not present. Cannot transform!\n";
      return nullptr;
    }

    Value *InitialIVValue2 = nullptr;
    Value *StepValue = nullptr;
    Value *FinalIVValue = nullptr;

    if (!isCanonical) {
      InitialIVValue2 = &lBounds->getInitialIVValue();
      StepValue = lBounds->getStepValue();

      if (!InitialIVValue2) {
        errs() << "No initial value present. Cannot transform loop.\n";
        return nullptr;
      }

      if (!StepValue) {
        errs() << "No step value present. Cannot transform loop.\n";
        return nullptr;
      }

      if (!isa<ConstantInt>(StepValue)) {
        errs() << "The step value is not constant. Cannot transform!\n";
        return nullptr;
      }
    }

    FinalIVValue = &lBounds->getFinalIVValue();

    if (!FinalIVValue) {
      errs() << "No final value present. Cannot transform loop.\n";
      return nullptr;
    }

    //errs() << "Attempting to transform sese loop " << headerBlock->getName() << " of " << F->getName() << " with " << iterations << " inner loop iterations --> " << *L << "\n";

    auto *indVarPhiInst = L->getInductionVariable(*SE);
    assert(indVarPhiInst->getType()->isIntegerTy() && "Induction variable is not of integer type!");

    if (isCanonical) {
      Value *canIndVarPhiInst = L->getCanonicalInductionVariable();
      assert((canIndVarPhiInst == indVarPhiInst) && "Canonical induction variable is not the same as the induction variable for a canonical loop");
    }

    /* Store Phi Nodes */
    InductionDescriptor IndDesc;
    SmallVector<PHINode *, 20> pnList;
    for (BasicBlock::iterator I = headerBlock->begin(); isa<PHINode>(I); ++I) {
      if (isa<PHINode>(I)) {
        PHINode *PN = cast<PHINode>(I);
        pnList.push_back(PN);
      }
    }

    /* Store Loop Blocks */
    auto innerSubLoops = L->getSubLoops();

    BasicBlock *loopExitingBlock = L->getExitingBlock();
    BasicBlock *loopExitBlock = L->getExitBlock();
    BranchInst *BI = dyn_cast_or_null<BranchInst>(loopExitingBlock->getTerminator());

    if (loopType == LoopLCC::HEADER_COLOCATED_EXIT) {
      assert((loopExitingBlock == headerBlock) && "The exit & header block should be same!");
    }

    assert(BI && BI->isConditional() && "Expecting conditional exit branch");
    assert((BI->getNumSuccessors() == 2) && "SESE loop with more than 2 successors is not handled");

    if (BI->getSuccessor(0) == loopExitBlock) {
      isInverseCond = true;
    }

    Value *valOrigCond = BI->getOperand(0);
    // BasicBlock *trueOperand = BI->getSuccessor(0);

    Instruction *splitFrontInst = headerBlock->getFirstNonPHI();
    assert(splitFrontInst && "SESE header block does not have any non-phi instructions. Not handled.");

    Instruction *splitBackInst = BI;
    BasicBlock *innerHeaderBlock = nullptr, *outerHeaderBlock = headerBlock;
    BasicBlock *innerExitingBlock = loopExitingBlock, *outerExitingBlock = nullptr;

    /******************* For first split *******************/
    innerHeaderBlock = SplitBlock(headerBlock, splitFrontInst, DT, LI, nullptr);
    innerHeaderBlock->setName("seseLoopOptBlock");

    /******************* For second split *******************/
    if (loopType == LoopLCC::HEADER_COLOCATED_EXIT) {
      outerExitingBlock = SplitBlock(innerHeaderBlock, splitBackInst, DT, LI, nullptr);
      outerExitingBlock->setName("seseLoopOptHCExitBlock");
    } else {
      outerExitingBlock = SplitBlock(loopExitingBlock, splitBackInst, DT, LI, nullptr);
      outerExitingBlock->setName("seseLoopOptHNCExitBlock");
    }

    /* Creating condition argument in the outer loop header */
    auto loopHdrCondArgInst = headerBlock->getFirstNonPHI();
    IRBuilder<> IRHead(loopHdrCondArgInst);
    Value *valIterations = IRHead.getIntN(SE->getTypeSizeInBits(indVarPhiInst->getType()), iterations);
    Value *valEndCond = nullptr;
    if (!isCanonical) {
      Value *valStep = IRHead.CreateMul(valIterations, StepValue);
      valEndCond = IRHead.CreateAdd(valStep, indVarPhiInst);
    } else {
      valEndCond = IRHead.CreateAdd(valIterations, indVarPhiInst);
    }

    /* Substituting PN values in new inner loop */
    Value *localIndVarPN = nullptr, *localIndVar = nullptr;
    for (auto PN : pnList) {
      PHINode *newPN = PHINode::Create(PN->getType(), 2, "phiIVClone", &innerHeaderBlock->front());

      if (PN == indVarPhiInst) {
        localIndVarPN = newPN;
        errs() << "Found local ind var: " << *PN << ", mapping it to " << *newPN << "\n";
      }

      for (int64_t i = PN->getNumIncomingValues() - 1; i >= 0; --i) {
        BasicBlock *incomingBB = PN->getIncomingBlock(i);
        Value *incomingVal = PN->getIncomingValue(i);
        if (incomingBB == outerExitingBlock) {
          newPN->addIncoming(incomingVal, innerExitingBlock);
          if (PN == indVarPhiInst) {
            //errs() << "2. Found local ind var: " << *PN << ", mapping it to " << *newPN << "\n";
            localIndVarPN = newPN;
            if (!localIndVar) {
              localIndVar = incomingVal;
              //errs() << "Setting local ind var: " << *incomingVal << "\n";
            } else {
              errs() << "Local ind var can't come twice. Old: " << *localIndVar << ", New:" << *incomingVal << "\n";
              exit(1);
            }
          }
        }
      }

      newPN->addIncoming(PN, outerHeaderBlock);

      for (Value::use_iterator UI = PN->use_begin(), UE = PN->use_end(); UI != UE;) {
        Use &U = *UI++;
        auto *Usr = dyn_cast<Instruction>(U.getUser());
        if (Usr && Usr->getParent() != outerHeaderBlock) {
          //errs() << "User to replace Phi: " << *Usr << "\n";
          if (Usr != newPN) {
            U.set(newPN);
          }
        } else {
          errs() << "User in header block to replace Phi: " << *Usr << "\n";
        }
      }
    }

    /* check if local induction variables in the inner loop is found */
    if (!localIndVarPN || !localIndVar) {
      errs() << "Local induction variables are not available. Aborting.\n";
      if (!localIndVarPN) {
        errs() << "localIndVarPN absent\n";
      }
      if (!localIndVar) {
        errs() << "localIndVar absent\n";
      }
      exit(1);
    }

    /******************* Creating new conditions for inner loop *******************/
    Instruction *innerLoopTermInst = dyn_cast_or_null<BranchInst>(innerExitingBlock->getTerminator());
    IRBuilder<> IRInnerLoop(innerLoopTermInst);

    Value *valInnerCICond = nullptr;
    Value *valNewCond = nullptr;
    if (!isInverseCond) {
      valInnerCICond = IRInnerLoop.CreateICmpNE(localIndVar, valEndCond, "canIndVarPredicate");
    } else {
      valInnerCICond = IRInnerLoop.CreateICmpEQ(localIndVar, valEndCond, "canIndVarPredicate");
    }

    Value *valInnerCICondExt = valInnerCICond;
    //errs() << "Old cond type: " << *valOrigCond->getType() << ", new cond type: " << *valInnerCICond->getType() << "\n";
    if (valOrigCond->getType() != valInnerCICond->getType()) {
      valInnerCICondExt = IRInnerLoop.CreateZExt(valInnerCICond, valOrigCond->getType(), "zeroExtend");
    }

    if (!isInverseCond) {
      valNewCond = IRInnerLoop.CreateAnd(valOrigCond, valInnerCICondExt, "newCond");
    } else {
      valNewCond = IRInnerLoop.CreateOr(valOrigCond, valInnerCICondExt, "newCond");
    }

    BranchInst *newBranch = nullptr;
    if (!isInverseCond) {
      newBranch = BranchInst::Create(/*ifTrue*/ innerHeaderBlock, /*ifFalse*/ outerExitingBlock, valNewCond);
    } else {
      newBranch = BranchInst::Create(/*ifTrue*/ outerExitingBlock, /*ifFalse*/ innerHeaderBlock, valNewCond);
    }

    Instruction *toBeReplacedTerm = innerExitingBlock->getTerminator();
    ReplaceInstWithInst(toBeReplacedTerm, newBranch);

    //DT->recalculate(*(innerHeaderBlock->getParent()));
    //if(SE) SE->forgetLoop(L);

    /* Add loop in loop info */
    //errs() << "OUTER LOOP BEFORE LOOP TRANSFORM " << *L << "\n";
    Loop *newInnerLoop = LI->AllocateLoop();
    L->addChildLoop(newInnerLoop);
    for (auto innerSubLoop : innerSubLoops) {
      //errs() << "Removing inner sub loop " << *innerSubLoop << " of outer loop " << *L << "( " << L->getHeader()->getParent()->getName() << " )\n";
      L->removeChildLoop(innerSubLoop);
      newInnerLoop->addChildLoop(innerSubLoop);
    }

    for (auto outerLoopBlock : L->getBlocks()) {
      if (outerLoopBlock != L->getHeader() && outerLoopBlock != L->getLoopLatch()) {
        //errs() << "ADDING BLOCK " << outerLoopBlock->getName() << "\n";
        newInnerLoop->addBlockEntry(outerLoopBlock);
      }
    }
    newInnerLoop->moveToHeader(innerHeaderBlock);
    //errs() << "OUTER LOOP AFTER LOOP TRANSFORM " << *L << "\n";

    /* Instrument the outer loop */
    BasicBlock *headerBBL = L->getHeader();
    errs() << "Applied sese loop transform on " << headerBBL->getName() << "( " << headerBBL->getParent()->getName() << "() )\n";
    LCCNode *newLCC = new UnitLCC(lccIDGen++, outerExitingBlock, outerExitingBlock->getFirstNonPHI(), &(outerExitingBlock->back()), false);
    UnitLCC *newUnitLCC = static_cast<UnitLCC *>(newLCC);

    /* instrumenting the new outer loop */
    Instruction *endBlockTermInst = dyn_cast_or_null<BranchInst>(outerExitingBlock->getTerminator());
    IRBuilder<> IREnd(endBlockTermInst);

    /* Adding prints for debugging */
//#if 1
#ifdef ADD_RUNTIME_PRINTS
    {
      Module *M = headerBlock->getModule();
      Function *printf_func = printf_prototype(M);
      llvm::Value *formatStr = IRHead.CreateGlobalStringPtr("\nLoop preheader()->ind var:%d, end cond:%d\n", "printInHeader");
      std::vector<llvm::Value *> args;
      args.push_back(formatStr);
      args.push_back(indVarPhiInst);
      args.push_back(valEndCond);
      IRHead.CreateCall(printf_func, args);
    }
#endif

//#if 1
#ifdef ADD_RUNTIME_PRINTS
    {
      Instruction *innerLoopNewTermInst = dyn_cast_or_null<BranchInst>(innerHeaderBlock->getTerminator());
      IRBuilder<> IRInnerLoopNew(innerLoopNewTermInst);
      Module *M = headerBlock->getModule();
      Function *printf_func = printf_prototype(M);
      llvm::Value *formatStr = IRInnerLoopNew.CreateGlobalStringPtr("\nInside inner loop():- ind var: %d, own cond: %d, orig cond: %d, combined cond: %d\n", "printInInnerLoop");
      //llvm::Value *formatStr = IRInnerLoopNew.CreateGlobalStringPtr("\nInside inner loop()\n", "printInInnerLoop");

      std::vector<llvm::Value *> args;
      args.push_back(formatStr);
      args.push_back(localIndVar);
      args.push_back(valInnerCICondExt);
      args.push_back(valOrigCond);
      args.push_back(valNewCond);
      IRInnerLoopNew.CreateCall(printf_func, args);
    }
#endif

//#if 1
#ifdef ADD_RUNTIME_PRINTS
    {
      Module *M = outerExitingBlock->getModule();
      Function *printf_func = printf_prototype(M);
      llvm::Value *formatStr = IREnd.CreateGlobalStringPtr("\nIn outer loop after inner:- outer ind var: %d, inner ind var: %d, own cond: %d, orig cond: %d, combined cond: %d\n", "printInEndBlock");
      std::vector<llvm::Value *> args;
      args.push_back(formatStr);
      args.push_back(indVarPhiInst);
      args.push_back(localIndVar);
      args.push_back(valInnerCICondExt);
      args.push_back(valOrigCond);
      args.push_back(valNewCond);
      IREnd.CreateCall(printf_func, args);
    }
#endif

    /* instrumenting the new outer loop */
    Value *loopIterations = nullptr;
    if (!isCanonical) {
      Value *loopIntv = IREnd.CreateSub(localIndVar, indVarPhiInst, "loop_intv");
      loopIterations = IREnd.CreateSDiv(loopIntv, StepValue, "loop_iter");
    } else {
      loopIterations = IREnd.CreateSub(localIndVar, indVarPhiInst, "loop_intv");
      ;
    }

    Value *loopIterationsExt = loopIterations;
    Value *loopBodyCost = IREnd.getInt64(numSelfLoopCost);
    if (loopIterations->getType() != loopBodyCost->getType()) {
      loopIterationsExt = IREnd.CreateZExt(loopIterations, loopBodyCost->getType(), "zeroExtendSLI");
    }
    Value *loopCost = IREnd.CreateMul(loopIterationsExt, loopBodyCost);

    /* register the outer loop for the cost instrumentation phase */
    newUnitLCC->instrumentValueForIC(loopCost);
    std::vector<LCCNode *> containers;
    containers.push_back(newLCC);
    bbToContainersMap[outerExitingBlock] = containers;

    return outerExitingBlock;
  }

  void addDebugPrints(Loop *L) {
    auto preheaderBB = L->getLoopPreheader();
    auto currBB = L->getHeader();
    auto phTermInst = preheaderBB->getTerminator();
    auto currTermInst = currBB->getTerminator();
    Module *M = preheaderBB->getModule();
    Function *printf_func = printf_prototype(M);
    IRBuilder<> IRPh(phTermInst);
    IRBuilder<> IR2(currTermInst);
    auto *indVarPhiInst = L->getInductionVariable(*SE);
    Value *indVarVal = nullptr;

    for (unsigned i = 0; i <= indVarPhiInst->getNumIncomingValues(); i++) {
      BasicBlock *incomingBB = indVarPhiInst->getIncomingBlock(i);
      if (incomingBB == preheaderBB) {
        indVarVal = indVarPhiInst->getIncomingValue(i);
        break;
      }
    }

    {
      llvm::Value *formatStr = IRPh.CreateGlobalStringPtr("\nLoop preheader()->ind var:%d\n", "printstr");
      std::vector<llvm::Value *> args;
      args.push_back(formatStr);
      args.push_back(indVarVal);
      IRPh.CreateCall(printf_func, args);
    }
    {
      llvm::Value *formatStr = IR2.CreateGlobalStringPtr("\nInside inner loop()\n", "printstrinnerloop");
      std::vector<llvm::Value *> args;
      args.push_back(formatStr);
      IR2.CreateCall(printf_func, args);
    }
  }

  BasicBlock *transformGenericSelfLoopWithoutBounds(Loop *L, int iterations, int numSelfLoopCost) {
    BasicBlock *onlyBlock = L->getHeader();
    Function *F = onlyBlock->getParent();
    auto lBounds = L->getBounds(*SE);
    bool isCanonical = false;
    bool isInverseCond = false; /* inverse condition is when first successor of loop condition is not the header of the loop */

    if (L->isCanonical(*SE)) {
      isCanonical = true;
      errs() << "Self loop is canonical. Going for special transformation with " << iterations << " iterations.\n";
    } else {
      errs() << "Self loop is not canonical. Going for generic transformation with " << iterations << " iterations.\n";
    }

    /* Checking preconditions */
    assert((iterations > 1) && "Too small number of iterations to instrument!");

    if (!lBounds) {
      errs() << "Bounds are not present. Cannot transform!\n";
      return nullptr;
    }

    Value *InitialIVValue2 = nullptr;
    Value *StepValue = nullptr;
    Value *FinalIVValue = nullptr;

    if (!isCanonical) {
      InitialIVValue2 = &lBounds->getInitialIVValue();
      StepValue = lBounds->getStepValue();

      if (!InitialIVValue2) {
        errs() << "No initial value present. Cannot transform loop.\n";
        return nullptr;
      }

      if (!StepValue) {
        errs() << "No step value present. Cannot transform loop.\n";
        return nullptr;
      }

      if (!isa<ConstantInt>(StepValue)) {
        errs() << "The step value is not constant. Cannot transform!\n";
        return nullptr;
      }
    }

    FinalIVValue = &lBounds->getFinalIVValue();

    if (!FinalIVValue) {
      errs() << "No final value present. Cannot transform loop.\n";
      return nullptr;
    }

    errs() << "Attempting to transform self loop " << onlyBlock->getName() << " of " << F->getName() << " with " << iterations << " inner loop iterations --> " << *L;

#ifdef ALL_DEBUG
    ConstantInt *stepCI = dyn_cast<ConstantInt>(StepValue);
    int64_t numStepVal = 0;
    if (stepCI->getBitWidth() <= 64) {
      numStepVal = stepCI->getSExtValue();
    }
    assert((numStepVal != 0) && "Step value cannot be 0");
    errs() << "Step value is " << numStepVal << "\n";
#endif

    auto *indVarPhiInst = L->getInductionVariable(*SE);
    assert(indVarPhiInst->getType()->isIntegerTy() && "Induction variable is not of integer type!");

    if (isCanonical) {
      Value *canIndVarPhiInst = L->getCanonicalInductionVariable();
      assert((canIndVarPhiInst == indVarPhiInst) && "Canonical induction variable is not the same as the induction variable for a canonical loop");
    }

#if 0
    auto preheaderBB = L->getLoopPreheader();
    Value *indVarVal = nullptr;
    /* find the value in preheader corresponding to the induction variable */
    for (unsigned i = 0; i <= indVarPhiInst->getNumIncomingValues(); i++) {
      BasicBlock *incomingBB = indVarPhiInst->getIncomingBlock(i);
      if (incomingBB == preheaderBB) {
        indVarVal = indVarPhiInst->getIncomingValue(i);
        break;
      }
    }
#endif

    /* Store Phi Nodes */
    InductionDescriptor IndDesc;
    SmallVector<PHINode *, 20> pnList;
    for (BasicBlock::iterator I = onlyBlock->begin(); isa<PHINode>(I); ++I) {
      if (isa<PHINode>(I)) {
        PHINode *PN = cast<PHINode>(I);
        //if (!InductionDescriptor::isInductionPHI(&*PN, *&L, SE, IndDesc))
        //continue;
        pnList.push_back(PN);
      }
    }

    BasicBlock *loopLatch = L->getLoopLatch();
    BasicBlock *loopHeader = L->getHeader();
    BranchInst *BI = dyn_cast_or_null<BranchInst>(loopLatch->getTerminator());

    if (loopLatch != onlyBlock) {
      errs() << "Self loop has different header " << onlyBlock->getName() << " & latches " << loopLatch->getName() << " in function " << onlyBlock->getParent()->getName() << "\n";
    }

    assert((loopLatch == onlyBlock) && "A self loop cannot have separate latches & headers!");
    assert((loopHeader == onlyBlock) && "A self loop cannot have headers & body!");
    //assert(LatchCmpInst && "Expecting the latch compare instruction to be a CmpInst");
    assert(BI && BI->isConditional() && "Expecting conditional latch branch");
    assert((BI->getNumSuccessors() == 2) && "Self loop with more than 2 successors is not handled");

    if (BI->getSuccessor(0) != loopHeader) {
      isInverseCond = true;
    }

    Value *valOrigCond = BI->getOperand(0);
    // BasicBlock *trueOperand = BI->getSuccessor(0);

    Instruction *splitFrontInst = onlyBlock->getFirstNonPHI();
    assert(splitFrontInst && "Self loop block does not have any non-phi instructions. Not handled.");

    Instruction *splitBackInst = BI;

    /******************* First split *******************/
    BasicBlock *newBlock = SplitBlock(onlyBlock, splitFrontInst, DT, LI, nullptr);
    newBlock->setName("selfLoopOptBlock");

    /******************* Second split *******************/
    BasicBlock *endBlock = SplitBlock(newBlock, splitBackInst, DT, LI, nullptr);
    endBlock->setName("selfLoopOptExitBlock");

    /* Creating condition argument in the outer loop header */
    auto loopHdrCondArgInst = onlyBlock->getFirstNonPHI();
    IRBuilder<> IRHead(loopHdrCondArgInst);
    Value *valIterations = IRHead.getIntN(SE->getTypeSizeInBits(indVarPhiInst->getType()), iterations);
    Value *valEndCond = nullptr;
    if (!isCanonical) {
      Value *valStep = IRHead.CreateMul(valIterations, StepValue);
      valEndCond = IRHead.CreateAdd(valStep, indVarPhiInst);
    } else {
      valEndCond = IRHead.CreateAdd(valIterations, indVarPhiInst);
    }

    /* Substituting PN values in new inner loop */
    Value *localIndVarPN = nullptr, *localIndVar = nullptr;
    for (auto PN : pnList) {
      PHINode *newPN = PHINode::Create(PN->getType(), 2, "phiIVClone", &newBlock->front());

      if (PN == indVarPhiInst) {
        localIndVarPN = newPN;
        errs() << "Found local ind var: " << *PN << ", mapping it to " << *newPN << "\n";
      }

      for (int64_t i = PN->getNumIncomingValues() - 1; i >= 0; --i) {
        BasicBlock *incomingBB = PN->getIncomingBlock(i);
        Value *incomingVal = PN->getIncomingValue(i);
        if (incomingBB == endBlock) {
          newPN->addIncoming(incomingVal, newBlock);
          if (PN == indVarPhiInst) {
            //errs() << "2. Found local ind var: " << *PN << ", mapping it to " << *newPN << "\n";
            localIndVarPN = newPN;
            if (!localIndVar) {
              localIndVar = incomingVal;
              //errs() << "Setting local ind var: " << *incomingVal << "\n";
            } else {
              errs() << "Local ind var can't come twice. Old: " << *localIndVar << ", New:" << *incomingVal << "\n";
              exit(1);
            }
          }
        }
      }

      newPN->addIncoming(PN, onlyBlock);

      for (Value::use_iterator UI = PN->use_begin(), UE = PN->use_end(); UI != UE;) {
        Use &U = *UI++;
        auto *Usr = dyn_cast<Instruction>(U.getUser());
        if (Usr && Usr->getParent() != onlyBlock) {
          //errs() << "User to replace Phi: " << *Usr << "\n";
          if (Usr != newPN) {
            U.set(newPN);
          }
        } else {
#ifdef ALL_DEBUG
          errs() << "User in header block to replace Phi: " << *Usr << "\n";
#endif
        }
      }
    }

    /* check if local induction variables in the inner loop is found */
    if (!localIndVarPN || !localIndVar) {
      errs() << "Local induction variables are not available. Aborting.\n";
      if (!localIndVarPN) {
        errs() << "localIndVarPN absent\n";
      }
      if (!localIndVar) {
        errs() << "localIndVar absent\n";
      }
      exit(1);
    }

    /******************* Creating new conditions for inner loop *******************/
    Instruction *innerLoopTermInst = dyn_cast_or_null<BranchInst>(newBlock->getTerminator());
    IRBuilder<> IRInnerLoop(innerLoopTermInst);

    Value *valInnerCICond = nullptr;
    Value *valNewCond = nullptr;
    if (!isInverseCond) {
      valInnerCICond = IRInnerLoop.CreateICmpNE(localIndVar, valEndCond, "indVarPredicate");
    } else {
      valInnerCICond = IRInnerLoop.CreateICmpEQ(localIndVar, valEndCond, "indVarPredicate");
    }

    Value *valInnerCICondExt = valInnerCICond;
    //errs() << "Old cond type: " << *valOrigCond->getType() << ", new cond type: " << *valInnerCICond->getType() << "\n";
    if (valOrigCond->getType() != valInnerCICond->getType()) {
      valInnerCICondExt = IRInnerLoop.CreateZExt(valInnerCICond, valOrigCond->getType(), "zeroExtend");
    }

    if (!isInverseCond) {
      valNewCond = IRInnerLoop.CreateAnd(valOrigCond, valInnerCICondExt, "newCond");
    } else {
      valNewCond = IRInnerLoop.CreateOr(valOrigCond, valInnerCICondExt, "newCond");
    }

    BranchInst *newBranch = nullptr;
    if (!isInverseCond) {
      newBranch = BranchInst::Create(/*ifTrue*/ newBlock, /*ifFalse*/ endBlock, valNewCond);
    } else {
      newBranch = BranchInst::Create(/*ifTrue*/ endBlock, /*ifFalse*/ newBlock, valNewCond);
    }

    Instruction *toBeReplacedTerm = newBlock->getTerminator();
    ReplaceInstWithInst(toBeReplacedTerm, newBranch);

    //DT->recalculate(*(newBlock->getParent()));
    //if(SE) SE->forgetLoop(L);

    /* Add loop in loop info */
    //errs() << "OUTER SELFLOOP BEFORE LOOP TRANSFORM " << *L << "\n";
    Loop *newInnerLoop = LI->AllocateLoop();
    L->addChildLoop(newInnerLoop);
    newInnerLoop->addBlockEntry(newBlock);
    newInnerLoop->moveToHeader(newBlock);
    //errs() << "OUTER SELFLOOP AFTER LOOP TRANSFORM " << *L << "\n";

    /* Instrument the outer loop */
    BasicBlock *headerBBL = L->getHeader();
    errs() << "Applied self loop transform on " << headerBBL->getName() << "( " << headerBBL->getParent()->getName() << "() )\n";
    LCCNode *newLCC = new UnitLCC(lccIDGen++, endBlock, endBlock->getFirstNonPHI(), &(endBlock->back()), false);

    /* instrumenting the new outer loop */
    Instruction *endBlockTermInst = dyn_cast_or_null<BranchInst>(endBlock->getTerminator());
    IRBuilder<> IREnd(endBlockTermInst);

    /* Adding prints for debugging */
//#if 1
#ifdef ADD_RUNTIME_PRINTS
    {
      Module *M = onlyBlock->getModule();
      Function *printf_func = printf_prototype(M);
      llvm::Value *formatStr = nullptr;
      if (!isCanonical)
        formatStr = IRHead.CreateGlobalStringPtr("\nLoop preheader()->ind var:%d, step val: %d, end cond:%d\n", "printInHeader");
      else
        formatStr = IRHead.CreateGlobalStringPtr("\nLoop preheader()->ind var:%d, end cond:%d\n", "printInHeader");
      std::vector<llvm::Value *> args;
      args.push_back(formatStr);
      args.push_back(indVarPhiInst);
      if (!isCanonical)
        args.push_back(StepValue);
      args.push_back(valEndCond);
      IRHead.CreateCall(printf_func, args);
    }
#endif

//#if 1
#ifdef ADD_RUNTIME_PRINTS
    {
      Instruction *innerLoopNewTermInst = dyn_cast_or_null<BranchInst>(newBlock->getTerminator());
      IRBuilder<> IRInnerLoopNew(innerLoopNewTermInst);
      Module *M = onlyBlock->getModule();
      Function *printf_func = printf_prototype(M);
      llvm::Value *formatStr = IRInnerLoopNew.CreateGlobalStringPtr("\nInside inner loop():- ind var: %d, own cond: %d, orig cond: %d, combined cond: %d\n", "printInInnerLoop");
      //llvm::Value *formatStr = IRInnerLoopNew.CreateGlobalStringPtr("\nInside inner loop()\n", "printInInnerLoop");

      std::vector<llvm::Value *> args;
      args.push_back(formatStr);
      args.push_back(localIndVar);
      args.push_back(valInnerCICondExt);
      args.push_back(valOrigCond);
      args.push_back(valNewCond);
      IRInnerLoopNew.CreateCall(printf_func, args);
    }
#endif

//#if 1
#ifdef ADD_RUNTIME_PRINTS
    {
      Module *M = endBlock->getModule();
      Function *printf_func = printf_prototype(M);
      llvm::Value *formatStr = IREnd.CreateGlobalStringPtr("\nIn outer loop after inner:- outer ind var: %d, inner ind var: %d, own cond: %d, orig cond: %d, combined cond: %d\n", "printInEndBlock");
      std::vector<llvm::Value *> args;
      args.push_back(formatStr);
      args.push_back(indVarPhiInst);
      args.push_back(localIndVar);
      args.push_back(valInnerCICondExt);
      args.push_back(valOrigCond);
      args.push_back(valNewCond);
      IREnd.CreateCall(printf_func, args);
    }
#endif

    /* instrumenting the new outer loop */
    Value *loopIterations = nullptr;
    if (!isCanonical) {
      Value *loopIntv = IREnd.CreateSub(localIndVar, indVarPhiInst, "loop_intv");
      loopIterations = IREnd.CreateSDiv(loopIntv, StepValue, "loop_iter");
    } else {
      loopIterations = IREnd.CreateSub(localIndVar, indVarPhiInst, "loop_intv");
    }

    Value *loopIterationsExt = loopIterations;
    Value *loopBodyCost = IREnd.getInt64(numSelfLoopCost);
    if (loopIterations->getType() != loopBodyCost->getType()) {
      loopIterationsExt = IREnd.CreateZExt(loopIterations, loopBodyCost->getType(), "zeroExtendSLI");
    }
    Value *loopCost = IREnd.CreateMul(loopIterationsExt, loopBodyCost);

    /* register the outer loop for the cost instrumentation phase */
    UnitLCC *newUnitLCC = static_cast<UnitLCC *>(newLCC);
    newUnitLCC->instrumentValueForIC(loopCost);
    std::vector<LCCNode *> containers;
    containers.push_back(newLCC);
    bbToContainersMap[endBlock] = containers;

    return endBlock;
  }

  BasicBlock *transformSelfLoopWithBounds(Loop *L, int iterations) {

    BasicBlock *onlyBlock = L->getHeader();
    Function *F = onlyBlock->getParent();
    bool isInverseCond = false; /* inverse condition is when first successor of loop condition is not the header of the loop */

    errs() << "Attempting to transform self loop " << onlyBlock->getName() << " of " << F->getName() << " with " << iterations << " inner loop iterations --> " << *L;

    Value *indVarPhiInst = L->getCanonicalInductionVariable();
    Instruction *splitFrontInst = onlyBlock->getFirstNonPHI();
    if (!splitFrontInst) {
      errs() << "Self loop block does not have any non-phi instructions. Not handled.";
      exit(1);
    }

    /* Store IV Phi Nodes */
    InductionDescriptor IndDesc;
    SmallVector<PHINode *, 20> pnList;
    for (BasicBlock::iterator I = onlyBlock->begin(); isa<PHINode>(I); ++I) {
      if (isa<PHINode>(I)) {
        PHINode *PN = cast<PHINode>(I);
        pnList.push_back(PN);
        //errs() << "Need to copy Phi Node to new block --> " << *PN << "\n";
      }
    }

    auto boundsL = L->getBounds(*SE);
    BasicBlock *loopLatch = L->getLoopLatch();
    BasicBlock *loopHeader = L->getHeader();
    BranchInst *BI = dyn_cast_or_null<BranchInst>(loopLatch->getTerminator());
    ICmpInst *LatchCmpInst = dyn_cast<ICmpInst>(BI->getCondition());
    bool isStepInstFirstOp = false;

    assert((loopLatch == onlyBlock) && "A self loop cannot have separate latches & headers!");
    assert((loopHeader == onlyBlock) && "A self loop cannot have headers & body!");
    assert(LatchCmpInst && "Expecting the latch compare instruction to be a CmpInst");
    assert(BI && BI->isConditional() && "Expecting conditional latch branch");
    assert((BI->getNumSuccessors() == 2) && "Self loop with more than 2 successors is not handled");
    assert(boundsL && "Loop has no bounds information!");
    assert((LatchCmpInst->getOperand(0) == &(boundsL->getStepInst()) || LatchCmpInst->getOperand(1) == &(boundsL->getStepInst())) && "Latch compare instruction is not using the step instruction!");
    assert(indVarPhiInst->getType()->isIntegerTy() && "Canonical Induction variable is not of integer type!");

    if (LatchCmpInst->getOperand(0) == &(boundsL->getStepInst())) {
      isStepInstFirstOp = true;
    } else {
      isStepInstFirstOp = false;
    }

    if (BI->getSuccessor(0) != loopHeader) {
      isInverseCond = true;
    }

    // Value *valOrigCond = BI->getOperand(0);
    // BasicBlock *trueOperand = BI->getSuccessor(0);
    //errs() << "Orig cond: " << *valOrigCond << "\n";

    /******************* For first split *******************/
    IRBuilder<> IR1(splitFrontInst);
    AllocaInst *allocaLim = IR1.CreateAlloca(indVarPhiInst->getType(), 0, "newLimit");
    //errs() << "Canonical Ind Var Type: " << *indVarPhiInst->getType() << "\n";
    Value *valIterations = IR1.getIntN(SE->getTypeSizeInBits(indVarPhiInst->getType()), iterations);
    Value *valIncInitial = IR1.CreateAdd(valIterations, indVarPhiInst);
    IR1.CreateStore(valIncInitial, allocaLim);

    /******************* For second split *******************/
    Instruction *splitBackInst = BI;
    IRBuilder<> IR2(splitBackInst);
    Value *valZero = IR2.getIntN(SE->getTypeSizeInBits(indVarPhiInst->getType()), 0);
    Value *valLimit = IR2.CreateLoad(allocaLim);
    Value *valNewCond = IR2.CreateICmpNE(valLimit, valZero);

    //MemorySSAUpdater MSSAU(MSSA);
    BranchInst *newBranch = nullptr;
    BasicBlock *newBlock = SplitBlock(onlyBlock, splitFrontInst, DT, LI, nullptr);
    BasicBlock *endBlock = newBlock->splitBasicBlock(splitBackInst->getIterator());
    //BranchInst *newBranch = IR2.CreateCondBr(valNegCondComp,newBlock,endBlock);
    if (!isInverseCond) {
      newBranch = BranchInst::Create(/*ifTrue*/ newBlock, /*ifFalse*/ endBlock, valNewCond);
    } else {
      newBranch = BranchInst::Create(/*ifTrue*/ endBlock, /*ifFalse*/ newBlock, valNewCond);
    }

    Instruction *toBeReplacedTerm = newBlock->getTerminator();
    ReplaceInstWithInst(toBeReplacedTerm, newBranch);

    newBlock->setName("selfLoopOptBlock");
    endBlock->setName("selfLoopOptExitBlock");

    for (auto PN : pnList) {
      PHINode *newPN = PHINode::Create(PN->getType(), 2, "phiIVClone", &newBlock->front());

      for (int64_t i = PN->getNumIncomingValues() - 1; i >= 0; --i) {
        BasicBlock *incomingBB = PN->getIncomingBlock(i);
        Value *incomingVal = PN->getIncomingValue(i);
        if (incomingBB == endBlock) {
          newPN->addIncoming(incomingVal, newBlock);
        }
      }
      newPN->addIncoming(PN, onlyBlock);
      for (Value::use_iterator UI = PN->use_begin(), UE = PN->use_end(); UI != UE;) {
        Use &U = *UI++;
        auto *Usr = dyn_cast<Instruction>(U.getUser());
        if (Usr && Usr->getParent() != onlyBlock) {
          //errs() << "User to replace Phi: " << *Usr << "\n";
          if (Usr != newPN) {
            U.set(newPN);
          }
        } else {
          errs() << "User in header block to replace Phi: " << *Usr << "\n";
        }
      }
    }

    //DT->recalculate(*(newBlock->getParent()));
    //if(SE) SE->forgetLoop(L);

    Loop *newInnerLoop = LI->AllocateLoop();
    L->addChildLoop(newInnerLoop);
    L->addBlockEntry(newBlock);
    newInnerLoop->addBlockEntry(newBlock);
    newInnerLoop->moveToHeader(newBlock);
    return endBlock;
  }

  BasicBlock *transformLoop(Loop *L, int iterations) {
    BasicBlock *onlyBlock = L->getHeader();
    Function *F = onlyBlock->getParent();
    bool isInverseCond = false; /* inverse condition is when first successor of loop condition is not the header of the loop */

    errs() << "Attempting to transform self loop " << onlyBlock->getName() << " of " << F->getName() << " with " << iterations << " inner loop iterations --> " << *L;

    Instruction *splitFrontInst = onlyBlock->getFirstNonPHI();
    if (!splitFrontInst) {
      errs() << "Self loop block does not have any non-phi instructions. Not handled.";
      exit(1);
    }

    /* Store IV Phi Nodes */
    InductionDescriptor IndDesc;
    SmallVector<PHINode *, 20> pnList;
    for (BasicBlock::iterator I = onlyBlock->begin(); isa<PHINode>(I); ++I) {
      if (isa<PHINode>(I)) {
        PHINode *PN = cast<PHINode>(I);
        //if (!InductionDescriptor::isInductionPHI(&*PN, *&L, SE, IndDesc))
        //continue;
        pnList.push_back(PN);
        //errs() << "Need to copy Phi Node to new block --> " << *PN << "\n";
      }
    }

    BasicBlock *loopLatch = L->getLoopLatch();
    BasicBlock *loopHeader = L->getHeader();
    BranchInst *BI = dyn_cast_or_null<BranchInst>(loopLatch->getTerminator());
    ICmpInst *LatchCmpInst = dyn_cast<ICmpInst>(BI->getCondition());

    assert((loopLatch == onlyBlock) && "A self loop cannot have separate latches & headers!");
    assert((loopHeader == onlyBlock) && "A self loop cannot have headers & body!");
    assert(LatchCmpInst && "Expecting the latch compare instruction to be a CmpInst");
    assert(BI && BI->isConditional() && "Expecting conditional latch branch");
    assert((BI->getNumSuccessors() == 2) && "Self loop with more than 2 successors is not handled");

    if (BI->getSuccessor(0) != loopHeader) {
      isInverseCond = true;
    }

    Value *valOrigCond = BI->getOperand(0);
    // BasicBlock *trueOperand = BI->getSuccessor(0);
    //errs() << "Orig cond: " << *valOrigCond << "\n";
    //ICmpInst *splitBackInst = dyn_cast<ICmpInst>(BI->getCondition());
    Instruction *splitBackInst = BI;

    IRBuilder<> IR1(splitFrontInst);
    AllocaInst *allocaIterator = IR1.CreateAlloca(IR1.getInt32Ty(), 0, "canIndVar");
    Value *valOne = IR1.getInt32(1);
    IR1.CreateStore(valOne, allocaIterator);

    IRBuilder<> IR2(splitBackInst);
    valOne = IR2.getInt32(1);
    Value *valIterator = IR2.CreateLoad(allocaIterator);
    Value *valInc = IR2.CreateAdd(valOne, valIterator);
    IR2.CreateStore(valInc, allocaIterator);

    Value *valTargetInterval = IR2.getInt32(iterations);
    Value *valInnerCICond = nullptr;
    Value *valNewCond = nullptr;
    BranchInst *newBranch = nullptr;

    if (!isInverseCond) {
      valInnerCICond = IR2.CreateICmpULE(valInc, valTargetInterval, "canIndVarPredicate");
    } else {
      valInnerCICond = IR2.CreateICmpUGT(valInc, valTargetInterval, "canIndVarPredicate");
    }

    Value *valInnerCICondExt = valInnerCICond;
    //errs() << "Old cond type: " << *valOrigCond->getType() << ", new cond type: " << *valInnerCICond->getType() << "\n";
    if (valOrigCond->getType() != valInnerCICond->getType()) {
      valInnerCICondExt = IR2.CreateZExt(valInnerCICond, valOrigCond->getType(), "zeroExtend");
    }

    if (!isInverseCond) {
      valNewCond = IR2.CreateAnd(valOrigCond, valInnerCICondExt, "newCond");
    } else {
      valNewCond = IR2.CreateOr(valOrigCond, valInnerCICondExt, "newCond");
    }
    //Value *valNegCondComp = IR2.CreateICmpNE(valNewCond, valZero, "newComp");

    //MemorySSAUpdater MSSAU(MSSA);
    BasicBlock *newBlock = SplitBlock(onlyBlock, splitFrontInst, DT, LI, nullptr);
    BasicBlock *endBlock = newBlock->splitBasicBlock(splitBackInst->getIterator());
    //BranchInst *newBranch = IR2.CreateCondBr(valNegCondComp,newBlock,endBlock);
    if (!isInverseCond) {
      newBranch = BranchInst::Create(/*ifTrue*/ newBlock, /*ifFalse*/ endBlock, valNewCond);
    } else {
      newBranch = BranchInst::Create(/*ifTrue*/ endBlock, /*ifFalse*/ newBlock, valNewCond);
    }

    Instruction *toBeReplacedTerm = newBlock->getTerminator();
    ReplaceInstWithInst(toBeReplacedTerm, newBranch);

    newBlock->setName("selfLoopOptBlock");
    endBlock->setName("selfLoopOptExitBlock");

    for (auto PN : pnList) {
      PHINode *newPN = PHINode::Create(PN->getType(), 2, "phiIVClone", &newBlock->front());

      for (int64_t i = PN->getNumIncomingValues() - 1; i >= 0; --i) {
        BasicBlock *incomingBB = PN->getIncomingBlock(i);
        Value *incomingVal = PN->getIncomingValue(i);
        if (incomingBB == endBlock) {
          newPN->addIncoming(incomingVal, newBlock);
        }
      }
      newPN->addIncoming(PN, onlyBlock);
      for (Value::use_iterator UI = PN->use_begin(), UE = PN->use_end(); UI != UE;) {
        Use &U = *UI++;
        auto *Usr = dyn_cast<Instruction>(U.getUser());
        if (Usr && Usr->getParent() != onlyBlock) {
          //errs() << "User to replace Phi: " << *Usr << "\n";
          if (Usr != newPN) {
            U.set(newPN);
          }
        } else {
          errs() << "User in header block to replace Phi: " << *Usr << "\n";
        }
      }
    }

    //DT->recalculate(*(newBlock->getParent()));
    //if(SE) SE->forgetLoop(L);

    Loop *newInnerLoop = LI->AllocateLoop();
    L->addChildLoop(newInnerLoop);
    L->addBlockEntry(newBlock);
    newInnerLoop->addBlockEntry(newBlock);
    newInnerLoop->moveToHeader(newBlock);
    return endBlock;
  }

  void checkNTransformLoops(Function *F) {
    //profile_loops(F);
    errs() << "Checking loops of " << F->getName() << " for transformation\n";
    for (LoopInfo::iterator itLI = LI->begin(), itLIEnd = LI->end(); itLI != itLIEnd; ++itLI) {
      Loop *L = *itLI;

      //if(!L->getInductionVariable(*SE))
      //continue;

      if (!L->getSubLoops().empty()) {
        continue;
      }

      bool isSelfLoop = false;
      BasicBlock *headerBBL = L->getHeader();
      succ_iterator succBB = succ_begin(headerBBL), succEndBB = succ_end(headerBBL);
      for (; succBB != succEndBB; ++succBB) {
        if (*succBB == headerBBL) {
          isSelfLoop = true;
          break;
        }
      }
      if (!isSelfLoop) {
        continue;
      }

      if (L->getNumBlocks() != 1) {
        continue;
      }

      if (!getLatchCmpInst(*L)) {
        continue;
      }

      transformLoop(L, 357); // This function is only used for profiling. So a random iteration number is used.
    }
  }

  void profile_loop_advanced(Loop *L) {
    errs() << "profile_loop_advanced() for " << *L << "\n";
    BasicBlock *H = L->getHeader();

    BasicBlock *Incoming = nullptr, *Backedge = nullptr;
    if (!L->getIncomingAndBackEdge(Incoming, Backedge)) {
      return;
    }

    ICmpInst *LatchCmpInst = getLatchCmpInst(*L);
    if (!LatchCmpInst) {
      errs() << "profile_loop_advanced: No latch!\n";
      return;
    }
    //Value *Op0 = LatchCmpInst->getOperand(0);
    //Value *Op1 = LatchCmpInst->getOperand(1);

    auto lBounds = L->getBounds(*SE);
    if (!lBounds) {
      return;
    }
    Value *InitialIVValue2 = &lBounds->getInitialIVValue();
    Value *FinalIVValue = &lBounds->getFinalIVValue();
    Value *StepValue = lBounds->getStepValue();
    auto lbStepInst = &lBounds->getStepInst();
    auto lbCanPredicate = lBounds->getCanonicalPredicate();
    // auto lbDirection = lBounds->getDirection();

    auto preheaderBB = L->getLoopPreheader();
    auto *indVarPhiInst = L->getInductionVariable(*SE);
    Value *indVarVal = nullptr;
    assert(indVarPhiInst->getType()->isIntegerTy() && "Induction variable is not of integer type!");

    for (unsigned i = 0; i <= indVarPhiInst->getNumIncomingValues(); i++) {
      BasicBlock *incomingBB = indVarPhiInst->getIncomingBlock(i);
      if (incomingBB == preheaderBB) {
        indVarVal = indVarPhiInst->getIncomingValue(i);
        break;
      }
    }
    if (SE->isSCEVable(indVarVal->getType())) {
      errs() << "Initial Value is scevable & value is " << *(SE->getSCEV(indVarVal)) << "\n";
      errs() << "Value name is " << indVarVal->getName() << "\n";
    }

    if (InitialIVValue2) {
      errs() << "InitialIVValue from bounds: " << *InitialIVValue2 << "\n";
    }
    if (FinalIVValue) {
      errs() << "FinalIVValue from bounds: " << *FinalIVValue << "\n";
    }
    if (StepValue) {
      errs() << "StepValue from bounds: " << *StepValue << "\n";
    }
    if (lbStepInst) {
      errs() << "StepInst from bounds: " << *lbStepInst << "\n";
    }
    if (lbCanPredicate) {
      errs() << "Canonical Predicate from bounds: " << lbCanPredicate << "\n"; // ICMP_NE : include/llvm/IR/InstrTypes.h
    }
    //errs() << "Direction from bounds: " << lbDirection << "\n";

    // Loop over all of the PHI nodes, looking for a canonical indvar.
    errs() << "profile_loop_advanced: Iterating over all Phi Nodes!\n";
    for (BasicBlock::iterator I = H->begin(); isa<PHINode>(I); ++I) {
      PHINode *PN = cast<PHINode>(I);

      InductionDescriptor IndDesc;
      if (!InductionDescriptor::isInductionPHI(&*PN, *&L, SE, IndDesc)) {
        //errs() << "Not an IV PN:- " << *PN << "\n";
        continue;
      } else {
        Value *InitialIVValue = IndDesc.getStartValue();
        Instruction *StepInst = IndDesc.getInductionBinOp();
        if (!InitialIVValue || !StepInst) {
          errs() << "PN:- " << *PN << " -- > Has no Initial or Step Value\n";
        } else {
          errs() << "IV PN:- " << *PN << "\nInitial Value: " << *InitialIVValue << "\nStep Inst: " << *StepInst << "\n";
        }
      }

      if (ConstantInt *CI =
              dyn_cast<ConstantInt>(PN->getIncomingValueForBlock(Incoming))) {
        if (CI->isZero()) {
          if (Instruction *Inc =
                  dyn_cast<Instruction>(PN->getIncomingValueForBlock(Backedge))) {
            if (Inc->getOpcode() == Instruction::Add && Inc->getOperand(0) == PN) {
              if (ConstantInt *CI = dyn_cast<ConstantInt>(Inc->getOperand(1))) {
                if (CI->isOne()) {
                  errs() << "Found Canonical IV: " << *PN << "\nInc: " << *Inc << "\n";
                  return;
                } else {
                  errs() << "IV: " << *PN << "\nInc: " << *Inc << "\nIncrement is not one: " << *CI << "\n";
                }
              } else {
                errs() << "IV: " << *PN << "\nInc: " << *Inc << "\nIncrement is not constant int: " << *CI << "\n";
              }
            } else {
              errs() << "IV: " << *PN << "\nInc: " << *Inc << "\nIncrement Opcode: " << Inc->getOpcode() << "\nOperand(0): " << *(Inc->getOperand(0)) << "\n";
            }
          } else {
            errs() << "IV: " << *PN << "\nIncrement is not found for Backedge: " << Backedge->getName() << "\n";
          }
        } else {
          errs() << "IV: " << *PN << "\nIV start value is not 0: " << *CI << "\n";
        }
      } else {
        errs() << "IV: " << *PN << "\nIV initialization value is not found for Incoming : " << Incoming->getName() << "\n";
      }
    }
    return;
  }

#if 0
  /* only for debugging */
  void useLoopUnrollPasses(Function *F) {
    for (LoopInfo::iterator itLI = LI->begin(), itLIEnd = LI->end(); itLI != itLIEnd; ++itLI) {

      Loop *L = *itLI;

      TargetTransformInfo::UnrollingPreferences UP = gatherUnrollingPreferences(
          L, SE, TTI, BFI, PSI, OptLevel, ProvidedThreshold, ProvidedCount,
          ProvidedAllowPartial, ProvidedRuntime, ProvidedUpperBound,
          ProvidedAllowPeeling);

      LoopUnrollResult UnrollResult = UnrollLoop(
          L,
          {UP.Count, TripCount, UP.Force, UP.Runtime, UP.AllowExpensiveTripCount,
           UseUpperBound, MaxOrZero, TripMultiple, UP.PeelCount, UP.UnrollRemainder,
           ForgetAllSCEV},
          LI, &SE, &DT, &AC, &ORE, PreserveLCSSA, &RemainderLoop);

      if (UnrollResult == LoopUnrollResult::Unmodified)
        return LoopUnrollResult::Unmodified;
    }
  }
#endif

  int g_NumSelfLoops = 0;
  int g_NumIndVar = 0;
  int g_NumCanIndVar = 0;
  int g_NumSelfLoopsWithCanIndVar = 0;
  int g_NumIterations = 0;
  int g_NumLoops = 0;

  void profile_loops(Function *F) {
    errs() << "Profiling loops for " << F->getName() << "\n";
    std::set<Loop *> visitedLoops;
    std::set<Loop *> unvisitedLoops;
    if (LI->begin() == LI->end()) {
      return;
    }

    std::vector<Loop *> selfLoopList;
    std::vector<Loop *> loopList;
    for (LoopInfo::iterator itLI = LI->begin(), itLIEnd = LI->end(); itLI != itLIEnd; ++itLI) {
      Loop *firstLoop = *itLI;
      unvisitedLoops.insert(firstLoop);
      errs() << "Top Level Loop: " << *firstLoop << "\n";
    }

    while (!unvisitedLoops.empty()) {
      auto itUVL = unvisitedLoops.begin();
      Loop *L = *itUVL;

      //errs() << "Size of unvisited loops set: " << unvisitedLoops.size() << "\n";
      {
        g_NumLoops++;
        // const SCEV *backEdgeTakenCountTopL = SE->getBackedgeTakenCount(L);
        auto numIterations = SE->getSmallConstantTripCount(L);
        // auto numMaxIterations = SE->getSmallConstantMaxTripCount(L);
        bool hasInductionVar = false, hasCanInductionVar = false;
        if (L->getInductionVariable(*SE)) {
          hasInductionVar = true;
        }
        if (L->getCanonicalInductionVariable()) {
          hasCanInductionVar = true;
        }
        auto boundsL = L->getBounds(*SE);

        BasicBlock *headerBBL = L->getHeader();
        if (numIterations) {
          g_NumIterations++;
          errs() << "Loop " << headerBBL->getName() << " in " << F->getName() << "() has numeric backedges" << numIterations << "\n";
        }
        if (hasInductionVar) {
          g_NumIndVar++;
        }

        //errs() << "Loop: " << *L << "\n";
        if (checkIfSelfLoop(L)) {
          //errs() << "Self Loop --> hasInductionVar: " << hasInductionVar << ", hasCanInductionVar: " << hasCanInductionVar << ": " << *L << " (" << L << ") -->\nIterations: " << *backEdgeTakenCountTopL << ", #trips: " << numIterations << ", #maxtrips: " << numMaxIterations << "\n";
          g_NumSelfLoops++;
          if (hasCanInductionVar) {
            g_NumCanIndVar++;
            g_NumSelfLoopsWithCanIndVar++;
            errs() << "Self Loop " << headerBBL->getName() << " in " << F->getName() << "() has canonical induction variable " << *(L->getCanonicalInductionVariable()) << "\n";
            selfLoopList.push_back(L);
            if (boundsL) {
              errs() << "Cannonical Predicate for Self Loop " << headerBBL->getName() << " in " << F->getName() << "(): " << boundsL->getCanonicalPredicate() << "\n";
              //transformSelfLoopWithoutBounds(L,150);
            } else {
              //transformSelfLoopWithoutBounds(L,150);
            }
          }
        } else {
          if (hasCanInductionVar) {
            g_NumCanIndVar++;
            errs() << "Loop " << headerBBL->getName() << " in " << F->getName() << "() has canonical induction variable " << *(L->getCanonicalInductionVariable()) << "\n";
            loopList.push_back(L);
            if (boundsL) {
              errs() << "Cannonical Predicate for Loop " << headerBBL->getName() << " in " << F->getName() << "(): " << boundsL->getCanonicalPredicate() << "\n";
            }

            //transformLoopWithoutBounds(L, 150);
          }
        }
      }

      unvisitedLoops.erase(itUVL);
      auto presentInVisited = visitedLoops.find(L);
      if (presentInVisited == visitedLoops.end()) {
        visitedLoops.insert(L);
      }

      for (LoopInfo::iterator itL = L->begin(), itLEnd = L->end(); itL != itLEnd; ++itL) {
        Loop *neighbourL = *itL;
        unvisitedLoops.insert(neighbourL);
      }
    }

    for (auto selfLoopI : selfLoopList) {
      transformSelfLoopWithoutBounds(selfLoopI, 150, 100);
    }

    // for(auto loopI : loopList) {
    //   transformSelfLoopWithoutBounds(loopI,150);
    // }

#if 0
    for (LoopInfo::iterator itLI = LI->begin(), itLIEnd = LI->end(); itLI != itLIEnd; ++itLI) {
      Loop *L = *itLI;
      const SCEV *backEdgeTakenCountTopL = SE->getBackedgeTakenCount(L);
      auto numIterations = SE->getSmallConstantTripCount(L);
      auto numMaxIterations = SE->getSmallConstantMaxTripCount(L);
      bool hasInductionVar = false, hasCanInductionVar = false;
      if (L->getInductionVariable(*SE))
        hasInductionVar = true;
      if (L->getCanonicalInductionVariable())
        hasCanInductionVar = true;
      auto boundsL = L->getBounds(*SE);
      //auto initialL = boundsL->getInitialIVValue();
      //InductionDescriptor IndDesc;
      //bool res = boundsL->getInductionDescriptor(SE,IndDesc);
      //ConstantInt *InitialIVValue = dyn_cast<ConstantInt>(&boundsL->getInitialIVValue());
      //ConstantInt *StepValue = dyn_cast_or_null<ConstantInt>(boundsL->getStepValue());

      /* check for self loop */
      BasicBlock *headerBBL = L->getHeader();
      if (checkIfSelfLoop(L)) {
        errs() << "Top level self loop (top level), hasInductionVar: " << hasInductionVar << ", hasCanInductionVar: " << hasCanInductionVar << ": " << *L << " (" << L << ") -->\nIterations: " << *backEdgeTakenCountTopL << ", #trips: " << numIterations << ", #maxtrips: " << numMaxIterations << "\n";
        g_NumSelfLoops++;
        if (hasInductionVar) {
          g_NumCanIndVar++;
          g_NumSelfLoopsWithCanIndVar++;
          errs() << "Self Loop " << *headerBBL->getName() << " in " << F->getName() << "() has canonical induction variable\n";
        }
      } else {
        errs() << "Top level loop, hasInductionVar: " << hasInductionVar << ", hasCanInductionVar: " << hasCanInductionVar << ": " << *L << " (" << L << ") -->\nIterations: " << *backEdgeTakenCountTopL << ", #trips: " << numIterations << ", #maxtrips: " << numMaxIterations << "\n";
        //if(boundsL)
        //errs() << "Bounds present\n";
        if (hasInductionVar) {
          g_NumCanIndVar++;
          errs() << "Loop " << *headerBBL->getName() << " in " << F->getName() << "() has canonical induction variable\n";
        }
      }
#if 0
      if (InitialIVValue)
        errs() << "InitialIVValue present\n";
      if (StepValue)
        errs() << "StepValue present\n";
#endif
      auto blockV = L->getBlocksVector();
      errs() << "\tBlocks:-\n";
      for (auto bb : blockV) {
        errs() << "\t" << bb->getName() << "\n";
      }

      auto canonicalIV = L->getCanonicalInductionVariable();
      if (canonicalIV)
        errs() << "Cannonical IV:" << *canonicalIV << "\n";
      else {
        errs() << "No canonical IV present!\n";
        profile_loop_advanced(L);
      }

      if (!L->empty()) {
        for (Loop::iterator itL = L->begin(), itLEnd = L->end(); itL != itLEnd; ++itL) {
          Loop *SL = *itL;
          const SCEV *backEdgeTakenCountTopSL = SE->getBackedgeTakenCount(SL);
          auto numIterationsSL = SE->getSmallConstantTripCount(SL);
          auto numMaxIterationsSL = SE->getSmallConstantMaxTripCount(SL);
          bool hasInductionVarSL = false, hasCanInductionVarSL = false;
          if (SL->getInductionVariable(*SE))
            hasInductionVarSL = true;
          if (SL->getCanonicalInductionVariable())
            hasCanInductionVarSL = true;

          /* check for self loop */
          BasicBlock *headerBBSL = SL->getHeader();
          succ_iterator succBBSL = succ_begin(headerBBSL), succEndBBSL = succ_end(headerBBSL);
          for (; succBBSL != succEndBBSL; ++succBBSL) {
            if (*succBBSL == headerBBSL) {
              errs() << "\tSelf loop (subloop), hasInductionVar: " << hasInductionVarSL << ", hasCanInductionVar: " << hasCanInductionVarSL << ": " << *SL << " (" << L << ") -->\n\tIterations: " << *backEdgeTakenCountTopSL << ", #trips: " << numIterationsSL << ", #maxtrips: " << numMaxIterationsSL << "\n";
              continue;
            }
          }
          errs() << "\tSubloop, hasInductionVar: " << hasInductionVarSL << ", hasCanInductionVar: " << hasCanInductionVarSL << ": " << *SL << " (" << L << ") -->\n\tIterations: " << *backEdgeTakenCountTopSL << ", #trips: " << numIterationsSL << ", #maxtrips: " << numMaxIterationsSL << "\n";

          auto blockV = SL->getBlocksVector();
          errs() << "\t\tBlocks:-\n";
          for (auto bb : blockV) {
            errs() << "\t\t" << bb->getName() << "\n";
          }
        }
      }
      errs() << "\n";
    }
#endif
  }

  /* Analysis Pass will evaluate cost of functions and encode where to instrument */
  void analyzeAndInstrFunc(Function &F) {

    if (F.isDeclaration()) {
      return;
    }

#if 0
    //if( (F->getName().compare("slave2")!=0) )
    //if( F.getName().compare("mycovcmp")!=0 )
    //if( F.getName().compare("change_display") == 0 )
    //return;
    //if( F.getName().compare("merge_worker") != 0 )
    //return;

    if (F.getName().compare("linear_regression_map") != 0)
      return;

#endif

    LLVMCtx = &F.getContext();
    PDT = &getAnalysis<PostDominatorTreeWrapperPass>(F).getPostDomTree();
    DT = &getAnalysis<DominatorTreeWrapperPass>(F).getDomTree();
    LI = &getAnalysis<LoopInfoWrapperPass>(F).getLoopInfo();
    BPI = &getAnalysis<BranchProbabilityInfoWrapperPass>(F).getBPI();
    SE = &getAnalysis<ScalarEvolutionWrapperPass>(F).getSE();
    //MSSA = &getAnalysis<MemorySSAWrapperPass>().getMSSA();

#if 0
    if (F.getName().compare("linear_regression_map") == 0) {
      profile_loops(&F); /* get statistics for loops */
      exit(1);
    }
#endif

    transformGraph(&F);
    initializeLCCGraph(&F);
    runPasses(&F);
#if 0
    //errs() << "Begin Analysis for " << F->getName() << " :-\n";
    if (F.getName().compare("slave2") == 0 || F.getName().compare("jacobcalc2") == 0 || F.getName().compare("laplacalc") == 0 || F.getName().compare("jacobcalc") == 0)
      runPasses(&F);
    else
      errs() << "NOT ANALYSING " << F.getName() << "\n";
#endif
  }

  /* Always adds the increment of Local Compiler Interrupt (LLC) before the instruction passed */
  /* NonTLLC configuration must initialize & instrument Locals at function calls before & after the pass */
  Value *incrementTLLC(Instruction &I, Value *costVal) {
    Function &F = *I.getFunction();

    IRBuilder<> IR(&I);
    LoadInst *Load = nullptr;
    GlobalVariable *lc = nullptr;

    if (gIsOnlyThreadLocal) {
      lc = F.getParent()->getGlobalVariable("LocalLC");
      Load = IR.CreateLoad(lc);
    } else {
      Load = IR.CreateLoad(gLocalCounter[I.getFunction()]);
    }

//#if 1
#ifdef ADD_RUNTIME_PRINTS
    {
      Module *M = I.getParent()->getModule();
      Function *printf_func = printf_prototype(M);
      llvm::Value *formatStr = IR.CreateGlobalStringPtr("\nValue added to compiler interrupt:- %ld\n", "print_clock_incr");
      std::vector<llvm::Value *> args;
      args.push_back(formatStr);
      args.push_back(costVal);
      IR.CreateCall(printf_func, args);
    }
#endif

    Value *Inc = IR.CreateAdd(costVal, Load);

    if (gIsOnlyThreadLocal) {
      IR.CreateStore(Inc, lc);
    } else {
      IR.CreateStore(Inc, gLocalCounter[I.getFunction()]);
    }

#ifdef PROFILING
    if (gIsOnlyThreadLocal) {
      GlobalVariable *cc = nullptr;
      LoadInst *ccLoad = nullptr;
      cc = F.getParent()->getGlobalVariable("commitCount");
      ccLoad = IR.CreateLoad(cc);
      Value *valOne = IR.getInt64(1);
      Value *ccInc = IR.CreateAdd(valOne, ccLoad);
      IR.CreateStore(ccInc, cc);
    }
#endif

    return Inc;
  }

  /* Always adds the increment of Local Compiler Interrupt (LLC) before the instruction passed */
  /* NonTLLC configuration must initialize & instrument Locals at function calls before & after the pass */
  Value *incrementTLLCWithCycles(Instruction &I) {

    Function *F = I.getFunction();

    IRBuilder<> IR(&I);
    LoadInst *Load = nullptr;
    GlobalVariable *lc = nullptr;

    if (gIsOnlyThreadLocal) {
      lc = F->getParent()->getGlobalVariable("LocalLC");
      Load = IR.CreateLoad(lc);
    } else {
      Load = IR.CreateLoad(gLocalCounter[I.getFunction()]);
    }

//#if 1
#ifdef ADD_RUNTIME_PRINTS
    {
      Module *M = I.getParent()->getModule();
      Function *printf_func = printf_prototype(M);
      llvm::Value *formatStr = IR.CreateGlobalStringPtr("\nValue added to compiler interrupt:- %ld\n", "print_clock_incr");
      std::vector<llvm::Value *> args;
      args.push_back(formatStr);
      args.push_back(costVal);
      IR.CreateCall(printf_func, args);
    }
#endif

    CallInst *now = IR.CreateIntrinsic(Intrinsic::readcyclecounter, {}, {});
    GlobalVariable *thenVar = F->getParent()->getGlobalVariable("LastCycleTS");
    LoadInst *then = IR.CreateLoad(thenVar);
    Value *timeDiff = IR.CreateSub(now, then);
    Value *Inc = IR.CreateAdd(timeDiff, Load);

    if (gIsOnlyThreadLocal) {
      IR.CreateStore(Inc, lc);
    } else {
      IR.CreateStore(Inc, gLocalCounter[I.getFunction()]);
    }
    IR.CreateStore(now, thenVar);

    return Inc;
  }

  int getCostOfInstrumentation() {
    int instrumentationCost = 0;
    if (checkIfInstGranIsDet()) {
      instrumentationCost = 9;
    } else if (checkIfInstGranIsIntermediate()) {
      instrumentationCost = 15;
    } else if (checkIfInstGranCycleBasedCounter()) {
      instrumentationCost = 35;
    }
    if (InstGranularity != NAIVE_ACCURATE && InstGranularity != OPTIMIZE_ACCURATE) {
      assert((instrumentationCost != 0) && "Instrumentation cost is not available for this type of configuration");
    }
    /* naive-acc & opt-acc have the overhead added in the external lib calls */
    return instrumentationCost;
  }

  /* NonTLLC configuration must initialize & instrument Locals at function calls before & after the pass */
  void pushToMLCfromTLLCifTSCExceeded(Instruction *I, Value *loadedLC, LoadInst *loadDisFlag = nullptr) {

    if (!checkIfInstGranIsIntermediate()) {
      errs() << "pushToMLCfromTLLCifTSCExceeded is not implemented for this Inst Gran!\n";
      exit(1);
    }

    Function *F = I->getParent()->getParent();
    Module *M = I->getModule();
    I->getParent()->setName("cycleCheckBlock");
    assert((TargetIntervalInCycles) && "Target interval is not provided.");

    IRBuilder<> IR(I);
#ifdef SHIFT
    int threshold = 0.9 * TargetIntervalInCycles;
#else
    int threshold = 0.9 * TargetIntervalInCycles;
#endif
    Value *cycleInterval = IR.getInt64(threshold);

    CallInst *now = IR.CreateIntrinsic(Intrinsic::readcyclecounter, {}, {});
    GlobalVariable *thenVar = F->getParent()->getGlobalVariable("LastCycleTS");
    LoadInst *then = IR.CreateLoad(thenVar);
    Value *timeDiff = IR.CreateSub(now, then);

    Value *condition = IR.CreateICmpUGE(timeDiff, cycleInterval, "exceeded_cycle");
    //Instruction *ti = llvm::SplitBlockAndInsertIfThen(condition, I, false, nullptr, nullptr, LI);
    Instruction *thenTerm, *elseTerm;
    llvm::SplitBlockAndInsertIfThenElse(condition, I, &thenTerm, &elseTerm);
    elseTerm->getParent()->setName("reduceClock");
    IR.SetInsertPoint(elseTerm);

#ifdef SHIFT
    int cycleToIRConst;
    bool shiftLeft = false;
    if (TargetInterval > TargetIntervalInCycles) {
      cycleToIRConst = TargetInterval / TargetIntervalInCycles;
      shiftLeft = true;
    } else {
      cycleToIRConst = TargetIntervalInCycles / TargetInterval;
      shiftLeft = false;
    }
    Value *cycleIntervalTotal = IR.getInt64(TargetIntervalInCycles);
    Value *remTime = IR.CreateSub(cycleIntervalTotal, timeDiff);
    Value *reduction = remTime;
    if (cycleToIRConst != 1) {
      int shiftBits = (log2(cycleToIRConst));

      Value *valFactor = IR.getInt64(shiftBits);
      if (shiftLeft)
        reduction = IR.CreateShl(remTime, valFactor);
      else
        reduction = IR.CreateLShr(remTime, valFactor);
    }

    Value *newLocalLC = IR.CreateSub(loadedLC, reduction);
#else
    /* Reset counter */
    //float thresh_perc = (float)FiberConfig/100;
    //float thresh_perc = (float)50/100;
    //int instrumentationCost = (thresh_perc*TargetInterval) + getCostOfInstrumentation();
    int instrumentationCost = (TargetInterval / 2) + getCostOfInstrumentation();
    //int resetCost = TargetIntervalInCycles-threshold + 15;
    Value *newLocalLC = IR.getInt64(instrumentationCost);
#endif
    if (gIsOnlyThreadLocal) {
      Value *lc = M->getGlobalVariable("LocalLC");
      IR.CreateStore(newLocalLC, lc);
    } else {
      IR.CreateStore(newLocalLC, gLocalCounter[F]);
    }

    /* reset Cycle count, only if condition succeeds */
    pushToMLCfromTLLC(thenTerm, loadedLC, loadDisFlag, now);
  }

  /* currTSC: current clock value that should be set when the condition for pushing succeeded */
  void pushToMLCfromTLLC(Instruction *I, Value *loadedLC, LoadInst *loadDisFlag = nullptr, Value *currTSC = nullptr) {
    Module *M = I->getModule();
    Function &F = *(I->getParent()->getParent());
    I->getParent()->setName("pushBlock");
    IRBuilder<> Builder(I);
    GlobalVariable *clockDisabledFlag = nullptr;

    /* Disable CI */
    Value *incrCnt = Builder.getInt32(1);
    if (loadDisFlag) {
      Value *disFlagVal = Builder.CreateAdd(loadDisFlag, incrCnt);
      if (gIsOnlyThreadLocal) {
        clockDisabledFlag = M->getGlobalVariable("lc_disabled_count");
        Builder.CreateStore(disFlagVal, clockDisabledFlag);
      } else {
        Builder.CreateStore(disFlagVal, gLocalFLag[&F]);
      }
    }

    int instrumentationCost = getCostOfInstrumentation();

    /* Load local counter */
    Value *valLC;
    Value *lc = nullptr;
    if (gIsOnlyThreadLocal) {
      lc = F.getParent()->getGlobalVariable("LocalLC");
    }

    Value *valZero = Builder.getInt64(instrumentationCost);
    if (gIsOnlyThreadLocal) {
      Builder.CreateStore(valZero, lc);
    } else {
      Builder.CreateStore(valZero, gLocalCounter[&F]);
    }

    if (currTSC) {
      Module *M = I->getModule();
      GlobalVariable *thenVar = M->getGlobalVariable("LastCycleTS");
      Builder.CreateStore(currTSC, thenVar);
      //CallInst *now = Builder.CreateIntrinsic(Intrinsic::readcyclecounter, {}, {});
      //Builder.CreateStore(now, thenVar);
    }

    if (loadedLC) {
      valLC = loadedLC;
    } else {
      if (gIsOnlyThreadLocal) {
        valLC = Builder.CreateLoad(lc, "lc.reg");
      } else {
        valLC = Builder.CreateLoad(gLocalCounter[&F], "lc.reg");
      }
    }

    /* Code for calling custom function at push time */
    std::vector<llvm::Value *> args;
    args.push_back(valLC);

    Value *hookFuncPtr = action_hook_prototype(I);
    auto hookFunc = Builder.CreateLoad(hookFuncPtr->getType()->getPointerElementType(), hookFuncPtr, "ci_handler");
    Builder.CreateCall(cast<FunctionType>(hookFunc->getType()->getPointerElementType()), hookFunc, args);

    /* Enable CI */
    if (loadDisFlag) {
      if (gIsOnlyThreadLocal) {
#if 1
        /* Its better to load the flag here again, to avoid any change in the flag value because of any preempted compiler interrupt that returned without restoring the flag back again. It will probably be preempted back again, where the value will be decremented properly. */
        clockDisabledFlag = M->getGlobalVariable("lc_disabled_count");
        LoadInst *loadDisFlag2 = Builder.CreateLoad(clockDisabledFlag);
        Value *disFlagVal = Builder.CreateSub(loadDisFlag2, incrCnt);
        Builder.CreateStore(disFlagVal, clockDisabledFlag);
#else
        /* store the loaded value */
        Builder.CreateStore(loadDisFlag, clockDisabledFlag);
#endif
      } else {
        errs() << "This path requires fix!\n";
        exit(1);
        Builder.CreateStore(loadDisFlag, gLocalFLag[&F]);
      }
    }

#ifdef PROFILING
    if (gIsOnlyThreadLocal) {
      GlobalVariable *pc = nullptr;
      LoadInst *pcLoad = nullptr;
      pc = F.getParent()->getGlobalVariable("pushCount");
      pcLoad = Builder.CreateLoad(pc);
      Value *valOne = Builder.getInt64(1);
      Value *pcInc = Builder.CreateAdd(valOne, pcLoad);
      Builder.CreateStore(pcInc, pc);
    }
#endif
  }

#if 1
  Value *action_hook_prototype(Instruction *I) {
    Module *M = I->getParent()->getParent()->getParent();
    IRBuilder<> Builder(I);
    std::vector<Type *> funcArgs;
    funcArgs.push_back(Builder.getInt64Ty());
    //Value* funcPtr = M->getGlobalVariable("intvActionHook",PointerType::getUnqual(FunctionType::get(Builder.getVoidTy(), funcArgs, false)));
    /* Declare the thread local interrupt handler pointer, if it is not present in the module. */
    Value *funcPtr = M->getOrInsertGlobal("intvActionHook", PointerType::getUnqual(FunctionType::get(Builder.getVoidTy(), funcArgs, false)));
    GlobalVariable *gCIFuncPtr = static_cast<GlobalVariable *>(funcPtr);
    gCIFuncPtr->setThreadLocalMode(GlobalValue::GeneralDynamicTLSModel);
    assert(funcPtr && "Could not find intvActionHook");

    return funcPtr;
  }
#else
  /* This function should be supplied in the module */
  Function *action_hook_prototype(Module *M) {
    //Function *func = M->getFunction("_Z14intvActionHookl");
    Function *func = M->getFunction("intvActionHook");
    if (!func) {
      func = M->getFunction("_Z14intvActionHookl");
      if (!func) {
        func = (Function *)(M->getGlobalVariable("intvActionHook"));
      }
    }
    assert(func && "intvActionHook() is not supplied in the module.");
    return func;
  }
#endif

  /* This function should be supplied in the module */
  Function *action_hook_prototype_cumulative(Module *M) {
    //Function *func = M->getFunction("_Z14intvActionHookl");
    Function *func = M->getFunction("intvActionHookCumulative");
    assert(func && "intvActionHook() is not supplied in the module.");
    return func;
  }

#if 0
  Function *fiber_yield_prototype(Module *M) {
    Type *result = Type::getVoidTy(M->getContext());
    llvm::ArrayRef<Type *> param = llvm::makeArrayRef(Type::getInt64Ty(M->getContext()), 1);
    FunctionType *hook_type = FunctionType::get(result, param, false);
    Function *func = cast<Function>(M->getOrInsertFunction("fiber_yield", hook_type));
    return func;
  }

  Function *hook_prototype(Module *M) {
    FunctionType *hook_type = TypeBuilder<void(long), false>::get(M->getContext());
    Function *func = cast<Function>(M->getOrInsertFunction("update_logical_clock", hook_type));
    return func;
  }

  void callHookFunc(Instruction *I, Value *clock_tick) {
    IRBuilder<> IR(I);
    Module *M = I->getModule();
    Function *hookFunc = hook_prototype(M);
    assert(hookFunc && "Hook function update_logical_clock() not found!");
    Value *args[1] = {clock_tick};
    IR.CreateCall(hookFunc, ArrayRef<Value *>(args, 1));
  }
#endif

  void instrumentExternalCalls(Instruction *I) {
    if (!gUseReadCycles) {
      errs() << "reading cycle counters is not enabled!\n";
      exit(1);
    }

    BasicBlock::iterator itI1(I);
    BasicBlock::iterator itI2(I);
    itI2++; // points to the next inst for the second readcycle call
    if (itI2 == I->getParent()->end()) {
      errs() << "Next instruction of external function call is null. This is impossible.\n";
      exit(1);
    }
    IRBuilder<> IR1(&*itI1);
    CallInst *cyc1 = IR1.CreateIntrinsic(Intrinsic::readcyclecounter, {}, {});
    IRBuilder<> IR2(&*itI2);
    CallInst *cyc2 = IR2.CreateIntrinsic(Intrinsic::readcyclecounter, {}, {});
    Value *costVal = IR2.CreateSub(cyc2, cyc1);
    instrumentIfLCEnabled(&*itI2, ALL_IR, costVal); // ALL_IR here means the cost value is created & passed to the routine, although the value passed is the cycle count difference & not the IR difference
  }

  void instrumentExternalCallsWithIntrinsic(std::list<Instruction *> *IList) {
    if (!gUseReadCycles) {
      errs() << "reading cycle counters is not enabled!\n";
      exit(1);
    }

    CallInst *cyc1, *cyc2;
    bool first = true;

    for (auto I : *IList) {
      BasicBlock::iterator itI2(I);
      if (first) {
        BasicBlock::iterator itI1(I);
        IRBuilder<> IR1(&*itI1);
        cyc1 = IR1.CreateIntrinsic(Intrinsic::readcyclecounter, {}, {});
        //errs() << "First I " << *itI1 << "\n";
        first = false;
      }

      itI2++;

      if (itI2 == I->getParent()->end()) {
        //errs() << "Next instruction of external function call is null. This is impossible.\n";
        exit(1);
      }

      IRBuilder<> IR2(&*itI2);
      errs() << "I " << *itI2 << "\n";
      cyc2 = IR2.CreateIntrinsic(Intrinsic::readcyclecounter, {}, {});
      Value *cycleDiff = IR2.CreateSub(cyc2, cyc1);
      cyc1 = cyc2;

      int cycleToIRConst;
      bool shiftLeft = false;
      if (TargetInterval > TargetIntervalInCycles) {
        cycleToIRConst = TargetInterval / TargetIntervalInCycles;
        shiftLeft = true;
      } else {
        cycleToIRConst = TargetIntervalInCycles / TargetInterval;
        shiftLeft = false;
      }

      Value *libCallOverhead = cycleDiff;
      if (cycleToIRConst != 1) {
        int shiftBits = (log2(cycleToIRConst));

        Value *valFactor = IR2.getInt64(shiftBits);
        if (shiftLeft) {
          libCallOverhead = IR2.CreateShl(cycleDiff, valFactor);
        } else {
          libCallOverhead = IR2.CreateLShr(cycleDiff, valFactor);
        }
      }
      //Value *valTwo = IR2.getInt64(2);
      //Value* libCallOverhead = IR2.CreateShl(cycleDiff, valTwo);
      instrumentIfLCEnabled(&*itI2, ALL_IR, libCallOverhead); // ALL_IR here means the cost value is created & passed to the routine, although the value passed is the cycle count difference & not the IR difference
      itI2++;                                                 // points to the next inst for the second readcycle call
    }
  }

  BasicBlock *instrumentIfLCEnabled(Instruction *I, eInstrumentType instrType, Value *incVal = nullptr) {
    IRBuilder<> IR(I);
    Value *flagSet = IR.getInt32(0);
    LoadInst *loadDisFlag = nullptr;
    if (gIsOnlyThreadLocal) {
      Module *M = I->getModule();
      GlobalVariable *clockDisabledFlag = M->getGlobalVariable("lc_disabled_count");
      loadDisFlag = IR.CreateLoad(clockDisabledFlag);
    } else {
      loadDisFlag = IR.CreateLoad(gLocalFLag[I->getFunction()]);
    }
    Value *condition = IR.CreateICmpEQ(loadDisFlag, flagSet, "clock_running");
    Instruction *ti = llvm::SplitBlockAndInsertIfThen(condition, I, false, nullptr, DT, LI);
    IR.SetInsertPoint(ti);
    ti->getParent()->setName("if_clock_enabled");
    instrumentGlobal(ti, instrType, incVal, loadDisFlag);
    Function::iterator blockItr(ti->getParent());
    blockItr++;
    blockItr->setName("postClockEnabledBlock");
    return &*blockItr;
  }

  void testNpushMLCfromTLLC(Instruction &I, Value *loadedLC, LoadInst *loadDisFlag = nullptr, bool useTSC = false) {
    IRBuilder<> IR(&I);
    Value *targetinterval = nullptr;
    if (checkIfInstGranCycleBasedCounter()) {
      targetinterval = IR.getInt64(TargetIntervalInCycles); // CYCLES
    } else {
      targetinterval = IR.getInt64(TargetInterval); // PI
    }
    Value *condition = IR.CreateICmpUGT(loadedLC, targetinterval, "commit");
    Instruction *ti = llvm::SplitBlockAndInsertIfThen(condition, &I, false, nullptr, DT, LI);
    IR.SetInsertPoint(ti);
    Function::iterator blockItr(ti->getParent());
    blockItr++;
    blockItr->setName("postInstrumentation");
    if (useTSC) {
      pushToMLCfromTLLCifTSCExceeded(ti, loadedLC, loadDisFlag);
    } else {
      pushToMLCfromTLLC(ti, loadedLC, loadDisFlag);
    }
    return;
  }

  /* Get the list of functions in module in call graph order */
  void getCallGraphOrder() {
    CallGraph &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
    for (scc_iterator<CallGraph *> CGI = scc_begin(&CG), CGE = scc_end(&CG); CGI != CGE; ++CGI) {
      std::vector<CallGraphNode *> NodeVec = *CGI;
      for (std::vector<CallGraphNode *>::iterator I = NodeVec.begin(), E = NodeVec.end(); I != E; ++I) {
        Function *F = (*I)->getFunction();
        if (F && !F->isDeclaration()) {
          CGOrderedFunc[F->getName()] = false;
          if (NodeVec.size() > 1) {
            isRecursiveFunc[F->getName().str()] = true;
            errs() << "Recursive func name: " << F->getName() << "\n";
          } else if (NodeVec.size() == 1 && CGI.hasCycle()) {
            isRecursiveFunc[F->getName().str()] = true;
            errs() << "Self-Recursive func name: " << F->getName() << "(" << F << ") --> " << isRecursiveFunc[F->getName().str()] << "\n";
          } else {
            isRecursiveFunc[F->getName().str()] = false;
            //errs() << "Func name: " << F->getName() << "(" << F << ") --> " << isRecursiveFunc[F->getName()] << "\n";
          }
        }
        //if (NodeVec.size() == 1 && CGI.hasLoop()) dout << "Function " << F->getName() << " is recursive\n";
      }
    }

#if 1
    errs() << "List of Functions in Call Graph order:-\n";
    for (auto funcInfo : CGOrderedFunc) {
      errs() << "Function: " << funcInfo.first << "\n";
    }
#endif
  }

  /* Clone all functions. Must be called after thread functions can be found */
  void cloneFunctions(Module &M) {
    errs() << "\n************************** CLONING FUNCTIONS ****************************\n";
    llvm::StringRef suffix("_uninstrumented");
    for (auto funcIt = CGOrderedFunc.begin(); funcIt != CGOrderedFunc.end(); funcIt++) {
      auto fName = (*funcIt).first;
      Function *F = M.getFunction(fName);

      /* Do not clone thread functions */
      if (isThreadFunc(F)) {
        continue;
      }

      ValueToValueMapTy VMap;
      std::string cloneFName(fName);
      cloneFName.append(suffix.str());
      Function *dupFunc = CloneFunction(F, VMap, nullptr);
      dupFunc->setName(cloneFName);
      CGOrderedFunc.insert(funcIt, std::make_pair(dupFunc->getName(), true));
      funcIt++;
    }

#ifdef LC_DEBUG
    errs() << "Total set of functions after cloning :-\n";
    for (auto funcIt = CGOrderedFunc.begin(); funcIt != CGOrderedFunc.end(); funcIt++) {
      errs() << (*funcIt).first << "\n";
    }
#endif
  }

#if 0
  /* Find all functions that are used as function pointers. Must run after checking recursive functions & thread functions. */
  void findFuncWithPointers(Module &M) {
    for (auto &F : M) {
      if (F.isDeclaration())
        continue;
      //if(F.getName().compare("expose_callback") != 0) continue;
      bool addFunctionToGlobal = false;
      //bool isRecursive = isRecursiveFunc[F.getName()];
      /* A thread function or a recursive function will be fully instrumented. So we don't need to clone it for function pointer usage. */
      if (isThreadFunc(&F))
        continue;
      //errs() << "Checking if " << F.getName() << "() has been involved in any function pointers\n";
      int usersOfFunc = 0;
      for (auto *U : F.users()) { // U is of type User*
        usersOfFunc++;
        //errs() << "User " << *U << "\n";
        if (auto I = dyn_cast<Instruction>(U)) {
          //errs() << "I: " << *I << "\n";
          if (CallInst *ci = dyn_cast<CallInst>(I)) {
            //errs() << "ci: " << *ci << "\n";
            if (!ci->getCalledFunction()) {
#ifdef LC_DEBUG
              errs() << "findFuncWithPointers(): Not a direct function call for " << F.getName() << "(). Inst is: " << *ci << "\n";
#endif
              addFunctionToGlobal = true;
              break;
            } else {
              //errs() << "getCalledFunction() is not null. I: " << *(ci->getCalledFunction()) << "\n";
              Function *calledFunction = ci->getCalledFunction();
              if (calledFunction->getName().compare(F.getName()) != 0) {
#ifdef LC_DEBUG
                errs() << "findFuncWithPointers(): User of function " << F.getName() << " calls a different function " << calledFunction->getName() << " \n";
#endif
                addFunctionToGlobal = true;
                break;
              }
            }
          } else {
#ifdef LC_DEBUG
            errs() << "findFuncWithPointers(): User is not a call instruction for " << F.getName() << "(). Instruction --> " << *I << "\n";
#endif
            addFunctionToGlobal = true;
            break;
          }
        } else {
#ifdef LC_DEBUG
          errs() << "findFuncWithPointers(): User is not an instruction for " << F.getName() << "().  User --> " << *U << "\n";
#endif
          addFunctionToGlobal = true;
          break;
        }
      }

      if (usersOfFunc == 0) {
#ifdef LC_DEBUG
        /* Function with no user may be used in a function pointer */
        errs() << "findFuncWithPointers(): " << F.getName() << "() has no user!!\n";
        addFunctionToGlobal = true;
#endif
      }

      if (addFunctionToGlobal) {
        funcUsedAsPointers.push_back(F.getName());
#ifdef LC_DEBUG
        errs() << "findFuncWithPointers: Added func " << F.getName() << " to list of functions that might have function pointers to it!\n";
#endif
      }
    }
  }

  bool isFuncPointer(Function *F) {
    //errs() << "Checking if " << F->getName() << " is a func pointer!!\n";
    if (std::find(funcUsedAsPointers.begin(), funcUsedAsPointers.end(), F->getName()) != funcUsedAsPointers.end()) {
      return true;
    } else
      return false;
  }
#endif

  /* Find all functions that are called using pthread_create */
  void findThreadFunc(Module &M) {
    StringRef mainFunc("main");
    threadFunc.push_back(mainFunc);
    for (auto &F : M) {
      for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
        if (CallInst *ci = dyn_cast<CallInst>(&*I)) {
          if (!ci->getCalledFunction()) {
#ifdef LC_DEBUG
            errs() << "findThreadFunc: Unresolved Call Inst in " << F.getName() << " : " << *ci << "\n";
#endif
            continue;
          }
          auto calleeName = ci->getCalledFunction()->getName();
          if (calleeName.compare("pthread_create") == 0) {
            if (auto funcPointer = dyn_cast<PointerType>(ci->getOperand(2)->getType())) {
              if (isa<FunctionType>(funcPointer->getElementType())) {
                auto funcArg = ci->getArgOperand(2);
                Value *func = funcArg->stripPointerCasts();
                StringRef fName = func->getName();
#ifdef LC_DEBUG
                errs() << "Thread function: " << fName << "\n";
#endif
                threadFunc.push_back(fName);
              }
            }
          }
        }
      }
    }
  }

  static Function *printf_prototype(Module *M) {
    Function *func = M->getFunction("printf");
    if (!func) {
      // PointerType *Pty = PointerType::get(IntegerType::get(M->getContext(), 8), 0);
      FunctionType *FuncTy9 = FunctionType::get(IntegerType::get(M->getContext(), 32), true);

      func = Function::Create(FuncTy9, GlobalValue::ExternalLinkage, "printf", M);
      func->setCallingConv(CallingConv::C);

      //AttrListPtr func_printf_PAL;
      //func->setAttributes(func_printf_PAL);

      //FunctionType *printf_type = TypeBuilder<int(char *, ...), false>::get(M->getContext());

      //Type *printf_type = Type::getIntTy(M->getContext());
      //llvm::ArrayRef<Type*> param = llvm::makeArrayRef(Type::getInt64Ty(M->getContext()),1);
      //Function *func = cast<Function>(M->getOrInsertFunction("printf", printf_type));
    }
    return func;
  }

#ifdef PROFILING
  void createPrintFuncDecl(Module &M) {
    /* Create function declaration */
    std::string funcName("printCountersPi");
    std::vector<Type *> formalvars;
    Type *ptrIntType = Type::getInt64Ty(M.getContext());
    PointerType *ptrStringType = PointerType::getUnqual(Type::getInt8Ty(M.getContext()));
#ifdef PRINT_LC_DEBUG_INFO
    PointerType *ptrStringType2 = PointerType::getUnqual(Type::getInt8Ty(M.getContext()));
#endif

    formalvars.push_back(ptrIntType);
    formalvars.push_back(ptrStringType);
#ifdef PRINT_LC_DEBUG_INFO
    formalvars.push_back(ptrStringType2);
#endif

    FunctionType *funcType = FunctionType::get(Type::getVoidTy(M.getContext()), formalvars, false);

    Function *F = Function::Create(funcType, Function::ExternalLinkage, funcName, &M);
    Function::arg_iterator AI = F->arg_begin();
    std::string valName("lc_val");
    AI->setName(valName);
    ++AI;
    std::string msgName("lc_msg");
    AI->setName(msgName);
#ifdef PRINT_LC_DEBUG_INFO
    ++AI;
    std::string funcArgName("func_name");
    AI->setName(funcArgName);
#endif
  }

  void createPrintFuncDefn(Module &M) {
    /* Create function declaration */
    std::string funcName("printCountersPi");
    std::vector<Type *> formalvars;
    Type *ptrIntType = Type::getInt64Ty(M.getContext());
    PointerType *ptrStringType = PointerType::getUnqual(Type::getInt8Ty(M.getContext()));
#ifdef PRINT_LC_DEBUG_INFO
    PointerType *ptrStringType2 = PointerType::getUnqual(Type::getInt8Ty(M.getContext()));
#endif

    formalvars.push_back(ptrIntType);
    formalvars.push_back(ptrStringType);
#ifdef PRINT_LC_DEBUG_INFO
    formalvars.push_back(ptrStringType2);
#endif

    FunctionType *funcType = FunctionType::get(Type::getVoidTy(M.getContext()), formalvars, false);

    Function *F = Function::Create(funcType, Function::ExternalLinkage, funcName, &M);
    Function::arg_iterator AI = F->arg_begin();
    std::string valName("lc_val");
    AI->setName(valName);
    ++AI;
    std::string msgName("lc_msg");
    AI->setName(msgName);
#ifdef PRINT_LC_DEBUG_INFO
    ++AI;
    std::string funcArgName("func_name");
    AI->setName(funcArgName);
#endif

    /* Create entry block */
    IRBuilder<> Builder(M.getContext());
    BasicBlock *BB = BasicBlock::Create(M.getContext(), "entry", F);
    Builder.SetInsertPoint(BB);

    /* Allocating for formal parameters */
    std::string val_name("lc_val.addr");
    std::string msg_name("lc_msg.addr");
#ifdef PRINT_LC_DEBUG_INFO
    std::string func_name("func_name.addr");
#endif
    AllocaInst *allocaInt = Builder.CreateAlloca(ptrIntType, 0, val_name);
    AllocaInst *allocaString = Builder.CreateAlloca(ptrStringType, 0, msg_name);
#ifdef PRINT_LC_DEBUG_INFO
    AllocaInst *allocaString2 = Builder.CreateAlloca(ptrStringType2, 0, func_name);
#endif
    AI = F->arg_begin();
    Builder.CreateStore(AI, allocaInt);
    ++AI;
    Builder.CreateStore(AI, allocaString);
#ifdef PRINT_LC_DEBUG_INFO
    ++AI;
    Builder.CreateStore(AI, allocaString2);
#endif

    Value *lc_val = Builder.CreateLoad(allocaInt);
    Value *lc_msg = Builder.CreateLoad(allocaString);
#ifdef PRINT_LC_DEBUG_INFO
    Value *lc_func = Builder.CreateLoad(allocaString2);
#endif
    //lc_val = Builder.CreateLoad(lc_val);

    /* Call printf */
    Function *printf_func = printf_prototype(&M);

    std::string printlc("display_string");
#ifdef PRINT_LC_DEBUG_INFO
    llvm::Value *formatStr = Builder.CreateGlobalStringPtr("\n%s()->%s:%llu\n", printlc);
    //llvm::Value *formatStr = Builder.CreateGlobalStringPtr("\n%s()->%s\n", printlc);
#else
    llvm::Value *formatStr = Builder.CreateGlobalStringPtr("\n%s:%llu\n", printlc);
#endif

    std::vector<llvm::Value *> args;
    args.push_back(formatStr);
#ifdef PRINT_LC_DEBUG_INFO
    args.push_back(lc_func);
#endif
    args.push_back(lc_msg);
    args.push_back(lc_val);
    Builder.CreateCall(printf_func, args);

    Builder.CreateRetVoid();
  }
#endif

  /* For cost analysis & instrumentation, phi instructions are ignored (cannot instrument before Phi inst) & 
     * the instrumentations must occur after master clock definitions in thread functions */
  Instruction *findNextValidInst(Instruction *I) { /* deprecated */
    Function *F = I->getFunction();
    BasicBlock *BB = I->getParent();
    Instruction *retInst = I;

    if (isThreadFunc(F)) {
      BasicBlock *entryBB = &(F->getEntryBlock());
      if (BB == entryBB) {
        if (I == &(BB->front())) {
          /* Find the clock definition in the first basic block of a thread beginner function */
          bool found = false;
          for (auto inst = inst_begin(F); inst != inst_end(F); inst++) {
            if (isa<StoreInst>(&*inst)) {
              unsigned numOfOperands = (*inst).getNumOperands();
              if (numOfOperands == 2) {
                auto op2 = (*inst).getOperand(1);
                if (op2->getName().compare("tlMasterClock") == 0) {
                  inst++;
                  retInst = &*inst;
#ifdef LC_DEBUG
                  errs() << "Valid start inst for thread func " << F->getName() << "() : " << *retInst << "\n";
#endif
                  found = true;
                  break;
                }
              }
            }
          }
          assert(found && "This program needs to be updated with Master Clock definitions.");
        }
      }
    }

    retInst = checkForPhi(retInst);
    return retInst;
  }

  /* printType says whether it is the function name or basic block name to be printed in the debug mode 
     * 0 is for function, 1 is for basic block name */
  void callPrintFunc(Instruction *I, bool printType = true) { /* deprecated */

    IRBuilder<> IR2(I);
    Module *M = I->getParent()->getParent()->getParent();
    Function *printint = M->getFunction("printCountersPi");
    std::string name;
    if (printType) {
      name = std::string(I->getParent()->getParent()->getName());
    } else {
      name = std::string(I->getParent()->getName());
    }
    std::string func_name_var(name);
    std::string func_name(func_name_var);
    llvm::Value *funcName = IR2.CreateGlobalStringPtr(func_name, func_name_var);

    //I=checkForPhi(I);
    //pushToMLCfromTLLC(I, nullptr);
    //GlobalVariable *clock = M->getGlobalVariable("MasterLC");
    //GlobalVariable *globalPointer = M->getGlobalVariable("tlMasterClock");
    GlobalVariable *globalPointer = M->getGlobalVariable("commitCount");
    LoadInst *clock = IR2.CreateLoad(globalPointer);
    std::string slc("clock_string");
    llvm::Value *clockMsg = IR2.CreateGlobalStringPtr("Probe Count", slc);
#ifdef PRINT_LC_DEBUG_INFO
    Value *clock_args[3] = {clock, clockMsg, funcName};
    IR2.CreateCall(printint, ArrayRef<Value *>(clock_args, 3));
#else
    Value *clock_args[2] = {clock, clockMsg};
    IR2.CreateCall(printint, ArrayRef<Value *>(clock_args, 2));
#endif

    globalPointer = M->getGlobalVariable("pushCount");
    clock = IR2.CreateLoad(globalPointer);
    std::string pc("push_counter_string");
    clockMsg = IR2.CreateGlobalStringPtr("CI Count", pc);
#ifdef PRINT_LC_DEBUG_INFO
    Value *pc_args[3] = {clock, clockMsg, funcName};
    IR2.CreateCall(printint, ArrayRef<Value *>(pc_args, 3));
#else
    Value *pc_args pc_args[2] = {clock, clockMsg};
    IR2.CreateCall(printint, ArrayRef<Value *>(pc_args, 2));
#endif
  }

  void createPrintCalls(Module &M) { /* deprecated */

    for (auto &F : M) {
      if (!F.isDeclaration()) {
        /* Call print function at the end of thread functions */
        for (auto threadFNames : threadFunc) {
          if (F.getName().compare(threadFNames) == 0) {
#ifdef LC_DEBUG
            errs() << "Adding Clock Printing in --> " << F.getName() << "()\n\n\n";
#endif
            for (auto &BB : F) {
              for (auto &I : BB) {
                if (isa<ReturnInst>(&I)) {
                  callPrintFunc(&I);
                }
              }
            }
          }
        }

#ifdef INTERVAL_ACCURACY
        for (auto &BB : F) {
          //std::string pushBlockName("pushBlock");
          std::string bbName(BB.getName());
          auto found = bbName.find("pushBlock");
          if (found != std::string::npos) {
            //errs() << "Call print function on " << *I << " in func " << I->getParent()->getParent()->getName() << "\n";
            errs() << "Calling print function in block " << bbName << " of function " << F.getName() << "\n";
            callPrintFunc(&BB.back());
          }
        }
#endif
        /* Call print function before exit functions */
        for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
#if 1
          /* call print function before every exit statement */
          if (CallInst *ci = dyn_cast<CallInst>(&*I)) {
            Function *calledFunction = ci->getCalledFunction();
            if (calledFunction) {
              if (calledFunction->getName().compare("exit") == 0) {
                callPrintFunc(&*I);
              }
            }
          }
#else
          /* Will not work if mergereturn pass is used. Then the unreachable inst moves to a different block 
             * from where the exit instructions cannot be found */
          if (isa<UnreachableInst>(&*I)) {
            /* The instruction before unreachable makes it unreachable, so the instrumentation should be one instruction before */
            inst_iterator inst = I;
            inst--;
            callPrintFunc(&*inst);
          }
#endif
        }

#ifdef ACCURACY
        /* Print compiler interrupt at the end of every function for the purpose of debugging. */
        if (F.getName().compare("printCountersPi") != 0)
          callPrintFunc(&F.back().back());
        /* print clock after every basic block */
        if (F.getName().compare("printCountersPi") != 0) {
          for (auto &BB : F) {
            callPrintFunc(&BB.back(), false);
          }
        }
#if 0
        if (F.getName().compare("INTERF") == 0) {
          for (auto &BB : F) {
            if (BB.getName().compare("for.body117.preheader") == 0) {
              callPrintFunc(&BB.front(), false);
              errs() << "Instrumenting for.body117.preheader\n";
            }
            if (BB.getName().compare("for.end181") == 0) {
              callPrintFunc(&BB.back(), false);
              errs() << "Instrumenting for.end181\n";
            }
          }
        }
        if (F.getName().compare("_Z11simple_loopi") == 0) {
          callPrintFunc(&F.front().front());
        }
        if (F.getName().compare("_Z12simple_whilei") == 0) {
          for (auto &BB : F) {
            if ((BB.getName().find("postPushBlock") == std::string::npos) && BB.getName().find("pushBlock") == std::string::npos)
              //if(BB.getName().compare("if.end42")==0){
              callPrintFunc(&BB.back(), false);
            //}
          }
        }
        if (F.getName().compare("POTENG") == 0) {
          for (auto &BB : F) {
            /*if((BB.getName().find("postPushBlock") == std::string::npos)
                  && BB.getName().find("pushBlock") == std::string::npos) */
            if (BB.getName().compare("if.end241") == 0) {
              callPrintFunc(&BB.back(), false);
            }
          }
        }
#endif
#endif
      }
    }
  }

  void writeCost(Module &M) {
    if (OutCostFilePath.empty()) {
      return;
    }
    unlink(OutCostFilePath.c_str());

    std::error_code EC;
    sys::fs::remove(OutCostFilePath);
    raw_fd_ostream fout(OutCostFilePath, EC, sys::fs::F_Text);
    fout << "Cost File\n";
    //std::ofstream fout(OutCostFilePath.c_str());
    for (auto &F : M) {
      if (F.isDeclaration()) {
        continue;
      }
      std::string funcName(F.getName());
      auto found = computedFuncInfo.find(&F);
      if (found != computedFuncInfo.end()) {
        InstructionCost *funcCost = found->second->cost;
        InstructionCost *simplifiedCost = simplifyCost(&F, funcCost);
        int numCost = getConstCostWithoutAssert(simplifiedCost);
        if (numCost > 0) {
          //errs() << "Writing cost for " << F.getName() << " : " << numCost << "\n";
          fout << funcName << ":";
          fout << *funcCost << "\n";
        }
      }
    }
    fout.close();
  }

  bool readCost() {
    /* There may not be any library cost file supplied */
    if (InCostFilePath.empty()) {
      errs() << "No library file supplied\n";
      return true;
    }
    std::ifstream fin;
    fin.open(InCostFilePath);
    bool first = true;

    if (!fin.good()) {
      //errs() << ConfigFile << "not found!\nAborting Compiler Interrupt Pass.\n";
      return false;
    }

    while (!fin.eof()) {
      char buf[128];
      char *token1, *token2;
      fin.getline(buf, 128);
      std::string str(buf);
      if (first) {
        first = false;
        if (str.compare("Cost File") != 0) {
          return false;
        } else {
          continue;
        }
      }
      if (std::string::npos != str.find(':')) {
        token1 = strtok(buf, ":");
        if (token1) {
          token2 = strtok(0, ":");
          int iCost = atoi(token2);
          //errs() << "Reading cost for func name: " << token1 << ":" << iCost << "\n";
          std::string funcName(token1);
          /* TODO: Write a string to instruction cost function */
          libraryInstructionCosts[funcName] = new InstructionCost(InstructionCost::CONST, iCost);
        }
      }
    }

    for (auto lib_cost : libraryInstructionCosts) {
      errs() << "Library function " << lib_cost.first << " : " << *(lib_cost.second) << "\n";
    }
    return true;
  }

  bool readConfig() {
    if (ConfigFile.empty()) {
      return true;
    }
    std::ifstream fin;
    fin.open(ConfigFile);
    int cost = -1;

    if (!fin.good()) {
      //errs() << ConfigFile << "not found!\nAborting Compiler Interrupt Pass.\n";
      return false;
    }

    while (!fin.eof()) {
      char buf[128];
      char *token1, *token2, *subtoken;
      fin.getline(buf, 128);

      std::string str(buf);
      if (std::string::npos != str.find(':')) {
        token1 = strtok(buf, ":");
        if (token1) {
          token2 = strtok(0, ":");

          subtoken = strtok(token1, "-");
          if (subtoken) {
            subtoken = strtok(0, "-");
          } else {
            //errs() << "Config file format is incorrect!\n";
            return false;
          }

          subtoken = strtok(token2, "-");
          if (subtoken) {
            subtoken = strtok(0, "-");
            cost = atoi(subtoken);
          } else {
            //errs() << "Config file format is incorrect!\n";
            return false;
          }
        }
      } else {
        if (!str.empty()) {
          if (cost != -1) {
            char *token = strtok(buf, " ");
            libraryInstructionCosts[token] = new InstructionCost(InstructionCost::CONST, cost);
          } else {
            //errs() << "Wrong file format. Cannot list library functions without specifying cost of class.\n";
            return false;
          }
        }
      }
    }
    return true;
  }

  /* All initial instrumentation prior to Analysis and Instrumentation passes should be done here */
  void initializeInstrumentation(Module &M) {
    auto initVal = llvm::ConstantInt::get(M.getContext(), llvm::APInt(64, 0, false));
    // auto initVal32 = llvm::ConstantInt::get(M.getContext(), llvm::APInt(32, 0, false));

    GlobalVariable *lc = new GlobalVariable(M, Type::getInt64Ty(M.getContext()), false, GlobalValue::ExternalLinkage, 0, "LocalLC");
    lc->setThreadLocalMode(GlobalValue::GeneralDynamicTLSModel);
    if (DefineClock) {
      lc->setInitializer(initVal);
    }

    GlobalVariable *interrupt_disabled_count = new GlobalVariable(M, Type::getInt32Ty(M.getContext()), false, GlobalValue::ExternalLinkage, 0, "lc_disabled_count");
    interrupt_disabled_count->setThreadLocalMode(GlobalValue::GeneralDynamicTLSModel);
#if 0
    // The variable is now maintained in the CI library
    if (DefineClock) {
      interrupt_disabled_count->setInitializer(initVal32);
    }
#endif

#ifdef PROFILING
    errs() << "Creating commitCount variable for profiling!!";
    GlobalVariable *cc = new GlobalVariable(M, Type::getInt64Ty(M.getContext()), false, GlobalValue::ExternalLinkage, 0, "commitCount");
    GlobalVariable *pc = new GlobalVariable(M, Type::getInt64Ty(M.getContext()), false, GlobalValue::ExternalLinkage, 0, "pushCount");
    cc->setThreadLocalMode(GlobalValue::GeneralDynamicTLSModel);
    pc->setThreadLocalMode(GlobalValue::GeneralDynamicTLSModel);
    if (DefineClock) {
      cc->setInitializer(initVal);
      pc->setInitializer(initVal);
    }
#endif

    /* Clone all functions */
    //cloneFunctions(M);
  }

  /* Code for IR generation from parametric cost */
  Value *scevToIR(Instruction *inst, const InstructionCost *fcost) {
    IRBuilder<> Builder(inst);
    Value *val = NULL;
    switch (fcost->_type) {
    case InstructionCost::CONST: {
      val = Builder.getInt64(fcost->_value);
      break;
    }
    case InstructionCost::ADD: {
      Value *left, *right;
      bool first = true;
      for (auto op : fcost->_operands) {
        if (first) {
          first = false;
          left = scevToIR(inst, op);
        } else {
          right = scevToIR(inst, op);
          IRBuilder<> Builder(inst);
          if (right->getType()->isPointerTy()) {
            right = Builder.CreateLoad(right);
          }
          if (left->getType()->isPointerTy()) {
            left = Builder.CreateLoad(left);
          }
          //errs() << "Addend types: " << *left->getType() << " & " << *right_val->getType() << "\n";
          val = Builder.CreateAdd(left, right, "sum", false, false);
          left = val;
        }
      }
      break;
    }
    case InstructionCost::UDIV: {
      Value *left = scevToIR(inst, fcost->_operands[0]);
      Value *right = scevToIR(inst, fcost->_operands[1]);
      IRBuilder<> Builder(inst);
      if (right->getType()->isPointerTy()) {
        right = Builder.CreateLoad(right);
      }
      if (left->getType()->isPointerTy()) {
        left = Builder.CreateLoad(left);
      }
      //Value *right_val = Builder.CreateLoad(right);
      //errs() << "Addend types: " << *left->getType() << " & " << *right_val->getType() << "\n";
      val = Builder.CreateUDiv(left, right, "quotient", false);
      break;
    }
    case InstructionCost::MUL: {
      Value *left, *right;
      bool first = true;
      for (auto op : fcost->_operands) {
        if (first) {
          first = false;
          left = scevToIR(inst, op);
        } else {
          right = scevToIR(inst, op);
          IRBuilder<> Builder(inst);
          if (right->getType()->isPointerTy()) {
            right = Builder.CreateLoad(right);
          }
          if (left->getType()->isPointerTy()) {
            left = Builder.CreateLoad(left);
          }
          //errs() << "left type: " << *(left->getType()) << ", right type: " << *(right->getType()) << ", left: " << *left << ", right: " << *right << "\n";
          val = Builder.CreateMul(left, right, "product", false, false);
          left = val;
        }
      }
      break;
    }
    //case InstructionCost::ADD_REC_EXPR:
    case InstructionCost::SMAX: {
      //BasicBlock::iterator instItr((*block)->front());
      //instItr++;
      assert(fcost->_operands.size() != 1);
      Value *left = scevToIR(inst, fcost->_operands[0]);
      Value *right = scevToIR(inst, fcost->_operands[1]);
      IRBuilder<> Builder(inst);
      Value *isMax = Builder.CreateICmpSGE(left, right, "smax");
      val = Builder.CreateSelect(isMax, left, right, "select_smax");
      //std::string name("smaxVal");
      //AllocaInst *alloca_inst;
      //alloca_inst = Builder.CreateAlloca(Builder.getInt32Ty(), 0, name);
      //Instruction *thenTerm, *elseTerm;
      //llvm::SplitBlockAndInsertIfThenElse(isMax, inst, &thenTerm, &elseTerm);
      //thenTerm->getParent()->setName("thenBlock");
      //elseTerm->getParent()->setName("elseBlock");
      //Builder.SetInsertPoint(thenTerm);
      //errs() << "SMAX : Left " << *left << "(" << *(left->getType()) << "), ";
      //val = Builder.CreateStore(left, alloca_inst);
      //Builder.SetInsertPoint(elseTerm);
      //errs() << "SMAX : Right " << *right << "(" << *(right->getType()) << ")\m";
      //val = Builder.CreateStore(right, alloca_inst);

      //Builder.SetInsertPoint(&*instItr);
      //Function::iterator *blockItr = elseTerm->getIterator();
      //Function::iterator blockItr(instItr->getParent());
      //Function::iterator blockItr(elseTerm->getParent());
      //blockItr++;
      //*block = &*blockItr;
      //*block = &(*instItr->getParent());
      //val = alloca_inst;
      break;
    }
    case InstructionCost::SMIN: {
      assert(fcost->_operands.size() != 1);
      Value *left = scevToIR(inst, fcost->_operands[0]);
      Value *right = scevToIR(inst, fcost->_operands[1]);
      IRBuilder<> Builder(inst);
      Value *isMin = Builder.CreateICmpSLE(left, right, "smin");
      val = Builder.CreateSelect(isMin, left, right, "select_smin");
      break;
    }
    case InstructionCost::UMAX: {
      //BasicBlock::iterator instItr((*block)->front());
      //instItr++;
      assert(fcost->_operands.size() != 1);
      Value *left = scevToIR(inst, fcost->_operands[0]);
      Value *right = scevToIR(inst, fcost->_operands[1]);
      IRBuilder<> Builder(inst);
      std::string name("umaxVal");
      Value *isMax = Builder.CreateICmpUGE(left, right, "umax");
      val = Builder.CreateSelect(isMax, left, right, "select_umax");
      //AllocaInst *alloca_inst;
      //alloca_inst = Builder.CreateAlloca(Builder.getInt32Ty(), 0, name);
      //Instruction *thenTerm, *elseTerm;
      //llvm::SplitBlockAndInsertIfThenElse(isMax, inst, &thenTerm, &elseTerm);
      //thenTerm->getParent()->setName("thenBlock");
      //elseTerm->getParent()->setName("elseBlock");
      //Builder.SetInsertPoint(thenTerm);
      //val = Builder.CreateStore(left, alloca_inst);
      //Builder.SetInsertPoint(elseTerm);
      //val = Builder.CreateStore(right, alloca_inst);

      //Builder.SetInsertPoint(&*instItr);
      //Function::iterator *blockItr = elseTerm->getIterator();
      //Function::iterator blockItr(instItr->getParent());
      //Function::iterator blockItr(elseTerm->getParent());
      //blockItr++;
      //*block = &*blockItr;
      //*block = &(*instItr->getParent());
      //val = alloca_inst;
      break;
    }
    case InstructionCost::UMIN: {
      assert(fcost->_operands.size() != 1);
      Value *left = scevToIR(inst, fcost->_operands[0]);
      Value *right = scevToIR(inst, fcost->_operands[1]);
      IRBuilder<> Builder(inst);
      Value *isMin = Builder.CreateICmpULE(left, right, "umin");
      val = Builder.CreateSelect(isMin, left, right, "select_umin");
      break;
    }
    case InstructionCost::ZERO_EXT: {
      Value *operand = scevToIR(inst, fcost->_operands[0]);
      IRBuilder<> Builder(inst);
      if (operand->getType()->isPointerTy()) {
        operand = Builder.CreateLoad(operand);
      }
      val = Builder.CreateZExt(operand, fcost->_castExprType, "zero_extend");
      break;
    }
    case InstructionCost::SIGN_EXT: {
      Value *operand = scevToIR(inst, fcost->_operands[0]);
      IRBuilder<> Builder(inst);
      if (operand->getType()->isPointerTy()) {
        operand = Builder.CreateLoad(operand);
      }
      val = Builder.CreateSExt(operand, fcost->_castExprType, "sign_extend");
      break;
    }
    case InstructionCost::TRUNC: {
      Value *operand = scevToIR(inst, fcost->_operands[0]);
      IRBuilder<> Builder(inst);
      if (operand->getType()->isPointerTy()) {
        operand = Builder.CreateLoad(operand);
      }
      val = Builder.CreateTrunc(operand, fcost->_castExprType, "trunc");
      break;
    }
    case InstructionCost::ARG: {
      auto F = inst->getParent()->getParent();
      auto arg = F->arg_begin() + fcost->_value; // F->arg_begin()[index]
      //errs() << "ARG(" << fcost->_value << "): " << *arg << "\n";
      //std::string nameFuncArg("funcArg");
      //std::string nameArg("arg");
      //val = Builder.CreateLoad(arg, nameArg);
      //AllocaInst *alloca_inst;
      //alloca_inst = Builder.CreateAlloca(Builder.getInt32Ty(), 0, nameFuncArg);
      //val = Builder.CreateLoad(arg, nameArg);
      val = arg;
      break;
    }
    default: {
      errs() << "Received Wrong type in scevToIR. Returning NULL.\n";
      return nullptr;
    }
    }

    return val;
  }

  void instrumentAllBlocks(Module &M) {
    errs() << "Instrumenting all blocks\n";

    initializeLastCycleTL(M);

    for (auto &F : M) {
      if (F.isDeclaration()) {
        continue;
      }

      if (isRestrictedFunction(&F)) {
        continue;
      }

      /* Initialize stat to 0 for every function */
      instrumentedInst = 0;

      /* Create local variables for loading & storing the thread local counter & flag */
      initializeLocals(&F);

      /* Find costs */
      std::map<Instruction *, int> costMap;
      for (auto &B : F) {
#if 0
        if (F.getName().compare("main") != 0)
          break;
        if ((B.getName().compare("for.body3.us.i") != 0)) {
          continue;
        }
        errs() << "Naive instrumenting " << B.getName() << "\n";
#endif
        Instruction *I = B.getTerminator();
        //int instCost = std::distance(B.begin(), B.end());
        int instCost = 0;
        for (auto &bbI : B) {
          if (!isa<PHINode>(&bbI)) {
            if (isa<LoadInst>(&bbI) || isa<StoreInst>(&bbI)) {
              instCost += MemOpsCost;
            } else if (checkIfExternalLibraryCall(&bbI)) {
              instCost += getLibCallCost();
            } else {
              instCost++;
            }
          }
        }
        costMap[I] = instCost;
        //errs() << "Storing cost " << instCost << " for basic block " << B.getName() << " of function " << F.getName() << "\n";
      }

      /* Instrument costs */
      for (auto it = costMap.begin(); it != costMap.end(); it++) {
        Instruction *I = it->first;
        int instCount = it->second;
        if (instCount != 0) {
          //errs() << "Loading cost " << instCount << " for basic block " << I->getParent()->getName() << " of function " << I->getFunction()->getName() << "\n";
          IRBuilder<> Builder(I);
          Value *val = Builder.getInt64(instCount);
          switch (InstGranularity) {
          case NAIVE_INTERMEDIATE:
            instrumentIfLCEnabled(I, PUSH_ON_CYCLES, val);
            break;
          case NAIVE_HEURISTIC_FIBER:
            instrumentGlobal(I, ALL_IR, val);
            break;
          case NAIVE_CYCLES:
            instrumentIfLCEnabled(I, INCR_ON_CYCLES);
            break;
          default:
            instrumentIfLCEnabled(I, ALL_IR, val);
            break;
          }
        }
      }

      /* Compute stats for naive */
      computeCostEvalStats(&F); // only because of computeInstrStats's dependence on the Fstat structure created by it
      computeInstrStats(&F);

      /* instrument locals */
      instrumentLocals(&F);

      if (InstGranularity == NAIVE_ACCURATE) {
        instrumentLibCallsWithCycleIntrinsic(&F);
      }
    }
  }

#if 0
  void replaceCIConfigCalls(Function &F) {
    Module *M = F.getParent();
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; I++) {
      if (CallInst *ci = dyn_cast<CallInst>(&*I)) {
        auto calledFunc = ci->getCalledFunction();
        if (calledFunc) {
          if (calledFunc->getName().compare("ci_disable") == 0) {
            IRBuilder<> Builder(&*I);
            Instruction *instToDel = &*I;
            GlobalVariable *clockDisabledFlag = M->getGlobalVariable("lc_disabled_count");
            LoadInst *loadDisFlag = Builder.CreateLoad(clockDisabledFlag);
            Value *incrCnt = Builder.getInt32(1);
            Value *disFlagVal = Builder.CreateAdd(loadDisFlag, incrCnt);
            if (!gIsOnlyThreadLocal) {
              Builder.CreateStore(disFlagVal, gLocalFLag[&F]);
            }
            Builder.CreateStore(disFlagVal, clockDisabledFlag);
            I++; // increment the pointer before deleting the pointed instruction
            instToDel->eraseFromParent();
            I--;
          } else if (calledFunc->getName().compare("ci_enable") == 0) {
            IRBuilder<> Builder(&*I);
            Instruction *instToDel = &*I;
            GlobalVariable *clockDisabledFlag = M->getGlobalVariable("lc_disabled_count");
            LoadInst *loadDisFlag = Builder.CreateLoad(clockDisabledFlag);
            Value *minFlagVal = Builder.getInt32(0);
            Value *condition = Builder.CreateICmpSGT(loadDisFlag, minFlagVal, "ci_flag_check");
            auto localLI = &getAnalysis<LoopInfoWrapperPass>(F).getLoopInfo();
            Instruction *ti = llvm::SplitBlockAndInsertIfThen(condition, &*I, false, nullptr, nullptr, localLI);
            ti->getParent()->setName("check_ci_disabled_flag_val");
            Builder.SetInsertPoint(ti);
            Value *decrCnt = Builder.getInt32(1);
            Value *disFlagVal = Builder.CreateSub(loadDisFlag, decrCnt);
            if (!gIsOnlyThreadLocal) {
              Builder.CreateStore(disFlagVal, gLocalFLag[&F]);
            }
            Builder.CreateStore(disFlagVal, clockDisabledFlag);
            instToDel->eraseFromParent();
            /* once the new branch has been instrumented, the inst_iterators will be malformed, & so we start over with a recursive call */
            replaceCIConfigCalls(F);
            break;
          }
        }
      }
    }
  }
#endif

  bool checkIfBackedge(BasicBlock *BB) {

    Loop *L = LI->getLoopFor(BB);
    bool isLatch = false;
    if (L) {
      if (L->isLoopLatch(BB)) {
        isLatch = true;
      }
    }

    bool isBackedge = false;
    SmallVector<std::pair<const BasicBlock *, const BasicBlock *>, 32> Edges;
    FindFunctionBackedges(*(BB->getParent()), Edges);
    for (const auto &Edge : Edges) {
      auto backEdgeBeginBB = Edge.first;
      if (BB == backEdgeBeginBB) {
        isBackedge = true;
        break;
      }
    }

    if (isLatch != isBackedge) {
      errs() << "WARNING: " << BB->getName() << " in function " << BB->getParent()->getName() << "() is a rare backedge. isLatch: " << isLatch << ", isBackedge: " << isBackedge << "\n";
    }

    if (isLatch || isBackedge) {
      return true;
    }

    return false;
  }

#if 1
  typedef struct CDBBCost {
    int _status;                 /* 0 - no call inst fence (use onlyCost), 1 - with call inst fence (use front & back) */
    int _onlyCost;               /* when there is no call instruction */
    int _frontCost;              /* cost before first call inst */
    int _backCost;               /* cost after last call inst */
    Instruction *_firstCallInst; /* first call inst */

    CDBBCost(int status, int onlyCost) : _status(status), _onlyCost(onlyCost), _frontCost(0), _backCost(0), _firstCallInst(nullptr) {
      if (_status != 0) {
        errs() << "Wrong status (1)\n";
        exit(1);
      }
    }

    CDBBCost(int status, int frontCost, int backCost, Instruction *firstInst) : _status(status), _onlyCost(0), _frontCost(frontCost), _backCost(backCost), _firstCallInst(firstInst) {
      if (_status != 1) {
        errs() << "Wrong status (0)\n";
        exit(1);
      }
      if (!firstInst) {
        errs() << "Block's first call instruction not provided!\n";
        exit(1);
      }
    }

    void updateCost(int onlyCost) {
      if (_status != 0) {
        errs() << "Wrong status (1). Cannot set only cost.\n";
        exit(1);
      }
      _onlyCost = onlyCost;
    }

    void updateFrontCost(int frontCost) {
      if (_status != 1) {
        errs() << "Wrong status (1). Cannot set front cost.\n";
        exit(1);
      }
      _frontCost = frontCost;
    }

    void updateBackCost(int backCost) {
      if (_status != 1) {
        errs() << "Wrong status (1). Cannot set back cost.\n";
        exit(1);
      }
      _backCost = backCost;
    }

    int getCost() {
      if (_status != 0) {
        errs() << "Wrong status (1). Cannot return only cost.\n";
        exit(1);
      }
      return _onlyCost;
    }

    int getFrontCost() {
      if (_status != 1) {
        errs() << "Wrong status (1). Cannot return front cost.\n";
        exit(1);
      }
      return _frontCost;
    }

    int getBackCost() {
      if (_status != 1) {
        errs() << "Wrong status (1). Cannot return back cost.\n";
        exit(1);
      }
      return _backCost;
    }

    Instruction *getFirstCallInst() {
      if (_status != 1) {
        errs() << "Wrong status (1). Cannot return first call inst.\n";
        exit(1);
      }
      return _firstCallInst;
    }

    bool hasInst() {
      if (_status) {
        return true;
      } else {
        return false;
      }
    }

  } CDBBCost;

  int getCDBlockFrontCost(BasicBlock *currBB, std::map<BasicBlock *, CDBBCost *> *costMap) {
    auto costIt = costMap->find(currBB);
    assert((costIt != costMap->end()) && "Basic block does not have a cost. This is impossible.");
    CDBBCost *bbCost = costIt->second;
    if (bbCost->hasInst()) {
      return bbCost->getFrontCost();
    } else {
      return bbCost->getCost();
    }
  }

  int getCDBlockBackCost(BasicBlock *currBB, std::map<BasicBlock *, CDBBCost *> *costMap) {
    auto costIt = costMap->find(currBB);
    assert((costIt != costMap->end()) && "Basic block does not have a cost. This is impossible.");
    CDBBCost *bbCost = costIt->second;
    if (bbCost->hasInst()) {
      return bbCost->getBackCost();
    } else {
      return bbCost->getCost();
    }
  }

  void setCDBlockFrontCost(BasicBlock *currBB, std::map<BasicBlock *, CDBBCost *> *costMap, int cost) {
    auto costIt = costMap->find(currBB);
    assert((costIt != costMap->end()) && "Basic block does not have a cost. This is impossible.");
    CDBBCost *bbCost = costIt->second;
    if (bbCost->hasInst()) {
      bbCost->updateFrontCost(cost);
    } else {
      bbCost->updateCost(cost);
    }
  }

  void setCDBlockBackCost(BasicBlock *currBB, std::map<BasicBlock *, CDBBCost *> *costMap, int cost) {
    auto costIt = costMap->find(currBB);
    assert((costIt != costMap->end()) && "Basic block does not have a cost. This is impossible.");
    CDBBCost *bbCost = costIt->second;
    if (bbCost->hasInst()) {
      bbCost->updateBackCost(cost);
    } else {
      bbCost->updateCost(cost);
    }
  }

  void combinePaths(Function *F, std::map<BasicBlock *, CDBBCost *> *costMap) {
    for (auto &BB : *F) {
      BasicBlock *currBB = &BB;
      int currCost = getCDBlockBackCost(currBB, costMap);
      if (currCost == -1) { /* already merged - case should not arise */
        continue;
      }

      bool succIsMergeNode = false;
      for (auto it = succ_begin(currBB), et = succ_end(currBB); it != et; ++it) {
        BasicBlock *succBB = *it;
        if (succBB->getSinglePredecessor()) {
          continue;
        } else {
          succIsMergeNode = true;
          break;
        }
      }

      /* cannot combine if successor is merge node */
      if (succIsMergeNode) {
        continue;
      }

      /* combine costs */
      for (auto it = succ_begin(currBB), et = succ_end(currBB); it != et; ++it) {
        BasicBlock *succBB = *it;
        int succCost = getCDBlockFrontCost(succBB, costMap);
        if (succCost == -1) { /* cannot merge with it - should not arise */
          continue;
        }
        errs() << "Merging path cost of " << currBB->getName() << "(" << currCost << "+" << succCost << ") to successor " << succBB->getName() << "\n";
        succCost += currCost;
        setCDBlockFrontCost(succBB, costMap, succCost);
      }
      setCDBlockBackCost(currBB, costMap, -1); /* won't instrument here */
    }
  }

  bool updateCoredetCosts(Function *F, std::map<BasicBlock *, CDBBCost *> *costMap) {
    for (auto &BB : *F) {
      BasicBlock *currBB = &BB;
      if (currBB->getSinglePredecessor()) { /* not a merge node, CDCommit has been removed earlier */
        continue;
      }

      int currCost = getCDBlockFrontCost(currBB, costMap);
      if (currCost == -1) { /* has already been visited */
        //errs() << "Current merge node cost is -1. This is not expected. Perhaps processing merge nodes in topographical order will help.\n";
        continue;
      }

      int minCost = -1, maxCost = -1, sumCost = 0, numPreds = 0;
      bool predHasBackedge = false, hasSiblings = false;

      for (auto predIt = pred_begin(currBB), predEt = pred_end(currBB); predIt != predEt; ++predIt) {
        BasicBlock *predBB = *predIt;

        /* check backedge */
        if (checkIfBackedge(predBB)) {
          predHasBackedge = true;
          break;
        }

        /* Merge node has siblings from one of its parents, removing CDCommit would mean it has to be effective in every sibling, which needs checking whether they are merge nodes again */
        if (!predBB->getSingleSuccessor()) {
          hasSiblings = true;
          break;
        }

        int predCost = getCDBlockBackCost(predBB, costMap);
        if (predCost == -1) { /* has no cost - might arise when these nodes have been processed in some previous pass */
          //errs() << "Current pred cost for " << predBB->getName() << " of merge node " << currBB->getName() << " (" << predBB->getParent()->getName() << ") is -1. This is not expected. Perhaps processing merge nodes in topographical order will help.\n";
          continue;
          //exit(1);
        }
        numPreds++;

        if (minCost == -1 || maxCost == -1) {
          minCost = predCost;
          maxCost = predCost;
        } else {
          if (minCost > predCost) {
            minCost = predCost;
          }
          if (maxCost < predCost) {
            maxCost = predCost;
          }
        }
        sumCost += predCost;
      }

      if (predHasBackedge || hasSiblings) { /* CDCommit cannot be removed */
        continue;
      }

      if (numPreds) {
        int avgCost = (sumCost / numPreds);
        if (avgCost) {
          avgCost += currCost;
          errs() << "Average cost (including block cost) for block " << currBB->getName() << " : " << avgCost << "(Sum: " << sumCost << ", #Preds: " << numPreds << ")\n";
          if (maxCost - minCost < ALLOWED_DEVIATION) {
            errs() << "Moving cd-commit average cost from predecessors to current " << currBB->getName() << "\n";
            for (auto predIt = pred_begin(currBB), predEt = pred_end(currBB); predIt != predEt; ++predIt) {
              BasicBlock *predBB = *predIt;
              setCDBlockBackCost(predBB, costMap, -1); /* won't instrument here */
              errs() << "Pred: " << predBB->getName() << "\n";
            }
            setCDBlockFrontCost(currBB, costMap, avgCost);
            return true;
          }
        }
      }
    }
    return false;
  }

  /* Instruments heuristic balance of Coredet */
  void instrumentCoredet(Module &M) {
    errs() << "Instrumenting for CoreDet\n";
    for (auto &F : M) {
      if (F.isDeclaration()) {
        continue;
      }
      if (isRestrictedFunction(&F)) {
        continue;
      }

#if 0
      if (F.getName().compare("traverse_subtree") != 0)
        continue;
#endif
      if (!gIsOnlyThreadLocal) {
        /* Create local variables for loading & storing the thread local counter & flag */
        initializeLocals(&F);
      }

      LI = &getAnalysis<LoopInfoWrapperPass>(F).getLoopInfo();
      std::map<BasicBlock *, CDBBCost *> *costMap = new std::map<BasicBlock *, CDBBCost *>();
      std::map<Instruction *, int> *instMap = new std::map<Instruction *, int>();
      errs() << "************************ Analyzing for " << F.getName() << "************************\n";

      /* initialize cost of every block */
      for (auto &B : F) {
        //int instCost = std::distance(B.begin(), B.end());
        int instCost = 0;
        bool fence = false;
        int frontCost = 0;
        Instruction *firstCallInst = nullptr;

        for (auto &I : B) {
          if (!isa<PHINode>(&I)) {
            if (isa<LoadInst>(I) || isa<StoreInst>(I)) {
              instCost += MemOpsCost;
            } else if (isa<CallInst>(&I)) {
              if (checkIfExternalLibraryCall(&I)) {
                instCost += ExtLibFuncCost;
              } else if (isa<DbgInfoIntrinsic>(I)) {
                instCost++;
              } else {
                instCost++;

                if (frontCost == 0) {
                  frontCost = instCost;
                  firstCallInst = &I;
                } else {
                  (*instMap)[&I] = instCost; // for all intermediate calls
                }

                instCost = 0;
                fence = true;
              }
            } else {
              instCost++;
            }
          }
        }
        /* instMap, at this point, contains all function calls that will be instrumented, i.e. all internal calls except the first call of the block */

        if (!fence) {
          errs() << "Block cost for " << B.getName() << " : " << instCost << "\n";
          (*costMap)[&B] = new CDBBCost(0, instCost);
        } else {
          errs() << "Block cost for " << B.getName() << " : front(" << frontCost << "), back(" << instCost << ")\n\tFirst call inst: " << *firstCallInst << "\n";
          (*costMap)[&B] = new CDBBCost(1, frontCost, instCost, firstCallInst);
        }
      }

      bool changed = false;
      int passes = 0;
      combinePaths(&F, costMap);
      do {
        changed = updateCoredetCosts(&F, costMap);
        passes++;
      } while (changed);
      errs() << "Function " << F.getName() << ": Coredet analysis converged in " << passes << " passes\n";

      /* Export the costs to the InstMap */
      for (auto it = costMap->begin(); it != costMap->end(); it++) {
        BasicBlock *BB = it->first;
        CDBBCost *bbCost = it->second;
        if (bbCost->hasInst()) {
          Instruction *firstInst = bbCost->getFirstCallInst();
          Instruction *lastInst = &(BB->back());
          int frontCost = bbCost->getFrontCost();
          int backCost = bbCost->getBackCost();

          /* if front & back costs are available, add instrumentation for first call & last instruction of the block */
          auto costIt = instMap->find(firstInst);
          assert((costIt == instMap->end()) && "First call instruction already has a cost. This is impossible.");
          if (frontCost != -1) {
            (*instMap)[firstInst] = frontCost;
          }

          costIt = instMap->find(lastInst);
          assert((costIt == instMap->end()) && "Last block instruction already has a cost. This is impossible.");
          if (backCost != -1) {
            (*instMap)[lastInst] = backCost;
          }
        } else {
          Instruction *lastInst = &(BB->back());
          int blockCost = bbCost->getCost();

          /* if block cost is available, add instrumentation to the last instruction of the block */
          auto costIt = instMap->find(lastInst);
          assert((costIt == instMap->end()) && "Last block instruction already has a cost. This is impossible.");
          if (blockCost != -1) {
            (*instMap)[lastInst] = blockCost;
          }
        }
      }

      // instrumentCosts
      errs() << "************************ Instrumenting for " << F.getName() << "************************\n";
#if 1
      /* Only for debug prints */
      for (auto it = instMap->begin(); it != instMap->end(); it++) {
        Instruction *I = it->first;
        int instCount = it->second;
        if (instCount != -1) {
          errs() << "Instrumenting cost " << instCount << " for basic block " << I->getParent()->getName() << "\n";
        }
      }
#endif
      for (auto it = instMap->begin(); it != instMap->end(); it++) {
        Instruction *I = it->first;
        int instCount = it->second;
        if (instCount != -1) {
          IRBuilder<> Builder(I);
          Value *val = Builder.getInt64(instCount);
          instrumentIfLCEnabled(I, ALL_IR, val);
        }
      }

      if (!gIsOnlyThreadLocal) {
        /* instrument locals */
        instrumentLocals(&F);
      }

      delete costMap;
    }
  }
#endif

  /* Instruments all function calls & backedges 
     * NOTE: Try to heuristically figure out the best weights that makes it match the accuracy of the opt clock 
     * For now, external lib call cost = ExtLibFuncCost, backedge & rest call cost = 1 */
  void instrumentLegacy(Module &M) {
    errs() << "Instrumenting for legacy\n";

    for (auto &F : M) {

      if (F.isDeclaration()) {
        continue;
      }
      if (isRestrictedFunction(&F)) {
        continue;
      }

      LI = &getAnalysis<LoopInfoWrapperPass>(F).getLoopInfo();

      /* Create local variables for loading & storing the thread local counter & flag */
      initializeLocals(&F);

      std::map<Instruction *, int> costMap;
      for (auto &B : F) {

        /* instrument backedges */
        if (checkIfBackedge(&B)) {
          Instruction *I = B.getTerminator();
          costMap[I] = 1;
          //errs() << F.getName() << "(): Instrumenting Backedge basic block " << B.getName() << "\n";
        }

        /* instrument function calls */
        //errs() << "Func: " << F.getName() << ", BB: " << B.getName() << "\n";
        for (auto &I : B) {
          if (isa<CallInst>(&I)) {
            if (isa<DbgInfoIntrinsic>(&I)) {
              continue;
            } else if (checkIfExternalLibraryCall(&I)) {
              costMap[&I] = ExtLibFuncCost;
            } else {
              costMap[&I] = 1;
            }
          }
        }
      }

      //errs() << "Starting legacy instrumentation\n";
      for (auto it = costMap.begin(); it != costMap.end(); it++) {
        Instruction *I = it->first;
        int instCount = it->second;
        if (instCount) {
          IRBuilder<> Builder(I);
          Value *val = Builder.getInt64(instCount);
          instrumentIfLCEnabled(I, ALL_IR, val);
        }
      }

      instrumentLocals(&F);
    }
  }

  void initializeLastCycleTL(Module &M) {
    errs() << "Created LastCycleTS\n";
    auto initVal = llvm::ConstantInt::get(M.getContext(), llvm::APInt(64, 0, false));
    GlobalVariable *cycle = new GlobalVariable(M, Type::getInt64Ty(M.getContext()), false, GlobalValue::ExternalLinkage, 0, "LastCycleTS");
    cycle->setThreadLocalMode(GlobalValue::GeneralDynamicTLSModel);
    if (DefineClock) {
      cycle->setInitializer(initVal);
    }

#if 0
    Function *mainF = M.getFunction("main");
    BasicBlock *enBB = &(mainF->getEntryBlock());
    Instruction *I = enBB->getFirstNonPHIOrDbg();
    IRBuilder<> IR(I);
    CallInst *latestcycle = IR.CreateIntrinsic(Intrinsic::readcyclecounter, {}, {});
    IR.CreateStore(latestcycle, cycle);
#endif
  }

  /* Instruments all function calls & backedges */
  void instrumentLegacyAccurate(Module &M) {
    errs() << "Instrumenting for accurate legacy\n";
    gIsOnlyThreadLocal = true;
    initializeLastCycleTL(M);

    for (auto &F : M) {

      if (F.isDeclaration()) {
        continue;
      }
      if (isRestrictedFunction(&F)) {
        continue;
      }

      LI = &getAnalysis<LoopInfoWrapperPass>(F).getLoopInfo();

      std::vector<Instruction *> costMap;
      for (auto &B : F) {
        /* instrument backedges */
        if (checkIfBackedge(&B)) {
          Instruction *I = B.getTerminator();
          costMap.push_back(I);
          //errs() << F.getName() << "(): Instrumenting Backedge basic block " << B.getName() << "\n";
        }

        /* instrument function calls */
        //errs() << "Func: " << F.getName() << ", BB: " << B.getName() << "\n";
        for (auto &I : B) {
          if (isa<CallInst>(&I)) {
            if (isa<DbgInfoIntrinsic>(&I)) {
              continue;
            }
            costMap.push_back(&I);
          }
        }
      }

      //errs() << "Starting legacy instrumentation\n";
      for (auto I : costMap) {
        instrumentIfLCEnabled(I, INCR_ON_CYCLES);
      }
    }
  }

  /* CompilerInterrupt Pass function */
  bool runOnModule(Module &M) override {
    gIsOnlyThreadLocal = false;
    int numFunctions = 0;

    assert((Configuration == 2) && "Only multi threaded thread lock configuration is supported\n");
    //assert((FiberConfig==5) && "Only fiber config 5 is allowed. It does not have any meaning now, but apps using something else, need to be double checked.\n");

    //#ifdef CRNT_DEBUG
    float thresh_perc = (float)FiberConfig / 100;
    errs() << "Fiber config " << thresh_perc << " not used anymore\n";
#if 1
    if (ClockType == PREDICTIVE) {
      errs() << "********************** Clock Type: Predictive";
    } else if (ClockType == INSTANTANEOUS) {
      errs() << "******************** Clock Type: Instantaneous";
    } else {
      errs() << "Invalid clock type!";
      exit(1);
    }

    switch (InstGranularity) {
    case OPTIMIZE_HEURISTIC:
      errs() << ", Instrumentation Granularity : Optimized (Local Var configuration is deprecated) **********************\n";
      exit(1);
      break;
    case OPTIMIZE_HEURISTIC_WITH_TL:
      errs() << ", Instrumentation Granularity : Optimized with Thread Local *********************\n";
      break;
    case OPTIMIZE_HEURISTIC_FIBER:
      errs() << ", Instrumentation Granularity : Optimized with Thread Local for Fiber, without disabling interrupts *********************\n";
      break;
    case NAIVE:
      errs() << ", Instrumentation Granularity : Naive **********************\n";
      exit(1);
      break;
    case NAIVE_TL:
      errs() << ", Instrumentation Granularity : Naive with Thread Local **********************\n";
      break;
    case LEGACY_HEURISTIC:
      exit(1);
      errs() << ", Instrumentation Granularity : Legacy (Backedge & function calls) **********************\n";
      break;
    case LEGACY_HEURISTIC_TL:
      errs() << ", Instrumentation Granularity : Legacy with Thread Local (Backedge & function calls) **********************\n";
      break;
    case COREDET_HEURISTIC_TL:
      errs() << ", Instrumentation Granularity : Coredet TL **********************\n";
      break;
    case COREDET_HEURISTIC:
      exit(1);
      errs() << ", Instrumentation Granularity : Coredet Local Var **********************\n";
      break;
    case LEGACY_ACCURATE:
      errs() << ", Instrumentation Granularity : Legacy using readcycles **********************\n";
      break;
    case OPTIMIZE_ACCURATE:
      errs() << ", Instrumentation Granularity : Opt accurate **********************\n";
      break;
    case NAIVE_ACCURATE:
      errs() << ", Instrumentation Granularity : Naive accurate **********************\n";
      break;
    case OPTIMIZE_INTERMEDIATE:
      errs() << ", Instrumentation Granularity : Opt intermediate **********************\n";
      break;
    case OPTIMIZE_CYCLES:
      errs() << ", Instrumentation Granularity : Opt cycles **********************\n";
      break;
    case OPTIMIZE_HEURISTIC_INTERMEDIATE_FIBER:
      errs() << ", Instrumentation Granularity : Opt intermediate for Fiber, without disabling interrupts **********************\n";
      break;
    case NAIVE_INTERMEDIATE:
      errs() << ", Instrumentation Granularity : Naive intermediate **********************\n";
      break;
    case NAIVE_HEURISTIC_FIBER:
      errs() << ", Instrumentation Granularity : Naive TL Fiber **********************\n";
      break;
    case NAIVE_CYCLES:
      errs() << ", Instrumentation Granularity : Naive cycles **********************\n";
      break;
    default:
      errs() << "**********************\nUnsupported Instrumentation Granularity (" << InstGranularity << ")!\n";
      exit(1);
    }

    errs() << "Running with configuration:\nPI: " << TargetInterval << ", CI: " << CommitInterval << ", Allowed Dev: " << ALLOWED_DEVIATION << ", Lib call cost: " << ExtLibFuncCost << ", Target Cycle: " << TargetIntervalInCycles << "\n";
#else
    if ((ClockType != PREDICTIVE) && (ClockType != INSTANTANEOUS)) {
      errs() << "Invalid clock type!";
      exit(1);
    }
    if (InstGranularity > NAIVE_INTERMEDIATE) {
      errs() << "**********************\nUnsupported Instrumentation Granularity (" << InstGranularity << ")!\n";
      exit(1);
    }
#endif

    errs() << "Target Interval in IR " << TargetInterval << ", in Cycles " << TargetIntervalInCycles << ", Commit Interval " << CommitInterval << "\n";
    //errs() << "Interval Exceeding Action configuration: " << FiberConfig << ", Push Interval: " << TargetInterval << "\n";

#if 0
    if (!readConfig()) {
      errs() << "Error reading configuration file\n";
      assert("Unable to read configuration file\n");
      exit(1); // in case assertions are not enabled
    }
#endif

    /* Function cost optimization & export/import is only available for all opt configurations */
    if (checkIfInstGranIsOpt()) {
      /* Check & read instruction weight configuration file */
      if (!readCost()) {
        assert("Unable to library's cost configuration file\n");
        errs() << "Error reading library's cost configuration file";
        return false;
      }
    }

#ifdef PROFILING
    /* Declare/define compiler interrupt printing function */
    if (DefineClock) {
      createPrintFuncDefn(M);
    } else {
      createPrintFuncDecl(M);
    }
#endif

#ifdef ALL_DEBUG
    /* Prints library function costs */
    for (auto lfc : libraryInstructionCosts) {
      errs() << "Cost of " << lfc.first << ":" << lfc.second->_value << "\n";
    }
#if 0
    /* Prints Dominator & Post Dominator Trees */
    for (auto &F : M) {
      if (!F.isDeclaration()) {
        DominatorTree &DT = getAnalysis<DominatorTreeWrapperPass>(F).getDomTree();
        PostDominatorTree &PDT = getAnalysis<PostDominatorTreeWrapperPass>(F).getPostDomTree();
        errs() << "Printing Dominator Tree\n";
        DT.print(errs());
        errs() << "Printing Post Dominator Tree\n";
        PDT.print(errs());
      }
    }
#endif
#endif

    if (ClockType == PREDICTIVE) {
      /* Define the fences */
      fenceList.insert("pthread_mutex_lock");
      fenceList.insert("pthread_mutex_unlock");
    }

    /* Find all functions that start a thread */
    findThreadFunc(M);

    /* Find all functions that start a thread */
    findCIfunctions(M);

    /* Populate the list of functions in module in call graph order */
    getCallGraphOrder();

#if 0
    /* Find all functions that are involved in anything other than call statements (most likely it is a pointer). Must be called after findThreadFunc() & getCallGraphOrder() have been called */
    findFuncWithPointers(M);
#endif

#if 0
    for (auto &F : M) {
      errs() << "Calling checkNTransformLoops for " << F.getName() << "\n";
      if (F.isDeclaration())
        continue;
      //if(F.getName().compare("map_reduce") == 0) {
      DT = &getAnalysis<DominatorTreeWrapperPass>(F).getDomTree();
      LI = &getAnalysis<LoopInfoWrapperPass>(F).getLoopInfo();
      SE = &getAnalysis<ScalarEvolutionWrapperPass>(F).getSE();
      MSSA = &getAnalysis<MemorySSAWrapperPass>(F).getMSSA();
      profile_loops(&F); /* get statistics for loops */
                         //checkNTransformLoops(&F); /* get statistics for loops */
                         //return true;
      //}
    }

    errs() << "#loops: " << g_NumLoops << "\n";
    errs() << "#self loops: " << g_NumSelfLoops << "\n";
    errs() << "#self loops with induction variables: " << g_NumSelfLoopsWithCanIndVar << "\n";
    errs() << "#loops with induction variables: " << g_NumIndVar << "\n";
    errs() << "#loops with canonical induction variables: " << g_NumCanIndVar << "\n";
    errs() << "#loops with numeric backedges: " << g_NumIterations << "\n";

    return true;
#endif

#if 0
    for (auto &F : M) {
      if (F.isDeclaration())
        continue;
      if (F.getName().compare("linear_regression_map") == 0) {
        errs() << "Calling useLoopUnrollPasses for " << F.getName() << "\n";
        DT = &getAnalysis<DominatorTreeWrapperPass>(F).getDomTree();
        LI = &getAnalysis<LoopInfoWrapperPass>(F).getLoopInfo();
        SE = &getAnalysis<ScalarEvolutionWrapperPass>(F).getSE();
        /* only for debugging */
        useLoopUnrollPasses(&F);
        break;
      }
    }
    return true;
#endif

    //findAllLibraryCalls(M);
    //return true;

    /* Initial instrumentations - must be done after callgraph traversal */
    initializeInstrumentation(M);

    switch (InstGranularity) {

    case NAIVE_TL:
    case NAIVE_INTERMEDIATE:
    case NAIVE_CYCLES:
    case NAIVE_HEURISTIC_FIBER: {
      //assert((TargetInterval>=TargetIntervalInCycles) && "IR interval should be greater than or equal to Cycle interval");
      gIsOnlyThreadLocal = true;
      instrumentAllBlocks(M);
      goto finishing_tasks;
      //return true;
    }

    case NAIVE_ACCURATE: {
      errs() << "Running Naive Accurate Clock\n";
      //assert((TargetInterval>=TargetIntervalInCycles) && "IR interval should be greater than or equal to Cycle interval");
      gIsOnlyThreadLocal = true;
      gUseReadCycles = true;
      instrumentAllBlocks(M);
      goto finishing_tasks;
      //return true;
    }

    case NAIVE: {
      gIsOnlyThreadLocal = false;
      instrumentAllBlocks(M);
      goto finishing_tasks;
      //return true;
    }

    case LEGACY_HEURISTIC: {
      gIsOnlyThreadLocal = false;
      instrumentLegacy(M);
      goto finishing_tasks;
      //return true;
    }

    case LEGACY_HEURISTIC_TL: {
      gIsOnlyThreadLocal = true;
      instrumentLegacy(M);
      goto finishing_tasks;
      //return true;
    }

    case LEGACY_ACCURATE: {
      gIsOnlyThreadLocal = true;
      instrumentLegacyAccurate(M);
      goto finishing_tasks;
      //return true;
    }

    case COREDET_HEURISTIC_TL: {
      gIsOnlyThreadLocal = true;
      instrumentCoredet(M);
      goto finishing_tasks;
      //return true;
    }

    case COREDET_HEURISTIC: {
      gIsOnlyThreadLocal = false;
      instrumentCoredet(M);
      goto finishing_tasks;
      //return true;
    }

#if 0
    case OPTIMIZE_HEURISTIC: {
      gIsOnlyThreadLocal = true;
      break;
    }
#endif
    case OPTIMIZE_HEURISTIC_WITH_TL: {
      gIsOnlyThreadLocal = true;
      break;
    }

    case OPTIMIZE_HEURISTIC_FIBER: {
      errs() << "Running Opt CI for fiber\n";
      gIsOnlyThreadLocal = true;
      break;
    }

    case OPTIMIZE_ACCURATE: {
      gIsOnlyThreadLocal = true;
      gUseReadCycles = true;
      //assert((TargetInterval>=TargetIntervalInCycles) && "IR interval should be greater than or equal to Cycle interval");
      break;
    }

    case OPTIMIZE_INTERMEDIATE:
    case OPTIMIZE_HEURISTIC_INTERMEDIATE_FIBER:
    case OPTIMIZE_CYCLES: {
      //assert((TargetInterval>=TargetIntervalInCycles) && "IR interval should be greater than or equal to Cycle interval");
      initializeLastCycleTL(M);
      gIsOnlyThreadLocal = true;
      break;
    }

    default:
      errs() << "Instruction Granularity " << InstGranularity << " is not valid.";
      exit(1);
    }

#ifdef CRNT_DEBUG
    errs() << "EVALUATION-PASS (in Callgraph order)\n";
#endif

    for (auto funcInfo : CGOrderedFunc) {
      Function *F = M.getFunction(funcInfo.first);
      //if(F->getName().compare("traverse_subtree")!=0) continue;
      if (F) {
        numFunctions++;
        if (!isRestrictedFunction(F)) {
#ifdef CRNT_DEBUG
          errs() << "\n\n/=========================== ANALYZE FOR " << F->getName() << " =============================/\n";
#endif
#ifdef ALL_DEBUG
          /* Testing */
          errs() << "Arguments of function " << F->getName() << " are:- \n";
          for (auto &arg : F->args()) {
            errs() << arg;
            if (!SE->isSCEVable(arg.getType()))
              errs() << " is not scevable\n";
            else
              errs() << " is scevable \n";
          }
#endif
          /* Analyze & instrument */
          analyzeAndInstrFunc(*F);
        }
      }
    }

    /* if a library is analysed, export its costs for use by its applications */
    if (checkIfInstGranIsOpt()) {
      writeCost(M);
    }

  finishing_tasks:
#ifdef PROFILING
    /* Print total statistics */
    errs() << "Total functions: " << numFunctions << "\n";
    errs() << "Total uninstrumented functions: " << numUninstrumentedFunc << "\n";
    printStats(nullptr);
    createPrintCalls(M);
    errs() << "#Total optimization of function costs: " << func_opts << "\n";
    /* Branches are transformed to match rules */
    errs() << "#Total preprocessing: " << preprocessing << "\n";
#endif

    /* returning true signifies that the code has been modified */
    return true;
  }
};
} // namespace

namespace llvm {
Pass *createCompilerInterruptPass() {
  return new CompilerInterrupt();
}
} // namespace llvm

// register the pass to load with opt
char CompilerInterrupt::ID = 0;
static RegisterPass<CompilerInterrupt> X("compilerinterrupt", "Compiler Interrupt Pass", true, false);