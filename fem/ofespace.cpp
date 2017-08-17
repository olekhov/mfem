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

#include "ofespace.hpp"
#include "ointerpolation.hpp"

namespace mfem {
  OccaFiniteElementSpace::OccaFiniteElementSpace(FiniteElementSpace *fespace_) :
    device(occa::currentDevice()),
    fespace(fespace_) {
    Init();
  }

  OccaFiniteElementSpace::OccaFiniteElementSpace(occa::device device_,
                                                 FiniteElementSpace *fespace_) :
    device(device_),
    fespace(fespace_) {
    Init();
  }

  void OccaFiniteElementSpace::Init() {
    SetupLocalGlobalMaps();
    SetupOperators();
    SetupKernels();
  }

  void OccaFiniteElementSpace::SetupLocalGlobalMaps() {
    const FiniteElement &fe = *(fespace->GetFE(0));
    const TensorBasisElement *el = dynamic_cast<const TensorBasisElement*>(&fe);

    const Table &e2dTable = fespace->GetElementToDofTable();
    const int *elementMap = e2dTable.GetJ();
    const int elements = fespace->GetNE();

    globalDofs = fespace->GetNDofs();
    localDofs  = fe.GetDof();

    const int *dofMap;
    if (el) {
      dofMap = el->GetDofMap().GetData();
    } else {
      int *dofMap_ = new int[localDofs];
      for (int i = 0; i < localDofs; ++i) {
        dofMap_[i] = i;
      }
      dofMap = dofMap_;
    }

    // Allocate device offsets and indices
    globalToLocalOffsets.allocate(device,
                                  globalDofs + 1);
    globalToLocalIndices.allocate(device,
                                  localDofs, elements);
    localToGlobalMap.allocate(device,
                              localDofs, elements);

    int *offsets = globalToLocalOffsets.ptr();
    int *indices = globalToLocalIndices.ptr();
    int *l2gMap  = localToGlobalMap.ptr();

    // We'll be keeping a count of how many local nodes point
    //   to its global dof
    for (int i = 0; i <= globalDofs; ++i) {
      offsets[i] = 0;
    }

    for (int e = 0; e < elements; ++e) {
      for (int d = 0; d < localDofs; ++d) {
        const int gid = elementMap[localDofs*e + d];
        ++offsets[gid + 1];
      }
    }
    // Aggregate to find offsets for each global dof
    for (int i = 1; i <= globalDofs; ++i) {
      offsets[i] += offsets[i - 1];
    }
    // For each global dof, fill in all local nodes that point
    //   to it
    for (int e = 0; e < elements; ++e) {
      for (int d = 0; d < localDofs; ++d) {
        const int gid = elementMap[localDofs*e + dofMap[d]];
        const int lid = localDofs*e + d;
        indices[offsets[gid]++] = lid;
        l2gMap[lid] = gid;
      }
    }
    // We shifted the offsets vector by 1 by using it
    //   as a counter. Now we shift it back.
    for (int i = globalDofs; i > 0; --i) {
      offsets[i] = offsets[i - 1];
    }
    offsets[0] = 0;

    globalToLocalOffsets.keepInDevice();
    globalToLocalIndices.keepInDevice();
    localToGlobalMap.keepInDevice();

    if (!el) {
      delete [] dofMap;
    }
  }

  void OccaFiniteElementSpace::SetupOperators() {
    const SparseMatrix *R = fespace->GetRestrictionMatrix();
    const Operator *P = fespace->GetProlongationMatrix();
    CreateRPOperators(device,
                      R, P,
                      restrictionOp,
                      prolongationOp);
  }

  void OccaFiniteElementSpace::SetupKernels() {
    occa::properties props("defines: {"
                           "  TILESIZE: 256,"
                           "}");

    globalToLocalKernel = device.buildKernel("occa://mfem/fem/fespace.okl",
                                             "GlobalToLocal",
                                             props);
    localToGlobalKernel = device.buildKernel("occa://mfem/fem/fespace.okl",
                                             "LocalToGlobal",
                                             props);
  }

  FiniteElementSpace* OccaFiniteElementSpace::GetFESpace() {
    return fespace;
  }

  int OccaFiniteElementSpace::GetGlobalDofs() const {
    return globalDofs;
  }

  int OccaFiniteElementSpace::GetLocalDofs() const {
    return localDofs;
  }

  const Operator* OccaFiniteElementSpace::GetRestrictionOperator() {
    return restrictionOp;
  }

  const Operator* OccaFiniteElementSpace::GetProlongationOperator() {
    return prolongationOp;
  }

  const occa::array<int> OccaFiniteElementSpace::GetLocalToGlobalMap() const {
    return localToGlobalMap;
  }

  void OccaFiniteElementSpace::GlobalToLocal(const OccaVector &globalVec,
                                             OccaVector &localVec) const {
    globalToLocalKernel(globalDofs,
                        globalToLocalOffsets,
                        globalToLocalIndices,
                        globalVec, localVec);
  }

  // Aggregate local node values to their respective global dofs
  void OccaFiniteElementSpace::LocalToGlobal(const OccaVector &localVec,
                                             OccaVector &globalVec) const {

    localToGlobalKernel(globalDofs,
                        globalToLocalOffsets,
                        globalToLocalIndices,
                        localVec, globalVec);
  }
}

#endif
