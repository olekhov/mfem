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
#include <iostream>
#include <cmath>

#include "obilininteg.hpp"

namespace mfem {
  std::map<occa::hash_t, OccaDofQuadMaps> OccaDofQuadMaps::AllDofQuadMaps;

  OccaGeometry OccaGeometry::Get(occa::device device,
                                 Mesh &mesh,
                                 const IntegrationRule &ir,
                                 const int flags) {

    OccaGeometry geom;
    if (!mesh.GetNodes()) {
      mesh.SetCurvature(1, false, -1, Ordering::byVDIM);
    }
    GridFunction &nodes = *(mesh.GetNodes());
    const FiniteElementSpace &fespace = *(nodes.FESpace());
    const FiniteElement &fe = *(fespace.GetFE(0));

    const int dims = fe.GetDim();
    const int elements = fespace.GetNE();
    const int numDofs = fe.GetDof();
    const int numQuad = ir.GetNPoints();

    Ordering::Type originalOrdering = fespace.GetOrdering();
    nodes.ReorderByVDim();
    geom.meshNodes.allocate(device,
                            dims, numDofs, elements);

    const Table &e2dTable = fespace.GetElementToDofTable();
    const int *elementMap = e2dTable.GetJ();
    for (int e = 0; e < elements; ++e) {
      for (int d = 0; d < numDofs; ++d) {
        const int gid = elementMap[d + numDofs*e];
        for (int dim = 0; dim < dims; ++dim) {
          geom.meshNodes(dim, d, e) = nodes[dim + gid*dims];
        }
      }
    }
    geom.meshNodes.keepInDevice();

    // Reorder the original gf back
    if (originalOrdering == Ordering::byNODES) {
      nodes.ReorderByNodes();
    } else {
      nodes.ReorderByVDim();
    }

    if (flags & Jacobian) {
      geom.J.allocate(device,
                      dims*dims, numQuad, elements);
    } else {
      geom.J.allocate(device, 1);
    }
    if (flags & JacobianInv) {
      geom.invJ.allocate(device,
                         dims*dims, numQuad, elements);
    } else {
      geom.invJ.allocate(device, 1);
    }
    if (flags & JacobianDet) {
      geom.detJ.allocate(device,
                         numQuad, elements);
    } else {
      geom.detJ.allocate(device, 1);
    }

    geom.J.stopManaging();
    geom.invJ.stopManaging();
    geom.detJ.stopManaging();

    OccaDofQuadMaps &maps = OccaDofQuadMaps::GetSimplexMaps(device, fe, ir);

    occa::properties props;
    props["defines/NUM_DOFS"] = numDofs;
    props["defines/NUM_QUAD"] = numQuad;
    props["defines/STORE_JACOBIAN"]     = (flags & Jacobian);
    props["defines/STORE_JACOBIAN_INV"] = (flags & JacobianInv);
    props["defines/STORE_JACOBIAN_DET"] = (flags & JacobianDet);

    occa::kernel init = device.buildKernel("occa://mfem/fem/geometry.okl",
                                           stringWithDim("InitGeometryInfo", fe.GetDim()),
                                           props);
    init(elements,
         maps.dofToQuadD,
         geom.meshNodes,
         geom.J, geom.invJ, geom.detJ);

    return geom;
  }

  OccaDofQuadMaps::OccaDofQuadMaps() :
    hash() {}

  OccaDofQuadMaps::OccaDofQuadMaps(const OccaDofQuadMaps &maps) {
    *this = maps;
  }

  OccaDofQuadMaps& OccaDofQuadMaps::operator = (const OccaDofQuadMaps &maps) {
    hash = maps.hash;
    dofToQuad   = maps.dofToQuad;
    dofToQuadD  = maps.dofToQuadD;
    quadToDof   = maps.quadToDof;
    quadToDofD  = maps.quadToDofD;
    quadWeights = maps.quadWeights;
    return *this;
  }

  OccaDofQuadMaps& OccaDofQuadMaps::GetTensorMaps(occa::device device,
                                                  const H1_TensorBasisElement &fe,
                                                  const IntegrationRule &ir) {

    occa::hash_t hash = (occa::hash(device)
                         ^ "Tensor Element"
                         ^ ("BasisType: " + occa::toString(fe.GetBasisType()))
                         ^ ("Order: " + occa::toString(fe.GetOrder()))
                         ^ ("Quad: " + occa::toString(ir.GetNPoints())));

    // If we've already made the dof-quad maps, reuse them
    OccaDofQuadMaps &maps = AllDofQuadMaps[hash];
    if (maps.hash.isInitialized()) {
      return maps;
    }

    // Create the dof-quad maps
    maps.hash = hash;

    const Poly_1D::Basis &basis = fe.GetBasis();
    const int order = fe.GetOrder();
    const int dofs = order + 1;
    const int dims = fe.GetDim();

    // Create the dof -> quadrature point map
    const IntegrationRule &ir1D = IntRules.Get(Geometry::SEGMENT, ir.GetOrder());
    const int quadPoints = ir1D.GetNPoints();
    const int quadPoints2D = quadPoints*quadPoints;
    const int quadPoints3D = quadPoints2D*quadPoints;
    const int quadPointsND = ((dims == 1) ? quadPoints :
                              ((dims == 2) ? quadPoints2D : quadPoints3D));

    // Initialize the dof -> quad mapping
    maps.dofToQuad.allocate(device,
                            quadPoints, dofs);
    maps.dofToQuadD.allocate(device,
                             quadPoints, dofs);
    maps.quadToDof.allocate(device,
                            dofs, quadPoints);
    maps.quadToDofD.allocate(device,
                             dofs, quadPoints);

    // Initialize quad weights
    maps.quadWeights.allocate(device,
                              quadPointsND);
    double *quadWeights1DData = new double[quadPoints];

    mfem::Vector d2q(dofs);
    mfem::Vector d2qD(dofs);
    for (int q = 0; q < quadPoints; ++q) {
      const IntegrationPoint &ip = ir1D.IntPoint(q);
      basis.Eval(ip.x, d2q, d2qD);
      quadWeights1DData[q] = ip.weight;
      for (int d = 0; d < dofs; ++d) {
        maps.dofToQuad(q, d)  = d2q[d];
        maps.dofToQuadD(q, d) = d2qD[d];
        maps.quadToDof(d, q)  = d2q[d];
        maps.quadToDofD(d, q) = d2qD[d];
      }
    }

    for (int q = 0; q < quadPointsND; ++q) {
      const int qx = q % quadPoints;
      const int qz = q / quadPoints2D;
      const int qy = (q - qz*quadPoints2D) / quadPoints;
      double w = quadWeights1DData[qx];
      if (dims > 1) {
        w *= quadWeights1DData[qy];
      }
      if (dims > 2) {
        w *= quadWeights1DData[qz];
      }
      maps.quadWeights[q] = w;
    }

    maps.dofToQuad.keepInDevice();
    maps.dofToQuadD.keepInDevice();
    maps.quadToDof.keepInDevice();
    maps.quadToDofD.keepInDevice();
    maps.quadWeights.keepInDevice();

    delete [] quadWeights1DData;

    return maps;
  }

  OccaDofQuadMaps& OccaDofQuadMaps::GetSimplexMaps(occa::device device,
                                                   const FiniteElement &fe,
                                                   const IntegrationRule &ir) {

    occa::hash_t hash = (occa::hash(device)
                         ^ "Simplex Element"
                         ^ ("Order: " + occa::toString(fe.GetOrder()))
                         ^ ("Quad: " + occa::toString(ir.GetNPoints())));

    // If we've already made the dof-quad maps, reuse them
    OccaDofQuadMaps &maps = AllDofQuadMaps[hash];
    if (maps.hash.isInitialized()) {
      return maps;
    }

    // Create the dof-quad maps
    maps.hash = hash;
    const int dims = fe.GetDim();
    const int numDofs = fe.GetDof();
    const int numQuad = ir.GetNPoints();

    // Initialize the dof -> quad mapping
    maps.dofToQuad.allocate(device,
                            numQuad, numDofs);
    maps.dofToQuadD.allocate(device,
                             dims, numQuad, numDofs);
    maps.quadToDof.allocate(device,
                            numDofs, numQuad);
    maps.quadToDofD.allocate(device,
                             dims, numDofs, numQuad);
    // Initialize quad weights
    maps.quadWeights.allocate(device,
                              numQuad);

    Vector d2q(numDofs);
    DenseMatrix d2qD(numDofs, dims);
    for (int q = 0; q < numQuad; ++q) {
      const IntegrationPoint &ip = ir.IntPoint(q);
      maps.quadWeights[q] = ip.weight;
      fe.CalcShape(ip, d2q);
      fe.CalcDShape(ip, d2qD);
      for (int d = 0; d < numDofs; ++d) {
        const double w = d2q[d];
        maps.dofToQuad(q, d) = w;
        maps.quadToDof(d, q) = w;
        for (int dim = 0; dim < dims; ++dim) {
          const double wD = d2qD(d, dim);
          maps.dofToQuadD(dim, q, d) = wD;
          maps.quadToDofD(dim, d, q) = wD;
        }
      }
    }

    maps.dofToQuad.keepInDevice();
    maps.dofToQuadD.keepInDevice();
    maps.quadToDof.keepInDevice();
    maps.quadToDofD.keepInDevice();
    maps.quadWeights.keepInDevice();

    return maps;
  }

  //---[ Integrator Defines ]-----------
  std::string stringWithDim(const std::string &s, const int dim) {
    std::string ret = s;
    ret += ('0' + (char) dim);
    ret += 'D';
    return ret;
  }

  int closestWarpBatchTo(const int value) {
    return ((value + 31) / 32) * 32;
  }

  int closestMultipleWarpBatch(const int multiple, const int maxSize) {
    int batch = (32 / multiple);
    int minDiff = 32 - (multiple * batch);
    for (int i = 64; i <= maxSize; i += 32) {
      const int newDiff = i - (multiple * (i / multiple));
      if (newDiff < minDiff) {
        batch = (i / multiple);
        minDiff = newDiff;
      }
    }
    return batch;
  }

  void setTensorProperties(const FiniteElement &fe,
                           const IntegrationRule &ir,
                           occa::properties &props) {

    const IntegrationRule &ir1D = IntRules.Get(Geometry::SEGMENT, ir.GetOrder());

    const int numDofs = fe.GetDof();
    const int numQuad = ir.GetNPoints();

    const int dofs1D = fe.GetOrder() + 1;
    const int quad1D = ir1D.GetNPoints();
    int dofsND = dofs1D;
    int quadND = quad1D;

    props["defines/USING_TENSOR_OPS"] = 1;
    props["defines/NUM_DOFS"] = numDofs;
    props["defines/NUM_QUAD"] = numQuad;

    for (int d = 1; d <= 3; ++d) {
      if (d > 1) {
        dofsND *= dofs1D;
        quadND *= quad1D;
      }
      props["defines"][stringWithDim("NUM_DOFS_", d)] = dofsND;
      props["defines"][stringWithDim("NUM_QUAD_", d)] = quadND;
    }

    // 1D Defines
    const int m1InnerBatch = 32 * ((quad1D + 31) / 32);
    props["defines/A1_ELEMENT_BATCH"]       = closestMultipleWarpBatch(quad1D, 2048);
    props["defines/M1_OUTER_ELEMENT_BATCH"] = closestMultipleWarpBatch(m1InnerBatch, 2048);
    props["defines/M1_INNER_ELEMENT_BATCH"] = m1InnerBatch;

    // 2D Defines
    props["defines/A2_ELEMENT_BATCH"] = 1;
    props["defines/A2_QUAD_BATCH"]    = 1;
    props["defines/M2_ELEMENT_BATCH"] = 32;

    // 3D Defines
    const int a3QuadBatch = closestMultipleWarpBatch(quadND, 2048);
    props["defines/A3_ELEMENT_BATCH"] = closestMultipleWarpBatch(a3QuadBatch, 2048);
    props["defines/A3_QUAD_BATCH"]    = a3QuadBatch;
  }

  void setSimplexProperties(const FiniteElement &fe,
                            const IntegrationRule &ir,
                            occa::properties &props) {

    const int numDofs = fe.GetDof();
    const int numQuad = ir.GetNPoints();
    const int maxDQ   = numDofs > numQuad ? numDofs : numQuad;

    props["defines/USING_TENSOR_OPS"] = 0;
    props["defines/NUM_DOFS"] = numDofs;
    props["defines/NUM_QUAD"] = numQuad;

    // 2D Defines
    const int quadBatch = closestWarpBatchTo(numQuad);
    props["defines/A2_ELEMENT_BATCH"] = closestMultipleWarpBatch(quadBatch, 2048);
    props["defines/A2_QUAD_BATCH"]    = quadBatch;
    props["defines/M2_INNER_BATCH"]   = closestWarpBatchTo(maxDQ);

    // 3D Defines
    props["defines/A3_ELEMENT_BATCH"] = closestMultipleWarpBatch(quadBatch, 2048);
    props["defines/A3_QUAD_BATCH"]    = quadBatch;
    props["defines/M3_INNER_BATCH"]   = closestWarpBatchTo(maxDQ);
  }

  //---[ Base Integrator ]--------------
  OccaIntegrator::OccaIntegrator() {}
  OccaIntegrator::~OccaIntegrator() {}

  OccaIntegrator* OccaIntegrator::CreateInstance(occa::device device_,
                                                 OccaBilinearForm &obf,
                                                 BilinearFormIntegrator *integrator_,
                                                 const occa::properties &props_,
                                                 const OccaIntegratorType itype_) {
    OccaIntegrator *newIntegrator = CreateInstance();

    newIntegrator->device = device_;
    newIntegrator->integrator = integrator_;
    newIntegrator->fespace = &(obf.GetFESpace());
    newIntegrator->mesh = &(obf.GetMesh());
    newIntegrator->props = props_;
    newIntegrator->itype = itype_;

    newIntegrator->Setup();

    return newIntegrator;
  }

  std::string OccaIntegrator::GetName() {
    return integrator->Name();
  }

  void OccaIntegrator::Setup() {}

  void OccaIntegrator::SetupCoefficient(const Coefficient *coeff,
                                        occa::properties &kernelProps) {

    if (dynamic_cast<const ConstantCoefficient*>(coeff)) {
      const ConstantCoefficient* c =
        dynamic_cast<const ConstantCoefficient*>(coeff);
      kernelProps["defines/COEFF_ARGS"] = "";
      kernelProps["defines/COEFF"]      = c->constant;
    } else if (dynamic_cast<const GridFunctionCoefficient*>(coeff)) {
      kernelProps["headers"].asArray();
      kernelProps["headers"] += ("double gridFunctionCoeff(const int e,\n"
                                 "                         const int q,\n"
                                 "                         const DofToQuad_t restrict dofToQuad,\n"
                                 "                         Local_t restrict gfValues) {\n"
                                 "  double c = 0;\n"
                                 "  for (int d = 0; d < NUM_DOFS; ++d) {\n"
                                 "    c += dofToQuad(q, d) * values(d, e);\n"
                                 "  }\n"
                                 "  return c;\n"
                                 "}\n\n");
      kernelProps["defines/COEFF_ARGS"] = ("const DofToQuad_t restrict dofToQuad,\n"
                                           "Local_t restrict gfValues,\n");
      kernelProps["defines/COEFF"]      = "gridFunctionCoeff(e, q, dofToQuad, gfValues)";
    } else {
      mfem_error("OccaIntegrator can only handle:\n"
                 "  - ConstantCoefficient\n"
                 "  - GridFunctionCoefficient\n");
    }
  }

  occa::kernel OccaIntegrator::GetAssembleKernel(const occa::properties &props) {
    const FiniteElement &fe = *(fespace->GetFE(0));
    return GetKernel(stringWithDim("Assemble", fe.GetDim()),
                     props);
  }

  occa::kernel OccaIntegrator::GetMultKernel(const occa::properties &props) {
    const FiniteElement &fe = *(fespace->GetFE(0));
    return GetKernel(stringWithDim("Mult", fe.GetDim()),
                     props);
  }

  occa::kernel OccaIntegrator::GetKernel(const std::string &kernelName,
                                         const occa::properties &props) {
    const std::string filename = GetName() + ".okl";
    return device.buildKernel("occa://mfem/fem/" + filename,
                              kernelName,
                              props);
  }
  //====================================

  //---[ Diffusion Integrator ]---------
  OccaDiffusionIntegrator::OccaDiffusionIntegrator() :
    OccaIntegrator() {}

  OccaDiffusionIntegrator::~OccaDiffusionIntegrator() {}

  OccaIntegrator* OccaDiffusionIntegrator::CreateInstance() {
    return new OccaDiffusionIntegrator();
  }

  void OccaDiffusionIntegrator::Setup() {
    occa::properties kernelProps = props;

    DiffusionIntegrator &integ = (DiffusionIntegrator&) *integrator;
    coeff = integ.GetCoefficient();
    SetupCoefficient(coeff, kernelProps);

    const FiniteElement &fe   = *(fespace->GetFE(0));
    const IntegrationRule &ir = integ.GetIntegrationRule(fe, fe);

    const H1_TensorBasisElement *el = dynamic_cast<const H1_TensorBasisElement*>(&fe);
    if (el) {
      maps = OccaDofQuadMaps::GetTensorMaps(device, *el, ir);
      setTensorProperties(fe, ir, kernelProps);
    } else {
      maps = OccaDofQuadMaps::GetSimplexMaps(device, fe, ir);
      setSimplexProperties(fe, ir, kernelProps);
    }

    const int dims = fe.GetDim();
    const int symmDims = (dims * (dims + 1)) / 2; // 1x1: 1, 2x2: 3, 3x3: 6

    const int elements = fespace->GetNE();
    const int quadraturePoints = ir.GetNPoints();

    assembledOperator.allocate(symmDims, quadraturePoints, elements);

    OccaGeometry geom = OccaGeometry::Get(device,
                                          *mesh, ir,
                                          OccaGeometry::Jacobian);
    jacobian = geom.J;

    // Setup assemble and mult kernels
    assembleKernel = GetAssembleKernel(kernelProps);
    multKernel     = GetMultKernel(kernelProps);
  }

  void OccaDiffusionIntegrator::Assemble() {
    if (dynamic_cast<const ConstantCoefficient*>(coeff)) {
      assembleKernel((int) fespace->GetNE(),
                     maps.quadWeights.memory(),
                     jacobian.memory(),
                     assembledOperator.memory());
    } else if (dynamic_cast<const GridFunctionCoefficient*>(coeff)) {
      assembleKernel((int) fespace->GetNE(),
                     maps.quadWeights.memory(),
                     jacobian.memory(),
                     maps.dofToQuad.memory(),
                     // GF vector
                     assembledOperator.memory());
    }
  }

  void OccaDiffusionIntegrator::Mult(OccaVector &x) {
    multKernel((int) fespace->GetNE(),
               maps.dofToQuad.memory(),
               maps.dofToQuadD.memory(),
               maps.quadToDof.memory(),
               maps.quadToDofD.memory(),
               assembledOperator.memory(),
               x);
  }
  //====================================

  //---[ Mass Integrator ]--------------
  OccaMassIntegrator::OccaMassIntegrator() :
    OccaIntegrator() {}

  OccaMassIntegrator::~OccaMassIntegrator() {}

  OccaIntegrator* OccaMassIntegrator::CreateInstance() {
    return new OccaMassIntegrator();
  }

  void OccaMassIntegrator::Setup() {
    occa::properties kernelProps = props;

    MassIntegrator &integ = (MassIntegrator&) *integrator;
    coeff = integ.GetCoefficient();
    SetupCoefficient(coeff, kernelProps);

    const FiniteElement &fe   = *(fespace->GetFE(0));
    const IntegrationRule &ir = integ.GetIntegrationRule(fe, fe);

    const H1_TensorBasisElement *el = dynamic_cast<const H1_TensorBasisElement*>(&fe);
    if (el) {
      maps = OccaDofQuadMaps::GetTensorMaps(device, *el, ir);
      setTensorProperties(fe, ir, kernelProps);
    } else {
      maps = OccaDofQuadMaps::GetSimplexMaps(device, fe, ir);
      setSimplexProperties(fe, ir, kernelProps);
    }

    const int elements = fespace->GetNE();
    const int quadraturePoints = ir.GetNPoints();

    assembledOperator.allocate(quadraturePoints, elements);

    OccaGeometry geom = OccaGeometry::Get(device,
                                          *mesh, ir,
                                          OccaGeometry::Jacobian);
    jacobian = geom.J;

    // Setup assemble and mult kernels
    assembleKernel = GetAssembleKernel(kernelProps);
    multKernel     = GetMultKernel(kernelProps);
  }

  void OccaMassIntegrator::Assemble() {
    if (dynamic_cast<const ConstantCoefficient*>(coeff)) {
      assembleKernel((int) fespace->GetNE(),
                     maps.quadWeights.memory(),
                     jacobian.memory(),
                     assembledOperator.memory());
    } else if (dynamic_cast<const GridFunctionCoefficient*>(coeff)) {
      assembleKernel((int) fespace->GetNE(),
                     maps.quadWeights.memory(),
                     jacobian.memory(),
                     maps.dofToQuad.memory(),
                     // GF vector
                     assembledOperator.memory());
    }
  }

  void OccaMassIntegrator::Mult(OccaVector &x) {
    multKernel((int) fespace->GetNE(),
               maps.dofToQuad.memory(),
               maps.dofToQuadD.memory(),
               maps.quadToDof.memory(),
               maps.quadToDofD.memory(),
               assembledOperator.memory(),
               x);
  }
  //====================================
}

#endif
