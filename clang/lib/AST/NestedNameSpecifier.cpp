//===- NestedNameSpecifier.cpp - C++ nested name specifiers ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file defines the NestedNameSpecifier class, which represents
//  a C++ nested-name-specifier.
//
//===----------------------------------------------------------------------===//

#include "clang/AST/NestedNameSpecifier.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/DependenceFlags.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/TemplateName.h"
#include "clang/AST/Type.h"
#include "clang/AST/TypeLoc.h"
#include "clang/Basic/LLVM.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceLocation.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>

using namespace clang;

NestedNameSpecifier *
NestedNameSpecifier::FindOrInsert(const ASTContext &Context,
                                  const NestedNameSpecifier &Mockup) {
  llvm::FoldingSetNodeID ID;
  Mockup.Profile(ID);

  void *InsertPos = nullptr;
  NestedNameSpecifier *NNS
    = Context.NestedNameSpecifiers.FindNodeOrInsertPos(ID, InsertPos);
  if (!NNS) {
    NNS =
        new (Context, alignof(NestedNameSpecifier)) NestedNameSpecifier(Mockup);
    Context.NestedNameSpecifiers.InsertNode(NNS, InsertPos);
  }

  return NNS;
}

NestedNameSpecifier *NestedNameSpecifier::Create(const ASTContext &Context,
                                                 NestedNameSpecifier *Prefix,
                                                 const IdentifierInfo *II) {
  assert(II && "Identifier cannot be NULL");
  assert((!Prefix || Prefix->isDependent()) && "Prefix must be dependent");

  NestedNameSpecifier Mockup;
  Mockup.Prefix.setPointer(Prefix);
  Mockup.Prefix.setInt(StoredIdentifier);
  Mockup.Specifier = const_cast<IdentifierInfo *>(II);
  return FindOrInsert(Context, Mockup);
}

NestedNameSpecifier *NestedNameSpecifier::Create(const ASTContext &Context,
                                                 NestedNameSpecifier *Prefix,
                                                 const NamespaceBaseDecl *NS) {
  assert(NS && "Namespace cannot be NULL");
  assert((!Prefix ||
          (Prefix->getAsType() == nullptr &&
           Prefix->getAsIdentifier() == nullptr)) &&
         "Broken nested name specifier");
  NestedNameSpecifier Mockup;
  Mockup.Prefix.setPointer(Prefix);
  Mockup.Prefix.setInt(StoredDecl);
  Mockup.Specifier = const_cast<NamespaceBaseDecl *>(NS);
  return FindOrInsert(Context, Mockup);
}

NestedNameSpecifier *NestedNameSpecifier::Create(const ASTContext &Context,
                                                 NestedNameSpecifier *Prefix,
                                                 const Type *T) {
  assert(T && "Type cannot be NULL");
  NestedNameSpecifier Mockup;
  Mockup.Prefix.setPointer(Prefix);
  Mockup.Prefix.setInt(StoredTypeSpec);
  Mockup.Specifier = const_cast<Type*>(T);
  return FindOrInsert(Context, Mockup);
}

NestedNameSpecifier *NestedNameSpecifier::Create(const ASTContext &Context,
                                                 const IdentifierInfo *II) {
  assert(II && "Identifier cannot be NULL");
  NestedNameSpecifier Mockup;
  Mockup.Prefix.setPointer(nullptr);
  Mockup.Prefix.setInt(StoredIdentifier);
  Mockup.Specifier = const_cast<IdentifierInfo *>(II);
  return FindOrInsert(Context, Mockup);
}

NestedNameSpecifier *
NestedNameSpecifier::GlobalSpecifier(const ASTContext &Context) {
  if (!Context.GlobalNestedNameSpecifier)
    Context.GlobalNestedNameSpecifier =
        new (Context, alignof(NestedNameSpecifier)) NestedNameSpecifier();
  return Context.GlobalNestedNameSpecifier;
}

NestedNameSpecifier *
NestedNameSpecifier::SuperSpecifier(const ASTContext &Context,
                                    CXXRecordDecl *RD) {
  NestedNameSpecifier Mockup;
  Mockup.Prefix.setPointer(nullptr);
  Mockup.Prefix.setInt(StoredDecl);
  Mockup.Specifier = RD;
  return FindOrInsert(Context, Mockup);
}

NestedNameSpecifier::SpecifierKind NestedNameSpecifier::getKind() const {
  if (!Specifier)
    return Global;

  switch (Prefix.getInt()) {
  case StoredIdentifier:
    return Identifier;

  case StoredDecl: {
    NamedDecl *ND = static_cast<NamedDecl *>(Specifier);
    return isa<CXXRecordDecl>(ND) ? Super : Namespace;
  }

  case StoredTypeSpec:
    return TypeSpec;
  }

  llvm_unreachable("Invalid NNS Kind!");
}

/// Retrieve the namespace or namespace alias stored in this nested name
/// specifier.
NamespaceBaseDecl *NestedNameSpecifier::getAsNamespace() const {
  if (Prefix.getInt() == StoredDecl)
    return dyn_cast<NamespaceBaseDecl>(static_cast<NamedDecl *>(Specifier));

  return nullptr;
}

/// Retrieve the record declaration stored in this nested name specifier.
CXXRecordDecl *NestedNameSpecifier::getAsRecordDecl() const {
  switch (Prefix.getInt()) {
  case StoredIdentifier:
    return nullptr;

  case StoredDecl:
    return dyn_cast<CXXRecordDecl>(static_cast<NamedDecl *>(Specifier));

  case StoredTypeSpec:
    return getAsType()->getAsCXXRecordDecl();
  }

  llvm_unreachable("Invalid NNS Kind!");
}

NestedNameSpecifierDependence NestedNameSpecifier::getDependence() const {
  switch (getKind()) {
  case Identifier: {
    // Identifier specifiers always represent dependent types
    auto F = NestedNameSpecifierDependence::Dependent |
             NestedNameSpecifierDependence::Instantiation;
    // Prefix can contain unexpanded template parameters.
    if (getPrefix())
      return F | getPrefix()->getDependence();
    return F;
  }

  case Namespace:
  case Global:
    return NestedNameSpecifierDependence::None;

  case Super: {
    CXXRecordDecl *RD = static_cast<CXXRecordDecl *>(Specifier);
    for (const auto &Base : RD->bases())
      if (Base.getType()->isDependentType())
        // FIXME: must also be instantiation-dependent.
        return NestedNameSpecifierDependence::Dependent;
    return NestedNameSpecifierDependence::None;
  }

  case TypeSpec: {
    NestedNameSpecifierDependence Dep =
        toNestedNameSpecifierDependendence(getAsType()->getDependence());
    if (NestedNameSpecifier *Prefix = getPrefix())
      Dep |=
          Prefix->getDependence() & ~NestedNameSpecifierDependence::Dependent;
    return Dep;
  }
  }
  llvm_unreachable("Invalid NNS Kind!");
}

bool NestedNameSpecifier::isDependent() const {
  return getDependence() & NestedNameSpecifierDependence::Dependent;
}

bool NestedNameSpecifier::isInstantiationDependent() const {
  return getDependence() & NestedNameSpecifierDependence::Instantiation;
}

bool NestedNameSpecifier::containsUnexpandedParameterPack() const {
  return getDependence() & NestedNameSpecifierDependence::UnexpandedPack;
}

bool NestedNameSpecifier::containsErrors() const {
  return getDependence() & NestedNameSpecifierDependence::Error;
}

const Type *
NestedNameSpecifier::translateToType(const ASTContext &Context) const {
  NestedNameSpecifier *Prefix = getPrefix();
  switch (getKind()) {
  case SpecifierKind::Identifier:
    return Context
        .getDependentNameType(ElaboratedTypeKeyword::None, Prefix,
                              getAsIdentifier())
        .getTypePtr();
  case SpecifierKind::TypeSpec: {
    const Type *T = getAsType();
    switch (T->getTypeClass()) {
    case Type::DependentTemplateSpecialization: {
      const auto *DT = cast<DependentTemplateSpecializationType>(T);
      const DependentTemplateStorage &DTN = DT->getDependentTemplateName();
      return Context
          .getDependentTemplateSpecializationType(
              ElaboratedTypeKeyword::None,
              {Prefix, DTN.getName(), DTN.hasTemplateKeyword()},
              DT->template_arguments())
          .getTypePtr();
    }
    case Type::Record:
    case Type::TemplateSpecialization:
    case Type::Using:
    case Type::Enum:
    case Type::Typedef:
    case Type::UnresolvedUsing:
      return Context
          .getElaboratedType(ElaboratedTypeKeyword::None, Prefix,
                             QualType(T, 0))
          .getTypePtr();
    default:
      assert(Prefix == nullptr && "unexpected type with elaboration");
      return T;
    }
  }
  case SpecifierKind::Global:
  case SpecifierKind::Namespace:
  case SpecifierKind::Super:
    // These are not representable as types.
    return nullptr;
  }
  llvm_unreachable("Unhandled SpecifierKind enum");
}

/// Print this nested name specifier to the given output
/// stream.
void NestedNameSpecifier::print(raw_ostream &OS, const PrintingPolicy &Policy,
                                bool ResolveTemplateArguments,
                                bool PrintFinalScopeResOp) const {
  if (getPrefix())
    getPrefix()->print(OS, Policy);

  switch (getKind()) {
  case Identifier:
    OS << getAsIdentifier()->getName();
    break;

  case Namespace: {
    NamespaceBaseDecl *Namespace = getAsNamespace();
    if (const auto *NS = dyn_cast<NamespaceDecl>(Namespace)) {
      assert(!NS->isAnonymousNamespace());
      OS << NS->getName();
    } else {
      OS << cast<NamespaceAliasDecl>(Namespace)->getName();
    }
    break;
  }

  case Global:
    OS << "::";
    return;

  case Super:
    OS << "__super";
    break;

  case TypeSpec: {
    PrintingPolicy InnerPolicy(Policy);
    InnerPolicy.SuppressScope = true;
    InnerPolicy.SuppressTagKeyword = true;
    QualType(getAsType(), 0).print(OS, InnerPolicy);
    break;
  }
  }

  if (PrintFinalScopeResOp)
    OS << "::";
}

LLVM_DUMP_METHOD void NestedNameSpecifier::dump(const LangOptions &LO) const {
  dump(llvm::errs(), LO);
}

LLVM_DUMP_METHOD void NestedNameSpecifier::dump() const { dump(llvm::errs()); }

LLVM_DUMP_METHOD void NestedNameSpecifier::dump(llvm::raw_ostream &OS) const {
  LangOptions LO;
  dump(OS, LO);
}

LLVM_DUMP_METHOD void NestedNameSpecifier::dump(llvm::raw_ostream &OS,
                                                const LangOptions &LO) const {
  print(OS, PrintingPolicy(LO));
}

unsigned
NestedNameSpecifierLoc::getLocalDataLength(NestedNameSpecifier *Qualifier) {
  assert(Qualifier && "Expected a non-NULL qualifier");

  // Location of the trailing '::'.
  unsigned Length = sizeof(SourceLocation::UIntTy);

  switch (Qualifier->getKind()) {
  case NestedNameSpecifier::Global:
    // Nothing more to add.
    break;

  case NestedNameSpecifier::Identifier:
  case NestedNameSpecifier::Namespace:
  case NestedNameSpecifier::Super:
    // The location of the identifier or namespace name.
    Length += sizeof(SourceLocation::UIntTy);
    break;

  case NestedNameSpecifier::TypeSpec:
    // The "void*" that points at the TypeLoc data.
    // Note: the 'template' keyword is part of the TypeLoc.
    Length += sizeof(void *);
    break;
  }

  return Length;
}

unsigned
NestedNameSpecifierLoc::getDataLength(NestedNameSpecifier *Qualifier) {
  unsigned Length = 0;
  for (; Qualifier; Qualifier = Qualifier->getPrefix())
    Length += getLocalDataLength(Qualifier);
  return Length;
}

/// Load a (possibly unaligned) source location from a given address
/// and offset.
static SourceLocation LoadSourceLocation(void *Data, unsigned Offset) {
  SourceLocation::UIntTy Raw;
  memcpy(&Raw, static_cast<char *>(Data) + Offset, sizeof(Raw));
  return SourceLocation::getFromRawEncoding(Raw);
}

/// Load a (possibly unaligned) pointer from a given address and
/// offset.
static void *LoadPointer(void *Data, unsigned Offset) {
  void *Result;
  memcpy(&Result, static_cast<char *>(Data) + Offset, sizeof(void*));
  return Result;
}

SourceRange NestedNameSpecifierLoc::getLocalSourceRange() const {
  if (!Qualifier)
    return SourceRange();

  unsigned Offset = getDataLength(Qualifier->getPrefix());
  switch (Qualifier->getKind()) {
  case NestedNameSpecifier::Global:
    return LoadSourceLocation(Data, Offset);

  case NestedNameSpecifier::Identifier:
  case NestedNameSpecifier::Namespace:
  case NestedNameSpecifier::Super:
    return SourceRange(
        LoadSourceLocation(Data, Offset),
        LoadSourceLocation(Data, Offset + sizeof(SourceLocation::UIntTy)));

  case NestedNameSpecifier::TypeSpec: {
    // The "void*" that points at the TypeLoc data.
    // Note: the 'template' keyword is part of the TypeLoc.
    void *TypeData = LoadPointer(Data, Offset);
    TypeLoc TL(Qualifier->getAsType(), TypeData);
    return SourceRange(TL.getBeginLoc(),
                       LoadSourceLocation(Data, Offset + sizeof(void*)));
  }
  }

  llvm_unreachable("Invalid NNS Kind!");
}

TypeLoc NestedNameSpecifierLoc::getTypeLoc() const {
  if (Qualifier->getKind() != NestedNameSpecifier::TypeSpec)
    return TypeLoc();

  // The "void*" that points at the TypeLoc data.
  unsigned Offset = getDataLength(Qualifier->getPrefix());
  void *TypeData = LoadPointer(Data, Offset);
  return TypeLoc(Qualifier->getAsType(), TypeData);
}

static void Append(char *Start, char *End, char *&Buffer, unsigned &BufferSize,
                   unsigned &BufferCapacity) {
  if (Start == End)
    return;

  if (BufferSize + (End - Start) > BufferCapacity) {
    // Reallocate the buffer.
    unsigned NewCapacity = std::max(
        (unsigned)(BufferCapacity ? BufferCapacity * 2 : sizeof(void *) * 2),
        (unsigned)(BufferSize + (End - Start)));
    if (!BufferCapacity) {
      char *NewBuffer = static_cast<char *>(llvm::safe_malloc(NewCapacity));
      if (Buffer)
        memcpy(NewBuffer, Buffer, BufferSize);
      Buffer = NewBuffer;
    } else {
      Buffer = static_cast<char *>(llvm::safe_realloc(Buffer, NewCapacity));
    }
    BufferCapacity = NewCapacity;
  }
  assert(Buffer && Start && End && End > Start && "Illegal memory buffer copy");
  memcpy(Buffer + BufferSize, Start, End - Start);
  BufferSize += End - Start;
}

/// Save a source location to the given buffer.
static void SaveSourceLocation(SourceLocation Loc, char *&Buffer,
                               unsigned &BufferSize, unsigned &BufferCapacity) {
  SourceLocation::UIntTy Raw = Loc.getRawEncoding();
  Append(reinterpret_cast<char *>(&Raw),
         reinterpret_cast<char *>(&Raw) + sizeof(Raw), Buffer, BufferSize,
         BufferCapacity);
}

/// Save a pointer to the given buffer.
static void SavePointer(void *Ptr, char *&Buffer, unsigned &BufferSize,
                        unsigned &BufferCapacity) {
  Append(reinterpret_cast<char *>(&Ptr),
         reinterpret_cast<char *>(&Ptr) + sizeof(void *),
         Buffer, BufferSize, BufferCapacity);
}

NestedNameSpecifierLocBuilder::
NestedNameSpecifierLocBuilder(const NestedNameSpecifierLocBuilder &Other)
    : Representation(Other.Representation) {
  if (!Other.Buffer)
    return;

  if (Other.BufferCapacity == 0) {
    // Shallow copy is okay.
    Buffer = Other.Buffer;
    BufferSize = Other.BufferSize;
    return;
  }

  // Deep copy
  Append(Other.Buffer, Other.Buffer + Other.BufferSize, Buffer, BufferSize,
         BufferCapacity);
}

NestedNameSpecifierLocBuilder &
NestedNameSpecifierLocBuilder::
operator=(const NestedNameSpecifierLocBuilder &Other) {
  Representation = Other.Representation;

  if (Buffer && Other.Buffer && BufferCapacity >= Other.BufferSize) {
    // Re-use our storage.
    BufferSize = Other.BufferSize;
    memcpy(Buffer, Other.Buffer, BufferSize);
    return *this;
  }

  // Free our storage, if we have any.
  if (BufferCapacity) {
    free(Buffer);
    BufferCapacity = 0;
  }

  if (!Other.Buffer) {
    // Empty.
    Buffer = nullptr;
    BufferSize = 0;
    return *this;
  }

  if (Other.BufferCapacity == 0) {
    // Shallow copy is okay.
    Buffer = Other.Buffer;
    BufferSize = Other.BufferSize;
    return *this;
  }

  // Deep copy.
  BufferSize = 0;
  Append(Other.Buffer, Other.Buffer + Other.BufferSize, Buffer, BufferSize,
         BufferCapacity);
  return *this;
}

void NestedNameSpecifierLocBuilder::Extend(ASTContext &Context, TypeLoc TL,
                                           SourceLocation ColonColonLoc) {
  Representation =
      NestedNameSpecifier::Create(Context, Representation, TL.getTypePtr());

  // Push source-location info into the buffer.
  SavePointer(TL.getOpaqueData(), Buffer, BufferSize, BufferCapacity);
  SaveSourceLocation(ColonColonLoc, Buffer, BufferSize, BufferCapacity);
}

void NestedNameSpecifierLocBuilder::Extend(ASTContext &Context,
                                           IdentifierInfo *Identifier,
                                           SourceLocation IdentifierLoc,
                                           SourceLocation ColonColonLoc) {
  Representation = NestedNameSpecifier::Create(Context, Representation,
                                               Identifier);

  // Push source-location info into the buffer.
  SaveSourceLocation(IdentifierLoc, Buffer, BufferSize, BufferCapacity);
  SaveSourceLocation(ColonColonLoc, Buffer, BufferSize, BufferCapacity);
}

void NestedNameSpecifierLocBuilder::Extend(ASTContext &Context,
                                           NamespaceBaseDecl *Namespace,
                                           SourceLocation NamespaceLoc,
                                           SourceLocation ColonColonLoc) {
  Representation = NestedNameSpecifier::Create(Context, Representation,
                                               Namespace);

  // Push source-location info into the buffer.
  SaveSourceLocation(NamespaceLoc, Buffer, BufferSize, BufferCapacity);
  SaveSourceLocation(ColonColonLoc, Buffer, BufferSize, BufferCapacity);
}

void NestedNameSpecifierLocBuilder::MakeGlobal(ASTContext &Context,
                                               SourceLocation ColonColonLoc) {
  assert(!Representation && "Already have a nested-name-specifier!?");
  Representation = NestedNameSpecifier::GlobalSpecifier(Context);

  // Push source-location info into the buffer.
  SaveSourceLocation(ColonColonLoc, Buffer, BufferSize, BufferCapacity);
}

void NestedNameSpecifierLocBuilder::MakeSuper(ASTContext &Context,
                                              CXXRecordDecl *RD,
                                              SourceLocation SuperLoc,
                                              SourceLocation ColonColonLoc) {
  Representation = NestedNameSpecifier::SuperSpecifier(Context, RD);

  // Push source-location info into the buffer.
  SaveSourceLocation(SuperLoc, Buffer, BufferSize, BufferCapacity);
  SaveSourceLocation(ColonColonLoc, Buffer, BufferSize, BufferCapacity);
}

void NestedNameSpecifierLocBuilder::MakeTrivial(ASTContext &Context,
                                                NestedNameSpecifier *Qualifier,
                                                SourceRange R) {
  Representation = Qualifier;

  // Construct bogus (but well-formed) source information for the
  // nested-name-specifier.
  BufferSize = 0;
  SmallVector<NestedNameSpecifier *, 4> Stack;
  for (NestedNameSpecifier *NNS = Qualifier; NNS; NNS = NNS->getPrefix())
    Stack.push_back(NNS);
  while (!Stack.empty()) {
    NestedNameSpecifier *NNS = Stack.pop_back_val();
    switch (NNS->getKind()) {
      case NestedNameSpecifier::Identifier:
      case NestedNameSpecifier::Namespace:
        SaveSourceLocation(R.getBegin(), Buffer, BufferSize, BufferCapacity);
        break;

      case NestedNameSpecifier::TypeSpec: {
        TypeSourceInfo *TSInfo
        = Context.getTrivialTypeSourceInfo(QualType(NNS->getAsType(), 0),
                                           R.getBegin());
        SavePointer(TSInfo->getTypeLoc().getOpaqueData(), Buffer, BufferSize,
                    BufferCapacity);
        break;
      }

      case NestedNameSpecifier::Global:
      case NestedNameSpecifier::Super:
        break;
    }

    // Save the location of the '::'.
    SaveSourceLocation(Stack.empty()? R.getEnd() : R.getBegin(),
                       Buffer, BufferSize, BufferCapacity);
  }
}

void NestedNameSpecifierLocBuilder::Adopt(NestedNameSpecifierLoc Other) {
  if (BufferCapacity)
    free(Buffer);

  if (!Other) {
    Representation = nullptr;
    BufferSize = 0;
    return;
  }

  // Rather than copying the data (which is wasteful), "adopt" the
  // pointer (which points into the ASTContext) but set the capacity to zero to
  // indicate that we don't own it.
  Representation = Other.getNestedNameSpecifier();
  Buffer = static_cast<char *>(Other.getOpaqueData());
  BufferSize = Other.getDataLength();
  BufferCapacity = 0;
}

NestedNameSpecifierLoc
NestedNameSpecifierLocBuilder::getWithLocInContext(ASTContext &Context) const {
  if (!Representation)
    return NestedNameSpecifierLoc();

  // If we adopted our data pointer from elsewhere in the AST context, there's
  // no need to copy the memory.
  if (BufferCapacity == 0)
    return NestedNameSpecifierLoc(Representation, Buffer);

  // FIXME: After copying the source-location information, should we free
  // our (temporary) buffer and adopt the ASTContext-allocated memory?
  // Doing so would optimize repeated calls to getWithLocInContext().
  void *Mem = Context.Allocate(BufferSize, alignof(void *));
  memcpy(Mem, Buffer, BufferSize);
  return NestedNameSpecifierLoc(Representation, Mem);
}
