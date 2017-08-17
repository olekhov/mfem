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
#  ifndef MFEM_OCCA_COEFFICIENT
#  define MFEM_OCCA_COEFFICIENT

#include <vector>

#include "occa.hpp"

namespace mfem {
  class OccaParameter {
  public:
    virtual ~OccaParameter();

    virtual OccaParameter* Clone() = 0;

    virtual void SetProps(occa::properties &props);

    virtual occa::kernelArg KernelArgs();
  };

  //---[ Define Parameter ]------------
  template <class TM>
  class OccaDefineParameter : public OccaParameter {
  private:
    const std::string name;
    TM value;

  public:
    OccaDefineParameter(const std::string &name_,
                        const TM &value_) :
      name(name_),
      value(value_) {}

    virtual OccaParameter* Clone() {
      return new OccaDefineParameter(name, value);
    }

    virtual void SetProps(occa::properties &props) {
      props["defines"][name] = value;
    }
  };
  //====================================

  //---[ Variable Parameter ]-----------
  template <class TM>
  class OccaVariableParameter : public OccaParameter {
  private:
    const std::string name;
    const TM &value;

  public:
    OccaVariableParameter(const std::string &name_,
                          const TM &value_) :
      name(name_),
      value(value_) {}

    virtual OccaParameter* Clone() {
      return new OccaVariableParameter(name, value);
    }

    virtual void SetProps(occa::properties &props) {
      std::string &args = (props["defines/COEFF_ARGS"]
                           .asString()
                           .string());
      // const TM name,\n"
      args += "const ";
      args += occa::primitiveinfo<TM>::name;
      args += ' ';
      args += name;
      args += ",\n";
    }

    virtual occa::kernelArg KernelArgs() {
      return occa::kernelArg(value);
    }
  };
  //====================================

  //---[ Include Parameter ]------------
  class OccaIncludeParameter : public OccaParameter {
  private:
    std::string filename;

  public:
    OccaIncludeParameter(const std::string &filename_);

    virtual OccaParameter* Clone();

    virtual void SetProps(occa::properties &props);
  };
  //====================================

  //---[ Source Parameter ]------------
  class OccaSourceParameter : public OccaParameter {
  private:
    std::string source;

  public:
    OccaSourceParameter(const std::string &filename_);

    virtual OccaParameter* Clone();

    virtual void SetProps(occa::properties &props);
  };
  //====================================

  //---[ Coefficient ]------------------
  class OccaCoefficient {
  private:
    std::string name;
    occa::json coeffValue, coeffArgs;

    std::vector<OccaParameter*> params;

  public:
    OccaCoefficient(const double value = 1.0);
    OccaCoefficient(const std::string &source);
    ~OccaCoefficient();

    OccaCoefficient(const OccaCoefficient &coeff);

    OccaCoefficient& SetName(const std::string &name_);

    template <class TM>
    OccaCoefficient& AddDefine(const std::string &name_, const TM &value) {
      params.push_back(new OccaDefineParameter<TM>(name_, value));
      return *this;
    }

    template <class TM>
    OccaCoefficient& AddVariable(const std::string &name_, const TM &value) {
      params.push_back(new OccaVariableParameter<TM>(name_, value));
      return *this;
    }

    OccaCoefficient& IncludeHeader(const std::string &filename);
    OccaCoefficient& IncludeSource(const std::string &source);

    OccaCoefficient& SetProps(occa::properties &props);

    operator occa::kernelArg ();
  };
  //====================================
}

#  endif
#endif