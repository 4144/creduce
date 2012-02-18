#include "ReducePointerLevel.h"

#include <sstream>

#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"

#include "RewriteUtils.h"
#include "TransformationManager.h"

using namespace clang;
using namespace llvm;

static const char *DescriptionMsg =
"Reduce pointer indirect level for a global/local variable. \
All valid variables are sorted by their indirect levels. \
The pass will ensure to first choose a valid variable \
with the largest indirect level. This mechanism could \
reduce the complexity of our implementation, because \
we don't have to consider the case where the chosen variable \
with the largest indirect level would be address-taken. \
Variables at non-largest-indirect-level are ineligible \
if they: \n\
  * being address-taken \n\
  * OR being used as LHS in any pointer form, e.g., \n\
    p, *p(assume *p is of pointer type), \n\
    while the RHS is NOT a UnaryOperator. \n";

static RegisterTransformation<ReducePointerLevel>
         Trans("reduce-pointer-level", DescriptionMsg);

class PointerLevelCollectionVisitor : public 
  RecursiveASTVisitor<PointerLevelCollectionVisitor> {

public:

  explicit PointerLevelCollectionVisitor(ReducePointerLevel *Instance)
    : ConsumerInstance(Instance)
  { }

  bool VisitDeclaratorDecl(DeclaratorDecl *DD);

  bool VisitUnaryOperator(UnaryOperator *UO);

  bool VisitBinaryOperator(BinaryOperator *BO);

private:

  ReducePointerLevel *ConsumerInstance;

  int getPointerIndirectLevel(const Type *Ty);

  bool isVAArgField(DeclaratorDecl *DD);

};

class PointerLevelRewriteVisitor : public 
  RecursiveASTVisitor<PointerLevelRewriteVisitor> {

public:

  explicit PointerLevelRewriteVisitor(ReducePointerLevel *Instance)
    : ConsumerInstance(Instance)
  { }

  bool VisitVarDecl(VarDecl *VD);

  bool VisitFieldDecl(FieldDecl *FD);

  bool VisitUnaryOperator(UnaryOperator *UO);

  bool VisitDeclRefExpr(DeclRefExpr *DRE);

  bool VisitMemberExpr(MemberExpr *ME);

  bool VisitArraySubscriptExpr(ArraySubscriptExpr *ASE);

private:

  ReducePointerLevel *ConsumerInstance;

};

int PointerLevelCollectionVisitor::getPointerIndirectLevel(const Type *Ty)
{
  int IndirectLevel = 0;
  QualType QT = Ty->getPointeeType();;
  while (!QT.isNull()) {
    IndirectLevel++;
    QT = QT.getTypePtr()->getPointeeType();
  }
  return IndirectLevel;
}
 
// Any better way to ginore these two fields 
// coming from __builtin_va_arg ?
bool PointerLevelCollectionVisitor::isVAArgField(DeclaratorDecl *DD)
{
  std::string Name = DD->getNameAsString();
  return (!Name.compare("reg_save_area") || 
          !Name.compare("overflow_arg_area"));
}

// I skipped IndirectFieldDecl for now
bool PointerLevelCollectionVisitor::VisitDeclaratorDecl(DeclaratorDecl *DD)
{
  if (isVAArgField(DD))
    return true;

  // Only consider FieldDecl and VarDecl
  Decl::Kind K = DD->getKind();
  if (!(K == Decl::Field) && !(K == Decl::Var))
    return true;

  const Type *Ty = DD->getType().getTypePtr();
  const ArrayType *ArrayTy = dyn_cast<ArrayType>(Ty);
  if (ArrayTy)
    Ty = ConsumerInstance->getArrayBaseElemType(ArrayTy);
  if (!Ty->isPointerType())
    return true;

  DeclaratorDecl *CanonicalDD = dyn_cast<DeclaratorDecl>(DD->getCanonicalDecl());
  TransAssert(CanonicalDD && "Bad DeclaratorDecl!");
  if (ConsumerInstance->VisitedDecls.count(CanonicalDD))
    return true;

  ConsumerInstance->ValidDecls.insert(CanonicalDD);
  ConsumerInstance->VisitedDecls.insert(CanonicalDD);
  int IndirectLevel = getPointerIndirectLevel(Ty);
  TransAssert((IndirectLevel > 0) && "Bad indirect level!");
  if (IndirectLevel > ConsumerInstance->MaxIndirectLevel)
    ConsumerInstance->MaxIndirectLevel = IndirectLevel;

  ConsumerInstance->addOneDecl(CanonicalDD, IndirectLevel);
  return true;
}

bool PointerLevelCollectionVisitor::VisitUnaryOperator(UnaryOperator *UO)
{
  if (UO->getOpcode() != UO_AddrOf)
    return true;

  const Expr *SubE = UO->getSubExpr()->IgnoreParenCasts();
  if (!dyn_cast<DeclRefExpr>(SubE) && !dyn_cast<MemberExpr>(SubE))
    return true;

  const DeclaratorDecl *DD = 
    ConsumerInstance->getCanonicalDeclaratorDecl(SubE);
  TransAssert(DD && "NULL DD!");

  ConsumerInstance->AddrTakenDecls.insert(DD);
  return true;
}

bool PointerLevelCollectionVisitor::VisitBinaryOperator(BinaryOperator *BO)
{
  if (!BO->isAssignmentOp() && !BO->isCompoundAssignmentOp())
    return true;

  Expr *Lhs = BO->getLHS();
  const Type *Ty = Lhs->getType().getTypePtr();
  if (!Ty->isPointerType())
    return true;

  Expr *Rhs = BO->getRHS()->IgnoreParenCasts();
  if (dyn_cast<DeclRefExpr>(Rhs) || 
      dyn_cast<UnaryOperator>(Rhs) ||
      dyn_cast<ArraySubscriptExpr>(Rhs))
    return true;

  const DeclaratorDecl *DD = ConsumerInstance->getRefDecl(Lhs);
  TransAssert(DD && "NULL DD!");

  ConsumerInstance->ValidDecls.erase(DD);
  return true;
}

bool PointerLevelRewriteVisitor::VisitFieldDecl(FieldDecl *FD)
{
  const FieldDecl *TheFD = dyn_cast<FieldDecl>(ConsumerInstance->TheDecl);
  // TheDecl is a VarDecl
  if (!TheFD)
    return true;

  const FieldDecl *CanonicalFD = dyn_cast<FieldDecl>(FD->getCanonicalDecl());
  if (CanonicalFD == TheFD)
    ConsumerInstance->rewriteFieldDecl(FD);

  return true;
}

bool PointerLevelRewriteVisitor::VisitVarDecl(VarDecl *VD)
{
  const VarDecl *TheVD = dyn_cast<VarDecl>(ConsumerInstance->TheDecl);
  if (TheVD) {
    const VarDecl *CanonicalVD = VD->getCanonicalDecl();
    if (CanonicalVD == TheVD) {
      ConsumerInstance->rewriteVarDecl(VD);
      return true;
    }
  }

  // TheDecl is a FieldDecl. 
  // But we still need to handle VarDecls which are type of 
  // struct/union where TheField could reside, if these VarDecls
  // have initializers

  if (!VD->hasInit())
    return true;

  const Type *VDTy = VD->getType().getTypePtr();
  if (!VDTy->isAggregateType())
    return true;

  const ArrayType *ArrayTy = dyn_cast<ArrayType>(VDTy);
  if (ArrayTy) {
    const Type *ArrayElemTy = ConsumerInstance->getArrayBaseElemType(ArrayTy);
    if (!ArrayElemTy->isStructureType() && !ArrayElemTy->isUnionType())
      return true;
    const RecordType *RDTy = dyn_cast<RecordType>(ArrayElemTy);
    TransAssert(RDTy && "Bad RDTy!");
    const RecordDecl *RD = RDTy->getDecl();
    ConsumerInstance->rewriteArrayInit(RD, VD->getInit());
    return true;
  }

  const RecordType *RDTy = dyn_cast<RecordType>(VDTy);
  const RecordDecl *RD = RDTy->getDecl();
  ConsumerInstance->rewriteRecordInit(RD, VD->getInit());
  return true;
}

bool PointerLevelRewriteVisitor::VisitUnaryOperator(UnaryOperator *UO)
{
  // TODO
  return true;
}

bool PointerLevelRewriteVisitor::VisitDeclRefExpr(DeclRefExpr *DRE)
{
  // TODO
  return true;
}

bool PointerLevelRewriteVisitor::VisitMemberExpr(MemberExpr *ME)
{
  // TODO
  return true;
}

bool PointerLevelRewriteVisitor::VisitArraySubscriptExpr(ArraySubscriptExpr *ASE)
{
  // TODO
  return true;
}

void ReducePointerLevel::Initialize(ASTContext &context) 
{
  Context = &context;
  SrcManager = &Context->getSourceManager();
  CollectionVisitor = new PointerLevelCollectionVisitor(this);
  RewriteVisitor = new PointerLevelRewriteVisitor(this);
  TheRewriter.setSourceMgr(Context->getSourceManager(), 
                           Context->getLangOptions());
}

void ReducePointerLevel::HandleTopLevelDecl(DeclGroupRef D) 
{
  for (DeclGroupRef::iterator I = D.begin(), E = D.end(); I != E; ++I) {
    CollectionVisitor->TraverseDecl(*I);
  }
}
 
void ReducePointerLevel::HandleTranslationUnit(ASTContext &Ctx)
{
  doAnalysis();
  
  if (QueryInstanceOnly)
    return;

  if (TransformationCounter > ValidInstanceNum) {
    TransError = TransMaxInstanceError;
    return;
  }

  TransAssert(CollectionVisitor && "NULL CollectionVisitor!");
  TransAssert(RewriteVisitor && "NULL CollectionVisitor!");
  Ctx.getDiagnostics().setSuppressAllDiagnostics(false);
  TransAssert(TheDecl && "NULL TheDecl!");

  setRecordDecl();
  RewriteVisitor->TraverseDecl(Ctx.getTranslationUnitDecl());

  if (Ctx.getDiagnostics().hasErrorOccurred() ||
      Ctx.getDiagnostics().hasFatalErrorOccurred())
    TransError = TransInternalError;
}

void ReducePointerLevel::doAnalysis(void)
{
  DeclSet *Decls;

  Decls = AllPtrDecls[MaxIndirectLevel];
  if (Decls) {
    for (DeclSet::const_iterator I = Decls->begin(), E = Decls->end();
         I != E; ++I) {
      if (!ValidDecls.count(*I))
        continue;
      ValidInstanceNum++;
      if (TransformationCounter == ValidInstanceNum)
        TheDecl = *I;
    }
  }

  for (int Idx = MaxIndirectLevel - 1; Idx > 0; --Idx) {
    Decls = AllPtrDecls[Idx];
    if (!Decls)
      continue;

    for (DeclSet::const_iterator I = Decls->begin(), E = Decls->end();
         I != E; ++I) {
      if (!ValidDecls.count(*I) || AddrTakenDecls.count(*I))
        continue;
      ValidInstanceNum++;
      if (TransformationCounter == ValidInstanceNum)
        TheDecl = *I;
    }
  }
}

void ReducePointerLevel::setRecordDecl(void)
{
  const FieldDecl *TheFD = dyn_cast<FieldDecl>(TheDecl);
  if (!TheFD)
    return;

  TheRecordDecl = TheFD->getParent();
}

const Expr *
ReducePointerLevel::ignoreSubscriptExprParenCasts(const Expr *E)
{
  const Expr *NewE = E->IgnoreParenCasts();
  const ArraySubscriptExpr *ASE;
  while (true) {
    ASE = dyn_cast<ArraySubscriptExpr>(NewE);
    if (!ASE)
      break;
    NewE = ASE->getBase()->IgnoreParenCasts();
  }
  TransAssert(NewE && "NULL NewE!");
  return NewE;
}

const DeclaratorDecl *ReducePointerLevel::getRefDecl(const Expr *Exp)
{
  const Expr *E = 
    ignoreSubscriptExprParenCasts(Exp);
  const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E);

  if (DRE)
    return getCanonicalDeclaratorDecl(DRE);

  const MemberExpr *ME = dyn_cast<MemberExpr>(E);
  if (ME)
    return getCanonicalDeclaratorDecl(ME);

  const UnaryOperator *UE = dyn_cast<UnaryOperator>(E);
  TransAssert(UE && "Bad UnaryOperator!");
  TransAssert((UE->getOpcode() == UO_Deref) && "Non-Deref Opcode!");
  const Expr *SubE = UE->getSubExpr();
  return getRefDecl(SubE);
}

void ReducePointerLevel::addOneDecl(const DeclaratorDecl *DD, 
                                    int IndirectLevel)
{
  DeclSet *DDSet = AllPtrDecls[IndirectLevel];
  if (!DDSet) {
    DDSet = new DeclSet::SmallPtrSet();
    AllPtrDecls[IndirectLevel] = DDSet;
  }
  DDSet->insert(DD);
}

const DeclaratorDecl *
ReducePointerLevel::getCanonicalDeclaratorDecl(const Expr *E)
{
  const DeclaratorDecl *DD;
  const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E);
  const MemberExpr *ME = dyn_cast<MemberExpr>(E);

  if (DRE) {
    const ValueDecl *ValueD = DRE->getDecl();
    DD = dyn_cast<DeclaratorDecl>(ValueD);
    TransAssert(DD && "Bad Declarator!"); 
  }
  else if (ME) {
    ValueDecl *OrigDecl = ME->getMemberDecl();
    FieldDecl *FD = dyn_cast<FieldDecl>(OrigDecl);

    // in C++, getMemberDecl returns a CXXMethodDecl.
    TransAssert(FD && "Unsupported C++ getMemberDecl!\n");
    DD = dyn_cast<DeclaratorDecl>(OrigDecl);
  }
  else {
    TransAssert(0 && "Bad Decl!");
  }

  const DeclaratorDecl *CanonicalDD = 
    dyn_cast<DeclaratorDecl>(DD->getCanonicalDecl());
  TransAssert(CanonicalDD && "NULL CanonicalDD!");
  return CanonicalDD;
}

const Type *ReducePointerLevel::getArrayBaseElemType(const ArrayType *ArrayTy)
{
  const Type *ArrayElemTy = ArrayTy->getElementType().getTypePtr();
  while (ArrayElemTy->isArrayType()) {
    const ArrayType *AT = dyn_cast<ArrayType>(ArrayElemTy);
    ArrayElemTy = AT->getElementType().getTypePtr();
  }
  TransAssert(ArrayElemTy && "Bad Array Element Type!");
  return ArrayElemTy;
}

void ReducePointerLevel::rewriteVarDecl(const VarDecl *VD)
{
  // TODO 
}

void ReducePointerLevel::rewriteFieldDecl(const FieldDecl *FD)
{
  // TODO 
}

void ReducePointerLevel::rewriteRecordInit(const RecordDecl *RD, 
                                           const Expr *Init)
{
  // TODO  
}

void ReducePointerLevel::rewriteArrayInit(const RecordDecl *RD, 
                                          const Expr *Init)
{
  // TODO  
}

ReducePointerLevel::~ReducePointerLevel(void)
{
  if (CollectionVisitor)
    delete CollectionVisitor;
  if (RewriteVisitor)
    delete RewriteVisitor;

  for (LevelToDeclMap::iterator I = AllPtrDecls.begin(), 
       E = AllPtrDecls.end(); I != E; ++I) {
    delete (*I).second;
  }
}

