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

#ifdef MFEM_USE_OCCA

#include "obilinearform.hpp"
#include "obilininteg.hpp"
#include "../linalg/osparsemat.hpp"

#include "tfe.hpp"

namespace mfem {
  //---[ Bilinear Form ]----------------
  OccaBilinearForm::OccaBilinearForm(OccaFiniteElementSpace *ofespace_) :
    Operator(ofespace_->GetGlobalDofs(),
             ofespace_->GetGlobalDofs()) {
    Init(occa::getDevice(), ofespace_, ofespace_);
  }

  OccaBilinearForm::OccaBilinearForm(occa::device device_,
                                     OccaFiniteElementSpace *ofespace_) :
    Operator(ofespace_->GetGlobalDofs(),
             ofespace_->GetGlobalDofs()) {
    Init(device, ofespace_, ofespace_);
  }

  OccaBilinearForm::OccaBilinearForm(OccaFiniteElementSpace *ofespace_,
                                     OccaFiniteElementSpace *ofespace2_) :
    Operator(ofespace_->GetGlobalDofs(),
             ofespace2_->GetGlobalDofs()) {
    Init(occa::getDevice(), ofespace_, ofespace2_);
  }

  OccaBilinearForm::OccaBilinearForm(occa::device device_,
                                     OccaFiniteElementSpace *ofespace_,
                                     OccaFiniteElementSpace *ofespace2_) :
    Operator(ofespace_->GetGlobalDofs(),
             ofespace2_->GetGlobalDofs()) {
    Init(device, ofespace_, ofespace2_);
  }

  void OccaBilinearForm::Init(occa::device device_,
                              OccaFiniteElementSpace *ofespace_,
                              OccaFiniteElementSpace *ofespace2_) {
    device = device_;

    ofespace = ofespace_;
    fespace  = ofespace_->GetFESpace();

    ofespace2 = ofespace2_;
    fespace2  = ofespace2_->GetFESpace();

    mesh = fespace->GetMesh();

    const int vdim  = fespace->GetVDim();
    const int vdim2 = fespace2->GetVDim();

    baseKernelProps["defines/NUM_VDIM"]  = vdim;
    baseKernelProps["defines/NUM_VDIM2"] = vdim2;

    const int elements   = GetNE();
    const int localDofs  = ofespace->GetLocalDofs();
    const int localDofs2 = ofespace2->GetLocalDofs();

    const bool spacesDiffer = ((localDofs * vdim) != (localDofs2 * vdim2));

    localX.SetSize(device, elements * localDofs);
    if (spacesDiffer) {
      localY.SetSize(device, elements * localDofs2);
    } else {
      localY.SetDataAndSize(localX.GetData(), localX.Size());
    }

    // First-touch policy when running with OpenMP
    if (device.mode() == "OpenMP") {
      occa::kernel initLocalKernel = device.buildKernel("occa://mfem/fem/utils.okl",
                                                        "InitLocalVector");
      initLocalKernel(elements, localDofs, vdim, localX);
      if (spacesDiffer) {
        initLocalKernel(elements, localDofs2, vdim2, localY);
      }
    }
  }

  occa::device OccaBilinearForm::GetDevice() {
    return device;
  }

  int OccaBilinearForm::BaseGeom() const {
    return mesh->GetElementBaseGeometry();
  }

  int OccaBilinearForm::GetDim() const {
    return mesh->Dimension();
  }

  int64_t OccaBilinearForm::GetNE() const {
    return mesh->GetNE();
  }

  Mesh& OccaBilinearForm::GetMesh() const {
    return *mesh;
  }

  FiniteElementSpace& OccaBilinearForm::GetFESpace() const {
    return *fespace;
  }

  FiniteElementSpace& OccaBilinearForm::GetFESpace2() const {
    return *fespace2;
  }

  OccaFiniteElementSpace& OccaBilinearForm::GetOccaFESpace() const {
    return *ofespace;
  }

  OccaFiniteElementSpace& OccaBilinearForm::GetOccaFESpace2() const {
    return *ofespace2;
  }

  int64_t OccaBilinearForm::GetNDofs() const {
    return fespace->GetNDofs();
  }

  int64_t OccaBilinearForm::GetNDofs2() const {
    return fespace2->GetNDofs();
  }

  int64_t OccaBilinearForm::GetVDim() const {
    return fespace->GetVDim();
  }

  int64_t OccaBilinearForm::GetVDim2() const {
    return fespace2->GetVDim();
  }

  const FiniteElement& OccaBilinearForm::GetFE(const int i) const {
    return *(fespace->GetFE(i));
  }

  const FiniteElement& OccaBilinearForm::GetFE2(const int i) const {
    return *(fespace2->GetFE(i));
  }

  // Adds new Domain Integrator.
  void OccaBilinearForm::AddDomainIntegrator(OccaIntegrator *integrator,
                                             const occa::properties &props) {
    AddIntegrator(integrator, props, DomainIntegrator);
  }

  // Adds new Boundary Integrator.
  void OccaBilinearForm::AddBoundaryIntegrator(OccaIntegrator *integrator,
                                               const occa::properties &props) {
    AddIntegrator(integrator, props, BoundaryIntegrator);
  }

  // Adds new interior Face Integrator.
  void OccaBilinearForm::AddInteriorFaceIntegrator(OccaIntegrator *integrator,
                                                   const occa::properties &props) {
    AddIntegrator(integrator, props, InteriorFaceIntegrator);
  }

  // Adds new boundary Face Integrator.
  void OccaBilinearForm::AddBoundaryFaceIntegrator(OccaIntegrator *integrator,
                                                   const occa::properties &props) {
    AddIntegrator(integrator, props, BoundaryFaceIntegrator);
  }

  // Adds Integrator based on OccaIntegratorType
  void OccaBilinearForm::AddIntegrator(OccaIntegrator *integrator,
                                       const occa::properties &props,
                                       const OccaIntegratorType itype) {
    if (integrator == NULL) {
      std::stringstream error_ss;
      error_ss << "OccaBilinearForm::";
      switch (itype) {
      case DomainIntegrator      : error_ss << "AddDomainIntegrator";       break;
      case BoundaryIntegrator    : error_ss << "AddBoundaryIntegrator";     break;
      case InteriorFaceIntegrator: error_ss << "AddInteriorFaceIntegrator"; break;
      case BoundaryFaceIntegrator: error_ss << "AddBoundaryFaceIntegrator"; break;
      }
      error_ss << " (...):\n"
               << "  Integrator is NULL";
      const std::string error = error_ss.str();
      mfem_error(error.c_str());
    }
    integrator->SetupIntegrator(*this, baseKernelProps + props, itype);
    integrators.push_back(integrator);
  }

  // Get the finite element space prolongation matrix
  const Operator* OccaBilinearForm::GetProlongation() const {
    return ofespace->GetProlongationOperator();
  }

  // Get the finite element space restriction matrix
  const Operator* OccaBilinearForm::GetRestriction() const {
    return ofespace->GetRestrictionOperator();
  }

  //
  void OccaBilinearForm::Assemble() {
    // [MISSING] Find geometric information that is needed by intergrators
    //             to share between integrators.
    const int integratorCount = (int) integrators.size();
    for (int i = 0; i < integratorCount; ++i) {
      integrators[i]->Assemble();
    }
  }

  void OccaBilinearForm::FormLinearSystem(const Array<int> &ess_tdof_list,
                                          OccaVector &x, OccaVector &b,
                                          Operator *&Aout,
                                          OccaVector &X, OccaVector &B,
                                          int copy_interior) {
    FormOperator(ess_tdof_list, Aout);
    InitRHS(ess_tdof_list, x, b, Aout, X, B, copy_interior);
  }

  void OccaBilinearForm::FormOperator(const Array<int> &ess_tdof_list,
                                      Operator *&Aout) {
    const Operator *P = GetProlongation();
    Operator *rap = this;

    if (P) {
      rap = new OccaRAPOperator(*P, *this, *P);
    }

    Aout = new OccaConstrainedOperator(device,
                                       rap, ess_tdof_list,
                                       rap != this);
  }

  void OccaBilinearForm::InitRHS(const Array<int> &ess_tdof_list,
                                 OccaVector &x, OccaVector &b,
                                 Operator *A,
                                 OccaVector &X, OccaVector &B,
                                 int copy_interior) {
    const Operator *P = GetProlongation();
    const Operator *R = GetRestriction();

    if (P) {
      // Variational restriction with P
      B.SetSize(device, P->Width());
      P->MultTranspose(b, B);
      X.SetSize(device, R->Height());
      R->Mult(x, X);
    } else {
      // rap, X and B point to the same data as this, x and b
      X.NewDataAndSize(x.GetData(), x.Size());
      B.NewDataAndSize(b.GetData(), b.Size());
    }

    if (!copy_interior) {
      X.SetSubVectorComplement(ess_tdof_list, 0.0);
    }

    OccaConstrainedOperator *cA = static_cast<OccaConstrainedOperator*>(A);
    if (cA) {
      cA->EliminateRHS(X, B);
    } else {
      mfem_error("OccaBilinearForm::InitRHS expects an OccaConstrainedOperator");
    }
  }

  // Matrix vector multiplication.
  void OccaBilinearForm::Mult(const OccaVector &x, OccaVector &y) const {
    ofespace->GlobalToLocal(x, localX);

    const int integratorCount = (int) integrators.size();
    for (int i = 0; i < integratorCount; ++i) {
      integrators[i]->Mult(localX, localY);
    }

    ofespace2->LocalToGlobal(localY, y);
  }

  // Matrix transpose vector multiplication.
  void OccaBilinearForm::MultTranspose(const OccaVector &x, OccaVector &y) const {
    mfem_error("occa::OccaBilinearForm::MultTranspose() is not overloaded!");
  }


  void OccaBilinearForm::RecoverFEMSolution(const OccaVector &X,
                                            const OccaVector &b,
                                            OccaVector &x) {
    TRecoverFEMSolution<OccaVector>(X, b, x);
  }

  // Frees memory bilinear form.
  OccaBilinearForm::~OccaBilinearForm() {
    // Make sure all integrators free their data
    IntegratorVector::iterator it = integrators.begin();
    while (it != integrators.end()) {
      delete *it;
      ++it;
    }
  }
  //====================================

  //---[ Constrained Operator ]---------
  occa::kernelBuilder OccaConstrainedOperator::mapDofBuilder =
                  makeCustomBuilder("vector_map_dofs",
                                    "const int idx = v2[i];"
                                    "v0[idx] = v1[idx];",
                                    "defines: { VTYPE2: 'int' }");

  occa::kernelBuilder OccaConstrainedOperator::clearDofBuilder =
                  makeCustomBuilder("vector_clear_dofs",
                                    "v0[v1[i]] = 0.0;",
                                    "defines: { VTYPE1: 'int' }");

  OccaConstrainedOperator::OccaConstrainedOperator(Operator *A_,
                                                   const Array<int> &constraintList_,
                                                   bool own_A_) :
    Operator(A_->Height(), A_->Width()) {
    Setup(occa::getDevice(), A_, constraintList_, own_A_);
  }

  OccaConstrainedOperator::OccaConstrainedOperator(occa::device device_,
                                                   Operator *A_,
                                                   const Array<int> &constraintList_,
                                                   bool own_A_) :
    Operator(A_->Height(), A_->Width()) {
    Setup(device_, A_, constraintList_, own_A_);
  }

  void OccaConstrainedOperator::Setup(occa::device device_,
                                      Operator *A_,
                                      const Array<int> &constraintList_,
                                      bool own_A_) {
    device = device_;

    A = A_;
    own_A = own_A_;

    constraintIndices = constraintList_.Size();
    if (constraintIndices) {
      constraintList.allocate(device,
                              constraintIndices,
                              constraintList_.GetData());
      constraintList.keepInDevice();
    }

    z.SetSize(device, height);
    w.SetSize(device, height);
  }

  void OccaConstrainedOperator::EliminateRHS(const OccaVector &x, OccaVector &b) const {
    if (constraintIndices == 0) {
      return;
    }
    occa::kernel mapDofs = mapDofBuilder.build(device);

    w = 0.0;

    mapDofs(constraintIndices, w, x, constraintList);

    A->Mult(w, z);

    b -= z;

    mapDofs(constraintIndices, b, x, constraintList);
  }

  void OccaConstrainedOperator::Mult(const OccaVector &x, OccaVector &y) const {
    if (constraintIndices == 0) {
      A->Mult(x, y);
      return;
    }

    occa::kernel mapDofs   = mapDofBuilder.build(device);
    occa::kernel clearDofs = clearDofBuilder.build(device);

    z = x;

    clearDofs(constraintIndices, z, constraintList);

    A->Mult(z, y);

    mapDofs(constraintIndices, y, x, constraintList);
  }

  OccaConstrainedOperator::~OccaConstrainedOperator() {
    if (own_A) {
      delete A;
    }
  }
  //====================================
}

#endif
