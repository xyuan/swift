//===--- TypeCheckAccess.cpp - Type Checking for Access Control -----------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements access control checking.
//
//===----------------------------------------------------------------------===//

#include "TypeCheckAccess.h"
#include "TypeAccessScopeChecker.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/ClangModuleLoader.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/TypeDeclFinder.h"

using namespace swift;

#define DEBUG_TYPE "TypeCheckAccess"

namespace {

/// A uniquely-typed boolean to reduce the chances of accidentally inverting
/// a check.
///
/// \see checkTypeAccess
enum class DowngradeToWarning: bool {
  No,
  Yes
};

/// Calls \p callback for each type in each requirement provided by
/// \p source.
static void forAllRequirementTypes(
    WhereClauseOwner &&source,
    llvm::function_ref<void(Type, TypeRepr *)> callback) {
  std::move(source).visitRequirements(TypeResolutionStage::Interface,
      [&](const Requirement &req, RequirementRepr *reqRepr) {
    switch (req.getKind()) {
    case RequirementKind::Conformance:
    case RequirementKind::SameType:
    case RequirementKind::Superclass:
      callback(req.getFirstType(),
               RequirementRepr::getFirstTypeRepr(reqRepr));
      callback(req.getSecondType(),
               RequirementRepr::getSecondTypeRepr(reqRepr));
      break;

    case RequirementKind::Layout:
      callback(req.getFirstType(),
               RequirementRepr::getFirstTypeRepr(reqRepr));
      break;
    }
    return false;
  });
}

/// \see checkTypeAccess
using CheckTypeAccessCallback =
    void(AccessScope, const TypeRepr *, DowngradeToWarning);

class AccessControlCheckerBase {
protected:
  bool checkUsableFromInline;

  void checkTypeAccessImpl(
      Type type, TypeRepr *typeRepr, AccessScope contextAccessScope,
      const DeclContext *useDC, bool mayBeInferred,
      llvm::function_ref<CheckTypeAccessCallback> diagnose);

  void checkTypeAccess(
      Type type, TypeRepr *typeRepr, const ValueDecl *context,
      bool mayBeInferred,
      llvm::function_ref<CheckTypeAccessCallback> diagnose);

  void checkTypeAccess(
      const TypeLoc &TL, const ValueDecl *context, bool mayBeInferred,
      llvm::function_ref<CheckTypeAccessCallback> diagnose) {
    return checkTypeAccess(TL.getType(), TL.getTypeRepr(), context,
                           mayBeInferred, diagnose);
  }

  void checkRequirementAccess(
      WhereClauseOwner &&source,
      AccessScope accessScope,
      const DeclContext *useDC,
      llvm::function_ref<CheckTypeAccessCallback> diagnose) {
    forAllRequirementTypes(std::move(source), [&](Type type, TypeRepr *typeRepr) {
      checkTypeAccessImpl(type, typeRepr, accessScope, useDC,
                          /*mayBeInferred*/false, diagnose);
    });
  }

  AccessControlCheckerBase(bool checkUsableFromInline)
      : checkUsableFromInline(checkUsableFromInline) {}

public:
  void checkGenericParamAccess(
    const GenericContext *ownerCtx,
    const Decl *ownerDecl,
    AccessScope accessScope,
    AccessLevel contextAccess);

  void checkGenericParamAccess(
    const GenericContext *ownerCtx,
    const ValueDecl *ownerDecl);
};

class TypeAccessScopeDiagnoser : private ASTWalker {
  AccessScope accessScope;
  const DeclContext *useDC;
  bool treatUsableFromInlineAsPublic;
  const ComponentIdentTypeRepr *offendingType = nullptr;

  bool walkToTypeReprPre(TypeRepr *TR) override {
    // Exit early if we've already found a problem type.
    if (offendingType)
      return false;

    auto CITR = dyn_cast<ComponentIdentTypeRepr>(TR);
    if (!CITR)
      return true;

    const ValueDecl *VD = CITR->getBoundDecl();
    if (!VD)
      return true;

    if (VD->getFormalAccessScope(useDC, treatUsableFromInlineAsPublic)
        != accessScope)
      return true;

    offendingType = CITR;
    return false;
  }

  bool walkToTypeReprPost(TypeRepr *T) override {
    // Exit early if we've already found a problem type.
    return offendingType != nullptr;
  }

  explicit TypeAccessScopeDiagnoser(AccessScope accessScope,
                                    const DeclContext *useDC,
                                    bool treatUsableFromInlineAsPublic)
    : accessScope(accessScope), useDC(useDC),
      treatUsableFromInlineAsPublic(treatUsableFromInlineAsPublic) {}

public:
  static const TypeRepr *findTypeWithScope(TypeRepr *TR,
                                           AccessScope accessScope,
                                           const DeclContext *useDC,
                                           bool treatUsableFromInlineAsPublic) {
    assert(!accessScope.isPublic() &&
           "why would we need to find a public access scope?");
    if (TR == nullptr)
      return nullptr;
    TypeAccessScopeDiagnoser diagnoser(accessScope, useDC,
                                       treatUsableFromInlineAsPublic);
    TR->walk(diagnoser);
    return diagnoser.offendingType;
  }
};

} // end anonymous namespace

/// Checks if the access scope of the type described by \p TL contains
/// \p contextAccessScope. If it isn't, calls \p diagnose with a TypeRepr
/// representing the offending part of \p TL.
///
/// The TypeRepr passed to \p diagnose may be null, in which case a particular
/// part of the type that caused the problem could not be found. The DeclContext
/// is never null.
///
/// If \p type might be partially inferred even when \p typeRepr is present
/// (such as for properties), pass \c true for \p mayBeInferred. (This does not
/// include implicitly providing generic parameters for the Self type, such as
/// using `Array` to mean `Array<Element>` in an extension of Array.) If
/// \p typeRepr is known to be absent, it's okay to pass \c false for
/// \p mayBeInferred.
void AccessControlCheckerBase::checkTypeAccessImpl(
    Type type, TypeRepr *typeRepr, AccessScope contextAccessScope,
    const DeclContext *useDC, bool mayBeInferred,
    llvm::function_ref<CheckTypeAccessCallback> diagnose) {
  auto &Context = useDC->getASTContext();
  if (Context.isAccessControlDisabled())
    return;
  // Don't spend time checking local declarations; this is always valid by the
  // time we get to this point.
  if (!contextAccessScope.isPublic() &&
      contextAccessScope.getDeclContext()->isLocalContext())
    return;

  AccessScope problematicAccessScope = AccessScope::getPublic();
  if (type) {
    Optional<AccessScope> typeAccessScope =
        TypeAccessScopeChecker::getAccessScope(type, useDC,
                                               checkUsableFromInline);

    // Note: This means that the type itself is invalid for this particular
    // context, because it references declarations from two incompatible scopes.
    // In this case we should have diagnosed the bad reference already.
    if (!typeAccessScope.hasValue())
      return;
    problematicAccessScope = *typeAccessScope;
  }

  auto downgradeToWarning = DowngradeToWarning::No;

  if (contextAccessScope.hasEqualDeclContextWith(problematicAccessScope) ||
      contextAccessScope.isChildOf(problematicAccessScope)) {

    // /Also/ check the TypeRepr, if present. This can be important when we're
    // unable to preserve typealias sugar that's present in the TypeRepr.
    if (!typeRepr)
      return;

    Optional<AccessScope> typeReprAccessScope =
        TypeAccessScopeChecker::getAccessScope(typeRepr, useDC,
                                               checkUsableFromInline);
    if (!typeReprAccessScope.hasValue())
      return;

    if (contextAccessScope.hasEqualDeclContextWith(*typeReprAccessScope) ||
        contextAccessScope.isChildOf(*typeReprAccessScope)) {
      // Only if both the Type and the TypeRepr follow the access rules can
      // we exit; otherwise we have to emit a diagnostic.
      return;
    }
    problematicAccessScope = *typeReprAccessScope;

  } else {
    // The type violates the rules of access control (with or without taking the
    // TypeRepr into account).

    if (typeRepr && mayBeInferred &&
        !Context.LangOpts.isSwiftVersionAtLeast(5) &&
        !useDC->getParentModule()->isResilient()) {
      // Swift 4.2 and earlier didn't check the Type when a TypeRepr was
      // present. However, this is a major hole when generic parameters are
      // inferred:
      //
      //   public let foo: Optional = VeryPrivateStruct()
      //
      // Downgrade the error to a warning in this case for source compatibility.
      Optional<AccessScope> typeReprAccessScope =
          TypeAccessScopeChecker::getAccessScope(typeRepr, useDC,
                                                 checkUsableFromInline);
      assert(typeReprAccessScope && "valid Type but not valid TypeRepr?");
      if (contextAccessScope.hasEqualDeclContextWith(*typeReprAccessScope) ||
          contextAccessScope.isChildOf(*typeReprAccessScope)) {
        downgradeToWarning = DowngradeToWarning::Yes;
      }
    }
  }

  const TypeRepr *complainRepr = TypeAccessScopeDiagnoser::findTypeWithScope(
      typeRepr, problematicAccessScope, useDC, checkUsableFromInline);
  diagnose(problematicAccessScope, complainRepr, downgradeToWarning);
}

/// Checks if the access scope of the type described by \p TL is valid for the
/// type to be the type of \p context. If it isn't, calls \p diagnose with a
/// TypeRepr representing the offending part of \p TL.
///
/// The TypeRepr passed to \p diagnose may be null, in which case a particular
/// part of the type that caused the problem could not be found.
///
/// If \p type might be partially inferred even when \p typeRepr is present
/// (such as for properties), pass \c true for \p mayBeInferred. (This does not
/// include implicitly providing generic parameters for the Self type, such as
/// using `Array` to mean `Array<Element>` in an extension of Array.) If
/// \p typeRepr is known to be absent, it's okay to pass \c false for
/// \p mayBeInferred.
void AccessControlCheckerBase::checkTypeAccess(
    Type type, TypeRepr *typeRepr, const ValueDecl *context, bool mayBeInferred,
    llvm::function_ref<CheckTypeAccessCallback> diagnose) {
  assert(!isa<ParamDecl>(context));
  const DeclContext *DC = context->getDeclContext();
  AccessScope contextAccessScope =
    context->getFormalAccessScope(
      nullptr, checkUsableFromInline);
  checkTypeAccessImpl(type, typeRepr, contextAccessScope, DC, mayBeInferred,
                      diagnose);
}

/// Highlights the given TypeRepr, and adds a note pointing to the type's
/// declaration if possible.
///
/// Just flushes \p diag as is if \p complainRepr is null.
static void highlightOffendingType(InFlightDiagnostic &diag,
                                   const TypeRepr *complainRepr) {
  if (!complainRepr) {
    diag.flush();
    return;
  }

  diag.highlight(complainRepr->getSourceRange());
  diag.flush();

  if (auto CITR = dyn_cast<ComponentIdentTypeRepr>(complainRepr)) {
    const ValueDecl *VD = CITR->getBoundDecl();
    VD->diagnose(diag::kind_declared_here, DescriptiveDeclKind::Type);
  }
}

void AccessControlCheckerBase::checkGenericParamAccess(
    const GenericContext *ownerCtx,
    const Decl *ownerDecl,
    AccessScope accessScope,
    AccessLevel contextAccess) {
  auto params = ownerCtx->getGenericParams();
  if (!params)
    return;

  // This must stay in sync with diag::generic_param_access.
  enum class ACEK {
    Parameter = 0,
    Requirement
  } accessControlErrorKind;
  auto minAccessScope = AccessScope::getPublic();
  const TypeRepr *complainRepr = nullptr;
  auto downgradeToWarning = DowngradeToWarning::Yes;

  auto callbackACEK = ACEK::Parameter;

  auto callback = [&](AccessScope typeAccessScope,
                      const TypeRepr *thisComplainRepr,
                      DowngradeToWarning thisDowngrade) {
    if (typeAccessScope.isChildOf(minAccessScope) ||
        (thisDowngrade == DowngradeToWarning::No &&
         downgradeToWarning == DowngradeToWarning::Yes) ||
        (!complainRepr &&
         typeAccessScope.hasEqualDeclContextWith(minAccessScope))) {
      minAccessScope = typeAccessScope;
      complainRepr = thisComplainRepr;
      accessControlErrorKind = callbackACEK;
      downgradeToWarning = thisDowngrade;
    }
  };

  auto *DC = ownerDecl->getDeclContext();

  for (auto param : *params) {
    if (param->getInherited().empty())
      continue;
    assert(param->getInherited().size() == 1);
    checkTypeAccessImpl(param->getInherited().front().getType(),
                        param->getInherited().front().getTypeRepr(),
                        accessScope, DC, /*mayBeInferred*/false, callback);
  }
  callbackACEK = ACEK::Requirement;

  checkRequirementAccess(WhereClauseOwner(
                           const_cast<GenericContext *>(ownerCtx)),
                         accessScope, DC, callback);

  if (minAccessScope.isPublic())
    return;

  // FIXME: Promote these to an error in the next -swift-version break.
  if (isa<SubscriptDecl>(ownerDecl) || isa<TypeAliasDecl>(ownerDecl))
    downgradeToWarning = DowngradeToWarning::Yes;

  auto &Context = ownerDecl->getASTContext();
  if (checkUsableFromInline) {
    if (!Context.isSwiftVersionAtLeast(5))
      downgradeToWarning = DowngradeToWarning::Yes;

    auto diagID = diag::generic_param_usable_from_inline;
    if (downgradeToWarning == DowngradeToWarning::Yes)
      diagID = diag::generic_param_usable_from_inline_warn;
    auto diag =
        Context.Diags.diagnose(ownerDecl, diagID, ownerDecl->getDescriptiveKind(),
                               accessControlErrorKind == ACEK::Requirement);
    highlightOffendingType(diag, complainRepr);
    return;
  }

  auto minAccess = minAccessScope.accessLevelForDiagnostics();

  bool isExplicit =
    ownerDecl->getAttrs().hasAttribute<AccessControlAttr>() ||
    isa<ProtocolDecl>(DC);
  auto diagID = diag::generic_param_access;
  if (downgradeToWarning == DowngradeToWarning::Yes)
    diagID = diag::generic_param_access_warn;
  auto diag = Context.Diags.diagnose(
      ownerDecl, diagID, ownerDecl->getDescriptiveKind(), isExplicit,
      contextAccess, minAccess, isa<FileUnit>(DC),
      accessControlErrorKind == ACEK::Requirement);
  highlightOffendingType(diag, complainRepr);
}

void AccessControlCheckerBase::checkGenericParamAccess(
    const GenericContext *ownerCtx,
    const ValueDecl *ownerDecl) {
  checkGenericParamAccess(ownerCtx, ownerDecl,
                          ownerDecl->getFormalAccessScope(
                              nullptr, checkUsableFromInline),
                          ownerDecl->getFormalAccess());
}

namespace {
class AccessControlChecker : public AccessControlCheckerBase,
                             public DeclVisitor<AccessControlChecker> {
public:
  AccessControlChecker()
      : AccessControlCheckerBase(/*checkUsableFromInline=*/false) {}

  void visit(Decl *D) {
    if (D->isInvalid() || D->isImplicit())
      return;

    DeclVisitor<AccessControlChecker>::visit(D);
  }

  // Force all kinds to be handled at a lower level.
  void visitDecl(Decl *D) = delete;
  void visitValueDecl(ValueDecl *D) = delete;

#define UNREACHABLE(KIND, REASON) \
  void visit##KIND##Decl(KIND##Decl *D) { \
    llvm_unreachable(REASON); \
  }
  UNREACHABLE(Import, "cannot appear in a type context")
  UNREACHABLE(Extension, "cannot appear in a type context")
  UNREACHABLE(TopLevelCode, "cannot appear in a type context")
  UNREACHABLE(Operator, "cannot appear in a type context")
  UNREACHABLE(PrecedenceGroup, "cannot appear in a type context")
  UNREACHABLE(Module, "cannot appear in a type context")

  UNREACHABLE(IfConfig, "does not have access control")
  UNREACHABLE(PoundDiagnostic, "does not have access control")
  UNREACHABLE(Param, "does not have access control")
  UNREACHABLE(GenericTypeParam, "does not have access control")
  UNREACHABLE(MissingMember, "does not have access control")
#undef UNREACHABLE

#define UNINTERESTING(KIND) \
  void visit##KIND##Decl(KIND##Decl *D) {}

  UNINTERESTING(EnumCase) // Handled at the EnumElement level.
  UNINTERESTING(Var) // Handled at the PatternBinding level.
  UNINTERESTING(Destructor) // Always correct.
  UNINTERESTING(Accessor) // Handled by the Var or Subscript.

  /// \see visitPatternBindingDecl
  void checkNamedPattern(const NamedPattern *NP, bool isTypeContext,
                         const llvm::DenseSet<const VarDecl *> &seenVars) {
    const VarDecl *theVar = NP->getDecl();
    if (seenVars.count(theVar) || theVar->isInvalid())
      return;

    checkTypeAccess(theVar->getInterfaceType(), nullptr, theVar,
                    /*mayBeInferred*/false,
                    [&](AccessScope typeAccessScope,
                        const TypeRepr *complainRepr,
                        DowngradeToWarning downgradeToWarning) {
      auto typeAccess = typeAccessScope.accessLevelForDiagnostics();
      bool isExplicit = theVar->getAttrs().hasAttribute<AccessControlAttr>() ||
                        isa<ProtocolDecl>(theVar->getDeclContext());
      auto theVarAccess =
          isExplicit ? theVar->getFormalAccess()
                     : typeAccessScope.requiredAccessForDiagnostics();
      auto diagID = diag::pattern_type_access_inferred;
      if (downgradeToWarning == DowngradeToWarning::Yes)
        diagID = diag::pattern_type_access_inferred_warn;
      auto &DE = theVar->getASTContext().Diags;
      auto diag = DE.diagnose(NP->getLoc(), diagID, theVar->isLet(),
                              isTypeContext, isExplicit, theVarAccess,
                              isa<FileUnit>(theVar->getDeclContext()),
                              typeAccess, theVar->getInterfaceType());
    });
  }

  void checkTypedPattern(const TypedPattern *TP, bool isTypeContext,
                         llvm::DenseSet<const VarDecl *> &seenVars) {
    VarDecl *anyVar = nullptr;
    TP->forEachVariable([&](VarDecl *V) {
      seenVars.insert(V);
      anyVar = V;
    });
    if (!anyVar)
      return;

    checkTypeAccess(TP->getTypeLoc(), anyVar, /*mayBeInferred*/true,
                    [&](AccessScope typeAccessScope,
                        const TypeRepr *complainRepr,
                        DowngradeToWarning downgradeToWarning) {
      auto typeAccess = typeAccessScope.accessLevelForDiagnostics();
      bool isExplicit = anyVar->getAttrs().hasAttribute<AccessControlAttr>() ||
                        isa<ProtocolDecl>(anyVar->getDeclContext());
      auto diagID = diag::pattern_type_access;
      if (downgradeToWarning == DowngradeToWarning::Yes)
        diagID = diag::pattern_type_access_warn;
      auto anyVarAccess =
          isExplicit ? anyVar->getFormalAccess()
                     : typeAccessScope.requiredAccessForDiagnostics();
      auto &DE = anyVar->getASTContext().Diags;
      auto diag = DE.diagnose(
          TP->getLoc(), diagID, anyVar->isLet(), isTypeContext, isExplicit,
          anyVarAccess, isa<FileUnit>(anyVar->getDeclContext()), typeAccess);
      highlightOffendingType(diag, complainRepr);
    });

    // Check the property wrapper types.
    for (auto attr : anyVar->getAttachedPropertyWrappers()) {
      checkTypeAccess(attr->getTypeLoc(), anyVar,
                      /*mayBeInferred=*/false,
                      [&](AccessScope typeAccessScope,
                          const TypeRepr *complainRepr,
                          DowngradeToWarning downgradeToWarning) {
        auto typeAccess = typeAccessScope.accessLevelForDiagnostics();
        bool isExplicit =
            anyVar->getAttrs().hasAttribute<AccessControlAttr>() ||
            isa<ProtocolDecl>(anyVar->getDeclContext());
        auto anyVarAccess =
            isExplicit ? anyVar->getFormalAccess()
                       : typeAccessScope.requiredAccessForDiagnostics();
        auto diag = anyVar->diagnose(diag::property_wrapper_type_access,
                                     anyVar->isLet(),
                                     isTypeContext,
                                     isExplicit,
                                     anyVarAccess,
                                     isa<FileUnit>(anyVar->getDeclContext()),
                                     typeAccess);
        highlightOffendingType(diag, complainRepr);
      });
    }
  }

  void visitPatternBindingDecl(PatternBindingDecl *PBD) {
    bool isTypeContext = PBD->getDeclContext()->isTypeContext();

    llvm::DenseSet<const VarDecl *> seenVars;
    for (auto idx : range(PBD->getNumPatternEntries())) {
      PBD->getPattern(idx)->forEachNode([&](const Pattern *P) {
        if (auto *NP = dyn_cast<NamedPattern>(P)) {
          // Only check individual variables if we didn't check an enclosing
          // TypedPattern.
          checkNamedPattern(NP, isTypeContext, seenVars);
          return;
        }

        auto *TP = dyn_cast<TypedPattern>(P);
        if (!TP)
          return;
        checkTypedPattern(TP, isTypeContext, seenVars);
      });
      seenVars.clear();
    }
  }

  void visitTypeAliasDecl(TypeAliasDecl *TAD) {
    checkGenericParamAccess(TAD, TAD);

    checkTypeAccess(TAD->getUnderlyingType(),
                    TAD->getUnderlyingTypeRepr(), TAD, /*mayBeInferred*/false,
                    [&](AccessScope typeAccessScope,
                        const TypeRepr *complainRepr,
                        DowngradeToWarning downgradeToWarning) {
      auto typeAccess = typeAccessScope.accessLevelForDiagnostics();
      bool isExplicit =
        TAD->getAttrs().hasAttribute<AccessControlAttr>() ||
        isa<ProtocolDecl>(TAD->getDeclContext());
      auto diagID = diag::type_alias_underlying_type_access;
      if (downgradeToWarning == DowngradeToWarning::Yes)
        diagID = diag::type_alias_underlying_type_access_warn;
      auto aliasAccess = isExplicit
        ? TAD->getFormalAccess()
        : typeAccessScope.requiredAccessForDiagnostics();
      auto diag = TAD->diagnose(diagID, isExplicit, aliasAccess, typeAccess,
                                isa<FileUnit>(TAD->getDeclContext()));
      highlightOffendingType(diag, complainRepr);
    });
  }

  void visitOpaqueTypeDecl(OpaqueTypeDecl *OTD) {
    // TODO(opaque): The constraint class/protocols on the opaque interface, as
    // well as the naming decl for the opaque type, need to be accessible.
  }

  void visitAssociatedTypeDecl(AssociatedTypeDecl *assocType) {
    // This must stay in sync with diag::associated_type_access.
    enum {
      ACEK_DefaultDefinition = 0,
      ACEK_Requirement
    } accessControlErrorKind;
    auto minAccessScope = AccessScope::getPublic();
    const TypeRepr *complainRepr = nullptr;
    auto downgradeToWarning = DowngradeToWarning::No;

    std::for_each(assocType->getInherited().begin(),
                  assocType->getInherited().end(),
                  [&](TypeLoc requirement) {
      checkTypeAccess(requirement, assocType, /*mayBeInferred*/false,
                      [&](AccessScope typeAccessScope,
                          const TypeRepr *thisComplainRepr,
                          DowngradeToWarning downgradeDiag) {
        if (typeAccessScope.isChildOf(minAccessScope) ||
            (!complainRepr &&
             typeAccessScope.hasEqualDeclContextWith(minAccessScope))) {
          minAccessScope = typeAccessScope;
          complainRepr = thisComplainRepr;
          accessControlErrorKind = ACEK_Requirement;
          downgradeToWarning = downgradeDiag;
        }
      });
    });
    checkTypeAccess(assocType->getDefaultDefinitionType(),
                    assocType->getDefaultDefinitionTypeRepr(), assocType,
                    /*mayBeInferred*/false,
                    [&](AccessScope typeAccessScope,
                        const TypeRepr *thisComplainRepr,
                        DowngradeToWarning downgradeDiag) {
      if (typeAccessScope.isChildOf(minAccessScope) ||
          (!complainRepr &&
           typeAccessScope.hasEqualDeclContextWith(minAccessScope))) {
        minAccessScope = typeAccessScope;
        complainRepr = thisComplainRepr;
        accessControlErrorKind = ACEK_DefaultDefinition;
        downgradeToWarning = downgradeDiag;
      }
    });

    checkRequirementAccess(assocType,
                           assocType->getFormalAccessScope(),
                           assocType->getDeclContext(),
                           [&](AccessScope typeAccessScope,
                               const TypeRepr *thisComplainRepr,
                               DowngradeToWarning downgradeDiag) {
      if (typeAccessScope.isChildOf(minAccessScope) ||
          (!complainRepr &&
           typeAccessScope.hasEqualDeclContextWith(minAccessScope))) {
        minAccessScope = typeAccessScope;
        complainRepr = thisComplainRepr;
        accessControlErrorKind = ACEK_Requirement;
        downgradeToWarning = downgradeDiag;

        // Swift versions before 5.0 did not check requirements on the
        // protocol's where clause, so emit a warning.
        if (!assocType->getASTContext().isSwiftVersionAtLeast(5))
          downgradeToWarning = DowngradeToWarning::Yes;
      }
    });

    if (!minAccessScope.isPublic()) {
      auto minAccess = minAccessScope.accessLevelForDiagnostics();
      auto diagID = diag::associated_type_access;
      if (downgradeToWarning == DowngradeToWarning::Yes)
        diagID = diag::associated_type_access_warn;
      auto diag = assocType->diagnose(diagID, assocType->getFormalAccess(),
                                      minAccess, accessControlErrorKind);
      highlightOffendingType(diag, complainRepr);
    }
  }

  void visitEnumDecl(EnumDecl *ED) {
    checkGenericParamAccess(ED, ED);

    if (ED->hasRawType()) {
      Type rawType = ED->getRawType();
      auto rawTypeLocIter = std::find_if(ED->getInherited().begin(),
                                         ED->getInherited().end(),
                                         [&](TypeLoc inherited) {
        if (!inherited.wasValidated())
          return false;
        return inherited.getType().getPointer() == rawType.getPointer();
      });
      if (rawTypeLocIter == ED->getInherited().end())
        return;
      checkTypeAccess(rawType, rawTypeLocIter->getTypeRepr(), ED,
                      /*mayBeInferred*/false,
                      [&](AccessScope typeAccessScope,
                          const TypeRepr *complainRepr,
                          DowngradeToWarning downgradeToWarning) {
        auto typeAccess = typeAccessScope.accessLevelForDiagnostics();
        bool isExplicit = ED->getAttrs().hasAttribute<AccessControlAttr>();
        auto diagID = diag::enum_raw_type_access;
        if (downgradeToWarning == DowngradeToWarning::Yes)
          diagID = diag::enum_raw_type_access_warn;
        auto enumDeclAccess = isExplicit
          ? ED->getFormalAccess()
          : typeAccessScope.requiredAccessForDiagnostics();
        auto diag = ED->diagnose(diagID, isExplicit, enumDeclAccess, typeAccess,
                                 isa<FileUnit>(ED->getDeclContext()));
        highlightOffendingType(diag, complainRepr);
      });
    }
  }

  void visitStructDecl(StructDecl *SD) {
    checkGenericParamAccess(SD, SD);
  }

  void visitClassDecl(ClassDecl *CD) {
    checkGenericParamAccess(CD, CD);

    if (const NominalTypeDecl *superclassDecl = CD->getSuperclassDecl()) {
      // Be slightly defensive here in the presence of badly-ordered
      // inheritance clauses.
      auto superclassLocIter = std::find_if(CD->getInherited().begin(),
                                            CD->getInherited().end(),
                                            [&](TypeLoc inherited) {
        if (!inherited.wasValidated())
          return false;
        Type ty = inherited.getType();
        if (ty->is<ProtocolCompositionType>())
          if (auto superclass = ty->getExistentialLayout().explicitSuperclass)
            ty = superclass;
        return ty->getAnyNominal() == superclassDecl;
      });
      // Sanity check: we couldn't find the superclass for whatever reason
      // (possibly because it's synthetic or something), so don't bother
      // checking it.
      if (superclassLocIter == CD->getInherited().end())
        return;

      auto outerDowngradeToWarning = DowngradeToWarning::No;
      if (superclassDecl->isGenericContext() &&
          !CD->getASTContext().isSwiftVersionAtLeast(5)) {
        // Swift 4 failed to properly check this if the superclass was generic,
        // because the above loop was too strict.
        outerDowngradeToWarning = DowngradeToWarning::Yes;
      }

      checkTypeAccess(CD->getSuperclass(), superclassLocIter->getTypeRepr(), CD,
                      /*mayBeInferred*/false,
                      [&](AccessScope typeAccessScope,
                          const TypeRepr *complainRepr,
                          DowngradeToWarning downgradeToWarning) {
        auto typeAccess = typeAccessScope.accessLevelForDiagnostics();
        bool isExplicit = CD->getAttrs().hasAttribute<AccessControlAttr>();
        auto diagID = diag::class_super_access;
        if (downgradeToWarning == DowngradeToWarning::Yes ||
            outerDowngradeToWarning == DowngradeToWarning::Yes)
          diagID = diag::class_super_access_warn;
        auto classDeclAccess = isExplicit
          ? CD->getFormalAccess()
          : typeAccessScope.requiredAccessForDiagnostics();

        auto diag =
            CD->diagnose(diagID, isExplicit, classDeclAccess, typeAccess,
                         isa<FileUnit>(CD->getDeclContext()),
                         superclassLocIter->getTypeRepr() != complainRepr);
        highlightOffendingType(diag, complainRepr);
      });
    }
  }

  void visitProtocolDecl(ProtocolDecl *proto) {
    // This must stay in sync with diag::protocol_access.
    enum {
      PCEK_Refine = 0,
      PCEK_Requirement
    } protocolControlErrorKind;

    auto minAccessScope = AccessScope::getPublic();
    const TypeRepr *complainRepr = nullptr;
    auto downgradeToWarning = DowngradeToWarning::No;
    DescriptiveDeclKind declKind = DescriptiveDeclKind::Protocol;

    // FIXME: Hack to ensure that we've computed the types involved here.
    ASTContext &ctx = proto->getASTContext();
    for (unsigned i : indices(proto->getInherited())) {
      (void)evaluateOrDefault(ctx.evaluator,
                              InheritedTypeRequest{
                                proto, i, TypeResolutionStage::Interface},
                              Type());
    }

    auto declKindForType = [](Type type) {
      if (isa<TypeAliasType>(type.getPointer()))
        return DescriptiveDeclKind::TypeAlias;
      else if (auto nominal = type->getAnyNominal())
        return nominal->getDescriptiveKind();
      else
        return DescriptiveDeclKind::Type;
    };

    std::for_each(proto->getInherited().begin(),
                  proto->getInherited().end(),
                  [&](TypeLoc requirement) {
      checkTypeAccess(requirement, proto, /*mayBeInferred*/false,
                      [&](AccessScope typeAccessScope,
                          const TypeRepr *thisComplainRepr,
                          DowngradeToWarning downgradeDiag) {
        if (typeAccessScope.isChildOf(minAccessScope) ||
            (!complainRepr &&
             typeAccessScope.hasEqualDeclContextWith(minAccessScope))) {
          minAccessScope = typeAccessScope;
          complainRepr = thisComplainRepr;
          protocolControlErrorKind = PCEK_Refine;
          downgradeToWarning = downgradeDiag;
          declKind = declKindForType(requirement.getType());
        }
      });
    });

    forAllRequirementTypes(proto, [&](Type type, TypeRepr *typeRepr) {
      checkTypeAccess(
          type, typeRepr, proto,
          /*mayBeInferred*/ false,
          [&](AccessScope typeAccessScope, const TypeRepr *thisComplainRepr,
              DowngradeToWarning downgradeDiag) {
            if (typeAccessScope.isChildOf(minAccessScope) ||
                (!complainRepr &&
                 typeAccessScope.hasEqualDeclContextWith(minAccessScope))) {
              minAccessScope = typeAccessScope;
              complainRepr = thisComplainRepr;
              protocolControlErrorKind = PCEK_Requirement;
              downgradeToWarning = downgradeDiag;
              declKind = declKindForType(type);
              // Swift versions before 5.0 did not check requirements on the
              // protocol's where clause, so emit a warning.
              if (!proto->getASTContext().isSwiftVersionAtLeast(5))
                downgradeToWarning = DowngradeToWarning::Yes;
            }
          });
    });

    if (!minAccessScope.isPublic()) {
      auto minAccess = minAccessScope.accessLevelForDiagnostics();
      bool isExplicit = proto->getAttrs().hasAttribute<AccessControlAttr>();
      auto protoAccess = isExplicit
          ? proto->getFormalAccess()
          : minAccessScope.requiredAccessForDiagnostics();
      auto diagID = diag::protocol_access;
      if (downgradeToWarning == DowngradeToWarning::Yes)
        diagID = diag::protocol_access_warn;
      auto diag = proto->diagnose(
          diagID, isExplicit, protoAccess, protocolControlErrorKind, minAccess,
          isa<FileUnit>(proto->getDeclContext()), declKind);
      highlightOffendingType(diag, complainRepr);
    }
  }

  void visitSubscriptDecl(SubscriptDecl *SD) {
    checkGenericParamAccess(SD, SD);

    auto minAccessScope = AccessScope::getPublic();
    const TypeRepr *complainRepr = nullptr;
    auto downgradeToWarning = DowngradeToWarning::No;
    bool problemIsElement = false;

    for (auto &P : *SD->getIndices()) {
      checkTypeAccess(
          P->getInterfaceType(), P->getTypeRepr(), SD, /*mayBeInferred*/ false,
          [&](AccessScope typeAccessScope, const TypeRepr *thisComplainRepr,
              DowngradeToWarning downgradeDiag) {
            if (typeAccessScope.isChildOf(minAccessScope) ||
                (!complainRepr &&
                 typeAccessScope.hasEqualDeclContextWith(minAccessScope))) {
              minAccessScope = typeAccessScope;
              complainRepr = thisComplainRepr;
              downgradeToWarning = downgradeDiag;
            }
          });
    }

    checkTypeAccess(SD->getElementTypeLoc(), SD, /*mayBeInferred*/false,
                    [&](AccessScope typeAccessScope,
                        const TypeRepr *thisComplainRepr,
                        DowngradeToWarning downgradeDiag) {
      if (typeAccessScope.isChildOf(minAccessScope) ||
          (!complainRepr &&
           typeAccessScope.hasEqualDeclContextWith(minAccessScope))) {
        minAccessScope = typeAccessScope;
        complainRepr = thisComplainRepr;
        downgradeToWarning = downgradeDiag;
        problemIsElement = true;
      }
    });

    if (!minAccessScope.isPublic()) {
      auto minAccess = minAccessScope.accessLevelForDiagnostics();
      bool isExplicit =
        SD->getAttrs().hasAttribute<AccessControlAttr>() ||
        isa<ProtocolDecl>(SD->getDeclContext());
      auto diagID = diag::subscript_type_access;
      if (downgradeToWarning == DowngradeToWarning::Yes)
        diagID = diag::subscript_type_access_warn;
      auto subscriptDeclAccess = isExplicit
        ? SD->getFormalAccess()
        : minAccessScope.requiredAccessForDiagnostics();
      auto diag = SD->diagnose(diagID, isExplicit, subscriptDeclAccess,
                               minAccess, problemIsElement);
      highlightOffendingType(diag, complainRepr);
    }
  }

  void visitAbstractFunctionDecl(AbstractFunctionDecl *fn) {
    bool isTypeContext = fn->getDeclContext()->isTypeContext();

    checkGenericParamAccess(fn, fn);

    // This must stay in sync with diag::function_type_access.
    enum {
      FK_Function = 0,
      FK_Method,
      FK_Initializer
    };

    auto minAccessScope = AccessScope::getPublic();
    const TypeRepr *complainRepr = nullptr;
    auto downgradeToWarning = DowngradeToWarning::No;

    for (auto *P : *fn->getParameters()) {
      checkTypeAccess(
          P->getInterfaceType(), P->getTypeRepr(), fn, /*mayBeInferred*/ false,
          [&](AccessScope typeAccessScope, const TypeRepr *thisComplainRepr,
              DowngradeToWarning downgradeDiag) {
            if (typeAccessScope.isChildOf(minAccessScope) ||
                (!complainRepr &&
                 typeAccessScope.hasEqualDeclContextWith(minAccessScope))) {
              minAccessScope = typeAccessScope;
              complainRepr = thisComplainRepr;
              downgradeToWarning = downgradeDiag;
            }
          });
    }

    bool problemIsResult = false;
    if (auto FD = dyn_cast<FuncDecl>(fn)) {
      checkTypeAccess(FD->getBodyResultTypeLoc(), FD, /*mayBeInferred*/false,
                      [&](AccessScope typeAccessScope,
                          const TypeRepr *thisComplainRepr,
                          DowngradeToWarning downgradeDiag) {
        if (typeAccessScope.isChildOf(minAccessScope) ||
            (!complainRepr &&
             typeAccessScope.hasEqualDeclContextWith(minAccessScope))) {
          minAccessScope = typeAccessScope;
          complainRepr = thisComplainRepr;
          downgradeToWarning = downgradeDiag;
          problemIsResult = true;
        }
      });
    }

    if (!minAccessScope.isPublic()) {
      auto minAccess = minAccessScope.accessLevelForDiagnostics();
      auto functionKind = isa<ConstructorDecl>(fn)
        ? FK_Initializer
        : isTypeContext ? FK_Method : FK_Function;
      bool isExplicit =
        fn->getAttrs().hasAttribute<AccessControlAttr>() ||
        isa<ProtocolDecl>(fn->getDeclContext());
      auto diagID = diag::function_type_access;
      if (downgradeToWarning == DowngradeToWarning::Yes)
        diagID = diag::function_type_access_warn;
      auto fnAccess = isExplicit
        ? fn->getFormalAccess()
        : minAccessScope.requiredAccessForDiagnostics();
      auto diag = fn->diagnose(diagID, isExplicit, fnAccess,
                               isa<FileUnit>(fn->getDeclContext()), minAccess,
                               functionKind, problemIsResult);
      highlightOffendingType(diag, complainRepr);
    }
  }

  void visitEnumElementDecl(EnumElementDecl *EED) {
    if (!EED->hasAssociatedValues())
      return;
    for (auto &P : *EED->getParameterList()) {
      checkTypeAccess(
          P->getInterfaceType(), P->getTypeRepr(), EED, /*mayBeInferred*/ false,
          [&](AccessScope typeAccessScope, const TypeRepr *complainRepr,
              DowngradeToWarning downgradeToWarning) {
            auto typeAccess = typeAccessScope.accessLevelForDiagnostics();
            auto diagID = diag::enum_case_access;
            if (downgradeToWarning == DowngradeToWarning::Yes)
              diagID = diag::enum_case_access_warn;
            auto diag =
                EED->diagnose(diagID, EED->getFormalAccess(), typeAccess);
            highlightOffendingType(diag, complainRepr);
          });
    }
  }
};

class UsableFromInlineChecker : public AccessControlCheckerBase,
                                public DeclVisitor<UsableFromInlineChecker> {
public:
  UsableFromInlineChecker()
      : AccessControlCheckerBase(/*checkUsableFromInline=*/true) {}

  static bool shouldSkipChecking(const ValueDecl *VD) {
    if (VD->getFormalAccess() != AccessLevel::Internal)
      return true;
    return !VD->isUsableFromInline();
  };

  void visit(Decl *D) {
    if (!D->getASTContext().isSwiftVersionAtLeast(4, 2))
      return;

    if (D->isInvalid() || D->isImplicit())
      return;

    if (auto *VD = dyn_cast<ValueDecl>(D))
      if (shouldSkipChecking(VD))
        return;

    DeclVisitor<UsableFromInlineChecker>::visit(D);
  }

  // Force all kinds to be handled at a lower level.
  void visitDecl(Decl *D) = delete;
  void visitValueDecl(ValueDecl *D) = delete;

#define UNREACHABLE(KIND, REASON) \
  void visit##KIND##Decl(KIND##Decl *D) { \
    llvm_unreachable(REASON); \
  }
  UNREACHABLE(Import, "cannot appear in a type context")
  UNREACHABLE(Extension, "cannot appear in a type context")
  UNREACHABLE(TopLevelCode, "cannot appear in a type context")
  UNREACHABLE(Operator, "cannot appear in a type context")
  UNREACHABLE(PrecedenceGroup, "cannot appear in a type context")
  UNREACHABLE(Module, "cannot appear in a type context")

  UNREACHABLE(Param, "does not have access control")
  UNREACHABLE(GenericTypeParam, "does not have access control")
  UNREACHABLE(MissingMember, "does not have access control")
#undef UNREACHABLE

#define UNINTERESTING(KIND) \
  void visit##KIND##Decl(KIND##Decl *D) {}

  UNINTERESTING(IfConfig) // Does not have access control.
  UNINTERESTING(PoundDiagnostic) // Does not have access control.
  UNINTERESTING(EnumCase) // Handled at the EnumElement level.
  UNINTERESTING(Var) // Handled at the PatternBinding level.
  UNINTERESTING(Destructor) // Always correct.
  UNINTERESTING(Accessor) // Handled by the Var or Subscript.
  UNINTERESTING(OpaqueType) // Handled by the Var or Subscript.

  /// If \p PBD declared stored instance properties in a fixed-contents struct,
  /// return said struct.
  static const StructDecl *
  getFixedLayoutStructContext(const PatternBindingDecl *PBD) {
    auto *parentStruct = dyn_cast<StructDecl>(PBD->getDeclContext());
    if (!parentStruct)
      return nullptr;
    if (!(parentStruct->getAttrs().hasAttribute<FrozenAttr>() ||         
          parentStruct->getAttrs().hasAttribute<FixedLayoutAttr>()) ||
        PBD->isStatic() || !PBD->hasStorage()) {
      return nullptr;
    }
    // We don't check for "in resilient modules" because there's no reason to
    // write '@_fixedLayout' on a struct in a non-resilient module.
    return parentStruct;
  }

  /// \see visitPatternBindingDecl
  void checkNamedPattern(const NamedPattern *NP,
                         const ValueDecl *fixedLayoutStructContext,
                         bool isTypeContext,
                         const llvm::DenseSet<const VarDecl *> &seenVars) {
    const VarDecl *theVar = NP->getDecl();
    if (!fixedLayoutStructContext && shouldSkipChecking(theVar))
      return;
    // Only check individual variables if we didn't check an enclosing
    // TypedPattern.
    if (seenVars.count(theVar) || theVar->isInvalid())
      return;

    checkTypeAccess(
        theVar->getInterfaceType(), nullptr,
        fixedLayoutStructContext ? fixedLayoutStructContext : theVar,
        /*mayBeInferred*/ false,
        [&](AccessScope typeAccessScope, const TypeRepr *complainRepr,
            DowngradeToWarning downgradeToWarning) {
          auto &Ctx = theVar->getASTContext();
          auto diagID = diag::pattern_type_not_usable_from_inline_inferred;
          if (fixedLayoutStructContext) {
            diagID = diag::pattern_type_not_usable_from_inline_inferred_frozen;
          } else if (!Ctx.isSwiftVersionAtLeast(5)) {
            diagID = diag::pattern_type_not_usable_from_inline_inferred_warn;
          }
          Ctx.Diags.diagnose(NP->getLoc(), diagID, theVar->isLet(),
                             isTypeContext, theVar->getInterfaceType());
        });
  }

  /// \see visitPatternBindingDecl
  void checkTypedPattern(const TypedPattern *TP,
                         const ValueDecl *fixedLayoutStructContext,
                         bool isTypeContext,
                         llvm::DenseSet<const VarDecl *> &seenVars) {
    // FIXME: We need an access level to check against, so we pull one out
    // of some random VarDecl in the pattern. They're all going to be the
    // same, but still, ick.
    VarDecl *anyVar = nullptr;
    TP->forEachVariable([&](VarDecl *V) {
      seenVars.insert(V);
      anyVar = V;
    });
    if (!anyVar)
      return;
    if (!fixedLayoutStructContext && shouldSkipChecking(anyVar))
      return;

    checkTypeAccess(
        TP->getTypeLoc(),
        fixedLayoutStructContext ? fixedLayoutStructContext : anyVar,
        /*mayBeInferred*/ true,
        [&](AccessScope typeAccessScope, const TypeRepr *complainRepr,
            DowngradeToWarning downgradeToWarning) {
          auto &Ctx = anyVar->getASTContext();
          auto diagID = diag::pattern_type_not_usable_from_inline;
          if (fixedLayoutStructContext)
            diagID = diag::pattern_type_not_usable_from_inline_frozen;
          else if (!Ctx.isSwiftVersionAtLeast(5))
            diagID = diag::pattern_type_not_usable_from_inline_warn;
          auto diag = Ctx.Diags.diagnose(TP->getLoc(), diagID, anyVar->isLet(),
                                         isTypeContext);
          highlightOffendingType(diag, complainRepr);
        });

    for (auto attr : anyVar->getAttachedPropertyWrappers()) {
      checkTypeAccess(attr->getTypeLoc(),
                      fixedLayoutStructContext ? fixedLayoutStructContext
                                               : anyVar,
                      /*mayBeInferred*/false,
                      [&](AccessScope typeAccessScope,
                          const TypeRepr *complainRepr,
                          DowngradeToWarning downgradeToWarning) {
        auto diag = anyVar->diagnose(
            diag::property_wrapper_type_not_usable_from_inline,
            anyVar->isLet(), isTypeContext);
        highlightOffendingType(diag, complainRepr);
      });
    }
  }

  void visitPatternBindingDecl(PatternBindingDecl *PBD) {
    bool isTypeContext = PBD->getDeclContext()->isTypeContext();

    // Stored instance properties in public/@usableFromInline fixed-contents
    // structs in resilient modules must always use public/@usableFromInline
    // types. In these cases, check the access against the struct instead of the
    // VarDecl, and customize the diagnostics.
    const ValueDecl *fixedLayoutStructContext =
        getFixedLayoutStructContext(PBD);

    llvm::DenseSet<const VarDecl *> seenVars;
    for (auto idx : range(PBD->getNumPatternEntries())) {
      PBD->getPattern(idx)->forEachNode([&](const Pattern *P) {
        if (auto *NP = dyn_cast<NamedPattern>(P)) {
          checkNamedPattern(NP, fixedLayoutStructContext, isTypeContext,
                            seenVars);
          return;
        }

        auto *TP = dyn_cast<TypedPattern>(P);
        if (!TP)
          return;
        checkTypedPattern(TP, fixedLayoutStructContext, isTypeContext,
                          seenVars);
      });
      seenVars.clear();
    }
  }

  void visitTypeAliasDecl(TypeAliasDecl *TAD) {
    checkGenericParamAccess(TAD, TAD);

    checkTypeAccess(TAD->getUnderlyingType(),
                    TAD->getUnderlyingTypeRepr(), TAD, /*mayBeInferred*/false,
                    [&](AccessScope typeAccessScope,
                        const TypeRepr *complainRepr,
                        DowngradeToWarning downgradeToWarning) {
      auto diagID = diag::type_alias_underlying_type_not_usable_from_inline;
      if (!TAD->getASTContext().isSwiftVersionAtLeast(5))
        diagID = diag::type_alias_underlying_type_not_usable_from_inline_warn;
      auto diag = TAD->diagnose(diagID);
      highlightOffendingType(diag, complainRepr);
    });
  }

  void visitAssociatedTypeDecl(AssociatedTypeDecl *assocType) {
    // This must stay in sync with diag::associated_type_not_usable_from_inline.
    enum {
      ACEK_DefaultDefinition = 0,
      ACEK_Requirement
    };

    std::for_each(assocType->getInherited().begin(),
                  assocType->getInherited().end(),
                  [&](TypeLoc requirement) {
      checkTypeAccess(requirement, assocType, /*mayBeInferred*/false,
                      [&](AccessScope typeAccessScope,
            const TypeRepr *complainRepr,
            DowngradeToWarning downgradeDiag) {
        auto diagID = diag::associated_type_not_usable_from_inline;
        if (!assocType->getASTContext().isSwiftVersionAtLeast(5))
          diagID = diag::associated_type_not_usable_from_inline_warn;
        auto diag = assocType->diagnose(diagID, ACEK_Requirement);
        highlightOffendingType(diag, complainRepr);
      });
    });
    checkTypeAccess(assocType->getDefaultDefinitionType(),
                    assocType->getDefaultDefinitionTypeRepr(), assocType,
                     /*mayBeInferred*/false,
                    [&](AccessScope typeAccessScope,
                        const TypeRepr *complainRepr,
                        DowngradeToWarning downgradeDiag) {
      auto diagID = diag::associated_type_not_usable_from_inline;
      if (!assocType->getASTContext().isSwiftVersionAtLeast(5))
        diagID = diag::associated_type_not_usable_from_inline_warn;
      auto diag = assocType->diagnose(diagID, ACEK_DefaultDefinition);
      highlightOffendingType(diag, complainRepr);
    });

    if (assocType->getTrailingWhereClause()) {
      auto accessScope =
        assocType->getFormalAccessScope(nullptr);
      checkRequirementAccess(assocType,
                             accessScope,
                             assocType->getDeclContext(),
                             [&](AccessScope typeAccessScope,
                                 const TypeRepr *complainRepr,
                                 DowngradeToWarning downgradeDiag) {
        auto diagID = diag::associated_type_not_usable_from_inline;
        if (!assocType->getASTContext().isSwiftVersionAtLeast(5))
          diagID = diag::associated_type_not_usable_from_inline_warn;
        auto diag = assocType->diagnose(diagID, ACEK_Requirement);
        highlightOffendingType(diag, complainRepr);
      });
    }
  }

  void visitEnumDecl(const EnumDecl *ED) {
    checkGenericParamAccess(ED, ED);

    if (ED->hasRawType()) {
      Type rawType = ED->getRawType();
      auto rawTypeLocIter = std::find_if(ED->getInherited().begin(),
                                         ED->getInherited().end(),
                                         [&](TypeLoc inherited) {
        if (!inherited.wasValidated())
          return false;
        return inherited.getType().getPointer() == rawType.getPointer();
      });
      if (rawTypeLocIter == ED->getInherited().end())
        return;
      checkTypeAccess(rawType, rawTypeLocIter->getTypeRepr(), ED,
                       /*mayBeInferred*/false,
                      [&](AccessScope typeAccessScope,
                          const TypeRepr *complainRepr,
                          DowngradeToWarning downgradeToWarning) {
        auto diagID = diag::enum_raw_type_not_usable_from_inline;
        if (!ED->getASTContext().isSwiftVersionAtLeast(5))
          diagID = diag::enum_raw_type_not_usable_from_inline_warn;
        auto diag = ED->diagnose(diagID);
        highlightOffendingType(diag, complainRepr);
      });
    }
  }

  void visitStructDecl(StructDecl *SD) {
    checkGenericParamAccess(SD, SD);
  }

  void visitClassDecl(ClassDecl *CD) {
    checkGenericParamAccess(CD, CD);

    if (CD->hasSuperclass()) {
      const NominalTypeDecl *superclassDecl = CD->getSuperclassDecl();
      // Be slightly defensive here in the presence of badly-ordered
      // inheritance clauses.
      auto superclassLocIter = std::find_if(CD->getInherited().begin(),
                                            CD->getInherited().end(),
                                            [&](TypeLoc inherited) {
        if (!inherited.wasValidated())
          return false;
        Type ty = inherited.getType();
        if (ty->is<ProtocolCompositionType>())
          if (auto superclass = ty->getExistentialLayout().explicitSuperclass)
            ty = superclass;
        return ty->getAnyNominal() == superclassDecl;
      });
      // Sanity check: we couldn't find the superclass for whatever reason
      // (possibly because it's synthetic or something), so don't bother
      // checking it.
      if (superclassLocIter == CD->getInherited().end())
        return;

      checkTypeAccess(CD->getSuperclass(), superclassLocIter->getTypeRepr(), CD,
                       /*mayBeInferred*/false,
                      [&](AccessScope typeAccessScope,
                          const TypeRepr *complainRepr,
                          DowngradeToWarning downgradeToWarning) {
        auto diagID = diag::class_super_not_usable_from_inline;
        if (!CD->getASTContext().isSwiftVersionAtLeast(5))
          diagID = diag::class_super_not_usable_from_inline_warn;
        auto diag = CD->diagnose(diagID, superclassLocIter->getTypeRepr() !=
                                             complainRepr);
        highlightOffendingType(diag, complainRepr);
      });
    }
  }

  void visitProtocolDecl(ProtocolDecl *proto) {
    // This must stay in sync with diag::protocol_usable_from_inline.
    enum {
      PCEK_Refine = 0,
      PCEK_Requirement
    };

    std::for_each(proto->getInherited().begin(),
                  proto->getInherited().end(),
                  [&](TypeLoc requirement) {
      checkTypeAccess(requirement, proto, /*mayBeInferred*/false,
                      [&](AccessScope typeAccessScope,
                          const TypeRepr *complainRepr,
                          DowngradeToWarning downgradeDiag) {
        auto diagID = diag::protocol_usable_from_inline;
        if (!proto->getASTContext().isSwiftVersionAtLeast(5))
          diagID = diag::protocol_usable_from_inline_warn;
        auto diag = proto->diagnose(diagID, PCEK_Refine);
        highlightOffendingType(diag, complainRepr);
      });
    });

    if (proto->getTrailingWhereClause()) {
      auto accessScope = proto->getFormalAccessScope(nullptr,
                                                     /*checkUsableFromInline*/true);
      checkRequirementAccess(proto,
                             accessScope,
                             proto->getDeclContext(),
                             [&](AccessScope typeAccessScope,
                                 const TypeRepr *complainRepr,
                                 DowngradeToWarning downgradeDiag) {
        auto diagID = diag::protocol_usable_from_inline;
        if (!proto->getASTContext().isSwiftVersionAtLeast(5))
          diagID = diag::protocol_usable_from_inline_warn;
        auto diag = proto->diagnose(diagID, PCEK_Requirement);
        highlightOffendingType(diag, complainRepr);
      });
    }
  }

  void visitSubscriptDecl(SubscriptDecl *SD) {
    checkGenericParamAccess(SD, SD);

    for (auto &P : *SD->getIndices()) {
      checkTypeAccess(
          P->getInterfaceType(), P->getTypeRepr(), SD, /*mayBeInferred*/ false,
          [&](AccessScope typeAccessScope, const TypeRepr *complainRepr,
              DowngradeToWarning downgradeDiag) {
            auto diagID = diag::subscript_type_usable_from_inline;
            if (!SD->getASTContext().isSwiftVersionAtLeast(5))
              diagID = diag::subscript_type_usable_from_inline_warn;
            auto diag = SD->diagnose(diagID, /*problemIsElement=*/false);
            highlightOffendingType(diag, complainRepr);
          });
    }

    checkTypeAccess(SD->getElementTypeLoc(), SD, /*mayBeInferred*/false,
                    [&](AccessScope typeAccessScope,
                        const TypeRepr *complainRepr,
                        DowngradeToWarning downgradeDiag) {
      auto diagID = diag::subscript_type_usable_from_inline;
      if (!SD->getASTContext().isSwiftVersionAtLeast(5))
        diagID = diag::subscript_type_usable_from_inline_warn;
      auto diag = SD->diagnose(diagID, /*problemIsElement=*/true);
      highlightOffendingType(diag, complainRepr);
    });
  }

  void visitAbstractFunctionDecl(AbstractFunctionDecl *fn) {
    bool isTypeContext = fn->getDeclContext()->isTypeContext();

    checkGenericParamAccess(fn, fn);

    // This must stay in sync with diag::function_type_usable_from_inline.
    enum {
      FK_Function = 0,
      FK_Method,
      FK_Initializer
    };

    auto functionKind = isa<ConstructorDecl>(fn)
      ? FK_Initializer
      : isTypeContext ? FK_Method : FK_Function;

    for (auto *P : *fn->getParameters()) {
      checkTypeAccess(
          P->getInterfaceType(), P->getTypeRepr(), fn, /*mayBeInferred*/ false,
          [&](AccessScope typeAccessScope, const TypeRepr *complainRepr,
              DowngradeToWarning downgradeDiag) {
            auto diagID = diag::function_type_usable_from_inline;
            if (!fn->getASTContext().isSwiftVersionAtLeast(5))
              diagID = diag::function_type_usable_from_inline_warn;
            auto diag = fn->diagnose(diagID, functionKind,
                                     /*problemIsResult=*/false);
            highlightOffendingType(diag, complainRepr);
          });
    }

    if (auto FD = dyn_cast<FuncDecl>(fn)) {
      checkTypeAccess(FD->getBodyResultTypeLoc(), FD, /*mayBeInferred*/false,
                      [&](AccessScope typeAccessScope,
                          const TypeRepr *complainRepr,
                          DowngradeToWarning downgradeDiag) {
        auto diagID = diag::function_type_usable_from_inline;
        if (!fn->getASTContext().isSwiftVersionAtLeast(5))
          diagID = diag::function_type_usable_from_inline_warn;
        auto diag = fn->diagnose(diagID, functionKind,
                                 /*problemIsResult=*/true);
        highlightOffendingType(diag, complainRepr);
      });
    }
  }

  void visitEnumElementDecl(EnumElementDecl *EED) {
    if (!EED->hasAssociatedValues())
      return;
    for (auto &P : *EED->getParameterList()) {
      checkTypeAccess(
          P->getInterfaceType(), P->getTypeRepr(), EED, /*mayBeInferred*/ false,
          [&](AccessScope typeAccessScope, const TypeRepr *complainRepr,
              DowngradeToWarning downgradeToWarning) {
            auto diagID = diag::enum_case_usable_from_inline;
            if (!EED->getASTContext().isSwiftVersionAtLeast(5))
              diagID = diag::enum_case_usable_from_inline_warn;
            auto diag = EED->diagnose(diagID);
            highlightOffendingType(diag, complainRepr);
          });
    }
  }
};

class ExportabilityChecker : public DeclVisitor<ExportabilityChecker> {
  class Diagnoser;

  void checkTypeImpl(
      Type type, const TypeRepr *typeRepr, const SourceFile &SF,
      const Diagnoser &diagnoser) {
    // Don't bother checking errors.
    if (type && type->hasError())
      return;

    bool foundAnyIssues = false;

    // Check the TypeRepr first (if present), because that will give us a
    // better diagonstic.
    if (typeRepr) {
      const_cast<TypeRepr *>(typeRepr)->walk(TypeReprIdentFinder(
          [&](const ComponentIdentTypeRepr *component) {
        ModuleDecl *M = component->getBoundDecl()->getModuleContext();
        if (!SF.isImportedImplementationOnly(M))
          return true;

        diagnoser.diagnoseType(component->getBoundDecl(), component);
        foundAnyIssues = true;
        // We still continue even in the diagnostic case to report multiple
        // violations.
        return true;
      }));
    }

    // Note that if we have a type, we can't skip checking it even if the
    // TypeRepr is okay, because that's how we check what conformances are
    // being used.
    //
    // We still don't want to do this if we found issues with the TypeRepr,
    // though, because that would result in some issues being reported twice.
    if (foundAnyIssues || !type)
      return;

    class ProblematicTypeFinder : public TypeDeclFinder {
      const SourceFile &SF;
      const Diagnoser &diagnoser;
    public:
      ProblematicTypeFinder(const SourceFile &SF, const Diagnoser &diagnoser)
        : SF(SF), diagnoser(diagnoser) {}

      void visitTypeDecl(const TypeDecl *typeDecl) {
        ModuleDecl *M = typeDecl->getModuleContext();
        if (!SF.isImportedImplementationOnly(M))
          return;

        diagnoser.diagnoseType(typeDecl, /*typeRepr*/nullptr);
      }

      void visitSubstitutionMap(SubstitutionMap subs) {
        for (ProtocolConformanceRef conformance : subs.getConformances()) {
          if (!conformance.isConcrete())
            continue;
          const ProtocolConformance *concreteConf = conformance.getConcrete();

          SubstitutionMap subConformanceSubs =
              concreteConf->getSubstitutions(SF.getParentModule());
          visitSubstitutionMap(subConformanceSubs);

          const RootProtocolConformance *rootConf =
              concreteConf->getRootConformance();
          ModuleDecl *M = rootConf->getDeclContext()->getParentModule();
          if (!SF.isImportedImplementationOnly(M))
            continue;
          diagnoser.diagnoseConformance(rootConf);
        }
      }

      Action visitNominalType(NominalType *ty) override {
        visitTypeDecl(ty->getDecl());
        return Action::Continue;
      }

      Action visitBoundGenericType(BoundGenericType *ty) override {
        visitTypeDecl(ty->getDecl());
        SubstitutionMap subs =
            ty->getContextSubstitutionMap(SF.getParentModule(), ty->getDecl());
        visitSubstitutionMap(subs);
        return Action::Continue;
      }

      Action visitTypeAliasType(TypeAliasType *ty) override {
        visitTypeDecl(ty->getDecl());
        visitSubstitutionMap(ty->getSubstitutionMap());
        return Action::Continue;
      }

      // We diagnose unserializable Clang function types in the
      // post-visitor so that we diagnose any unexportable component
      // types first.
      Action walkToTypePost(Type T) override {
        if (auto fnType = T->getAs<AnyFunctionType>()) {
          if (auto clangType = fnType->getClangFunctionType()) {
            auto loader = T->getASTContext().getClangModuleLoader();
            // Serialization will serialize the sugared type if it can,
            // but we need the canonical type to be serializable or else
            // canonicalization (e.g. in SIL) might break things.
            if (!loader->isSerializable(clangType, /*check canonical*/ true)) {
              diagnoser.diagnoseClangFunctionType(T, clangType);
            }
          }
        }
        return TypeDeclFinder::walkToTypePost(T);
      }
    };

    type.walk(ProblematicTypeFinder(SF, diagnoser));
  }

  void checkType(
      Type type, const TypeRepr *typeRepr, const Decl *context,
      const Diagnoser &diagnoser) {
    auto *SF = context->getDeclContext()->getParentSourceFile();
    assert(SF && "checking a non-source declaration?");
    return checkTypeImpl(type, typeRepr, *SF, diagnoser);
  }

  void checkType(
      const TypeLoc &TL, const Decl *context, const Diagnoser &diagnoser) {
    checkType(TL.getType(), TL.getTypeRepr(), context, diagnoser);
  }

  void checkGenericParams(const GenericContext *ownerCtx,
                          const ValueDecl *ownerDecl) {
    const auto params = ownerCtx->getGenericParams();
    if (!params)
      return;

    for (auto param : *params) {
      if (param->getInherited().empty())
        continue;
      assert(param->getInherited().size() == 1);
      checkType(param->getInherited().front(), ownerDecl,
                getDiagnoser(ownerDecl));
    }

    forAllRequirementTypes(WhereClauseOwner(
                             const_cast<GenericContext *>(ownerCtx)),
                           [&](Type type, TypeRepr *typeRepr) {
      checkType(type, typeRepr, ownerDecl, getDiagnoser(ownerDecl));
    });
  }

  // This enum must be kept in sync with
  // diag::decl_from_implementation_only_module and
  // diag::conformance_from_implementation_only_module.
  enum class Reason : unsigned {
    General,
    ExtensionWithPublicMembers,
    ExtensionWithConditionalConformances
  };

  class Diagnoser {
    const Decl *D;
    Reason reason;
  public:
    Diagnoser(const Decl *D, Reason reason) : D(D), reason(reason) {}

    void diagnoseType(const TypeDecl *offendingType,
                      const TypeRepr *complainRepr) const {
      ModuleDecl *M = offendingType->getModuleContext();
      auto diag = D->diagnose(diag::decl_from_implementation_only_module,
                              offendingType->getDescriptiveKind(),
                              offendingType->getFullName(),
                              static_cast<unsigned>(reason), M->getName());
      highlightOffendingType(diag, complainRepr);
    }

    void diagnoseConformance(const ProtocolConformance *offendingConformance) const {
      ModuleDecl *M = offendingConformance->getDeclContext()->getParentModule();
      D->diagnose(diag::conformance_from_implementation_only_module,
                  offendingConformance->getType(),
                  offendingConformance->getProtocol()->getFullName(),
                  static_cast<unsigned>(reason), M->getName());
    }

    void diagnoseClangFunctionType(Type fnType, const clang::Type *type) const {
      D->diagnose(diag::unexportable_clang_function_type, fnType);
    }
  };

  Diagnoser getDiagnoser(const Decl *D, Reason reason = Reason::General) {
    return Diagnoser(D, reason);
  }

public:
  ExportabilityChecker() {}

  static bool shouldSkipChecking(const ValueDecl *VD) {
    if (VD->getAttrs().hasAttribute<ImplementationOnlyAttr>())
      return true;

    // Accessors are handled as part of their Var or Subscript, and we don't
    // want to redo extension signature checking for them.
    if (isa<AccessorDecl>(VD))
      return true;

    // Is this part of the module's API or ABI?
    AccessScope accessScope =
        VD->getFormalAccessScope(nullptr,
                                 /*treatUsableFromInlineAsPublic*/true);
    if (accessScope.isPublic())
      return false;

    // Is this a stored property in a non-resilient struct or class?
    auto *property = dyn_cast<VarDecl>(VD);
    if (!property || !property->hasStorage() || property->isStatic())
      return true;
    auto *parentNominal = dyn_cast<NominalTypeDecl>(property->getDeclContext());
    if (!parentNominal || parentNominal->isResilient())
      return true;

    // Is that struct or class part of the module's API or ABI?
    AccessScope parentAccessScope = parentNominal->getFormalAccessScope(
        nullptr, /*treatUsableFromInlineAsPublic*/true);
    if (parentAccessScope.isPublic())
      return false;

    return true;
  }

  void checkOverride(const ValueDecl *VD) {
    const ValueDecl *overridden = VD->getOverriddenDecl();
    if (!overridden)
      return;

    auto *SF = VD->getDeclContext()->getParentSourceFile();
    assert(SF && "checking a non-source declaration?");

    ModuleDecl *M = overridden->getModuleContext();
    if (SF->isImportedImplementationOnly(M)) {
      VD->diagnose(diag::implementation_only_override_import_without_attr,
                   overridden->getDescriptiveKind())
          .fixItInsert(VD->getAttributeInsertionLoc(false),
                       "@_implementationOnly ");
      overridden->diagnose(diag::overridden_here);
      return;
    }

    if (overridden->getAttrs().hasAttribute<ImplementationOnlyAttr>()) {
      VD->diagnose(diag::implementation_only_override_without_attr,
                   overridden->getDescriptiveKind())
          .fixItInsert(VD->getAttributeInsertionLoc(false),
                       "@_implementationOnly ");
      overridden->diagnose(diag::overridden_here);
      return;
    }

    // FIXME: Check storage decls where the setter is in a separate module from
    // the getter, which is a thing Objective-C can do. The ClangImporter
    // doesn't make this easy, though, because it just gives the setter the same
    // DeclContext as the property or subscript, which means we've lost the
    // information about whether its module was implementation-only imported.
  }

  void visit(Decl *D) {
    if (D->isInvalid() || D->isImplicit())
      return;

    if (auto *VD = dyn_cast<ValueDecl>(D)) {
      if (shouldSkipChecking(VD))
        return;
      checkOverride(VD);
    }

    DeclVisitor<ExportabilityChecker>::visit(D);
  }

  // Force all kinds to be handled at a lower level.
  void visitDecl(Decl *D) = delete;
  void visitValueDecl(ValueDecl *D) = delete;

#define UNREACHABLE(KIND, REASON) \
  void visit##KIND##Decl(KIND##Decl *D) { \
    llvm_unreachable(REASON); \
  }
  UNREACHABLE(Import, "not applicable")
  UNREACHABLE(TopLevelCode, "not applicable")
  UNREACHABLE(Module, "not applicable")

  UNREACHABLE(Param, "handled by the enclosing declaration")
  UNREACHABLE(GenericTypeParam, "handled by the enclosing declaration")
  UNREACHABLE(MissingMember, "handled by the enclosing declaration")
#undef UNREACHABLE

#define UNINTERESTING(KIND) \
  void visit##KIND##Decl(KIND##Decl *D) {}

  UNINTERESTING(PrefixOperator) // Does not reference other decls.
  UNINTERESTING(PostfixOperator) // Does not reference other decls.
  UNINTERESTING(IfConfig) // Not applicable.
  UNINTERESTING(PoundDiagnostic) // Not applicable.
  UNINTERESTING(EnumCase) // Handled at the EnumElement level.
  UNINTERESTING(Destructor) // Always correct.
  UNINTERESTING(Accessor) // Handled by the Var or Subscript.
  UNINTERESTING(OpaqueType) // TODO

  // Handled at the PatternBinding level; if the pattern has a simple
  // "name: TheType" form, we can get better results by diagnosing the TypeRepr.
  UNINTERESTING(Var)

  /// \see visitPatternBindingDecl
  void checkNamedPattern(const NamedPattern *NP,
                         const llvm::DenseSet<const VarDecl *> &seenVars) {
    const VarDecl *theVar = NP->getDecl();
    if (shouldSkipChecking(theVar))
      return;

    checkOverride(theVar);

    // Only check the type of individual variables if we didn't check an
    // enclosing TypedPattern.
    if (seenVars.count(theVar) || theVar->isInvalid())
      return;

    checkType(theVar->getInterfaceType(), /*typeRepr*/nullptr, theVar,
              getDiagnoser(theVar));
  }

  /// \see visitPatternBindingDecl
  void checkTypedPattern(const TypedPattern *TP,
                         llvm::DenseSet<const VarDecl *> &seenVars) {
    // FIXME: We need to figure out if this is a stored or computed property,
    // so we pull out some random VarDecl in the pattern. They're all going to
    // be the same, but still, ick.
    const VarDecl *anyVar = nullptr;
    TP->forEachVariable([&](VarDecl *V) {
      seenVars.insert(V);
      anyVar = V;
    });
    if (!anyVar)
      return;
    if (shouldSkipChecking(anyVar))
      return;

    checkType(TP->getTypeLoc(), anyVar, getDiagnoser(anyVar));
  }

  void visitPatternBindingDecl(PatternBindingDecl *PBD) {
    llvm::DenseSet<const VarDecl *> seenVars;
    for (auto idx : range(PBD->getNumPatternEntries())) {
      PBD->getPattern(idx)->forEachNode([&](const Pattern *P) {
        if (auto *NP = dyn_cast<NamedPattern>(P)) {
          checkNamedPattern(NP, seenVars);
          return;
        }

        auto *TP = dyn_cast<TypedPattern>(P);
        if (!TP)
          return;
        checkTypedPattern(TP, seenVars);
      });
      seenVars.clear();
    }
  }

  void visitTypeAliasDecl(TypeAliasDecl *TAD) {
    checkGenericParams(TAD, TAD);
    checkType(TAD->getUnderlyingType(),
              TAD->getUnderlyingTypeRepr(), TAD, getDiagnoser(TAD));
  }

  void visitAssociatedTypeDecl(AssociatedTypeDecl *assocType) {
    llvm::for_each(assocType->getInherited(),
                   [&](TypeLoc requirement) {
      checkType(requirement, assocType, getDiagnoser(assocType));
    });
    checkType(assocType->getDefaultDefinitionType(),
              assocType->getDefaultDefinitionTypeRepr(), assocType,
              getDiagnoser(assocType));

    if (assocType->getTrailingWhereClause()) {
      forAllRequirementTypes(assocType,
                             [&](Type type, TypeRepr *typeRepr) {
        checkType(type, typeRepr, assocType, getDiagnoser(assocType));
      });
    }
  }

  void visitNominalTypeDecl(const NominalTypeDecl *nominal) {
    checkGenericParams(nominal, nominal);

    llvm::for_each(nominal->getInherited(),
                   [&](TypeLoc nextInherited) {
      checkType(nextInherited, nominal, getDiagnoser(nominal));
    });
  }

  void visitProtocolDecl(ProtocolDecl *proto) {
    llvm::for_each(proto->getInherited(),
                  [&](TypeLoc requirement) {
      checkType(requirement, proto, getDiagnoser(proto));
    });

    if (proto->getTrailingWhereClause()) {
      forAllRequirementTypes(proto, [&](Type type, TypeRepr *typeRepr) {
        checkType(type, typeRepr, proto, getDiagnoser(proto));
      });
    }
  }

  void visitSubscriptDecl(SubscriptDecl *SD) {
    checkGenericParams(SD, SD);

    for (auto &P : *SD->getIndices()) {
      checkType(P->getInterfaceType(), P->getTypeRepr(), SD,
                getDiagnoser(SD));
    }
    checkType(SD->getElementTypeLoc(), SD, getDiagnoser(SD));
  }

  void visitAbstractFunctionDecl(AbstractFunctionDecl *fn) {
    checkGenericParams(fn, fn);

    for (auto *P : *fn->getParameters())
      checkType(P->getInterfaceType(), P->getTypeRepr(), fn,
                getDiagnoser(fn));
  }

  void visitFuncDecl(FuncDecl *FD) {
    visitAbstractFunctionDecl(FD);
    checkType(FD->getBodyResultTypeLoc(), FD, getDiagnoser(FD));
  }

  void visitEnumElementDecl(EnumElementDecl *EED) {
    if (!EED->hasAssociatedValues())
      return;
    for (auto &P : *EED->getParameterList())
      checkType(P->getInterfaceType(), P->getTypeRepr(), EED,
                getDiagnoser(EED));
  }

  void checkConstrainedExtensionRequirements(ExtensionDecl *ED,
                                             Reason reason) {
    if (!ED->getTrailingWhereClause())
      return;
    forAllRequirementTypes(ED, [&](Type type, TypeRepr *typeRepr) {
      checkType(type, typeRepr, ED, getDiagnoser(ED, reason));
    });
  }

  void visitExtensionDecl(ExtensionDecl *ED) {
    auto extendedType = ED->getExtendedNominal();
    assert(extendedType && "valid extension with no extended type?");
    if (!extendedType || shouldSkipChecking(extendedType))
      return;

    // FIXME: We should allow conforming to implementation-only protocols,
    // but just hide that from interfaces.
    llvm::for_each(ED->getInherited(),
                   [&](TypeLoc nextInherited) {
      checkType(nextInherited, ED, getDiagnoser(ED));
    });

    bool hasPublicMembers = llvm::any_of(ED->getMembers(),
                                         [](const Decl *member) -> bool {
      auto *valueMember = dyn_cast<ValueDecl>(member);
      if (!valueMember)
        return false;
      return !shouldSkipChecking(valueMember);
    });

    if (hasPublicMembers) {
      checkType(ED->getExtendedType(),  ED->getExtendedTypeRepr(), ED,
                getDiagnoser(ED, Reason::ExtensionWithPublicMembers));
    }

    if (hasPublicMembers || !ED->getInherited().empty()) {
      Reason reason =
          hasPublicMembers ? Reason::ExtensionWithPublicMembers
                           : Reason::ExtensionWithConditionalConformances;
      checkConstrainedExtensionRequirements(ED, reason);
    }
  }

  void checkPrecedenceGroup(const PrecedenceGroupDecl *PGD,
                            const Decl *refDecl, SourceLoc diagLoc,
                            SourceRange refRange) {
    const SourceFile *SF = refDecl->getDeclContext()->getParentSourceFile();
    ModuleDecl *M = PGD->getModuleContext();
    if (!SF->isImportedImplementationOnly(M))
      return;

    auto &DE = PGD->getASTContext().Diags;
    auto diag =
        DE.diagnose(diagLoc, diag::decl_from_implementation_only_module,
                    PGD->getDescriptiveKind(), PGD->getName(),
                    static_cast<unsigned>(Reason::General), M->getName());
    if (refRange.isValid())
      diag.highlight(refRange);
    diag.flush();
    PGD->diagnose(diag::decl_declared_here, PGD->getName());
  }

  void visitInfixOperatorDecl(InfixOperatorDecl *IOD) {
    // FIXME: Handle operator designated types (which also applies to prefix
    // and postfix operators).
    if (auto *precedenceGroup = IOD->getPrecedenceGroup()) {
      if (!IOD->getIdentifierLocs().empty()) {
        checkPrecedenceGroup(precedenceGroup, IOD, IOD->getLoc(),
                             IOD->getIdentifierLocs().front());
      }
    }
  }

  void visitPrecedenceGroupDecl(PrecedenceGroupDecl *PGD) {
    llvm::for_each(PGD->getLowerThan(),
                   [&](const PrecedenceGroupDecl::Relation &relation) {
      checkPrecedenceGroup(relation.Group, PGD, PGD->getLowerThanLoc(),
                           relation.NameLoc);
    });
    llvm::for_each(PGD->getHigherThan(),
                   [&](const PrecedenceGroupDecl::Relation &relation) {
      checkPrecedenceGroup(relation.Group, PGD, PGD->getHigherThanLoc(),
                           relation.NameLoc);
    });
  }
};
} // end anonymous namespace

static void checkExtensionGenericParamAccess(const ExtensionDecl *ED) {
  auto *AA = ED->getAttrs().getAttribute<AccessControlAttr>();
  if (!AA)
    return;
  AccessLevel userSpecifiedAccess = AA->getAccess();

  AccessScope desiredAccessScope = AccessScope::getPublic();
  switch (userSpecifiedAccess) {
  case AccessLevel::Private:
    assert((ED->isInvalid() ||
            ED->getDeclContext()->isModuleScopeContext()) &&
           "non-top-level extensions make 'private' != 'fileprivate'");
    LLVM_FALLTHROUGH;
  case AccessLevel::FilePrivate: {
    const DeclContext *DC = ED->getModuleScopeContext();
    bool isPrivate = (userSpecifiedAccess == AccessLevel::Private);
    desiredAccessScope = AccessScope(DC, isPrivate);
    break;
  }
  case AccessLevel::Internal:
    desiredAccessScope = AccessScope(ED->getModuleContext());
    break;
  case AccessLevel::Public:
  case AccessLevel::Open:
    break;
  }

  AccessControlChecker().checkGenericParamAccess(
      ED, ED, desiredAccessScope, userSpecifiedAccess);
}

void swift::checkAccessControl(Decl *D) {
  if (isa<ValueDecl>(D) || isa<PatternBindingDecl>(D)) {
    AccessControlChecker().visit(D);
    UsableFromInlineChecker().visit(D);
  } else if (auto *ED = dyn_cast<ExtensionDecl>(D)) {
    checkExtensionGenericParamAccess(ED);
  }

  ExportabilityChecker().visit(D);
}
