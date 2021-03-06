/**
 * Copyright (c) 2016, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#pragma once
#include <clang/AST/DeclVisitor.h>
#include <clang/Basic/SourceManager.h>

#include "atdlib/ATDWriter.h"
namespace ASTLib {

using namespace clang;
template <class ATDWriter>
class NamePrinter : public ConstDeclVisitor<NamePrinter<ATDWriter>> {
  typedef typename ATDWriter::ObjectScope ObjectScope;
  typedef typename ATDWriter::ArrayScope ArrayScope;
  typedef typename ATDWriter::TupleScope TupleScope;
  typedef typename ATDWriter::VariantScope VariantScope;

  const SourceManager &SM;
  ATDWriter &OF;

  PrintingPolicy getPrintingPolicy();
  void printTemplateArgList(llvm::raw_ostream &OS,
                            const ArrayRef<TemplateArgument> Args);

 public:
  NamePrinter(const SourceManager &SM, ATDWriter &OF) : SM(SM), OF(OF) {}

  // implementation is inspired by NamedDecl::printQualifiedName
  // but with better handling for anonymous structs,unions and namespaces
  void printDeclName(const NamedDecl &D);

  void VisitNamedDecl(const NamedDecl *D);
  void VisitNamespaceDecl(const NamespaceDecl *ND);
  void VisitTagDecl(const TagDecl *TD);
  void VisitFunctionDecl(const FunctionDecl *FD);
};

// 64 bits fnv-1a
const uint64_t FNV64_hash_start = 14695981039346656037ULL;
const uint64_t FNV64_prime = 1099511628211ULL;
uint64_t fnv64Hash(const char *s, int n) {
  uint64_t hash = FNV64_hash_start;
  for (int i = 0; i < n; ++i) {
    hash ^= s[i];
    hash *= FNV64_prime;
  }
  return hash;
}

const int templateLengthThreshold = 40;
template <class ATDWriter>
void NamePrinter<ATDWriter>::printTemplateArgList(
    llvm::raw_ostream &OS, const ArrayRef<TemplateArgument> Args) {
  SmallString<64> Buf;
  llvm::raw_svector_ostream tmpOS(Buf);
  TemplateSpecializationType::PrintTemplateArgumentList(
      tmpOS, Args, getPrintingPolicy());
  if (tmpOS.str().size() > templateLengthThreshold) {
    OS << "<";
    OS.write_hex(fnv64Hash(tmpOS.str().data(), tmpOS.str().size()));
    OS << ">";
  } else {
    OS << tmpOS.str();
  }
}

template <class ATDWriter>
void NamePrinter<ATDWriter>::printDeclName(const NamedDecl &D) {
  const DeclContext *Ctx = D.getDeclContext();
  SmallVector<const NamedDecl *, 8> Contexts;
  Contexts.push_back(&D);

  // Don't dump full qualifier for variables declared
  // inside a function/method/block
  // For structs defined inside functions, dump fully qualified name
  if (Ctx->isFunctionOrMethod() && !isa<TagDecl>(&D)) {
    Ctx = nullptr;
  }

  while (Ctx && isa<NamedDecl>(Ctx)) {
    Contexts.push_back(cast<NamedDecl>(Ctx));
    Ctx = Ctx->getParent();
  }

  ArrayScope aScope(OF, Contexts.size());
  // dump list in reverse
  for (const Decl *Ctx : Contexts) {
    ConstDeclVisitor<NamePrinter<ATDWriter>>::Visit(Ctx);
  }
}

template <class ATDWriter>
void NamePrinter<ATDWriter>::VisitNamedDecl(const NamedDecl *D) {
  OF.emitString(D->getNameAsString());
}

template <class ATDWriter>
void NamePrinter<ATDWriter>::VisitNamespaceDecl(const NamespaceDecl *ND) {
  if (ND->isAnonymousNamespace()) {
    PresumedLoc PLoc = SM.getPresumedLoc(ND->getLocation());
    std::string file = "invalid_loc";
    if (PLoc.isValid()) {
      file = PLoc.getFilename();
    }
    OF.emitString("anonymous_namespace_" + file);
  } else {
    // for non-anonymous namespaces, fallback to normal behavior
    VisitNamedDecl(ND);
  }
}

template <class ATDWriter>
void NamePrinter<ATDWriter>::VisitTagDecl(const TagDecl *D) {
  // heavily inspired by clang's TypePrinter::printTag() function
  SmallString<64> Buf;
  llvm::raw_svector_ostream StrOS(Buf);
  if (const IdentifierInfo *II = D->getIdentifier()) {
    StrOS << II->getName();
  } else if (TypedefNameDecl *Typedef = D->getTypedefNameForAnonDecl()) {
    StrOS << Typedef->getIdentifier()->getName();
  } else {
    if (isa<CXXRecordDecl>(D) && cast<CXXRecordDecl>(D)->isLambda()) {
      StrOS << "lambda";
    } else {
      StrOS << "anonymous_" << D->getKindName();
    }
    PresumedLoc PLoc = SM.getPresumedLoc(D->getLocation());
    if (PLoc.isValid()) {
      StrOS << "_" << PLoc.getFilename() << ':' << PLoc.getLine() << ':'
            << PLoc.getColumn();
    }
  }
  if (const ClassTemplateSpecializationDecl *Spec =
          dyn_cast<ClassTemplateSpecializationDecl>(D)) {
    ArrayRef<TemplateArgument> Args;
    const TemplateArgumentList &TemplateArgs = Spec->getTemplateArgs();
    Args = TemplateArgs.asArray();
    printTemplateArgList(StrOS, Args);
  }
  OF.emitString(StrOS.str());
}

template <class ATDWriter>
void NamePrinter<ATDWriter>::VisitFunctionDecl(const FunctionDecl *FD) {
  std::string template_str = "";
  // add instantiated template arguments for readability
  if (const TemplateArgumentList *TemplateArgs =
          FD->getTemplateSpecializationArgs()) {
    SmallString<64> Buf;
    llvm::raw_svector_ostream StrOS(Buf);
    printTemplateArgList(StrOS, TemplateArgs->asArray());
    template_str = StrOS.str();
  }
  OF.emitString(FD->getNameAsString() + template_str);
}

template <class ATDWriter>
PrintingPolicy NamePrinter<ATDWriter>::getPrintingPolicy() {
  // configure what to print
  LangOptions LO;
  PrintingPolicy Policy(LO);
  // print tag types
  Policy.IncludeTagDefinition = false;
  // don't print fully qualified names - we do it ourselves
  Policy.SuppressScope = true;
  // print locations of anonymous tags
  Policy.AnonymousTagLocations = true;
  // don't add 'struct' inside a name regardless of language
  Policy.SuppressTagKeyword = true;

  return Policy;
}

} // end of namespace ASTLib
