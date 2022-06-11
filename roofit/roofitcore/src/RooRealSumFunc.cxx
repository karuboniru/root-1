/*****************************************************************************
 * Project: RooFit                                                           *
 * Package: RooFitCore                                                       *
 * @(#)root/roofitcore:$Id$
 * Authors:                                                                  *
 *   WV, Wouter Verkerke, UC Santa Barbara, verkerke@slac.stanford.edu       *
 *   DK, David Kirkby,    UC Irvine,         dkirkby@uci.edu                 *
 *                                                                           *
 * Copyright (c) 2000-2005, Regents of the University of California          *
 *                          and Stanford University. All rights reserved.    *
 *                                                                           *
 * Redistribution and use in source and binary forms,                        *
 * with or without modification, are permitted according to the terms        *
 * listed in LICENSE (http://roofit.sourceforge.net/license.txt)             *
 *****************************************************************************/

//////////////////////////////////////////////////////////////////////////////
///
/// Class RooRealSumFunc implements a PDF constructed from a sum of
/// functions:
/// ```
///                 Sum(i=1,n-1) coef_i * func_i(x) + [ 1 - (Sum(i=1,n-1) coef_i ] * func_n(x)
///   pdf(x) =    ------------------------------------------------------------------------------
///             Sum(i=1,n-1) coef_i * Int(func_i)dx + [ 1 - (Sum(i=1,n-1) coef_i ] * Int(func_n)dx
///
/// ```
/// where coef_i and func_i are RooAbsReal objects, and x is the collection of dependents.
/// In the present version coef_i may not depend on x, but this limitation may be removed in the future
///
/// ### Difference between RooAddPdf / RooRealSum{Func|Pdf}
/// - RooAddPdf is a PDF of PDFs, *i.e.* its components need to be normalised and non-negative.
/// - RooRealSumPdf is a PDF of functions, *i.e.*, its components can be negative, but their sum cannot be. The normalisation
///   is computed automatically, unless the PDF is extended (see above).
/// - RooRealSumFunc is a sum of functions. It is neither normalised, nor need it be positive.

#include "Riostream.h"

#include "TIterator.h"
#include "TList.h"
#include "TClass.h"
#include "RooRealSumFunc.h"
#include "RooRealProxy.h"
#include "RooPlot.h"
#include "RooRealVar.h"
#include "RooAddGenContext.h"
#include "RooRealConstant.h"
#include "RooRealIntegral.h"
#include "RooMsgService.h"
#include "RooNameReg.h"
#include "RooTrace.h"

#include <algorithm>
#include <memory>

using namespace std;

ClassImp(RooRealSumFunc);

bool RooRealSumFunc::_doFloorGlobal = false;

//_____________________________________________________________________________
RooRealSumFunc::RooRealSumFunc() : _normIntMgr(this, 10)
{
   // Default constructor
   // coverity[UNINIT_CTOR]
   _doFloor = false;
   TRACE_CREATE
}

//_____________________________________________________________________________
RooRealSumFunc::RooRealSumFunc(const char *name, const char *title)
   : RooAbsReal(name, title), _normIntMgr(this, 10), _haveLastCoef(false),
     _funcList("!funcList", "List of functions", this), _coefList("!coefList", "List of coefficients", this),
     _doFloor(false)
{
   // Constructor with name and title
   TRACE_CREATE
}

//_____________________________________________________________________________
RooRealSumFunc::RooRealSumFunc(const char *name, const char *title, RooAbsReal &func1, RooAbsReal &func2,
                               RooAbsReal &coef1)
   : RooAbsReal(name, title), _normIntMgr(this, 10), _haveLastCoef(false),
     _funcList("!funcList", "List of functions", this), _coefList("!coefList", "List of coefficients", this),
     _doFloor(false)
{
   // Construct p.d.f consisting of coef1*func1 + (1-coef1)*func2
   // The input coefficients and functions are allowed to be negative
   // but the resulting sum is not, which is enforced at runtime

   // Special constructor with two functions and one coefficient

   _funcList.add(func1);
   _funcList.add(func2);
   _coefList.add(coef1);
   TRACE_CREATE
}

//_____________________________________________________________________________
RooRealSumFunc::RooRealSumFunc(const char *name, const char *title, const RooArgList &inFuncList,
                               const RooArgList &inCoefList)
   : RooAbsReal(name, title), _normIntMgr(this, 10), _haveLastCoef(false),
     _funcList("!funcList", "List of functions", this), _coefList("!coefList", "List of coefficients", this),
     _doFloor(false)
{
   // Constructor p.d.f implementing sum_i [ coef_i * func_i ], if N_coef==N_func
   // or sum_i [ coef_i * func_i ] + (1 - sum_i [ coef_i ] )* func_N if Ncoef==N_func-1
   //
   // All coefficients and functions are allowed to be negative
   // but the sum is not, which is enforced at runtime.

   const std::string ownName(GetName() ? GetName() : "");
   if (!(inFuncList.getSize() == inCoefList.getSize() + 1 || inFuncList.getSize() == inCoefList.getSize())) {
      coutE(InputArguments) << "RooRealSumFunc::RooRealSumFunc(" << ownName
                            << ") number of pdfs and coefficients inconsistent, must have Nfunc=Ncoef or Nfunc=Ncoef+1"
                            << "\n";
      assert(0);
   }

   // Constructor with N functions and N or N-1 coefs

   std::string funcName;
   for (unsigned int i = 0; i < inCoefList.size(); ++i) {
      const auto& func = inFuncList[i];
      const auto& coef = inCoefList[i];

      if (!dynamic_cast<RooAbsReal const*>(&coef)) {
         const std::string coefName(coef.GetName() ? coef.GetName() : "");
         coutW(InputArguments) << "RooRealSumFunc::RooRealSumFunc(" << ownName << ") coefficient " << coefName
                               << " is not of type RooAbsReal, ignored"
                               << "\n";
         continue;
      }
      if (!dynamic_cast<RooAbsReal const*>(&func)) {
         funcName = (func.GetName() ? func.GetName() : "");
         coutW(InputArguments) << "RooRealSumFunc::RooRealSumFunc(" << ownName << ") func " << funcName
                               << " is not of type RooAbsReal, ignored"
                               << "\n";
         continue;
      }
      _funcList.add(func);
      _coefList.add(coef);
   }

   if (inFuncList.size() == inCoefList.size() + 1) {
      const auto& func = inFuncList[inFuncList.size()-1];
      if (!dynamic_cast<RooAbsReal const*>(&func)) {
         funcName = (func.GetName() ? func.GetName() : "");
         coutE(InputArguments) << "RooRealSumFunc::RooRealSumFunc(" << ownName << ") last func " << funcName
                               << " is not of type RooAbsReal, fatal error\n";
         throw std::invalid_argument("RooRealSumFunc: Function passed as is not of type RooAbsReal.");
      }
      _funcList.add(func);
   } else {
      _haveLastCoef = true;
   }

   TRACE_CREATE
}

//_____________________________________________________________________________
RooRealSumFunc::RooRealSumFunc(const RooRealSumFunc &other, const char *name)
   : RooAbsReal(other, name), _normIntMgr(other._normIntMgr, this), _haveLastCoef(other._haveLastCoef),
     _funcList("!funcList", this, other._funcList), _coefList("!coefList", this, other._coefList),
     _doFloor(other._doFloor)
{
   // Copy constructor

   TRACE_CREATE
}

//_____________________________________________________________________________
RooRealSumFunc::~RooRealSumFunc()
{
   TRACE_DESTROY
}

//_____________________________________________________________________________
double RooRealSumFunc::evaluate() const
{
   // Calculate the current value

   double value(0);

   // Do running sum of coef/func pairs, calculate lastCoef.
   RooFIter funcIter = _funcList.fwdIterator();
   RooFIter coefIter = _coefList.fwdIterator();
   RooAbsReal *coef;
   RooAbsReal *func;

   // N funcs, N-1 coefficients
   double lastCoef(1);
   while ((coef = (RooAbsReal *)coefIter.next())) {
      func = (RooAbsReal *)funcIter.next();
      double coefVal = coef->getVal();
      if (coefVal) {
         cxcoutD(Eval) << "RooRealSumFunc::eval(" << GetName() << ") coefVal = " << coefVal
                       << " funcVal = " << func->ClassName() << "::" << func->GetName() << " = " << func->getVal()
                       << endl;
         if (func->isSelectedComp()) {
            value += func->getVal() * coefVal;
         }
         lastCoef -= coef->getVal();
      }
   }

   if (!_haveLastCoef) {
      // Add last func with correct coefficient
      func = (RooAbsReal *)funcIter.next();
      if (func->isSelectedComp()) {
         value += func->getVal() * lastCoef;
      }

      cxcoutD(Eval) << "RooRealSumFunc::eval(" << GetName() << ") lastCoef = " << lastCoef
                    << " funcVal = " << func->getVal() << endl;

      // Warn about coefficient degeneration
      if (lastCoef < 0 || lastCoef > 1) {
         coutW(Eval) << "RooRealSumFunc::evaluate(" << GetName()
                     << " WARNING: sum of FUNC coefficients not in range [0-1], value=" << 1 - lastCoef << endl;
      }
   }

   // Introduce floor if so requested
   if (value < 0 && (_doFloor || _doFloorGlobal)) {
      value = 0;
   }

   return value;
}


//_____________________________________________________________________________
bool RooRealSumFunc::checkObservables(const RooArgSet *nset) const
{
   // Check if FUNC is valid for given normalization set.
   // Coeffient and FUNC must be non-overlapping, but func-coefficient
   // pairs may overlap each other
   //
   // In the present implementation, coefficients may not be observables or derive
   // from observables

   bool ret(false);

   for (unsigned int i=0; i < _coefList.size(); ++i) {
      const auto& coef = _coefList[i];
      const auto& func = _funcList[i];

      if (func.observableOverlaps(nset, coef)) {
         coutE(InputArguments) << "RooRealSumFunc::checkObservables(" << GetName() << "): ERROR: coefficient "
                               << coef.GetName() << " and FUNC " << func.GetName()
                               << " have one or more observables in common" << endl;
         ret = true;
      }
      if (coef.dependsOn(*nset)) {
         coutE(InputArguments) << "RooRealPdf::checkObservables(" << GetName() << "): ERROR coefficient "
                               << coef.GetName() << " depends on one or more of the following observables";
         nset->Print("1");
         ret = true;
      }
   }

   return ret;
}

//_____________________________________________________________________________
Int_t RooRealSumFunc::getAnalyticalIntegralWN(RooArgSet &allVars, RooArgSet &analVars, const RooArgSet *normSet2,
                                              const char *rangeName) const
{
   // cout <<
   // "RooRealSumFunc::getAnalyticalIntegralWN:"<<GetName()<<"("<<allVars<<",analVars,"<<(normSet2?*normSet2:RooArgSet())<<","<<(rangeName?rangeName:"<none>")
   // << endl;
   // Advertise that all integrals can be handled internally.

   // Handle trivial no-integration scenario
   if (allVars.empty())
      return 0;
   if (_forceNumInt)
      return 0;

   // Select subset of allVars that are actual dependents
   analVars.add(allVars);
   RooArgSet *normSet = normSet2 ? getObservables(normSet2) : 0;

   // Check if this configuration was created before
   Int_t sterileIdx(-1);
   CacheElem *cache = (CacheElem *)_normIntMgr.getObj(normSet, &analVars, &sterileIdx, RooNameReg::ptr(rangeName));
   if (cache) {
      // cout <<
      // "RooRealSumFunc("<<this<<")::getAnalyticalIntegralWN:"<<GetName()<<"("<<allVars<<","<<analVars<<","<<(normSet2?*normSet2:RooArgSet())<<","<<(rangeName?rangeName:"<none>")
      // << " -> " << _normIntMgr.lastIndex()+1 << " (cached)" << endl;
      return _normIntMgr.lastIndex() + 1;
   }

   // Create new cache element
   cache = new CacheElem;

   // Make list of function projection and normalization integrals
  for (const auto elm : _funcList) {
    const auto func = static_cast<RooAbsReal*>(elm);
      RooAbsReal *funcInt = func->createIntegral(analVars, rangeName);
     if(funcInt->InheritsFrom(RooRealIntegral::Class())) ((RooRealIntegral*)funcInt)->setAllowComponentSelection(true);
      cache->_funcIntList.addOwned(*funcInt);
      if (normSet && normSet->getSize() > 0) {
         RooAbsReal *funcNorm = func->createIntegral(*normSet);
         cache->_funcNormList.addOwned(*funcNorm);
      }
   }

   // Store cache element
   Int_t code = _normIntMgr.setObj(normSet, &analVars, (RooAbsCacheElement *)cache, RooNameReg::ptr(rangeName));

   if (normSet) {
      delete normSet;
   }

   // cout <<
   // "RooRealSumFunc("<<this<<")::getAnalyticalIntegralWN:"<<GetName()<<"("<<allVars<<","<<analVars<<","<<(normSet2?*normSet2:RooArgSet())<<","<<(rangeName?rangeName:"<none>")
   // << " -> " << code+1 << endl;
   return code + 1;
}

//_____________________________________________________________________________
double RooRealSumFunc::analyticalIntegralWN(Int_t code, const RooArgSet *normSet2, const char *rangeName) const
{
   // cout <<
   // "RooRealSumFunc::analyticalIntegralWN:"<<GetName()<<"("<<code<<","<<(normSet2?*normSet2:RooArgSet())<<","<<(rangeName?rangeName:"<none>")
   // << endl;
   // Implement analytical integrations by deferring integration of component
   // functions to integrators of components

   // Handle trivial passthrough scenario
   if (code == 0)
      return getVal(normSet2);

   // WVE needs adaptation for rangeName feature
   CacheElem *cache = (CacheElem *)_normIntMgr.getObjByIndex(code - 1);
   if (cache == 0) { // revive the (sterilized) cache
      // cout <<
      // "RooRealSumFunc("<<this<<")::analyticalIntegralWN:"<<GetName()<<"("<<code<<","<<(normSet2?*normSet2:RooArgSet())<<","<<(rangeName?rangeName:"<none>")
      // << ": reviving cache "<< endl;
      std::unique_ptr<RooArgSet> vars(getParameters(RooArgSet()));
      RooArgSet iset = _normIntMgr.selectFromSet2(*vars, code - 1);
      RooArgSet nset = _normIntMgr.selectFromSet1(*vars, code - 1);
      RooArgSet dummy;
      Int_t code2 = getAnalyticalIntegralWN(iset, dummy, &nset, rangeName);
      assert(code == code2); // must have revived the right (sterilized) slot...
      (void)code2;
      cache = (CacheElem *)_normIntMgr.getObjByIndex(code - 1);
      assert(cache != 0);
   }

   RooFIter funcIntIter = cache->_funcIntList.fwdIterator();
   RooFIter coefIter = _coefList.fwdIterator();
   RooFIter funcIter = _funcList.fwdIterator();
   RooAbsReal *coef(0), *funcInt(0), *func(0);
   double value(0);

   // N funcs, N-1 coefficients
   double lastCoef(1);
   while ((coef = (RooAbsReal *)coefIter.next())) {
      funcInt = (RooAbsReal *)funcIntIter.next();
      func = (RooAbsReal *)funcIter.next();
      double coefVal = coef->getVal(normSet2);
      if (coefVal) {
         assert(func);
         if (normSet2 == 0 || func->isSelectedComp()) {
            assert(funcInt);
            value += funcInt->getVal() * coefVal;
         }
         lastCoef -= coef->getVal(normSet2);
      }
   }

   if (!_haveLastCoef) {
      // Add last func with correct coefficient
      funcInt = (RooAbsReal *)funcIntIter.next();
      if (normSet2 == 0 || func->isSelectedComp()) {
         assert(funcInt);
         value += funcInt->getVal() * lastCoef;
      }

      // Warn about coefficient degeneration
      if (lastCoef < 0 || lastCoef > 1) {
         coutW(Eval) << "RooRealSumFunc::evaluate(" << GetName()
                     << " WARNING: sum of FUNC coefficients not in range [0-1], value=" << 1 - lastCoef << endl;
      }
   }

   double normVal(1);
   if (normSet2 && normSet2->getSize() > 0) {
      normVal = 0;

      // N funcs, N-1 coefficients
      RooAbsReal *funcNorm;
      RooFIter funcNormIter = cache->_funcNormList.fwdIterator();
      RooFIter coefIter2 = _coefList.fwdIterator();
      while ((coef = (RooAbsReal *)coefIter2.next())) {
         funcNorm = (RooAbsReal *)funcNormIter.next();
         double coefVal = coef->getVal(normSet2);
         if (coefVal) {
            assert(funcNorm);
            normVal += funcNorm->getVal() * coefVal;
         }
      }

      // Add last func with correct coefficient
      if (!_haveLastCoef) {
         funcNorm = (RooAbsReal *)funcNormIter.next();
         assert(funcNorm);
         normVal += funcNorm->getVal() * lastCoef;
      }
   }

   return value / normVal;
}

//_____________________________________________________________________________
std::list<double> *RooRealSumFunc::binBoundaries(RooAbsRealLValue &obs, double xlo, double xhi) const
{
   list<double> *sumBinB = 0;
   bool needClean(false);

   RooFIter iter = _funcList.fwdIterator();
   RooAbsReal *func;
   // Loop over components pdf
   while ((func = (RooAbsReal *)iter.next())) {

      list<double> *funcBinB = func->binBoundaries(obs, xlo, xhi);

      // Process hint
      if (funcBinB) {
         if (!sumBinB) {
            // If this is the first hint, then just save it
            sumBinB = funcBinB;
         } else {

            list<double> *newSumBinB = new list<double>(sumBinB->size() + funcBinB->size());

            // Merge hints into temporary array
            merge(funcBinB->begin(), funcBinB->end(), sumBinB->begin(), sumBinB->end(), newSumBinB->begin());

            // Copy merged array without duplicates to new sumBinBArrau
            delete sumBinB;
            delete funcBinB;
            sumBinB = newSumBinB;
            needClean = true;
         }
      }
   }

   // Remove consecutive duplicates
   if (needClean) {
      list<double>::iterator new_end = unique(sumBinB->begin(), sumBinB->end());
      sumBinB->erase(new_end, sumBinB->end());
   }

   return sumBinB;
}

//_____________________________________________________________________________B
bool RooRealSumFunc::isBinnedDistribution(const RooArgSet &obs) const
{
   // If all components that depend on obs are binned that so is the product

   RooFIter iter = _funcList.fwdIterator();
   RooAbsReal *func;
   while ((func = (RooAbsReal *)iter.next())) {
      if (func->dependsOn(obs) && !func->isBinnedDistribution(obs)) {
         return false;
      }
   }

   return true;
}

//_____________________________________________________________________________
std::list<double> *RooRealSumFunc::plotSamplingHint(RooAbsRealLValue &obs, double xlo, double xhi) const
{
   list<double> *sumHint = 0;
   bool needClean(false);

   RooFIter iter = _funcList.fwdIterator();
   RooAbsReal *func;
   // Loop over components pdf
   while ((func = (RooAbsReal *)iter.next())) {

      list<double> *funcHint = func->plotSamplingHint(obs, xlo, xhi);

      // Process hint
      if (funcHint) {
         if (!sumHint) {

            // If this is the first hint, then just save it
            sumHint = funcHint;

         } else {

            list<double> *newSumHint = new list<double>(sumHint->size() + funcHint->size());

            // Merge hints into temporary array
            merge(funcHint->begin(), funcHint->end(), sumHint->begin(), sumHint->end(), newSumHint->begin());

            // Copy merged array without duplicates to new sumHintArrau
            delete sumHint;
            sumHint = newSumHint;
            needClean = true;
         }
      }
   }

   // Remove consecutive duplicates
   if (needClean) {
      list<double>::iterator new_end = unique(sumHint->begin(), sumHint->end());
      sumHint->erase(new_end, sumHint->end());
   }

   return sumHint;
}

//_____________________________________________________________________________
void RooRealSumFunc::setCacheAndTrackHints(RooArgSet &trackNodes)
{
   // Label OK'ed components of a RooRealSumFunc with cache-and-track
   RooFIter siter = funcList().fwdIterator();
   RooAbsArg *sarg;
   while ((sarg = siter.next())) {
      if (sarg->canNodeBeCached() == Always) {
         trackNodes.add(*sarg);
         // cout << "tracking node RealSumFunc component " << sarg->ClassName() << "::" << sarg->GetName() << endl
         // ;
      }
   }
}

//_____________________________________________________________________________
void RooRealSumFunc::printMetaArgs(ostream &os) const
{
   // Customized printing of arguments of a RooRealSumFuncy to more intuitively reflect the contents of the
   // product operator construction

   bool first(true);

   if (_coefList.getSize()!=0) {
       auto funcIter = _funcList.begin();

       for (const auto coef : _coefList) {
         if (!first) {
            os << " + ";
         } else {
            first = false;
         }
         const auto func = *(funcIter++);
         os << coef->GetName() << " * " << func->GetName();
       }

       if (funcIter != _funcList.end()) {
         os << " + [%] * " << (*funcIter)->GetName() ;
       }
   } else {

      for (const auto func : _funcList) {
         if (!first) {
            os << " + ";
         } else {
            first = false;
         }
         os << func->GetName();
      }
   }

   os << " ";
}
