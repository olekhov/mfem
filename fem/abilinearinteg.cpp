// Copyright (c) 2010, Lawrence Livermore National Security, LLC. Produced at
// the Lawrence Livermore National Laboratory. LLNL-CODE-443211. All Rights
// reserved. See file COPYRIGHT for details.
//
// This file is part of the MFEM library. For more information and source code
// availability see http://mfem.org.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the GNU Lesser General Public License (as published by the Free
// Software Foundation) version 2.1 dated February 1999.

#include "../config/config.hpp"

#if defined(MFEM_USE_OCCA) && defined(MFEM_USE_ACROTENSOR)

#include "abilinearinteg.hpp"
#include "occa/modes/cuda.hpp"

namespace mfem {

AcroIntegrator::AcroIntegrator()
  {}

AcroIntegrator::~AcroIntegrator() {

}

void AcroIntegrator::Setup() {
  if (device.mode() == "CUDA") {
    onGPU = true;
    TE.SetExecutorType("OneOutPerThread");
    acro::setCudaContext(occa::cuda::getContext(device));
  } else {
    onGPU = false;
    TE.SetExecutorType("CPUInterpreted");
  }

  DiffusionIntegrator integ;
  const FiniteElement &fe   = *(fespace->GetFE(0));
  const IntegrationRule &ir = integ.GetIntegrationRule(fe, fe);

  SetMaps(ir);

  const IntegrationRule &ir1D = IntRules.Get(Geometry::SEGMENT, ir.GetOrder());
  nDim    = fe.GetDim();
  nElem  = fespace->GetNE();
  nDof   = fe.GetDof();
  nQuad   = ir.GetNPoints();
  nDof1D = fe.GetOrder() + 1;
  nQuad1D = ir1D.GetNPoints();

  if (nDim > 3) {
    mfem_error("AcroIntegrator tensor computations don't support dim > 3.");
  }

  //Get AcroTensors pointing to the B/G data
  //Note:  We are giving acrotensor the same pointer for the GPU and CPU
  //so one of them is obviously wrong.  This works as long as we don't use
  //touch the wrong one
  double *b_ptr = maps.quadToDof.memory().ptr<double>();
  double *g_ptr = maps.quadToDofD.memory().ptr<double>();
  double *w_ptr = maps.quadWeights.memory().ptr<double>();
  if (hasTensorBasis) {
    B.Init(nQuad1D, nDof1D, b_ptr, b_ptr, onGPU);
    G.Init(nQuad1D, nDof1D, g_ptr, g_ptr, onGPU);
    std::vector<int> wdims(nDim, nQuad1D);
    W.Init(wdims, w_ptr, w_ptr, onGPU);
  } else {
    B.Init(nQuad, nDof, b_ptr, b_ptr, onGPU);
    G.Init(nQuad, nDof, nDim, g_ptr, g_ptr, onGPU);
    W.Init(nQuad, w_ptr, w_ptr, onGPU);
  }
}


}

#endif